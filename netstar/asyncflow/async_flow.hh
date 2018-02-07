// Double direction async flow. Connect both client and server.
#ifndef _ASYNC_FLOW_HH
#define _ASYNC_FLOW_HH

#include "core/shared_ptr.hh"
#include "core/stream.hh"

#include "netstar/asyncflow/async_flow_io.hh"
#include "netstar/asyncflow/async_flow_util.hh"
#include "netstar/rte_packet.hh"

#include <unordered_map>
#include <array>


using namespace seastar;

namespace netstar {

template<typename Ppr, af_side Side>
class async_flow;
template<typename Ppr>
class af_initial_context;
template<typename Ppr>
class async_flow_manager;
template<typename Ppr>
using client_async_flow = async_flow<Ppr, af_side::client>;
template<typename Ppr>
using server_async_flow = async_flow<Ppr, af_side::server>;

namespace internal {

template<typename Ppr>
class async_flow_impl;

template<typename Ppr>
class async_flow_impl : public enable_lw_shared_from_this<async_flow_impl<Ppr>>{
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;
    friend class async_flow<Ppr, af_side::client>;
    friend class async_flow<Ppr, af_side::server>;
    friend class async_flow_manager<Ppr>;
    friend class af_initial_context<Ppr>;
    friend class lw_shared_ptr<async_flow_impl<Ppr>>;

    async_flow_manager<Ppr>& _manager;
    af_work_unit<Ppr> _client;
    af_work_unit<Ppr> _server;
    bool _initial_context_destroyed;

private:
    // General helper utility function, useful for reducing the
    // boilerplates used in this class.
    af_work_unit<Ppr>& get_work_unit(bool is_client){
        return is_client ? _client : _server;
    }

    void close_ppr_and_remove_flow_key(af_work_unit<Ppr>& work_unit) {
        work_unit.ppr_close = true;
        if(work_unit.flow_key) {
            _manager.remove_mapping_on_flow_table(*(work_unit.flow_key));
            work_unit.flow_key = std::experimental::nullopt;
        }
    }

    filtered_events<EventEnumType> preprocess_packet(
            af_work_unit<Ppr>& working_unit, rte_packet& pkt, bool is_send) {
        generated_events<EventEnumType> ge =
                is_send ?
                working_unit.ppr.handle_packet_send(pkt) :
                working_unit.ppr.handle_packet_recv(pkt);
        filtered_events<EventEnumType> fe =
                is_send ?
                working_unit.send_events.filter(ge) :
                working_unit.recv_events.filter(ge);

        if(fe.on_close_event()) {
            async_flow_debug("async_flow_impl: Close preprocessor and remove flow key.\n");
            close_ppr_and_remove_flow_key(working_unit);
        }
        return fe;
    }

    void preprocess_and_forward(rte_packet pkt, bool is_client, bool is_send) {
        auto& working_unit = get_work_unit(is_client);
        auto ge = is_send ? working_unit.ppr.handle_packet_send(pkt) :
                            working_unit.ppr.handle_packet_recv(pkt);
        if(ge.on_close_event()){
            async_flow_debug("async_flow_impl: Close preprocessor and remove flow key.\n");
            close_ppr_and_remove_flow_key(working_unit);
        }
        internal_packet_forward(std::move(pkt), is_client, is_send);
    }

    void internal_packet_forward(rte_packet pkt, bool is_client, bool is_send) {
        if(is_send) {
            handle_packet_recv(std::move(pkt), !is_client);
        }
        else{
            send_packet_out(std::move(pkt), is_client);
        }
    }

    void action_after_packet_handle(af_work_unit<Ppr>& working_unit,
                                    rte_packet pkt,
                                    bool is_client, bool is_send) {
        if(working_unit.loop_fn != nullptr) {
            if(!working_unit.cur_context) {
                async_flow_assert(working_unit.buffer_q.empty());
                auto fe = preprocess_packet(working_unit, pkt, is_send);
                if(fe.no_event()) {
                    internal_packet_forward(std::move(pkt), is_client, is_send);
                }
                else{
                    working_unit.cur_context.emplace(std::move(pkt), fe, is_send);
                    invoke_async_loop(working_unit, is_client);
                }
            }
            else{
                working_unit.buffer_q.emplace_back(std::move(pkt), is_send);
            }
        }
        else{
            preprocess_and_forward(std::move(pkt), is_client, is_send);
        }
    }

private:
    // Critical functions for controlling the loop
    void close_async_loop(bool is_client) {
        auto& working_unit = get_work_unit(is_client);
        working_unit.loop_fn = nullptr;
        while(!working_unit.buffer_q.empty()) {
            auto& next_pkt = working_unit.buffer_q.front();
            preprocess_and_forward(std::move(next_pkt.pkt), is_client, next_pkt.is_send);
            working_unit.buffer_q.pop_front();
        }
        working_unit.async_loop_quit_pr->set_value();
        working_unit.async_loop_quit_pr = {};
    }

    void loop_fn_post_handler(bool is_client, af_action action) {
        auto& working_unit = get_work_unit(is_client);
        auto& context = working_unit.cur_context.value();

        if(action == af_action::forward || action == af_action::close_forward) {
            internal_packet_forward(std::move(context.pkt), is_client, context.is_send);
        }

        working_unit.cur_context = {};

        if(action == af_action::close_drop || action == af_action::close_forward) {
            close_async_loop(is_client);
            return;
        }

        while(!working_unit.buffer_q.empty()) {
            auto& next_pkt = working_unit.buffer_q.front();
            auto fe = preprocess_packet(working_unit,
                    next_pkt.pkt, next_pkt.is_send);
            if(fe.no_event()) {
                internal_packet_forward(std::move(next_pkt.pkt),
                                        is_client,
                                        next_pkt.is_send);
                working_unit.buffer_q.pop_front();
            }
            else{
                working_unit.cur_context.emplace(std::move(next_pkt.pkt),
                                                 fe,
                                                 next_pkt.is_send);
                working_unit.buffer_q.pop_front();
                invoke_async_loop(working_unit, is_client);
                return;
            }
        }

        if(working_unit.ppr_close == true) {
            working_unit.cur_context.emplace(rte_packet(),
                                             filtered_events<EventEnumType>::make_close_event(),
                                             true);
            invoke_async_loop(working_unit, is_client);
        }
    }

    void async_loop_exception_handler(bool is_client, std::exception_ptr cur_exception) {
        this->_initial_context_destroyed = false;
        auto& working_unit = this->get_work_unit(is_client);
        working_unit.cur_context = {};
        working_unit.loop_fn = nullptr;
        while(!working_unit.buffer_q.empty()) {
            working_unit.buffer_q.pop_front();
        }
        working_unit.async_loop_quit_pr->set_exception(std::move(cur_exception));
        // working_unit.async_loop_quit_pr = {};
    }

    // invoke_async_loop can be as simple as this:
    // working_unit.loop_fn().then([this, is_client](af_action action){
    //    loop_fn_post_handler(is_client, action);
    // })
    // However, this will not handle exceptions thrown when executing
    // loop_fn. In order to catch the exception, we replace the
    // implementation with this.
    void invoke_async_loop(af_work_unit<Ppr>& working_unit, bool is_client) {
        try {
            working_unit.loop_fn().then_wrapped([this, is_client](auto&& f){
                if(!f.failed()) {
                    auto action = f.get0();
                    this->loop_fn_post_handler(is_client, action);
                }
                else {
                    this->async_loop_exception_handler(is_client, f.get_exception());
                }
            });
        }
        catch(...) {
            async_loop_exception_handler(is_client, std::current_exception());
        }
    }

private:
    // Internal interfaces, exposed to async_flow and
    // async_flow manager.
    async_flow_impl(async_flow_manager<Ppr>& manager,
                    uint8_t client_direction,
                    FlowKeyType* client_flow_key)
        : _manager(manager)
        , _client(true, client_direction, [this](bool is_client){this->ppr_passive_close(is_client);})
        , _server(false, manager.get_reverse_direction(client_direction),[this](bool is_client){this->ppr_passive_close(is_client);})
        , _initial_context_destroyed(false) {
        _client.flow_key = *client_flow_key;
    }

    void destroy_initial_context() {
        _initial_context_destroyed = true;
    }

    void handle_packet_send(rte_packet pkt, uint8_t direction) {
        async_flow_debug("async_flow_impl: handle_packet_send is called\n");

        bool is_client = (direction == _client.direction);
        auto& working_unit = get_work_unit(is_client);

        if(  working_unit.ppr_close ||
             !_initial_context_destroyed) {
            // Unconditionally drop the packet.
            return;
        }

        action_after_packet_handle(working_unit, std::move(pkt),
                                   is_client, true);
    }

    void handle_packet_recv(rte_packet pkt, bool is_client){
        async_flow_debug("async_flow_impl: handle_packet_recv is called\n");

        auto& working_unit = get_work_unit(is_client);

        if(working_unit.ppr_close || !pkt) {
            // Unconditionally drop the packet.
            return;
        }

        if(!working_unit.flow_key) {
            async_flow_assert(!is_client);
            FlowKeyType flow_key = working_unit.ppr.get_reverse_flow_key(pkt);
            _manager.add_new_mapping_to_flow_table(flow_key, this->shared_from_this());
        }

        action_after_packet_handle(working_unit, std::move(pkt),
                                   is_client, false);
    }

    void send_packet_out(rte_packet pkt, bool is_client){
        if(pkt) {
            auto& working_unit = get_work_unit(is_client);
            _manager.send(std::move(pkt), working_unit.direction);
            async_flow_debug("async_flow_impl: send packet out from direction %d.\n", working_unit.direction);
        }
    }

    void ppr_passive_close(bool is_client){
        auto& working_unit = get_work_unit(is_client);
        close_ppr_and_remove_flow_key(working_unit);

        if(working_unit.loop_fn != nullptr && !working_unit.cur_context) {
            working_unit.cur_context.emplace(rte_packet(),
                                             filtered_events<EventEnumType>::make_close_event(),
                                             true);
            invoke_async_loop(working_unit, is_client);
        }
    }

private:
    // Async loop initialization sequences after acquring the
    // initial packet context.
    void event_registration (bool is_client, bool is_send, EventEnumType ev) {
        auto& working_unit = get_work_unit(is_client);
        auto& events = is_send ? working_unit.send_events : working_unit.recv_events;
        events.register_event(ev);
    }

    void event_unregistration (bool is_client, bool is_send, EventEnumType ev) {
        auto& working_unit = get_work_unit(is_client);
        auto& events = is_send ? working_unit.send_events : working_unit.recv_events;
        events.unregister_event(ev);
    }

    future<> run_async_loop(bool is_client, std::function<future<af_action>()> fn) {
        auto& working_unit = get_work_unit(is_client);

        async_flow_assert(working_unit.loop_fn==nullptr &&
                          !working_unit.cur_context &&
                          !working_unit.async_loop_quit_pr &&
                          !working_unit.ppr_close);

        working_unit.loop_fn = std::move(fn);
        working_unit.async_loop_quit_pr = promise<>();
        return working_unit.async_loop_quit_pr->get_future();
    }

public:
    ~async_flow_impl() {
       async_flow_debug("async_flow_impl: deconstruction.\n");
       async_flow_assert(!_client.cur_context);
       async_flow_assert(!_server.cur_context);
   }
};

} // namespace internal

template<typename Ppr, af_side Side>
class async_flow{
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    using EventEnumType = typename Ppr::EventEnumType;
    impl_type _impl;
public:
    explicit async_flow(impl_type impl)
        : _impl(std::move(impl)) {
    }
    ~async_flow(){
    }
    async_flow(const async_flow& other) = delete;
    async_flow(async_flow&& other) noexcept
        : _impl(std::move(other._impl)) {
    }
    async_flow& operator=(const async_flow& other) = delete;
    async_flow& operator=(async_flow&& other) {
        if(&other != this){
            this->~async_flow();
            new (this) async_flow(std::move(other));
        }
        return *this;
    }

    void register_events(af_send_recv sr, EventEnumType ev) {
        _impl->event_registration(static_cast<bool>(Side), static_cast<bool>(sr), ev);
    }

    void unregister_events(af_send_recv sr, EventEnumType ev) {
        _impl->event_unregistration(static_cast<bool>(Side), static_cast<bool>(sr), ev);
    }
    future<> run_async_loop(std::function<future<af_action>()> fn) {
        return _impl->run_async_loop(static_cast<bool>(Side), std::move(fn));
    }
};

template<typename Ppr>
class af_initial_context {
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    friend class async_flow_manager<Ppr>;

    impl_type _impl_ptr;
    rte_packet _pkt;
    uint8_t _direction;
private:
    explicit af_initial_context(rte_packet pkt, uint8_t direction,
                                impl_type impl_ptr)
        : _impl_ptr(std::move(impl_ptr))
        , _pkt(std::move(pkt))
        , _direction(direction) {
    }
public:
    af_initial_context(af_initial_context&& other) noexcept
        : _impl_ptr(std::move(other._impl_ptr))
        , _pkt(std::move(other._pkt))
        , _direction(other._direction) {
    }
    af_initial_context& operator=(af_initial_context&& other) noexcept {
        if(&other != this) {
            this->~af_initial_context();
            new (this) af_initial_context(std::move(other));
        }
        return *this;
    }
    ~af_initial_context(){
        if(_impl_ptr) {
            _impl_ptr->destroy_initial_context();
            _impl_ptr->handle_packet_send(std::move(_pkt), _direction);
        }
    }
    client_async_flow<Ppr> get_client_async_flow() {
        return client_async_flow<Ppr>(_impl_ptr);
    }
    server_async_flow<Ppr> get_server_async_flow() {
        return server_async_flow<Ppr>(_impl_ptr);
    }
};

template<typename Ppr>
class async_flow_manager {
    using FlowKeyType = typename Ppr::FlowKeyType;
    using HashFunc = typename Ppr::HashFunc;
    using impl_type = seastar::lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    friend class internal::async_flow_impl<Ppr>;

    struct queue_item {
        impl_type impl_ptr;
        rte_packet pkt;
        uint8_t direction;

        queue_item(impl_type impl_ptr_arg,
                   rte_packet pkt_arg,
                   uint8_t direction_arg)
            : impl_ptr(std::move(impl_ptr_arg))
            , pkt(std::move(pkt_arg))
            , direction(direction_arg) {
        }
    };

    std::unordered_map<FlowKeyType, seastar::lw_shared_ptr<internal::async_flow_impl<Ppr>>, HashFunc> _flow_table;
    std::array<internal::async_flow_io<Ppr>, 2> _directions;
    seastar::queue<queue_item> _new_ic_q{Ppr::async_flow_config::new_flow_queue_size};

public:
    async_flow_manager() {
    }

    // Public interface for accepting new async_flow.
    seastar::future<> on_new_initial_context() {
        return _new_ic_q.not_empty();
    }

    // Public interface for retrieving new initial context.
    af_initial_context<Ppr> get_initial_context() {
        async_flow_assert(!_new_ic_q.empty());
        auto qitem = _new_ic_q.pop();
        return af_initial_context<Ppr>(std::move(qitem.pkt), qitem.direction, std::move(qitem.impl_ptr));
    }

    // Public interface for querying the remaining number of
    // active flows.
    size_t peek_active_flow_num() {
        return _flow_table.size();
    }

    // This is used to receive packets from the hookpoint.
    void receive_from_hookpoint(uint8_t direction, // Which direction
                                unsigned hook_id, // The id of the hookpoint
                                seastar::stream<rte_packet, FlowKeyType*>& hook_input_stream // An input stream from hook
                                ) {
        _directions[direction].receive_from_hookpoint(hook_id, hook_input_stream,
                        [this, direction](rte_packet pkt, FlowKeyType* key) {
                async_flow_debug("Receive a new packet from direction %d\n", direction);
                auto afi = _flow_table.find(*key);
                if(afi == _flow_table.end()) {
                    if(!_new_ic_q.full() &&
                       (_flow_table.size() <
                        Ppr::async_flow_config::max_flow_table_size) ){
                        auto impl_lw_ptr =
                                seastar::make_lw_shared<internal::async_flow_impl<Ppr>>(
                                    (*this), direction, key
                                );
                        auto succeed = _flow_table.insert({*key, impl_lw_ptr}).second;
                        assert(succeed);
                        _new_ic_q.push(queue_item(std::move(impl_lw_ptr), std::move(pkt), direction));
                    }
                }
                else {
                    afi->second->handle_packet_send(std::move(pkt), direction);
                }

                return seastar::make_ready_future<>();
            }
        );
    }

    // This is used to send packets to the hookpoint.
    seastar::subscription<rte_packet> send_to_hookpoint(
            uint8_t direction, // Which direction
            unsigned hook_id, // The id of the hookpoint
            std::function<seastar::future<>(rte_packet)> send_fn // The function for receiving
            ) {
        return _directions[direction].send_to_hookpoint(hook_id, std::move(send_fn));
    }

private:
    // Send packet out from a direction.
    seastar::future<> send(rte_packet pkt, uint8_t direction) {
        return _directions[direction].get_send_stream().produce(std::move(pkt));
    }

    // Get the reverse direction of the original direction.
    uint8_t get_reverse_direction(uint8_t direction) {
        if(direction == 0) {
            return 1;
        }
        else {
            return 0;
        }
    }

    // Add a new mapping to flow table. This is basically
    void add_new_mapping_to_flow_table(FlowKeyType& flow_key,
                                       seastar::lw_shared_ptr<internal::async_flow_impl<Ppr>> impl_lw_ptr){
        auto succeed = _flow_table.insert({flow_key, impl_lw_ptr}).second;
        assert(succeed);
    }

    // Remove a mapping from the flow table.
    void remove_mapping_on_flow_table(FlowKeyType& flow_key) {
        _flow_table.erase(flow_key);
    }
};

} // namespace netstar

#endif
