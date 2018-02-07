/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2014 Cloudius Systems
 */

#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/temporary_buffer.hh"
#include "core/distributed.hh"
#include <vector>
#include <iostream>

#include "netstar/port_manager.hh"
#include "netstar/stack/stack_manager.hh"
#include "netstar/hookpoint/hook_manager.hh"

using namespace seastar;
using namespace netstar;

static std::string str_ping{"ping"};
static std::string str_txtx{"txtx"};
static std::string str_rxrx{"rxrx"};
static std::string str_pong{"pong"};
static std::string str_unknow{"unknow cmd"};
static int tx_msg_total_size = 100 * 1024 * 1024;
static int tx_msg_size = 4 * 1024;
static int tx_msg_nr = tx_msg_total_size / tx_msg_size;
// static int rx_msg_size = 4 * 1024;
static std::string str_txbuf(tx_msg_size, 'X');
static bool enable_tcp = false;
static bool enable_sctp = false;

class tcp_server {
    std::vector<server_socket> _tcp_listeners;
    std::vector<server_socket> _sctp_listeners;
    unsigned _stack_id;
public:
    tcp_server(unsigned stack_id) : _stack_id(stack_id) {}

    future<> listen(ipv4_addr addr) {
        if (enable_tcp) {
            listen_options lo;
            lo.proto = transport::TCP;
            lo.reuse_address = true;
            // _tcp_listeners.push_back(engine().listen(make_ipv4_address(addr), lo));
            _tcp_listeners.push_back(stack_manager::get().stack(_stack_id).listen(make_ipv4_address(addr), lo));
            do_accepts(_tcp_listeners);
        }

        if (enable_sctp) {
            listen_options lo;
            lo.proto = transport::SCTP;
            lo.reuse_address = true;
            // _sctp_listeners.push_back(engine().listen(make_ipv4_address(addr), lo));
            _sctp_listeners.push_back(stack_manager::get().stack(_stack_id).listen(make_ipv4_address(addr), lo));
            do_accepts(_sctp_listeners);
        }
        return make_ready_future<>();
    }

    // FIXME: We should properly tear down the service here.
    future<> stop() {
        return make_ready_future<>();
    }

    void do_accepts(std::vector<server_socket>& listeners) {
        int which = listeners.size() - 1;
        listeners[which].accept().then([this, &listeners] (connected_socket fd, socket_address addr) mutable {
            // fprint(std::cout, "Get a new connection.\n");
            auto conn = new connection(*this, std::move(fd), addr);
            conn->process().then_wrapped([conn] (auto&& f) {
                delete conn;
                try {
                    f.get();
                } catch (std::exception& ex) {
                    std::cout << "request error " << ex.what() << "\n";
                }
            });
            do_accepts(listeners);
        }).then_wrapped([] (auto&& f) {
            try {
                f.get();
            } catch (std::exception& ex) {
                std::cout << "accept failed: " << ex.what() << "\n";
            }
        });
    }
    class connection {
        connected_socket _fd;
        input_stream<char> _read_buf;
        output_stream<char> _write_buf;
    public:
        connection(tcp_server& server, connected_socket&& fd, socket_address addr)
            : _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output()) {}
        future<> process() {
             return read();
        }
        future<> read() {
            if (_read_buf.eof()) {
                return make_ready_future();
            }
            // Expect 4 bytes cmd from client
            size_t n = 4;
            return _read_buf.read_exactly(n).then([this] (temporary_buffer<char> buf) {
                if (buf.size() == 0) {
                    return make_ready_future();
                }
                auto cmd = std::string(buf.get(), buf.size());
                // pingpong test
                if (cmd == str_ping) {
                    return _write_buf.write(str_pong).then([this] {
                        return _write_buf.flush();
                    }).then([this] {
                        return this->read();
                    });
                // server tx test
                } else if (cmd == str_txtx) {
                    return tx_test();
                // server tx test
                } else if (cmd == str_rxrx) {
                    return rx_test();
                // unknow test
                } else {
                    return _write_buf.write(str_unknow).then([this] {
                        return _write_buf.flush();
                    }).then([] {
                        return make_ready_future();
                    });
                }
            });
        }
        future<> do_write(int end) {
            if (end == 0) {
                return make_ready_future<>();
            }
            return _write_buf.write(str_txbuf).then([this] {
                return _write_buf.flush();
            }).then([this, end] {
                return do_write(end - 1);
            });
        }
        future<> tx_test() {
            return do_write(tx_msg_nr).then([this] {
                return _write_buf.close();
            }).then([] {
                return make_ready_future<>();
            });
        }
        future<> do_read() {
            return _read_buf.read()/*_exactly(rx_msg_size)*/.then([this] (temporary_buffer<char> buf) {
                if (buf.size() == 0) {
                    return make_ready_future();
                } else {
                    return do_read();
                }
            });
        }
        future<> rx_test() {
            return do_read().then([] {
                return make_ready_future<>();
            });
        }
    };
};

namespace bpo = boost::program_options;

int main(int ac, char** av) {
    app_template app;
    app.add_options()
        ("port", bpo::value<uint16_t>()->default_value(10000), "TCP server port")
        ("tcp", bpo::value<std::string>()->default_value("yes"), "tcp listen")
        ("sctp", bpo::value<std::string>()->default_value("no"), "sctp listen") ;
    return app.run_deprecated(ac, av, [&] {
        auto& config = app.configuration();
        uint16_t port = config["port"].as<uint16_t>();
        enable_tcp = config["tcp"].as<std::string>() == "yes";
        enable_sctp = config["sctp"].as<std::string>() == "yes";
        if (!enable_tcp && !enable_sctp) {
            fprint(std::cerr, "Error: no protocols enabled. Use \"--tcp yes\" and/or \"--sctp yes\" to enable\n");
            return engine().exit(1);
        }
        auto server0 = new distributed<tcp_server>;
        auto server1 = new distributed<tcp_server>;

        /*server->start().then([server = std::move(server), port] () mutable {
            engine().at_exit([server] {
                return server->stop();
            });
            server->invoke_on_all(&tcp_server::listen, ipv4_addr{port});
        }).then([port] {
            std::cout << "Seastar TCP server listening on port " << port << " ...\n";
        });*/

        port_manager::get().add_port(config, 0, port_type::standard).then([&config]{
            return port_manager::get().add_port(config, 1, port_type::standard);
        }).then([]{
            return stack_manager::get().add_stack(0, "10.28.1.12", "10.28.1.1", "255.255.255.0");
        }).then([]{
            return stack_manager::get().add_stack(1, "10.29.1.12", "10.29.1.1", "255.255.255.0");
        }).then([]{
            return hook_manager::get().add_hook_point(hook_type::pure_stack, 0);
        }).then([]{
            return hook_manager::get().add_hook_point(hook_type::pure_stack, 1);
        }).then([]{
            return hook_manager::get().invoke_on_all(0, &hook::attach_stack, unsigned(0));
        }).then([]{
            return hook_manager::get().invoke_on_all(1, &hook::attach_stack, unsigned(1));
        }).then([]{
            return hook_manager::get().invoke_on_all(0, &hook::check_and_start);
        }).then([]{
            return hook_manager::get().invoke_on_all(1, &hook::check_and_start);
        }).then([server0, port]{
            return server0->start(0).then([server0, port] () mutable{
                engine().at_exit([server0]{
                    return server0->stop();
                });
                return server0->invoke_on_all(&tcp_server::listen, ipv4_addr{port});
            });
        }).then([port](){
           std::cout << "Seastar TCP server0 listening on port " << port << " ...\n";
           return;
        }).then([server1, port]{
            return server1->start(1).then([server1, port] () mutable{
                engine().at_exit([server1]{
                    return server1->stop();
                });
                return server1->invoke_on_all(&tcp_server::listen, ipv4_addr{port});
            });
        }).then([port](){
           std::cout << "Seastar TCP server1 listening on port " << port << " ...\n";
           return;
        });
    });
}
