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

#include "netstar/preprocessor/tcp_ppr.hh"

#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/sleep.hh"

#include "netstar/port_manager.hh"
#include "netstar/stack/stack_manager.hh"
#include "netstar/hookpoint/hook_manager.hh"
#include "netstar/mica/mica_client.hh"

#include "netstar/asyncflow/sd_async_flow.hh"
#include "netstar/asyncflow/async_flow.hh"

#include "netstar/preprocessor/udp_ppr.hh"

using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

struct fake_val {
    uint64_t v[3];
};

class forwarder;
distributed<forwarder> forwarders;

class forwarder {
    sd_async_flow_manager<tcp_ppr> _tcp_forward;
    sd_async_flow_manager<udp_ppr> _udp_forward;
    mica_client& _mc;
public:
    forwarder() : _mc(std::ref(mica_manager::get().mc())){
        hook_manager::get().hOok(0).send_to_sdaf_manager(_tcp_forward);
        hook_manager::get().hOok(0).receive_from_sdaf_manager(_tcp_forward);

        hook_manager::get().hOok(0).send_to_sdaf_manager(_udp_forward);
        hook_manager::get().hOok(0).receive_from_sdaf_manager(_udp_forward);
    }

    future<> stop() {
        return make_ready_future<>();
    }

    future<> mica_test(int ) {
        // only test mica performance on thread 1.
        return repeat([this]{
            uint64_t key = engine().cpu_id()+1;
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            return _mc.query(Operation::kGet,sizeof(key), key_buf.get_temp_buffer(),
                             0, temporary_buffer<char>()).then([this](mica_response response){
                assert(response.get_result() == Result::kNotFound);

                uint64_t key = engine().cpu_id()+1;
                extendable_buffer key_buf;
                key_buf.fill_data(key);

                uint64_t val = 6;
                extendable_buffer val_buf;
                val_buf.fill_data(val);
                return _mc.query(Operation::kSet, sizeof(key), key_buf.get_temp_buffer(),
                                 sizeof(val), val_buf.get_temp_buffer());
            }).then([this](mica_response response){
                assert(response.get_result() == Result::kSuccess);

                uint64_t key = engine().cpu_id()+1;
                extendable_buffer key_buf;
                key_buf.fill_data(key);

                return _mc.query(Operation::kGet,sizeof(key), key_buf.get_temp_buffer(),
                                 0, temporary_buffer<char>());
            }).then([this](mica_response response){
                assert(response.get_value<uint64_t>() == 6);

                uint64_t key = engine().cpu_id()+1;
                extendable_buffer key_buf;
                key_buf.fill_data(key);

                return _mc.query(Operation::kDelete,
                                 sizeof(key), key_buf.get_temp_buffer(),
                                 0, temporary_buffer<char>());
            }).then([this](mica_response response){
                assert(response.get_result() == Result::kSuccess);
            }).then_wrapped([](auto&& f){
                try{
                    f.get();
                    fprint(std::cout, "mica_test succeeds on core %d!\n", engine().cpu_id());
                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                }
                catch(...) {
                    fprint(std::cout, "mica_test fails on core %d, retry in 3s.\n", engine().cpu_id());
                    return sleep(3s).then([]{
                        return stop_iteration::no;
                    });
                }
            });
        });
    }

    struct wtf {
        uint64_t v1;
        uint64_t v2;
    };

    void run_udp_manager(int) {
        repeat([this]{
            return _udp_forward.on_new_initial_context().then([this]() mutable {
                auto ic = _udp_forward.get_initial_context();

                do_with(ic.get_sd_async_flow(), [this](sd_async_flow<udp_ppr>& ac){
                    ac.register_events(udp_events::pkt_in);
                    return ac.run_async_loop([&ac, this](){
                        if(ac.cur_event().on_close_event()) {
                            return make_ready_future<af_action>(af_action::close_forward);
                        }

                        auto src_ip = wtf{ac.get_flow_key_hash(), ac.get_flow_key_hash()};
                        extendable_buffer key_buf;
                        key_buf.fill_data(src_ip);
                        return this->_mc.query(Operation::kGet, sizeof(src_ip), key_buf.get_temp_buffer(),
                                               0, temporary_buffer<char>())/*.then([&ac, this](mica_response response){
                            auto src_ip = wtf{ac.get_flow_key_hash(), ac.get_flow_key_hash()};
                            extendable_buffer key_buf;
                            key_buf.fill_data(src_ip);

                            if(response.get_result() == Result::kNotFound) {
                                // fprint(std::cout,"Key does not exist.\n");
                                fake_val val;
                                extendable_buffer val_buf;
                                val_buf.fill_data(val);

                                return this->_mc.query(Operation::kSet,
                                        sizeof(src_ip), key_buf.get_temp_buffer(),
                                        sizeof(val), val_buf.get_temp_buffer());
                            }
                            else{
                                // fprint(std::cout,"Key exist.\n");
                                fake_val val;
                                extendable_buffer val_buf;
                                val_buf.fill_data(val);

                                return this->_mc.query(Operation::kSet,
                                                       sizeof(src_ip), key_buf.get_temp_buffer(),
                                                       sizeof(val), val_buf.get_temp_buffer());
                            }
                        }).then([&ac, this](mica_response response){
                            auto src_ip = wtf{ac.get_flow_key_hash(), ac.get_flow_key_hash()};
                            extendable_buffer key_buf;
                            key_buf.fill_data(src_ip);
                            return this->_mc.query(Operation::kGet, sizeof(src_ip), key_buf.get_temp_buffer(),
                                                   0, temporary_buffer<char>()).then([&ac, this](mica_response response){
                                auto src_ip = wtf{ac.get_flow_key_hash(), ac.get_flow_key_hash()};
                                extendable_buffer key_buf;
                                key_buf.fill_data(src_ip);
                                if(response.get_result() == Result::kNotFound) {
                                    // fprint(std::cout,"Key does not exist.\n");
                                    fake_val val;
                                    extendable_buffer val_buf;
                                    val_buf.fill_data(val);
                                    return this->_mc.query(Operation::kSet,
                                            sizeof(src_ip), key_buf.get_temp_buffer(),
                                            sizeof(val), val_buf.get_temp_buffer());
                                }
                                else{
                                    // fprint(std::cout,"Key exist.\n");
                                    fake_val val;
                                    extendable_buffer val_buf;
                                    val_buf.fill_data(val);
                                    return this->_mc.query(Operation::kSet,
                                                           sizeof(src_ip), key_buf.get_temp_buffer(),
                                                           sizeof(val), val_buf.get_temp_buffer());
                                }
                            });
                        }).then([&ac, this](mica_response response){
                            auto src_ip = wtf{ac.get_flow_key_hash(), ac.get_flow_key_hash()};
                            extendable_buffer key_buf;
                            key_buf.fill_data(src_ip);
                            return this->_mc.query(Operation::kGet, sizeof(src_ip), key_buf.get_temp_buffer(),
                                                   0, temporary_buffer<char>()).then([&ac, this](mica_response response){
                                auto src_ip = wtf{ac.get_flow_key_hash(), ac.get_flow_key_hash()};
                                extendable_buffer key_buf;
                                key_buf.fill_data(src_ip);
                                if(response.get_result() == Result::kNotFound) {
                                    // fprint(std::cout,"Key does not exist.\n");
                                    fake_val val;
                                    extendable_buffer val_buf;
                                    val_buf.fill_data(val);
                                    return this->_mc.query(Operation::kSet,
                                            sizeof(src_ip), key_buf.get_temp_buffer(),
                                            sizeof(val), val_buf.get_temp_buffer());
                                }
                                else{
                                    // fprint(std::cout,"Key exist.\n");
                                    fake_val val;
                                    extendable_buffer val_buf;
                                    val_buf.fill_data(val);
                                    return this->_mc.query(Operation::kSet,
                                                           sizeof(src_ip), key_buf.get_temp_buffer(),
                                                           sizeof(val), val_buf.get_temp_buffer());
                                }
                            });
                        })*/.then_wrapped([&ac, this](auto&& f){
                            try{
                                f.get();
                                return af_action::forward;

                            }
                            catch(...){
                                if(this->_mc.nr_request_descriptors() == 0){
                                    this->_insufficient_mica_rd_erorr += 1;
                                }
                                else{
                                    this->_mica_timeout_error += 1;
                                }
                                return af_action::drop;
                            }
                        });
                    });
                }).then([](){
                    // printf("client async flow is closed.\n");
                });

                return stop_iteration::no;
            });
        });
    }

    struct info {
        uint64_t ingress_received;
        uint64_t egress_send;
        size_t active_flow_num;

        unsigned mica_timeout_error;
        unsigned insufficient_mica_rd_erorr;

        uint64_t mica_send;
        uint64_t mica_recv;

        unsigned epoch_mismatch;

        void operator+=(const info& o) {
            ingress_received += o.ingress_received;
            egress_send += o.egress_send;
            active_flow_num += o.active_flow_num;
            mica_timeout_error += o.mica_timeout_error;
            insufficient_mica_rd_erorr += o.insufficient_mica_rd_erorr;
            mica_send += o.mica_send;
            mica_recv += o.mica_recv;
            epoch_mismatch += o.epoch_mismatch;
        }
    };
    info _old{0,0,0,0,0,0,0, 0};
    unsigned _mica_timeout_error = 0;
    unsigned _insufficient_mica_rd_erorr=0;

    future<info> get_info() {
        /*return make_ready_future<info>(info{_ingress_port.get_qp_wrapper().rx_pkts(),
                                            _egress_port.get_qp_wrapper().tx_pkts(),
                                            _egress_port.peek_failed_send_cout(),
                                            _udp_manager.peek_active_flow_num()});*/
        return make_ready_future<info>(info{port_manager::get().pOrt(0).rx_pkts(),
                                            port_manager::get().pOrt(0).tx_pkts(),
                                            _udp_forward.peek_active_flow_num(),
                                            _mica_timeout_error,
                                            _insufficient_mica_rd_erorr,
                                            port_manager::get().pOrt(1).rx_pkts(),
                                            port_manager::get().pOrt(1).tx_pkts(),
                                            });
    }
    void collect_stats(int) {
        repeat([this]{
            return forwarders.map_reduce(adder<info>(), &forwarder::get_info).then([this](info i){
                fprint(std::cout, "ingress_received=%d, egress_send=%d, active_flow_num=%d, mica_timeout_error=%d, insufficient_mica_rd_erorr=%d, mica_send=%d, mica_recv=%d.\n",
                        i.ingress_received-_old.ingress_received,
                        i.egress_send - _old.egress_send,
                        i.active_flow_num,
                        i.mica_timeout_error - _old.mica_timeout_error,
                        i.insufficient_mica_rd_erorr - _old.insufficient_mica_rd_erorr,
                        i.mica_send - _old.mica_send,
                        i.mica_recv - _old.mica_recv);
                _old = i;
            }).then([]{
                return seastar::sleep(1s).then([]{
                    return stop_iteration::no;
                });
            });
        });
    }
};



int main(int ac, char** av) {
    app_template app;
    sd_async_flow_manager<tcp_ppr> m1;
    sd_async_flow_manager<udp_ppr> m2;
    async_flow_manager<tcp_ppr> m3;
    async_flow_manager<udp_ppr> m4;

    return app.run_deprecated(ac, av, [&app] {
        auto& opts = app.configuration();

        port_manager::get().add_port(opts, 0, port_type::standard).then([&opts]{
            return port_manager::get().add_port(opts, 1, port_type::fdir);
        }).then([&opts]{
            return mica_manager::get().add_mica_client(opts, 1);
        }).then([]{
            return hook_manager::get().add_hook_point(hook_type::sd_async_flow, 0);
        }).then([]{
            engine().at_exit([]{
                return forwarders.stop();
            });
            return forwarders.start();
        }).then([]{
            return hook_manager::get().invoke_on_all(0, &hook::check_and_start);
        })/*.then([]{
            return forwarders.invoke_on_all(&forwarder::mica_test, 1);
        }).then([]{
            return forwarders.invoke_on_all(&forwarder::mica_test, 1);
        }).then([]{
            return forwarders.invoke_on_all(&forwarder::mica_test, 1);
        })*/.then([]{
            return forwarders.invoke_on_all(&forwarder::run_udp_manager, 1);
        }).then([]{
            return forwarders.invoke_on(0, &forwarder::collect_stats, 1);
        }).then([]{
            fprint(std::cout, "forwarder runs!\n");

        });

    });
}
