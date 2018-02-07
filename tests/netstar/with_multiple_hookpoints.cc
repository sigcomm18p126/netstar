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
#include "netstar/port_manager.hh"

#include "netstar/stack/stack_manager.hh"
#include "netstar/hookpoint/hook_manager.hh"
#include "netstar/mica/mica_client.hh"

#include "netstar/asyncflow/sd_async_flow.hh"
#include "netstar/asyncflow/async_flow.hh"

#include "netstar/preprocessor/udp_ppr.hh"

using namespace seastar;
using namespace netstar;

class runner {
    sd_async_flow_manager<tcp_reorder_ppr> _tcp_forward;
    sd_async_flow_manager<tcp_ppr> _tcp_reverse;
    sd_async_flow_manager<udp_ppr> _udp_forward;
    sd_async_flow_manager<udp_ppr> _udp_reverse;
public:
    runner() {
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
};

distributed<runner> r;

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
            return r.start();
        }).then([]{
            return hook_manager::get().invoke_on_all(0, &hook::check_and_start);
        }).then([]{
            return hook_manager::get().invoke_on_all(1, &hook::check_and_start);
        });
    });
}
