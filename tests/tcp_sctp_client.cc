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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "core/app-template.hh"
#include "core/future-util.hh"
#include "core/distributed.hh"
#include "core/sleep.hh"

#include "netstar/port_manager.hh"
#include "netstar/stack/stack_manager.hh"
#include "netstar/hookpoint/hook_manager.hh"
using namespace netstar;

using namespace seastar;
using namespace net;
using namespace std::chrono_literals;

// To change the size of the transmitted message, change rx_msg_size!!!!
// Otherwise it leads to stupid errors!!!!
static int rx_msg_size = /*4 * 1024*/4*1024;
static int tx_msg_total_size = 100 * 1024 * 1024;
static int tx_msg_size = rx_msg_size;
static int tx_msg_nr = tx_msg_total_size / tx_msg_size;
static std::string str_txbuf(tx_msg_size, 'X');

class client;
distributed<client> clients;

transport protocol = transport::TCP;

class client {
private:
    static constexpr unsigned _pings_per_connection = 10000;
    unsigned _total_pings;
    unsigned _concurrent_connections;
    ipv4_addr _server_addr;
    std::string _test;
    lowres_clock::time_point _earliest_started;
    lowres_clock::time_point _latest_finished;
    bool _earliest_started_recorded = false;
    size_t _processed_bytes;
    unsigned _num_reported;

    timer<lowres_clock> _reporter;
    size_t _previous_monitored_processed_bytes = 0;
public:
    size_t local_processed_bytes=0;
public:
    class connection {
        connected_socket _fd;
        input_stream<char> _read_buf;
        output_stream<char> _write_buf;
        size_t _bytes_read = 0;
        size_t _bytes_write = 0;
    public:
        connection(connected_socket&& fd)
            : _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output()) {}

        future<> do_read() {
            return _read_buf.read_exactly(rx_msg_size).then([this] (temporary_buffer<char> buf) {
                _bytes_read += buf.size();
                clients.local().local_processed_bytes += buf.size();
                if (buf.size() == 0) {
                    return make_ready_future();
                } else {
                    return do_read();
                }
            });
        }

        future<> do_write(int end) {
            if (end == 0) {
                return make_ready_future();
            }
            return _write_buf.write(str_txbuf).then([this] {
                _bytes_write += tx_msg_size;
                clients.local().local_processed_bytes += tx_msg_size;
                return _write_buf.flush();
            }).then([this, end] {
                return do_write(end - 1);
            });
        }

        future<> ping(int times) {
            return _write_buf.write("ping").then([this] {
                return _write_buf.flush();
            }).then([this, times] {
                return _read_buf.read_exactly(4).then([this, times] (temporary_buffer<char> buf) {
                    if (buf.size() != 4) {
                        fprint(std::cerr, "illegal packet received: %d\n", buf.size());
                        return make_ready_future();
                    }
                    auto str = std::string(buf.get(), buf.size());
                    if (str != "pong") {
                        fprint(std::cerr, "illegal packet received: %d\n", buf.size());
                        return make_ready_future();
                    }
                    if (times > 0) {
                        return ping(times - 1);
                    } else {
                        return make_ready_future();
                    }
                });
            });
        }

        future<size_t> rxrx() {
            return _write_buf.write("rxrx").then([this] {
                return _write_buf.flush();
            }).then([this] {
                return do_write(tx_msg_nr).then([this] {
                    return _write_buf.close();
                }).then([this] {
                    return make_ready_future<size_t>(_bytes_write);
                });
            });
        }

        future<size_t> txtx() {
            return _write_buf.write("txtx").then([this] {
                return _write_buf.flush();
            }).then([this] {
                return do_read().then([this] {
                    return make_ready_future<size_t>(_bytes_read);
                });
            });
        }
    };
private:
    std::vector<connection*> _connected_connections;
public:
    future<> ping_test(connection *conn) {
        auto started = lowres_clock::now();
        return conn->ping(_pings_per_connection).then([started] {
            auto finished = lowres_clock::now();
            clients.invoke_on(0, &client::ping_report, started, finished);
        });
    }

    future<> rxrx_test(connection *conn) {
        auto started = lowres_clock::now();
        return conn->rxrx().then([started] (size_t bytes) {
            auto finished = lowres_clock::now();
            clients.invoke_on(0, &client::rxtx_report, started, finished, bytes);
        });
    }

    future<> txtx_test(connection *conn) {
        auto started = lowres_clock::now();
        return conn->txtx().then([started] (size_t bytes) {
            auto finished = lowres_clock::now();
            clients.invoke_on(0, &client::rxtx_report, started, finished, bytes);
        });
    }

    void ping_report(lowres_clock::time_point started, lowres_clock::time_point finished) {
        if (!_earliest_started_recorded) {
            _earliest_started_recorded = true;
            _earliest_started = started;
        }
        if (_latest_finished < finished)
            _latest_finished = finished;
        if (++_num_reported == _concurrent_connections) {
            auto elapsed = _latest_finished - _earliest_started;
            auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            auto secs = static_cast<double>(usecs) / static_cast<double>(1000 * 1000);
            fprint(std::cout, "========== ping ============\n");
            fprint(std::cout, "Server: %s\n", _server_addr);
            fprint(std::cout,"Connections: %u\n", _concurrent_connections);
            fprint(std::cout, "Total PingPong: %u\n", _total_pings);
            fprint(std::cout, "Total Time(Secs): %f\n", secs);
            fprint(std::cout, "Requests/Sec: %f\n",
                static_cast<double>(_total_pings) / secs);
            clients.stop().then([] {
                engine().exit(0);
            });
        }
    }

    void rxtx_report(lowres_clock::time_point started, lowres_clock::time_point finished, size_t bytes) {
        if (!_earliest_started_recorded) {
            _earliest_started_recorded = true;
            _earliest_started = started;
        }
        if (_latest_finished < finished)
            _latest_finished = finished;
        _processed_bytes += bytes;
        if (++_num_reported == _concurrent_connections) {
            auto elapsed = _latest_finished - _earliest_started;
            auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            auto secs = static_cast<double>(usecs) / static_cast<double>(1000 * 1000);
            fprint(std::cout, "========== %s ============\n", _test);
            fprint(std::cout, "Server: %s\n", _server_addr);
            fprint(std::cout, "Connections: %u\n", _concurrent_connections);
            fprint(std::cout, "Bytes Received(MiB): %u\n", _processed_bytes/1024/1024);
            fprint(std::cout, "Total Time(Secs): %f\n", secs);
            fprint(std::cout, "Bandwidth(Gbits/Sec): %f\n",
                static_cast<double>((_processed_bytes * 8)) / (1000 * 1000 * 1000) / secs);
            clients.stop().then([] {
                engine().exit(0);
            });
        }
    }

    future<> start_connections(ipv4_addr server_addr, std::string test, unsigned ncon) {
        _server_addr = server_addr;
        _concurrent_connections = ncon * smp::count;
        _total_pings = _pings_per_connection * _concurrent_connections;
        _test = test;
        _latest_finished = lowres_clock::now();

        /*return repeat([server_addr](){
            socket_address local = socket_address(::sockaddr_in{AF_INET, INADDR_ANY, {0}});
            return stack_manager::get().stack(0).connect(make_ipv4_address(server_addr), local, protocol).then_wrapped([](auto&& future_fd){
                try {
                    future_fd.get();
                    return make_ready_future<stop_iteration>(stop_iteration::yes);;
                }
                catch(...) {
                    fprint(std::cout, "Attempted connection fails on core %d, try again in 2s.\n", engine().cpu_id());
                    return sleep(std::chrono::seconds(2s)).then([]{
                         return stop_iteration::no;
                    });
                }
            });
        }).then([server_addr, test, ncon, this]{
            return repeat([server_addr, test, ncon, this](){
                socket_address local = socket_address(::sockaddr_in{AF_INET, INADDR_ANY, {0}});
                return stack_manager::get().stack(0).connect(make_ipv4_address(server_addr), local, protocol).then_wrapped([this, ncon](future<connected_socket> f){
                    try{
                        auto t = f.get();
                        auto conn = new connection(std::move(std::get<0>(t)));
                        _connected_connections.push_back(conn);
                        if(_connected_connections.size() == ncon){
                            return clients.invoke_on(0, &client::report_connection_done, ncon, engine().cpu_id()).then([](){
                                return stop_iteration::yes;
                            });
                        }
                        else{
                            return make_ready_future<stop_iteration>(stop_iteration::no);
                        }
                    }
                    catch(std::exception& e){
                        fprint(std::cout, "Exception happen when creating the connection.\n");
                        return make_ready_future<stop_iteration>(stop_iteration::no);
                    }
                });
            });
        });*/
        return repeat([server_addr, test, ncon, this](){
            socket_address local = socket_address(::sockaddr_in{AF_INET, INADDR_ANY, {0}});
            return stack_manager::get().stack(0).connect(make_ipv4_address(server_addr), local, protocol).then_wrapped([this, ncon](future<connected_socket> f){
                try{
                    auto t = f.get();
                    auto conn = new connection(std::move(std::get<0>(t)));
                    _connected_connections.push_back(conn);
                    if(_connected_connections.size() == ncon){
                        return clients.invoke_on(0, &client::report_connection_done, ncon, engine().cpu_id()).then([](){
                            return stop_iteration::yes;
                        });
                    }
                    else{
                        return make_ready_future<stop_iteration>(stop_iteration::no);
                    }
                }
                catch(std::exception& e){
                    fprint(std::cout, "Exception happen when creating the connection.\n");
                    return make_ready_future<stop_iteration>(stop_iteration::no);
                }
            });
        });
    }
    void report_connection_done(int ncon, unsigned cpu_id) {
        fprint(std::cout, "%d connections are created on core %d\n", ncon, cpu_id);
    }
    void start_the_test(std::string test) {
        for(auto conn : _connected_connections) {
            (this->*tests.at(test))(conn).then_wrapped([conn] (auto&& f) {
                delete conn;
                try {
                    f.get();
                } catch (std::exception& ex) {
                    fprint(std::cerr, "request error: %s\n", ex.what());
                }
            });
        }
        _connected_connections.clear();
    }
    void start_bandwidth_monitoring(int) {
        assert(!_reporter.armed());
        _previous_monitored_processed_bytes = local_processed_bytes;
        _reporter.set_callback([this](){
            size_t diff = local_processed_bytes - _previous_monitored_processed_bytes;
            float bandwidth = static_cast<double>((diff * 8)) / (1000.0 * 1000.0 * 1000.0);
            _previous_monitored_processed_bytes = local_processed_bytes;
            clients.invoke_on(0, &client::report_bandwidth_monitoring, bandwidth, engine().cpu_id());
        });
        _reporter.arm_periodic(1s);
    }
    void report_bandwidth_monitoring(float bandwidth, unsigned core_id) {
        fprint(std::cout, "TCP bandwidth on core %d: %f(Gbits/sec)\n", core_id, bandwidth);
        if(core_id == 0){
            fprint(std::cout, "Remaining active connections: %d.\n", _concurrent_connections-_num_reported);
        }
    }
    future<> stop() {
        if(_reporter.armed()) {
            _reporter.cancel();
        }
        return make_ready_future();
    }

    typedef future<> (client::*test_fn)(connection *conn);
    static const std::map<std::string, test_fn> tests;
public:
    class connection_tester {
        connected_socket _fd;
        output_stream<char> _write_buf;
        size_t _bytes_write = 0;
        size_t _snap_shot = 0;
        unsigned _invoke_counter = 0;
        timer<lowres_clock> _t;
        bool _quit = false;
        promise<> _pr;
    public:
        connection_tester(connected_socket&& fd)
            : _fd(std::move(fd))
            , _write_buf(_fd.output()) {}

        future<> do_write() {
            return _write_buf.write(str_txbuf).then([this] {
                if(_quit) {
                    return make_ready_future();
                }
                else{
                    _bytes_write += tx_msg_size;
                    return _write_buf.flush().then([this]{
                        if(_quit){
                            return make_ready_future();
                        }
                        else{
                            return do_write();
                        }
                    });
                }
            });
        }

        future<> rxrx() {
            return _write_buf.write("rxrx").then([this] {
                return _write_buf.flush();
            }).then([this] {
                return do_write();
            });
        }

        future<> run() {
            rxrx().then_wrapped([this](auto&& f){
                try {
                    f.get();
                }
                catch(...){
                    if(_t.armed()) {
                        _t.cancel();
                        _pr.set_exception(std::runtime_error("wtf?"));
                    }
                }
                delete this;
            });
            _t.set_callback([this]{
                _invoke_counter += 1;
                if(engine().cpu_id() == 0) {
                    // only print on core 0.
                    fprint(std::cout,"snap_shot=%d, bytes_write=%d.\n", _snap_shot, _bytes_write);
                }
                if(_snap_shot == _bytes_write)  {
                    _quit = true;
                    _fd.shutdown_output();
                    _t.cancel();
                    _pr.set_exception(std::runtime_error("wtf?"));
                }
                if(_invoke_counter == 20) {
                    _quit = true;
                    _fd.shutdown_output();
                    _t.cancel();
                    _pr.set_value();
                }
                _snap_shot = _bytes_write;
            });
            _t.arm_periodic(1s);
            return _pr.get_future();
        }
    };

    future<> run_tester(ipv4_addr server_addr){
        return repeat([server_addr](){
            socket_address local = socket_address(::sockaddr_in{AF_INET, INADDR_ANY, {0}});
            return stack_manager::get().stack(0).connect(make_ipv4_address(server_addr), local, protocol).then_wrapped([](auto&& future_fd){
                try {
                    auto t = future_fd.get();
                    auto tester = new connection_tester(std::move(std::get<0>(t)));
                    fprint(std::cout, "A new connection tester is created on core %d.\n", engine().cpu_id());
                    return tester->run().then_wrapped([](auto&& f){
                        try{
                            f.get();
                            fprint(std::cout, "connection tester succeeds on core %d.\n", engine().cpu_id());
                            return stop_iteration::yes;
                        }
                        catch(...){
                            fprint(std::cout, "connection tester fails on core %d.\n", engine().cpu_id());
                            return stop_iteration::no;
                        }
                    });
                }
                catch(...) {
                    fprint(std::cout, "Fails to create connection teste on core %d, try again in 2s.\n", engine().cpu_id());
                    return sleep(std::chrono::seconds(2s)).then([]{
                         return stop_iteration::no;
                    });
                }
            });
        });
    }
};

namespace bpo = boost::program_options;

int main(int ac, char ** av) {
    app_template app;
    app.add_options()
        ("server", bpo::value<std::string>()->required(), "Server address")
        ("test", bpo::value<std::string>()->default_value("ping"), "test type(ping | rxrx | txtx)")
        ("conn", bpo::value<unsigned>()->default_value(16), "nr connections per cpu")
        ("proto", bpo::value<std::string>()->default_value("tcp"), "transport protocol tcp|sctp")
        ("time", bpo::value<unsigned>()->default_value(60), "total transmission time")
        ;
    unsigned max = 1;

    return app.run_deprecated(ac, av, [&app, max] {
        auto& config = app.configuration();
        auto server = config["server"].as<std::string>();
        auto test = config["test"].as<std::string>();
        auto ncon = config["conn"].as<unsigned>();
        auto proto = config["proto"].as<std::string>();
        auto time = config["time"].as<unsigned>();

        auto sem = std::make_shared<semaphore>(0);

        size_t total_transmission_bytes = static_cast<size_t>(1024*1024*1024)*static_cast<size_t>(time)/static_cast<size_t>(8);
        total_transmission_bytes *= 10;
        size_t per_connection_transmission_bytes = total_transmission_bytes/static_cast<size_t>((ncon*smp::count));
        tx_msg_nr = per_connection_transmission_bytes/tx_msg_size + 1;

        if (proto == "tcp") {
            protocol = transport::TCP;
        } else if (proto == "sctp") {
            protocol = transport::SCTP;
        } else {
            fprint(std::cerr, "Error: --proto=tcp|sctp\n");
            return engine().exit(1);
        }

        if (!client::tests.count(test)) {
            fprint(std::cerr, "Error: -test=ping | rxrx | txtx\n");
            return engine().exit(1);
        }

        /*clients.start().then([server,sem, max]{
            for(unsigned i=0; i<max; i++) {
                clients.invoke_on_all(&client::run_tester, ipv4_addr{server}).then([sem](){
                   fprint(std::cout, "tester finishes.\n");
                   sem->signal();
                });
            }
            return sem->wait(max);
        }).then([server, test, ncon] () {
            return clients.invoke_on_all(&client::start_connections, ipv4_addr{server}, test, ncon).then([](){
                fprint(std::cout, "All connections are done.\n");
            });
        }).then([test](){
            clients.invoke_on_all(&client::start_the_test, test);
            clients.invoke_on_all(&client::start_bandwidth_monitoring, 1);
        });*/
        port_manager::get().add_port(config, config["dpdk-port-idx"].as<unsigned>(), port_type::standard).then([&config]{
            return stack_manager::get().add_stack(0,
                                                  config["host-ipv4-addr"].as<std::string>(),
                                                  config["gw-ipv4-addr"].as<std::string>(),
                                                  config["netmask-ipv4-addr"].as<std::string>());
        }).then([]{
            return hook_manager::get().add_hook_point(hook_type::pure_stack, 0);
        }).then([]{
            return hook_manager::get().invoke_on_all(0, &hook::attach_stack, unsigned(0));
        }).then([]{
            return hook_manager::get().invoke_on_all(0, &hook::check_and_start);
        }).then([server, sem, max]{
            return clients.start().then([server, sem, max]{
                for(unsigned i=0; i<max; i++) {
                    clients.invoke_on_all(&client::run_tester, ipv4_addr{server}).then([sem](){
                       fprint(std::cout, "tester finishes.\n");
                       sem->signal();
                    });
                }
                return sem->wait(max);
            });
        }).then([server, test, ncon] () {
            return clients.invoke_on_all(&client::start_connections, ipv4_addr{server}, test, ncon).then([](){
                fprint(std::cout, "All connections are done.\n");
            });
        }).then([test](){
            clients.invoke_on_all(&client::start_the_test, test);
            clients.invoke_on_all(&client::start_bandwidth_monitoring, 1);
        });;
    });
}

const std::map<std::string, client::test_fn> client::tests = {
        {"ping", &client::ping_test},
        {"rxrx", &client::rxrx_test},
        {"txtx", &client::txtx_test},
};
