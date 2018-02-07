#ifndef _HOOK_MANAGER_HH
#define _HOOK_MANAGER_HH

#include "netstar/hookpoint/dummy_hook.hh"
#include "netstar/hookpoint/pure_stack_hook.hh"
#include "netstar/hookpoint/sd_async_flow_hook.hh"
#include <boost/iterator/counting_iterator.hpp>

namespace netstar{

namespace internal {

template<typename T>
struct hook_point_launcher {
    static_assert(std::is_base_of<hook, T>::value, "The type parameter is not derived from hook.\n");
    using hook_shard = shard_container_trait<T>;
    using shard_t = typename hook_shard::shard_t;
    using instance_t = typename hook_shard::instance_t;

    template <typename... Args>
    static seastar::future<std::vector<hook*>> launch(Args&&... args) {
        auto vec = std::make_shared<std::vector<T*>>(seastar::smp::count, nullptr);
        auto shard_sptr = std::make_shared<shard_t>();

        return shard_sptr->start(std::forward<Args>(args)...).then([shard_sptr, vec]{
            return shard_sptr->invoke_on_all(&instance_t::save_container_ptr,
                                             vec.get());
        }).then([shard_sptr]{
            return shard_sptr->stop();
        }).then([shard_sptr, vec]{
            std::vector<hook*> ret;
            for(auto derived_ptr : *vec) {
                ret.push_back(derived_ptr);
            }
            return std::move(ret);
        });
    }
};


} // namespace internal

enum class hook_type {
    dummy,
    pure_stack,
    sd_async_flow
};

class hook_manager {
    std::vector<std::vector<hook*>> _hooks;
    std::vector<seastar::promise<>> _prs;
    std::vector<unsigned> _port_ids;

public:
    seastar::future<> add_hook_point(hook_type type, unsigned port_id) {
        assert(hook_point_check(port_id));

        unsigned which_one = _prs.size();
        _prs.emplace_back();
        _port_ids.push_back(port_id);

        switch(type) {
        case hook_type::dummy: {
            launch<internal::dummy_hook>(which_one, port_id);
            break;
        }
        case hook_type::pure_stack: {
            launch<internal::pure_stack_hook>(which_one, port_id);
            break;
        }
        case hook_type::sd_async_flow: {
            launch<internal::sd_async_flow_hook>(which_one, port_id, which_one);
            break;
        }
        default:
            break;
        }

        return _prs.at(which_one).get_future();
    }

public:
    static hook_manager& get() {
        static hook_manager hm;
        return hm;
    }

    hook& hOok(unsigned hook_point_id) {
        return *(_hooks.at(hook_point_id).at(seastar::engine().cpu_id()));
    }

    unsigned hook_port_id(unsigned hook_point_id) {
        return _port_ids.at(hook_point_id);
    }

    template <typename... Args>
    inline
    seastar::future<> invoke_on_all(unsigned hook_point_id, void (hook::*func)(Args...), Args... args) {
        assert(hook_point_id < _port_ids.size());

        return seastar::parallel_for_each(boost::irange<unsigned>(0, _hooks.at(hook_point_id).size()),
                [hook_point_id, this, func, args...] (unsigned c) {
            return seastar::smp::submit_to(c, [c, hook_point_id, this, func, args...] {
                auto inst = _hooks.at(hook_point_id).at(c);
                ((*inst).*func)(args...);
            });
        });
    }

private:
    bool hook_point_check(unsigned port_id) {
        for(auto id : _port_ids) {
            if(id == port_id) {
                seastar::fprint(std::cout, "hook_manager ERROR: port %d has been registered.\n", port_id);
                return false;
            }
        }
        return true;
    }

    template<typename T, typename... Args>
    void launch(unsigned which_one, Args&&... args) {
        internal::hook_point_launcher<T>::launch(std::forward<Args>(args)...).then(
                [this, which_one](auto&& vec){
            _hooks.push_back(std::move(vec));
            _prs.at(which_one).set_value();
        });
    }
};

} // namespace netstar

#endif // _HOOK_MANAGER_HH
