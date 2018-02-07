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

#include "net/packet-data-source.hh"

#include "netstar/port_manager.hh"
#include "netstar/stack/stack_manager.hh"
#include "netstar/hookpoint/hook_manager.hh"
#include "netstar/mica/mica_client.hh"
#include "netstar/asyncflow/sd_async_flow.hh"
#include "netstar/asyncflow/async_flow.hh"
#include "netstar/preprocessor/udp_ppr.hh"

#include "http/request_parser.hh"
#include "http/http_response_parser.hh"

using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

class forwarder;
distributed<forwarder> forwarders;

class ids {
    enum class ids_state_code {
        abnormal_tcp_flow,
        intrusion_detected,
        parser_error,
        normal_detection
    };

    sd_async_flow<tcp_reorder_ppr> _af;
    ids_state_code _state_code;
    http_request_parser _parser;
    input_stream<char> _cur_istream;

public:
    ids(sd_async_flow<tcp_reorder_ppr>&& af)
        : _af(std::move(af))
        , _state_code(ids_state_code::normal_detection) {}

    future<> configure() {
        _af.register_events(tcp_reorder_events::new_data);
        _af.register_events(tcp_reorder_events::abnormal_connection);
        _state_code = ids_state_code::normal_detection;
        _parser.init();
        return _af.run_async_loop([this](){
            if(_state_code == ids_state_code::abnormal_tcp_flow) {
                return handle_abnormal_tcp_flow();
            }
            else if(_state_code == ids_state_code::intrusion_detected) {
                return handle_intrusion_detected();
            }
            else if(_state_code == ids_state_code::parser_error) {
                return handle_parser_error();
            }
            else{
                return handle_normal_detection();
            }
        });
    }

private:

    // The major detection procedure.
    future<af_action> handle_normal_detection() {
        if(_af.cur_event().on_close_event()) {
            if(_af.cur_event().on_event(tcp_reorder_events::abnormal_connection)) {
                return make_ready_future<af_action>(af_action::close_drop);
            }

            if(_af.cur_event().on_event(tcp_reorder_events::new_data)) {
                auto pkt = _af.get_ppr()->read_reordering_buffer();
                _cur_istream = as_input_stream(std::move(pkt));
                return detect().then([this](af_action a){
                    if(a==af_action::forward) {
                        return af_action::close_forward;
                    }
                    else{
                        return af_action::close_drop;
                    }
                });
            }

            return make_ready_future<af_action>(af_action::close_forward);
        }

        if(_af.cur_event().on_event(tcp_reorder_events::abnormal_connection)) {
            _state_code = ids_state_code::abnormal_tcp_flow;
            return make_ready_future<af_action>(af_action::drop);
        }
        else{
            auto pkt = _af.get_ppr()->read_reordering_buffer();
            _cur_istream = as_input_stream(std::move(pkt));
            return detect();
        }
    }

    future<af_action> detect() {
        return _cur_istream.consume(_parser).then([this]{
            if(_parser.is_done()) {
                // Do some processing.
                // fprint(std::cout, "Get a new HTTP request.\n");
                _parser.init();

                if(_cur_istream.eof()) {
                    return make_ready_future<af_action>(af_action::forward);
                }
                else {
                    return detect();
                }
            }
            else if(_parser.is_error()) {
                // fprint(std::cout, "The parser generates an error.\n");
                _parser.init();
                _cur_istream = input_stream<char>();
                _state_code = ids_state_code::parser_error;
                _af.register_events(tcp_reorder_events::pkt_in);
                return make_ready_future<af_action>(af_action::drop);
            }
            else {
                // Parser wants more.
                assert(_cur_istream.eof());
                return make_ready_future<af_action>(af_action::forward);
            }
        });
    }

    // If abnormal_connection event is raised, the TCP flow is identified
    // as abnormal. There will only be abnormal_connection events raised
    // for each received packets in the future, and we should drop all the packets.
    future<af_action> handle_abnormal_tcp_flow() {
        if(_af.cur_event().on_close_event()) {
            return make_ready_future<af_action>(af_action::close_drop);
        }
        else {
            return make_ready_future<af_action>(af_action::drop);
        }
    }

    // The HTTP payload doesn't pass the AC atomaton. In this case,
    // we should first register pkt_in events. Then we should
    // retrieve all the connection data, and drop all the packets.
    future<af_action> handle_intrusion_detected() {
        if(_af.cur_event().on_close_event()) {
            return make_ready_future<af_action>(af_action::close_drop);
        }
        else {
            if(_af.cur_event().on_event(tcp_reorder_events::new_data)) {
                auto pkt = _af.get_ppr()->read_reordering_buffer();
            }
            return make_ready_future<af_action>(af_action::drop);
        }
    }

    // The connection is not transmitting valid HTTP request. Just
    // apply the same operation as if an intrusion has been detected.
    future<af_action> handle_parser_error() {
        return handle_intrusion_detected();
    }
};

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
            return _tcp_forward.on_new_initial_context().then([this]() mutable {
                auto ic = _tcp_forward.get_initial_context();

                do_with(ids(ic.get_sd_async_flow()), [](ids& instance){
                    return instance.configure();
                });

                return stop_iteration::no;
            });
        });

        repeat([this]{
            return _tcp_reverse.on_new_initial_context().then([this]() mutable {
                auto ic = _tcp_reverse.get_initial_context();

                /*do_with(ic.get_sd_async_flow(), [](sd_async_flow<tcp_ppr>& ac){
                    ac.register_events(tcp_events::pkt_in);
                    return ac.run_async_loop([&ac](){

                        if(ac.cur_event().on_close_event()) {
                            return make_ready_future<af_action>(af_action::close_forward);
                        }
                        return make_ready_future<af_action>(af_action::forward);
                    });
                }).then([](){

                });*/

                return stop_iteration::no;
            });
        });
    }

    struct info {
        uint64_t ingress_received;
        uint64_t ingress_send;
        uint64_t egress_received;
        uint64_t egress_send;
        size_t active_flow_num;
        size_t reverse_flow_num;
        void operator+=(const info& o) {
            ingress_received += o.ingress_received;
            ingress_send += o.ingress_send;
            egress_received += o.egress_received;
            egress_send += o.egress_send;
            active_flow_num += o.active_flow_num;
            reverse_flow_num += o.reverse_flow_num;
        }
    };
    info _old{0,0,0,0,0,0};

    future<info> get_info() {
        return make_ready_future<info>(info{port_manager::get().pOrt(0).rx_pkts(),
                                            port_manager::get().pOrt(0).tx_pkts(),
                                            port_manager::get().pOrt(1).rx_pkts(),
                                            port_manager::get().pOrt(1).tx_pkts(),
                                            _tcp_forward.peek_active_flow_num(),
                                            _tcp_reverse.peek_active_flow_num()});
    }
    void collect_stats(int) {
        repeat([this]{
            return forwarders.map_reduce(adder<info>(), &forwarder::get_info).then([this](info i){
                fprint(std::cout, "p0_recv=%d, p0_send=%d, p1_recv=%d, p1_send=%d, fwd_tcp_num=%d, reverse_tcp_num=%d.\n",
                        i.ingress_received-_old.ingress_received,
                        i.ingress_send-_old.ingress_send,
                        i.egress_received - _old.egress_received,
                        i.egress_send - _old.egress_send,
                        i.active_flow_num,
                        i.reverse_flow_num);
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
