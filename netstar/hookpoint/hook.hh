#ifndef _HOOK_HH
#define _HOOK_HH

#include "netstar/preprocessor/tcp_reorder_ppr.hh"
#include "netstar/preprocessor/tcp_ppr.hh"
#include "netstar/preprocessor/udp_ppr.hh"
#include "netstar/asyncflow/sd_async_flow.hh"
#include "netstar/asyncflow/async_flow.hh"

#include "netstar/port_manager.hh"
#include "netstar/stack/stack_manager.hh"

namespace netstar{

class hook {
protected:
    port& _p;
    std::function<seastar::future<> (rte_packet)> _recv_func;
    std::experimental::optional<seastar::subscription<rte_packet>> _sub;

    void start_receving() {
        // Move or copy?
        _sub.emplace(_p.receive(_recv_func));
    }

public:
    explicit hook(port& p)
        : _p(p)
        , _recv_func([](rte_packet p){return seastar::make_ready_future<>();}) {
        seastar::fprint(std::cout, "hook point is created on core %d.\n", seastar::engine().cpu_id());
        assert(p.get_qid() == seastar::engine().cpu_id());
    }

    virtual ~hook() {}

    virtual void update_target_port(unsigned port_id) {
        seastar::fprint(std::cout,"update_target_port is not defined for a hook.\n");
        abort();
    }

    virtual void attach_stack(unsigned stack_id) {
        seastar::fprint(std::cout,"attach_stack is not defined for a hook.\n");
        abort();
    }

    virtual void check_and_start() = 0;

    // Interfaces for dealing with sd_async_flows.
    virtual void receive_from_sdaf_manager(sd_async_flow_manager<udp_ppr>& manager) {
        seastar::fprint(std::cout,"interface with sd_async_flow_manager is not defined.\n");
        abort();
    }

    virtual void send_to_sdaf_manager(sd_async_flow_manager<udp_ppr>& manager) {
        seastar::fprint(std::cout,"interface with sd_async_flow_manager is not defined.\n");
        abort();
    }

    virtual void receive_from_sdaf_manager(sd_async_flow_manager<tcp_ppr>& manager) {
        seastar::fprint(std::cout,"interface with sd_async_flow_manager is not defined.\n");
        abort();
    }

    virtual void send_to_sdaf_manager(sd_async_flow_manager<tcp_ppr>& manager) {
        seastar::fprint(std::cout,"interface with sd_async_flow_manager is not defined.\n");
        abort();
    }

    virtual void receive_from_sdaf_manager(sd_async_flow_manager<tcp_reorder_ppr>& manager) {
        seastar::fprint(std::cout,"interface with sd_async_flow_manager is not defined.\n");
        abort();
    }

    virtual void send_to_sdaf_manager(sd_async_flow_manager<tcp_reorder_ppr>& manager) {
        seastar::fprint(std::cout,"interface with sd_async_flow_manager is not defined.\n");
        abort();
    }

    // Interfaces for dealing with async_flows
    virtual void connect_to_af_manager(async_flow_manager<udp_ppr>& manager) {
        seastar::fprint(std::cout,"interface with async_flow_manager is not defined.\n");
        abort();
    }

    virtual void connect_to_af_manager(async_flow_manager<tcp_ppr>& manager) {
        seastar::fprint(std::cout,"interface with async_flow_manager is not defined.\n");
        abort();
    }
};

} // namespace netstar

#endif // _HOOK_HH
