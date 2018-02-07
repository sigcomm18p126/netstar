#ifndef _SHARD_CONTAINER_HH
#define _SHARD_CONTAINER_HH

#include <memory>
#include <vector>

#include "core/future.hh"
#include "core/reactor.hh"

namespace netstar {

namespace internal {

template<typename T>
struct shard_container {
    T* t;

    template <typename... Args>
    shard_container(Args&&... args) {
        auto obj = std::make_unique<T>(std::forward<Args>(args)...);
        t = obj.get();
        seastar::engine().at_destroy([obj = std::move(obj)] {});
    }

    seastar::future<> stop() {
        return seastar::make_ready_future<>();
    }

    T& get_contained() {
        return *t;
    }

    void save_container_ptr(std::vector<T*>* vec) {
        vec->at(seastar::engine().cpu_id()) = t;
    }
};

} // namespace internal

template<typename T>
struct shard_container_trait {
    using shard_t = seastar::distributed<internal::shard_container<T>>;
    using instance_t = internal::shard_container<T>;
};

} // namespace netstar

#endif
