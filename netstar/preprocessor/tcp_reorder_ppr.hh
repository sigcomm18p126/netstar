#ifndef _TCP_REORDER_PPR_HH
#define _TCP_REORDER_PPR_HH

#include "net/tcp.hh"

#include "core/timer.hh"
#include "core/lowres_clock.hh"

#include "netstar/asyncflow/async_flow_util.hh"
#include "netstar/rte_packet.hh"

#include <deque>

namespace netstar {

// #define TCP_REORDER_DEBUG 1

template <typename... Args>
void tcp_reorder_debug(const char* fmt, Args&&... args) {
#ifdef TCP_REORDER_DEBUG
    seastar::fprint(std::cout, fmt, std::forward<Args>(args)...);
#endif
}

enum class tcp_reorder_events : uint8_t{
    pkt_in=0,
    new_data=1,
    abnormal_connection=2
};

class tcp_reorder_ppr{
private:
    bool _is_client;
    seastar::timer<seastar::lowres_clock> _t;
    std::function<void(bool)> _close_fn;
    bool _seen_syn;
    seastar::net::tcp_seq _rcv_next;
    seastar::net::tcp_seq _rcv_initial;
    uint32_t _rcv_window;
    std::deque<seastar::net::packet> _rcv_data;
    seastar::net::tcp_packet_merger _rcv_out_of_order;
    bool _abnormal_detected;

public:
    using EventEnumType = tcp_reorder_events;
    using FlowKeyType = seastar::net::l4connid<seastar::net::ipv4_traits>;
    using HashFunc = seastar::net::l4connid<seastar::net::ipv4_traits>::connid_hash;

    tcp_reorder_ppr(bool is_client, std::function<void(bool)> close_fn)
        : _is_client(is_client)
        , _close_fn(std::move(close_fn))
        , _seen_syn(false)
        , _rcv_window(3737600)
        , _abnormal_detected(false) {
        _t.set_callback([this]{
            // fprint(std::cout, "timer called.\n");
            this->_close_fn(this->_is_client);
        });
        _t.arm(std::chrono::seconds(async_flow_config::flow_expire_seconds));
    }

private:
    bool segment_acceptable(seastar::net::tcp_seq seg_seq, unsigned seg_len) {
        if (seg_len == 0 && _rcv_window == 0) {
            // SEG.SEQ = RCV.NXT
            return seg_seq == _rcv_next;
        } else if (seg_len == 0 && _rcv_window > 0) {
            // RCV.NXT =< SEG.SEQ < RCV.NXT+RCV.WND
            return (_rcv_next <= seg_seq) && (seg_seq < _rcv_next + _rcv_window);
        } else if (seg_len > 0 && _rcv_window > 0) {
            // RCV.NXT =< SEG.SEQ < RCV.NXT+RCV.WND
            //    or
            // RCV.NXT =< SEG.SEQ+SEG.LEN-1 < RCV.NXT+RCV.WND
            bool x = (_rcv_next <= seg_seq) && seg_seq < (_rcv_next + _rcv_window);
            bool y = (_rcv_next <= seg_seq + seg_len - 1) && (seg_seq + seg_len - 1 < _rcv_next + _rcv_window);
            return x || y;
        } else  {
            // SEG.LEN > 0 RCV.WND = 0, not acceptable
            return false;
        }
    }

    void insert_out_of_order(seastar::net::tcp_seq seg, seastar::net::packet p) {
        _rcv_out_of_order.merge(seg, std::move(p));
    }

    bool merge_out_of_order() {
        bool merged = false;
        if (_rcv_out_of_order.map.empty()) {
            return merged;
        }
        for (auto it = _rcv_out_of_order.map.begin(); it != _rcv_out_of_order.map.end();) {
            auto& p = it->second;
            auto seg_beg = it->first;
            auto seg_len = p.len();
            auto seg_end = seg_beg + seg_len;
            if (seg_beg <= _rcv_next && _rcv_next < seg_end) {
                // This segment has been received out of order and its previous
                // segment has been received now
                auto trim = _rcv_next - seg_beg;
                if (trim) {
                    p.trim_front(trim);
                    seg_len -= trim;
                }
                _rcv_next += seg_len;
                _rcv_data.push_back(std::move(p));
                // Since c++11, erase() always returns the value of the following element
                it = _rcv_out_of_order.map.erase(it);
                merged = true;
            } else if (_rcv_next >= seg_end) {
                // This segment has been receive already, drop it
                it = _rcv_out_of_order.map.erase(it);
            } else {
                // seg_beg > _rcv.need, can not merge. Note, seg_beg can grow only,
                // so we can stop looking here.
                it++;
                break;
            }
        }
        return merged;
    }

public:
    generated_events<EventEnumType> handle_packet_send(rte_packet& pkt){
        generated_events<EventEnumType> ge;

        // First, if abnormal behavior is already detected, finish preprocessing
        // immediately.
        if(_abnormal_detected) {
            tcp_reorder_debug("This tcp flow has been recognized as abnormal.\n");

            ge.event_happen(tcp_reorder_events::abnormal_connection);
            ge.event_happen(tcp_reorder_events::pkt_in);
            if(_t.armed()) {
                _t.cancel();
                _t.arm(std::chrono::seconds(async_flow_config::flow_expire_seconds));
            }
            return ge;
        }

        // The reordering processing start from here.
        // Note this is a full packet, with eth/ip/tcp
        auto iph = pkt.get_header<seastar::net::ip_hdr>(sizeof(seastar::net::eth_hdr));
        auto th = pkt.get_header(sizeof(seastar::net::eth_hdr)+(iph->ihl*4), seastar::net::tcp_hdr::len);
        auto h = seastar::net::tcp_hdr::read(th);

        if(!_abnormal_detected) {
            // Second, check for abnormal behavior
            if( (!h.f_syn && !_seen_syn) ) {
                tcp_reorder_debug("An abnormal behavior is detected for this TCP flow.\n");

                _abnormal_detected = true;
                _rcv_out_of_order.map.clear();

                ge.event_happen(tcp_reorder_events::abnormal_connection);
                ge.event_happen(tcp_reorder_events::pkt_in);
                if(_t.armed()) {
                    _t.cancel();
                    _t.arm(std::chrono::seconds(async_flow_config::flow_expire_seconds));
                }
                return ge;
            }
        }

        // Third, check for the syn packet.
        if(h.f_syn && !_seen_syn) {
            // This is a syn packet.
            // Set up the initial values.
            _seen_syn = true;
            seastar::net::tcp_seq seg_seq = h.seq;
            _rcv_next = seg_seq + 1;
            _rcv_initial = seg_seq;

            tcp_reorder_debug("Receive syn packet. _rcv_next=%d, _rcv_initial=%d.\n", _rcv_next, _rcv_initial);
        }

        if(_seen_syn) {
            // Try to reconstruct the payload here.
            unsigned header_length = sizeof(seastar::net::eth_hdr)+(iph->ihl*4)+(h.data_offset * 4);
            unsigned seg_len = pkt.len()-header_length;
            seastar::net::tcp_seq seg_seq = h.seq;

            auto result = segment_acceptable(seg_seq, seg_len);
            if(!result) {
                // segment is not acceptable, reordering processing is over.
                ge.event_happen(tcp_reorder_events::pkt_in);
                if(_t.armed()) {
                    _t.cancel();
                    _t.arm(std::chrono::seconds(async_flow_config::flow_expire_seconds));
                }
                return ge;
            }

            // At this time, translate to Seastar packet
            auto pkt_opt = pkt.get_packet();
            // If the translation fails, we may have a problem, do an assertion.
            assert(pkt_opt);
            seastar::net::packet p(std::move(*pkt_opt));

            // p initially has ip/eth/tcp, trim useless field
            p.trim_front(header_length);
            assert(p.len() == seg_len);

            // Do an adjustment.
            if (seg_seq < _rcv_next) {
                // ignore already acknowledged data
                auto dup = std::min(uint32_t(_rcv_next - seg_seq), seg_len);
                p.trim_front(dup);
                seg_len -= dup;
                seg_seq += dup;
            }

            if (seg_seq != _rcv_next) {
                tcp_reorder_debug("Receive segment with seg_seq=%d and seq_len=%d, out of order.\n", seg_seq, seg_len);
                insert_out_of_order(seg_seq, std::move(p));

                ge.event_happen(tcp_reorder_events::pkt_in);
                if(_t.armed()) {
                    _t.cancel();
                    _t.arm(std::chrono::seconds(async_flow_config::flow_expire_seconds));
                }
                return ge;
            }

            // check for rst flag
            if(h.f_rst) {
                // Performa an active close.
                ge.event_happen(tcp_reorder_events::pkt_in);
                ge.close_event_happen();
                if(_t.armed()) {
                    _t.cancel();
                }
                return ge;
            }

            if(p.len() > 0) {
                tcp_reorder_debug("Receive segment with seg_seq=%d and seq_len=%d, in order.\n", seg_seq, seg_len);
                _rcv_data.push_back(std::move(p));
                _rcv_next += seg_len;
                merge_out_of_order();
                ge.event_happen(tcp_reorder_events::new_data);
            }
        }

        ge.event_happen(tcp_reorder_events::pkt_in);
        if(_t.armed()) {
            _t.cancel();
            _t.arm(std::chrono::seconds(async_flow_config::flow_expire_seconds));
        }
        return ge;
    }

    seastar::net::packet read_reordering_buffer() {
        seastar::net::packet p;
        for (auto&& q : _rcv_data) {
            p.append(std::move(q));
        }
        _rcv_data.clear();
        return p;
    }

    generated_events<EventEnumType> handle_packet_recv(rte_packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen(tcp_reorder_events::pkt_in);
        return ge;
    }

    FlowKeyType get_reverse_flow_key(rte_packet& pkt){
        auto ip_hd_ptr = pkt.get_header<seastar::net::ip_hdr>(sizeof(seastar::net::eth_hdr));
        auto tcp_hd_ptr = pkt.get_header<seastar::net::tcp_hdr>(sizeof(seastar::net::eth_hdr)+sizeof(seastar::net::ip_hdr));
        return FlowKeyType{seastar::net::ntoh(ip_hd_ptr->src_ip),
                           seastar::net::ntoh(ip_hd_ptr->dst_ip),
                           seastar::net::ntoh(tcp_hd_ptr->src_port),
                           seastar::net::ntoh(tcp_hd_ptr->dst_port)};
    }

    struct async_flow_config {
        static constexpr int max_event_context_queue_size = 5;
        static constexpr int new_flow_queue_size = 20000;
        static constexpr int max_flow_table_size = 100000;

        static constexpr int flow_expire_seconds = 180;

        static FlowKeyType get_flow_key(rte_packet& pkt){
            auto ip_hd_ptr = pkt.get_header<seastar::net::ip_hdr>(sizeof(seastar::net::eth_hdr));
            auto tcp_hd_ptr = pkt.get_header<seastar::net::tcp_hdr>(sizeof(seastar::net::eth_hdr)+sizeof(seastar::net::ip_hdr));
            return FlowKeyType{seastar::net::ntoh(ip_hd_ptr->dst_ip),
                               seastar::net::ntoh(ip_hd_ptr->src_ip),
                               seastar::net::ntoh(tcp_hd_ptr->dst_port),
                               seastar::net::ntoh(tcp_hd_ptr->src_port)};
        }
    };
};

} // namespace netstar

#endif
