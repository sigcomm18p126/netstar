// Single direction async-flow. Connect only client.
#ifndef _SD_ASYNC_FLOW_HH
#define _SD_ASYNC_FLOW_HH

#include "core/shared_ptr.hh"
#include "core/stream.hh"

#include "netstar/asyncflow/async_flow_io.hh"
#include "netstar/asyncflow/async_flow_util.hh"
#include "netstar/rte_packet.hh"
#include "netstar/mica/util/hash.h"

#include <unordered_map>
#include <array>

namespace netstar {

// Forward declearations.
template<typename Ppr>
class sd_async_flow;
template<typename Ppr>
class sd_af_initial_context;
template<typename Ppr>
class sd_async_flow_manager;

namespace internal {

// Implementation forward decleration.
template<typename Ppr>
class sd_async_flow_impl;

template<typename Ppr>
class sd_async_flow_impl : public seastar::enable_lw_shared_from_this<sd_async_flow_impl<Ppr>>{
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;
    friend class sd_async_flow<Ppr>;

    sd_async_flow_manager<Ppr>& _manager;
    af_work_unit<Ppr> _client;
    uint8_t _reverse_direction_for_client;
    bool _initial_context_destroyed;
    uint64_t _flow_key_hash;

private:

    void close_ppr_and_remove_flow_key() {
        _client.ppr_close = true;
        if(_client.flow_key) {
            _manager.remove_mapping_on_flow_table(*(_client.flow_key));
            _client.flow_key = std::experimental::nullopt;
        }
    }

    filtered_events<EventEnumType> preprocess_packet(rte_packet& pkt) {
        generated_events<EventEnumType> ge = _client.ppr.handle_packet_send(pkt);
        filtered_events<EventEnumType> fe = _client.send_events.filter(ge);

        if(fe.on_close_event()) {
            async_flow_debug("sd_async_flow_impl: Close preprocessor and remove flow key.\n");
            close_ppr_and_remove_flow_key();
        }
        return fe;
    }

    void preprocess_and_forward(rte_packet pkt) {
        auto ge = _client.ppr.handle_packet_send(pkt);
        if(ge.on_close_event()){
            async_flow_debug("sd_async_flow_impl: Close preprocessor and remove flow key.\n");
            close_ppr_and_remove_flow_key();
        }
        internal_packet_forward(std::move(pkt));
    }

    void internal_packet_forward(rte_packet pkt) {
        if(pkt) {
            _manager.send(std::move(pkt), _reverse_direction_for_client);
            async_flow_debug("sd_async_flow_impl: send packet out to direction %d.\n", _reverse_direction_for_client);
        }
    }

private:
    // Critical functions for controlling the loop
    void close_async_loop() {
        _client.loop_fn = nullptr;
        while(!_client.buffer_q.empty()) {
            auto& next_pkt = _client.buffer_q.front();
            preprocess_and_forward(std::move(next_pkt.pkt));
            _client.buffer_q.pop_front();
        }
        _client.async_loop_quit_pr->set_value();
        _client.async_loop_quit_pr = {};
    }

    void loop_fn_post_handler(af_action action) {
        auto& context = _client.cur_context.value();

        if(action == af_action::forward || action == af_action::close_forward) {
            internal_packet_forward(std::move(context.pkt));
        }

        _client.cur_context = {};

        if(action == af_action::close_drop || action == af_action::close_forward) {
            close_async_loop();
            return;
        }

        while(!_client.buffer_q.empty()) {
            auto& next_pkt = _client.buffer_q.front();
            auto fe = preprocess_packet(next_pkt.pkt);
            if(fe.no_event()) {
                internal_packet_forward(std::move(next_pkt.pkt));
                _client.buffer_q.pop_front();
            }
            else{
                _client.cur_context.emplace(std::move(next_pkt.pkt),
                                            fe,
                                            next_pkt.is_send);
                _client.buffer_q.pop_front();
                invoke_async_loop();
                return;
            }
        }

        if(_client.ppr_close == true) {
            _client.cur_context.emplace(rte_packet(),
                                        filtered_events<EventEnumType>::make_close_event(),
                                        true);
            invoke_async_loop();
        }
    }

    void async_loop_exception_handler(std::exception_ptr ptr) {
        this->_initial_context_destroyed = false;
        _client.cur_context = {};
        _client.loop_fn = nullptr;
        while(!_client.buffer_q.empty()) {
            _client.buffer_q.pop_front();
        }
        _client.async_loop_quit_pr->set_exception(std::move(ptr));
        // _client.async_loop_quit_pr = {};
    }

    // Invoke the async loop to process interested events.
    // Recursively invoke async loop again if the flow has not finished.
    void invoke_async_loop() {
        try {
            _client.loop_fn().then_wrapped([this](auto&& f){
                if(!f.failed()) {
                    auto action = f.get0();
                    this->loop_fn_post_handler(action);
                }
                else{
                    this->async_loop_exception_handler(f.get_exception());
                }
            });
        }
        catch(...) {
            async_loop_exception_handler(std::current_exception());
        }
    }

public:
    // Internal interfaces, exposed to async_flow and
    // async_flow manager.
    sd_async_flow_impl(sd_async_flow_manager<Ppr>& manager,
                       uint8_t client_direction,
                       FlowKeyType* client_flow_key)
        : _manager(manager)
        , _client(true, client_direction, [this](bool){this->ppr_passive_close();})
        , _reverse_direction_for_client(manager.get_reverse_direction(client_direction))
        , _initial_context_destroyed(false) {
        _client.flow_key = *client_flow_key;
        _flow_key_hash = mica::util::hash(reinterpret_cast<char*>(client_flow_key), sizeof(FlowKeyType));
    }

    ~sd_async_flow_impl() {
        async_flow_debug("sd_async_flow_impl: deconstruction.\n");
        async_flow_assert(!_client.cur_context);
    }

    void destroy_initial_context() {
        _initial_context_destroyed = true;
    }

    void handle_packet_send(rte_packet pkt, uint8_t direction) {
        async_flow_debug("sd_async_flow_impl: handle_packet_send is called\n");
        async_flow_assert(direction == _client.direction);

        if(  _client.ppr_close ||
             !_initial_context_destroyed) {
            // Unconditionally drop the packet if the preprocessor
            // has been closed or the initial context has not been destroyed.
            return;
        }

        if(_client.loop_fn != nullptr) {
            if(!_client.cur_context) {
                async_flow_assert(_client.buffer_q.empty());
                auto fe = preprocess_packet(pkt);
                if(fe.no_event()) {
                    internal_packet_forward(std::move(pkt));
                }
                else{
                    _client.cur_context.emplace(std::move(pkt), fe, true);
                    invoke_async_loop();
                }
            }
            else{
                _client.buffer_q.emplace_back(std::move(pkt), true);
            }
        }
        else{
            preprocess_and_forward(std::move(pkt));
        }
    }

    void ppr_passive_close(){
        close_ppr_and_remove_flow_key();

        if(_client.loop_fn != nullptr && !_client.cur_context) {
            _client.cur_context.emplace(rte_packet(),
                                        filtered_events<EventEnumType>::make_close_event(),
                                        true);
            invoke_async_loop();
        }
    }

private:
    // Async loop initialization sequences after acquring the
    // initial packet context.
    void event_registration (EventEnumType ev) {
        _client.send_events.register_event(ev);
    }

    void event_unregistration (EventEnumType ev) {
        _client.send_events.unregister_event(ev);
    }

    seastar::future<> run_async_loop(std::function<seastar::future<af_action>()> fn) {
        async_flow_assert(_client.loop_fn==nullptr &&
                          !_client.cur_context &&
                          !_client.async_loop_quit_pr &&
                          !_client.ppr_close);

        _client.loop_fn = std::move(fn);
        _client.async_loop_quit_pr = seastar::promise<>();
        return _client.async_loop_quit_pr->get_future();
    }
};

} // namespace internal

template<typename Ppr>
class sd_async_flow{
    using impl_type = seastar::lw_shared_ptr<internal::sd_async_flow_impl<Ppr>>;
    using EventEnumType = typename Ppr::EventEnumType;
    impl_type _impl;
public:
    explicit sd_async_flow(impl_type impl)
        : _impl(std::move(impl)) {
    }
    ~sd_async_flow(){
    }
    sd_async_flow(const sd_async_flow& other) = delete;
    sd_async_flow(sd_async_flow&& other) noexcept
        : _impl(std::move(other._impl)) {
    }
    sd_async_flow& operator=(const sd_async_flow& other) = delete;
    sd_async_flow& operator=(sd_async_flow&& other) {
        if(&other != this){
            this->~sd_async_flow();
            new (this) sd_async_flow(std::move(other));
        }
        return *this;
    }

    void register_events(EventEnumType ev) {
        _impl->event_registration(ev);
    }

    void unregister_events(EventEnumType ev) {
        _impl->event_unregistration(ev);
    }

    rte_packet& cur_packet() {
        return _impl->_client.cur_context.value().pkt;
    }

    filtered_events<EventEnumType> cur_event() {
        return _impl->_client.cur_context.value().fe;
    }

    uint64_t get_flow_key_hash () {
        return _impl->_flow_key_hash;
    }

    // This method should not be implemented in this way...
    Ppr* get_ppr () {
        return &(_impl->_client.ppr);
    }

    // One shot interface, continuous call without shutting down
    // the current async loop will abort the program
    seastar::future<> run_async_loop(std::function<seastar::future<af_action>()> fn) {
        return _impl->run_async_loop(std::move(fn));
    }
};

template<typename Ppr>
class sd_af_initial_context {
    using impl_type = seastar::lw_shared_ptr<internal::sd_async_flow_impl<Ppr>>;
    friend class sd_async_flow_manager<Ppr>;

    impl_type _impl_ptr;
    rte_packet _pkt;
    uint8_t _direction;
private:
    explicit sd_af_initial_context(rte_packet pkt, uint8_t direction,
                                   impl_type impl_ptr)
        : _impl_ptr(std::move(impl_ptr))
        , _pkt(std::move(pkt))
        , _direction(direction) {
    }
public:
    sd_af_initial_context(sd_af_initial_context&& other) noexcept
        : _impl_ptr(std::move(other._impl_ptr))
        , _pkt(std::move(other._pkt))
        , _direction(other._direction) {
    }
    sd_af_initial_context& operator=(sd_af_initial_context&& other) noexcept {
        if(&other != this) {
            this->~sd_af_initial_context();
            new (this) sd_af_initial_context(std::move(other));
        }
        return *this;
    }
    ~sd_af_initial_context(){
        if(_impl_ptr) {
            _impl_ptr->destroy_initial_context();
            _impl_ptr->handle_packet_send(std::move(_pkt), _direction);
        }
    }
    sd_async_flow<Ppr> get_sd_async_flow() {
        return sd_async_flow<Ppr>(_impl_ptr);
    }
};

template<typename Ppr>
class sd_async_flow_manager {
    using FlowKeyType = typename Ppr::FlowKeyType;
    using HashFunc = typename Ppr::HashFunc;
    using impl_type = seastar::lw_shared_ptr<internal::sd_async_flow_impl<Ppr>>;
    friend class internal::sd_async_flow_impl<Ppr>;

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

    std::unordered_map<FlowKeyType, seastar::lw_shared_ptr<internal::sd_async_flow_impl<Ppr>>, HashFunc> _flow_table;
    std::array<internal::async_flow_io<Ppr>, 2> _directions;
    seastar::queue<queue_item> _new_ic_q{Ppr::async_flow_config::new_flow_queue_size};

public:
    sd_async_flow_manager() {
    }

    // Public interface for accepting new async_flow.
    seastar::future<> on_new_initial_context() {
        return _new_ic_q.not_empty();
    }

    // Public interface for retrieving new initial context.
    sd_af_initial_context<Ppr> get_initial_context() {
        async_flow_assert(!_new_ic_q.empty());
        auto qitem = _new_ic_q.pop();
        return sd_af_initial_context<Ppr>(std::move(qitem.pkt), qitem.direction, std::move(qitem.impl_ptr));
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
                                seastar::make_lw_shared<internal::sd_async_flow_impl<Ppr>>(
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
                                       seastar::lw_shared_ptr<internal::sd_async_flow_impl<Ppr>> impl_lw_ptr){
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
