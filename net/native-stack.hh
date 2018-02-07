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

#ifndef STACK_HH_
#define STACK_HH_

#include "net/net.hh"
#include <boost/program_options.hpp>

// patch by djp
// Add header files.
#include "net/ip.hh"
#include "net/dhcp.hh"
#include "native-stack-impl.hh"

namespace seastar {

namespace net {

// patch by djp
// Expose native_network_stack
class native_network_stack : public network_stack {
public:
    static thread_local promise<std::unique_ptr<network_stack>> ready_promise;
private:
    interface _netif;
    ipv4 _inet;
    bool _dhcp = false;
    promise<> _config;
    timer<> _timer;

    future<> run_dhcp(bool is_renew = false, const dhcp::lease & res = dhcp::lease());
    void on_dhcp(bool, const dhcp::lease &, bool);
    void set_ipv4_packet_filter(ip_packet_filter* filter) {
        _inet.set_packet_filter(filter);
    }
    using tcp4 = tcp<ipv4_traits>;
public:
    explicit native_network_stack(boost::program_options::variables_map opts, std::shared_ptr<device> dev);
    virtual server_socket listen(socket_address sa, listen_options opt) override;
    virtual ::seastar::socket socket() override;
    virtual udp_channel make_udp_channel(ipv4_addr addr) override;
    virtual future<> initialize() override;
    // by djp
    // Move the implementation to .cc file.
    /*static future<std::unique_ptr<network_stack>> create(boost::program_options::variables_map opts) {
        if (engine().cpu_id() == 0) {
            create_native_net_device(opts);
        }
        return ready_promise.get_future();
    }*/
    static future<std::unique_ptr<network_stack>> create(boost::program_options::variables_map opts);
    virtual bool has_per_core_namespace() override { return true; };
    void arp_learn(ethernet_address l2, ipv4_address l3) {
        _inet.learn(l2, l3);
    }
    friend class native_server_socket_impl<tcp4>;
    // by djp
    // Add another constructor.
    native_network_stack(std::shared_ptr<device> dev,
                         std::string ipv4_addr,
                         std::string gw_addr="192.168.122.1",
                         std::string netmask="255.255.255.0");
    // by djp
    // expose ipv4
    ipv4& get_inet() {
        return std::ref(_inet);
    }
};

void create_native_stack(boost::program_options::variables_map opts, std::shared_ptr<device> dev);

}

}

#endif /* STACK_HH_ */
