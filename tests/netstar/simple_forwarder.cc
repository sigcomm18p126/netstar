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

class forwarder;
distributed<forwarder> forwarders;

class forwarder {
    sd_async_flow_manager<tcp_reorder_ppr> _tcp_forward;
    sd_async_flow_manager<tcp_ppr> _tcp_reverse;
    sd_async_flow_manager<udp_ppr> _udp_forward;
    sd_async_flow_manager<udp_ppr> _udp_reverse;
public:
    forwarder() {
        hook_manager::get().hOok(0).send_to_sdaf_manager(_tcp_forward);
        hook_manager::get().hOok(0).receive_from_sdaf_manager(_tcp_reverse);

        hook_manager::get().hOok(1).receive_from_sdaf_manager(_tcp_forward);
        hook_manager::get().hOok(1).send_to_sdaf_manager(_tcp_reverse);

        hook_manager::get().hOok(0).send_to_sdaf_manager(_udp_forward);
        hook_manager::get().hOok(0).receive_from_sdaf_manager(_udp_reverse);

        hook_manager::get().hOok(1).receive_from_sdaf_manager(_udp_forward);
        hook_manager::get().hOok(1).send_to_sdaf_manager(_udp_reverse);
    }

    future<> stop() {
        return make_ready_future<>();
    }

    void run_udp_manager(int) {
        repeat([this]{
            return _udp_forward.on_new_initial_context().then([this]() mutable {
                auto ic = _udp_forward.get_initial_context();

                do_with(ic.get_sd_async_flow(), [](sd_async_flow<udp_ppr>& ac){
                    ac.register_events(udp_events::pkt_in);
                    return ac.run_async_loop([&ac](){
                        // printf("client async loop runs!\n");
                        if(ac.cur_event().on_close_event()) {
                            return make_ready_future<af_action>(af_action::close_forward);
                        }
                        return make_ready_future<af_action>(af_action::forward);
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
        void operator+=(const info& o) {
            ingress_received += o.ingress_received;
            egress_send += o.egress_send;
            active_flow_num += o.active_flow_num;
        }
    };
    info _old{0,0,0};

    future<info> get_info() {
        return make_ready_future<info>(info{port_manager::get().pOrt(0).rx_pkts(),
                                            port_manager::get().pOrt(1).tx_pkts(),
                                            _udp_forward.peek_active_flow_num()});
    }
    void collect_stats(int) {
        repeat([this]{
            return forwarders.map_reduce(adder<info>(), &forwarder::get_info).then([this](info i){
                fprint(std::cout, "ingress_received=%d, egress_send=%d, active_flow_num=%d.\n",
                        i.ingress_received-_old.ingress_received,
                        i.egress_send - _old.egress_send,
                        i.active_flow_num);
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

        /*return port_manager::get().add_port(opts, 0, port_type::standard).then([&opts]{
            return port_manager::get().add_port(opts, 1, port_type::standard);
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
        });*/
        /*port_manager::get().add_port(opts, 0, port_type::standard).then([&opts]{
            return port_manager::get().add_port(opts, 0, port_type::fdir);
        }).then([&opts]{
            return mica_manager::get().add_mica_client(opts, 1);
        });*/
        port_manager::get().add_port(opts, 0, port_type::standard).then([&opts]{
            return port_manager::get().add_port(opts, 1, port_type::standard);
        }).then([]{
            return hook_manager::get().add_hook_point(hook_type::sd_async_flow, 0);
        }).then([]{
            return hook_manager::get().add_hook_point(hook_type::sd_async_flow, 1);
        }).then([]{
            return forwarders.start();
        }).then([]{
            return hook_manager::get().invoke_on_all(0, &hook::check_and_start);
        }).then([]{
            return hook_manager::get().invoke_on_all(1, &hook::check_and_start);
        }).then([]{
            return forwarders.invoke_on_all(&forwarder::run_udp_manager, 1);
        }).then([]{
            return forwarders.invoke_on(0, &forwarder::collect_stats, 1);
        });
    });
}
