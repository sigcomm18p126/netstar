#ifndef _ASYNC_FLOW_UTIL_HH
#define _ASYNC_FLOW_UTIL_HH

#include <cstdint>
#include <assert.h>

#include "core/future.hh"
#include "core/circular_buffer.hh"

#include "netstar/rte_packet.hh"

namespace netstar{

template<typename Enum>
class generated_events;

template<typename Enum>
class filtered_events;

// #define ENABLE_AF_ASSERTION

#ifndef ENABLE_AF_ASSERTION
#define async_flow_assert(condition) ((void)0)
#else
#define async_flow_assert(condition) assert(condition)
#endif

// #define ASYNC_FLOW_DEBUG

template <typename... Args>
void async_flow_debug(const char* fmt, Args&&... args) {
#ifdef ASYNC_FLOW_DEBUG
    seastar::fprint(std::cout, fmt, std::forward<Args>(args)...);
#endif
}

enum class af_side : bool {
    client=true,
    server=false
};

enum class af_send_recv : bool {
    send=true,
    recv=false
};

enum class af_action {
    forward,
    drop,
    close_forward,
    close_drop
};

class async_flow_unexpected_quit : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "async_flow quits unexpectedly\n";
    }
};

namespace internal {

using event_storage_type = uint16_t;

template<typename Enum>
class registered_events;

template<typename Enum>
class registered_events {
    using est = event_storage_type;
    est _registered_events;
public:
    registered_events() {
        unregister_all_events();
    }
public:
    void register_event(Enum ev) {
        async_flow_assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = 1 << static_cast<est>(ev);
        _registered_events |= mask;
    }

    void unregister_event(Enum ev) {
        async_flow_assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = ~(1 << static_cast<est>(ev));
        _registered_events &= mask;
    }

    void unregister_all_events(){
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        _registered_events = mask;
    }

    filtered_events<Enum> filter(generated_events<Enum> ge){
        return filtered_events<Enum>(_registered_events & ge._generated_events);
    }
};

struct buffered_packet {
    rte_packet pkt;
    bool is_send;

    buffered_packet(rte_packet pkt_arg, bool is_send_arg)
        : pkt(std::move(pkt_arg))
        , is_send(is_send_arg){
    }
};

template<typename Ppr>
struct packet_context {
    using EventEnumType = typename Ppr::EventEnumType;
    rte_packet pkt;
    filtered_events<EventEnumType> fe;
    bool is_send;

    packet_context(rte_packet pkt_arg,
                   filtered_events<EventEnumType> fe_arg,
                   bool is_send_arg)
        : pkt(std::move(pkt_arg))
        , fe(fe_arg)
        , is_send(is_send_arg) {
    }
};

template<typename Ppr>
struct af_work_unit {
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;

    Ppr ppr;
    std::experimental::optional<seastar::promise<>> async_loop_quit_pr;
    registered_events<EventEnumType> send_events;
    registered_events<EventEnumType> recv_events;
    seastar::circular_buffer<buffered_packet> buffer_q;
    std::experimental::optional<FlowKeyType> flow_key;
    std::experimental::optional<packet_context<Ppr>> cur_context;
    std::function<seastar::future<af_action>()> loop_fn;
    uint8_t direction;
    bool ppr_close;
    bool is_client;

    af_work_unit(bool is_client_arg,
                 uint8_t direction_arg,
                 std::function<void(bool)> close_fn)
        : ppr(is_client_arg, std::move(close_fn))
        , direction(direction_arg)
        , ppr_close(false)
        , is_client(is_client_arg) {
        buffer_q.reserve(Ppr::async_flow_config::max_event_context_queue_size);
        loop_fn = nullptr;
    }
};

} // namespace internal

template<typename Enum>
class generated_events {
    friend class internal::registered_events<Enum>;
    using est = internal::event_storage_type;
    est _generated_events;
public:
    generated_events() {
        _generated_events = 0;
    }

public:
    void event_happen(Enum ev){
        async_flow_assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = 1 << static_cast<est>(ev);
        _generated_events |= mask;
    }

    void close_event_happen(){
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        _generated_events |= mask;
    }

    bool on_close_event() const {
       est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
       return (_generated_events&mask) != 0;
   }

    void clear(){
        _generated_events = 0;
    }
};

template<typename Enum>
class filtered_events {
    friend class internal::registered_events<Enum>;
    using est = internal::event_storage_type;
    est _filtered_events;
public:
    filtered_events(est fe)
        : _filtered_events(fe){
    }
public:
    bool on_event(Enum ev) const {
        async_flow_assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = 1 << static_cast<est>(ev);
        return (_filtered_events&mask) != 0;
    }

    bool on_close_event() const {
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        return (_filtered_events&mask) != 0;
    }

    bool no_event() const {
        return _filtered_events == 0;
    }

    static filtered_events make_close_event() {
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        return filtered_events(mask);
    }
};

} // namespace netstar

#endif
