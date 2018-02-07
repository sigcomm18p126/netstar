#ifndef _UDP_PPR_HH
#define _UDP_PPR_HH

#include "core/timer.hh"
#include "core/lowres_clock.hh"

#include "net/net.hh"
#include "net/ip.hh"

#include "netstar/asyncflow/async_flow_util.hh"
#include "netstar/rte_packet.hh"

namespace netstar {

enum class udp_events : uint8_t{
    pkt_in=0
};

class udp_ppr{
private:
    bool _is_client;
    seastar::timer<seastar::lowres_clock> _t;
    std::function<void(bool)> _close_fn;
public:
    using EventEnumType = udp_events;
    using FlowKeyType = seastar::net::l4connid<seastar::net::ipv4_traits>;
    using HashFunc = seastar::net::l4connid<seastar::net::ipv4_traits>::connid_hash;

    udp_ppr(bool is_client, std::function<void(bool)> close_fn)
        : _is_client(is_client)
        , _close_fn(std::move(close_fn)){
        _t.set_callback([this]{
            // fprint(std::cout, "timer called.\n");
            this->_close_fn(this->_is_client);
        });
        _t.arm(std::chrono::seconds(async_flow_config::flow_expire_seconds));
    }

public:
    generated_events<EventEnumType> handle_packet_send(rte_packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen(udp_events::pkt_in);
        if(_t.armed()) {
            _t.cancel();
            _t.arm(std::chrono::seconds(async_flow_config::flow_expire_seconds));
        }
        return ge;
    }

    generated_events<EventEnumType> handle_packet_recv(rte_packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen(udp_events::pkt_in);
        return ge;
    }

    FlowKeyType get_reverse_flow_key(rte_packet& pkt){
        auto ip_hd_ptr = pkt.get_header<seastar::net::ip_hdr>(sizeof(seastar::net::eth_hdr));
        auto udp_hd_ptr = pkt.get_header<seastar::net::udp_hdr>(sizeof(seastar::net::eth_hdr)+sizeof(seastar::net::ip_hdr));
        return FlowKeyType{seastar::net::ntoh(ip_hd_ptr->src_ip),
                           seastar::net::ntoh(ip_hd_ptr->dst_ip),
                           seastar::net::ntoh(udp_hd_ptr->src_port),
                           seastar::net::ntoh(udp_hd_ptr->dst_port)};
    }

    struct async_flow_config {
        static constexpr int max_event_context_queue_size = 5;
        static constexpr int new_flow_queue_size = 1000;
        static constexpr int max_flow_table_size = 100000;
        static constexpr int flow_expire_seconds = 5;

        static FlowKeyType get_flow_key(rte_packet& pkt){
            auto ip_hd_ptr = pkt.get_header<seastar::net::ip_hdr>(sizeof(seastar::net::eth_hdr));
            auto udp_hd_ptr = pkt.get_header<seastar::net::udp_hdr>(sizeof(seastar::net::eth_hdr)+sizeof(seastar::net::ip_hdr));
            return FlowKeyType{seastar::net::ntoh(ip_hd_ptr->dst_ip),
                               seastar::net::ntoh(ip_hd_ptr->src_ip),
                               seastar::net::ntoh(udp_hd_ptr->dst_port),
                               seastar::net::ntoh(udp_hd_ptr->src_port)};
        }
    };
};

} // namespace netstar

#endif
