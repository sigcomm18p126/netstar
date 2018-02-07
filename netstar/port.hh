#ifndef _PORT_HH
#define _PORT_HH

#include <memory>
#include <experimental/optional>

#include "core/future.hh"
#include "core/stream.hh"
#include "core/circular_buffer.hh"

#include "net/net.hh"

#include "netstar/rte_packet.hh"

namespace netstar {

namespace internal {

struct qp_wrapper{
    uint16_t qid;
    seastar::net::device* dev;
    std::unique_ptr<seastar::net::qp> qp;

    explicit qp_wrapper(boost::program_options::variables_map opts,
                         seastar::net::device* dev,
                         uint16_t qid) :
                         qid(qid), dev(dev){
        // Each core must has a hardware queue. We enforce this!
        assert(qid < dev->hw_queues_count());

        // The default qp initialization taking from native stack
        qp = dev->init_local_queue(opts, qid);
        dev->update_local_queue(qp.get());
    }
};

} // namespace internal

class hook;
class mica_client;

class port {
    uint16_t _port_id; // This is actually dpdk device id.
    internal::qp_wrapper _qp_wrapper;
    bool _receive_configured;
    seastar::circular_buffer<rte_packet> _rte_packet_sendq;
    seastar::circular_buffer<seastar::net::packet> _seastar_packet_sendq;

public:
    explicit port(boost::program_options::variables_map opts,
                  seastar::net::device* dev,
                  uint16_t port_id) :
        _port_id(port_id),
        _qp_wrapper(opts, dev, seastar::engine().cpu_id()),
        _receive_configured(false) {

        _qp_wrapper.qp->register_packet_provider([this](){
            std::experimental::optional<seastar::net::packet> p;
            if (!_seastar_packet_sendq.empty()) {
                p = std::move(_seastar_packet_sendq.front());
                _seastar_packet_sendq.pop_front();
            }
            return p;
        });

        _qp_wrapper.qp->register_rte_packet_provider([this](){
            std::experimental::optional<rte_packet> p;
            if (!_rte_packet_sendq.empty()) {
                p = std::move(_rte_packet_sendq.front());
                _rte_packet_sendq.pop_front();
            }
            return p;
        });

        seastar::fprint(std::cout, "port is created on core %d.\n", seastar::engine().cpu_id());
    }

    port(port&& other) = delete;
    port(const port& other) = delete;
    port& operator=(const port& other) = delete;
    port& operator=(const port&& other) = delete;

public:
    inline void send_seastar_packet(seastar::net::packet p) {
        _seastar_packet_sendq.push_back(std::move(p));
    }


    inline void send_rte_packet(rte_packet p) {
        _rte_packet_sendq.push_back(std::move(p));
    }

private:
    friend class hook;
    // Provide a customized receive function for the underlying qp.
    seastar::subscription<rte_packet>
    receive(std::function<seastar::future<> (rte_packet)> next_packet) {
        assert(!_receive_configured);
        _receive_configured = true;
        return _qp_wrapper.dev->receive_rte_packet(std::move(next_packet));
    }

    friend class mica_client;
    seastar::subscription<seastar::net::packet>
    receive_seastar_packet(std::function<seastar::future<> (seastar::net::packet pkt)> next_packet) {
        assert(!_receive_configured);
        _receive_configured = true;
        return _qp_wrapper.dev->receive(std::move(next_packet));
    }

public:
    seastar::net::ethernet_address get_eth_addr() {
        return _qp_wrapper.dev->hw_address();
    }

    const seastar::rss_key_type& get_rss_key() {
        return _qp_wrapper.dev->rss_key();
    }

    unsigned hash2cpu(uint32_t hash) {
        return _qp_wrapper.dev->hash2qid(hash);
    }

    unsigned get_qid(){
        return _qp_wrapper.qid;
    }

    uint16_t get_hw_queues_count() {
        return _qp_wrapper.dev->hw_queues_count();
    }

    uint64_t rx_bytes() {
        return _qp_wrapper.qp->rx_bytes();
    }

    uint64_t rx_pkts() {
        return _qp_wrapper.qp->rx_pkts();
    }

    uint64_t tx_bytes() {
        return _qp_wrapper.qp->tx_bytes();
    }

    uint64_t tx_pkts() {
        return _qp_wrapper.qp->tx_pkts();
    }

    uint16_t get_dev_id() {
        return _port_id;
    }
};

} // namespace netstar

#endif // _PORT_REFACTOR_HH
