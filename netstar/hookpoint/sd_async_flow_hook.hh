#ifndef _SD_ASYNC_FLOW_HOOK_HH
#define _SD_ASYNC_FLOW_HOOK_HH

#include "net/tcp.hh"
#include "net/net.hh"

#include "netstar/hookpoint/hook.hh"
#include "netstar/hookpoint/io_connector.hh"

#include <rte_tcp.h>

namespace netstar {

namespace internal {

class sd_async_flow_hook : public hook {
    io_connector<udp_ppr::FlowKeyType> _udp_connector;
    io_connector<tcp_ppr::FlowKeyType> _tcp_connector;
    unsigned _hookpoint_id;
    std::function<seastar::future<>(rte_packet)> _send_fn;

public:
    sd_async_flow_hook(unsigned port_id, unsigned hookpoint_id)
        : hook(std::ref(port_manager::get().pOrt(port_id)))
        , _udp_connector()
        , _tcp_connector()
        , _hookpoint_id(hookpoint_id){

        if(port_id == 0) {
            _send_fn =  [this](rte_packet pkt){
                auto eth_h = pkt.get_header<net::eth_hdr>(0);
                if(!eth_h) {
                    return make_ready_future<>();
                }

                eth_h->src_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x60};
                eth_h->dst_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x08, 0x00};
                _p.send_rte_packet(std::move(pkt));
                return make_ready_future<>();
            };
        }
        else {
            _send_fn =  [this](rte_packet pkt){
                auto eth_h = pkt.get_header<net::eth_hdr>(0);
                if(!eth_h) {
                    return make_ready_future<>();
                }

                eth_h->src_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x62};
                eth_h->dst_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x07, 0x82};
                _p.send_rte_packet(std::move(pkt));
                return make_ready_future<>();
            };
        }

        _recv_func = [this](rte_packet pkt) {
            auto eth_h = pkt.get_header<seastar::net::eth_hdr>(0);
            if(!eth_h) {
                return make_ready_future<>();
            }

            if(seastar::net::ntoh(eth_h->eth_proto) == static_cast<uint16_t>(seastar::net::eth_protocol_num::ipv4)) {
                auto ip_h = pkt.get_header<seastar::net::ip_hdr>(sizeof(seastar::net::eth_hdr));
                if(!ip_h) {
                    return make_ready_future<>();
                }

                // If we have a stack, the packet can go to the stack here.
                {

                }

                // The following code blocks checks and regulates
                // incoming IP packets.
                auto h = seastar::net::ntoh(*ip_h);
                unsigned ip_len = h.len;
                unsigned ip_hdr_len = h.ihl * 4;
                unsigned pkt_len = pkt.len() - sizeof(seastar::net::eth_hdr);
                auto offset = h.offset();
                if (pkt_len > ip_len) {
                    pkt.trim_back(pkt_len - ip_len);
                } else if (pkt_len < ip_len) {
                    return make_ready_future<>();
                }
                if (h.mf() == true || offset != 0) {
                    return make_ready_future<>();
                }

                if(h.ip_proto == static_cast<uint8_t>(seastar::net::ip_protocol_num::udp)) {
                    auto udp_h =
                            pkt.get_header<seastar::net::udp_hdr>(
                                    sizeof(seastar::net::eth_hdr)+ip_hdr_len);
                    if(!udp_h) {
                        return make_ready_future<>();
                    }

                    udp_ppr::FlowKeyType fk{seastar::net::ntoh(ip_h->dst_ip),
                                            seastar::net::ntoh(ip_h->src_ip),
                                            seastar::net::ntoh(udp_h->dst_port),
                                            seastar::net::ntoh(udp_h->src_port)};
                    _udp_connector.send_stream->produce(std::move(pkt), &fk);
                    return make_ready_future<>();
                }
                else if(h.ip_proto == static_cast<uint8_t>(seastar::net::ip_protocol_num::tcp)) {
                    auto tcp_h =
                            pkt.get_header<::tcp_hdr>(
                                    sizeof(seastar::net::eth_hdr)+ip_hdr_len);
                    if(!tcp_h) {
                        return make_ready_future<>();
                    }

                    auto data_offset = tcp_h->data_off >> 4;
                    if (size_t(data_offset * 4) < seastar::net::tcp_hdr::len) {
                       return make_ready_future<>();
                    }

                    tcp_ppr::FlowKeyType fk{seastar::net::ntoh(ip_h->dst_ip),
                                            seastar::net::ntoh(ip_h->src_ip),
                                            seastar::net::ntoh(tcp_h->dst_port),
                                            seastar::net::ntoh(tcp_h->src_port)};
                    _tcp_connector.send_stream->produce(std::move(pkt), &fk);
                    return make_ready_future<>();
                }
                else{
                    return make_ready_future<>();
                }
            }
            else{
                return make_ready_future<>();
            }
        };
    }

    virtual void receive_from_sdaf_manager(sd_async_flow_manager<udp_ppr>& manager) {
        assert(!_udp_connector.recv_sub);
        _udp_connector.recv_sub.emplace(manager.send_to_hookpoint(1, _hookpoint_id, _send_fn));
    }

    virtual void send_to_sdaf_manager(sd_async_flow_manager<udp_ppr>& manager) {
        assert(!_udp_connector.send_stream);
        _udp_connector.send_stream.emplace();
        manager.receive_from_hookpoint(0, _hookpoint_id, *(_udp_connector.send_stream));
    }

    virtual void receive_from_sdaf_manager(sd_async_flow_manager<tcp_ppr>& manager) {
        assert(!_tcp_connector.recv_sub);
        _tcp_connector.recv_sub.emplace(manager.send_to_hookpoint(1, _hookpoint_id, _send_fn));
    }

    virtual void send_to_sdaf_manager(sd_async_flow_manager<tcp_ppr>& manager) {
        assert(!_tcp_connector.send_stream);
        _tcp_connector.send_stream.emplace();
        manager.receive_from_hookpoint(0, _hookpoint_id, *(_tcp_connector.send_stream));
    }

    virtual void receive_from_sdaf_manager(sd_async_flow_manager<tcp_reorder_ppr>& manager) {
        assert(!_tcp_connector.recv_sub);
        _tcp_connector.recv_sub.emplace(manager.send_to_hookpoint(1, _hookpoint_id, _send_fn));
    }

    virtual void send_to_sdaf_manager(sd_async_flow_manager<tcp_reorder_ppr>& manager) {
        assert(!_tcp_connector.send_stream);
        _tcp_connector.send_stream.emplace();
        manager.receive_from_hookpoint(0, _hookpoint_id, *(_tcp_connector.send_stream));
    }


    virtual void check_and_start() override {
        assert(_udp_connector.recv_sub);
        assert(_udp_connector.send_stream);
        assert(_tcp_connector.recv_sub);
        assert(_tcp_connector.send_stream);
        start_receving();
    }
};

} // namespace internal

} // namespace netstar

#endif
