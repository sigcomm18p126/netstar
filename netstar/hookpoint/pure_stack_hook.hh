#ifndef _PURE_STACK_HOOK_HH
#define _PURE_STACK_HOOK_HH

#include "netstar/hookpoint/hook.hh"

namespace netstar {

namespace internal {

class pure_stack_hook : public hook {
    unsigned _port_id;
    unsigned _stack_id;
    seastar::net::device* _stack_dummy_device;

public:
    pure_stack_hook(unsigned port_id)
        : hook(std::ref(port_manager::get().pOrt(port_id)))
        , _port_id(port_id)
        , _stack_id(0)
        , _stack_dummy_device(nullptr){
    }


    virtual void attach_stack(unsigned stack_id) override {
        _stack_id = stack_id;
        _stack_dummy_device = stack_manager::get().dummy_dev(stack_id);
        _recv_func = [this](rte_packet pkt) {
            // seastar::fprint(std::cout, "port %d receive rte_packet.\n", _port_id);
            auto p = pkt.get_packet();
            if(p) {
                _stack_dummy_device->l2receive(std::move(*p));
            }
            return seastar::make_ready_future<>();
        };
    }

    virtual void check_and_start() override {
        assert(_port_id == stack_manager::get().port_id(_stack_id));
        assert(port_manager::get().type(_port_id) != port_type::fdir);
        start_receving();
    }
};

}

}

#endif

