#ifndef _MICA_CLIENT
#define _MICA_CLIENT

#include "netstar/mica/mica_def.hh"
#include "netstar/mica/roundup.hh"
#include "netstar/mica/extendable_buffer.hh"
#include "netstar/mica/util/hash.h"

#include "netstar/port.hh"
#include "netstar/port_manager.hh"
#include "netstar/shard_container.hh"

#include "net/packet.hh"
#include "net/udp.hh"
#include "net/ip_checksum.hh"
#include "net/ip.hh"
#include "net/net.hh"
#include "net/byteorder.hh"

#include "core/future.hh"
#include "core/chunked_fifo.hh"
#include "core/scattered_message.hh"

#include <string>
#include <experimental/optional>
#include <utility>
#include <chrono>
#include <boost/program_options/variables_map.hpp>

namespace netstar {

using namespace seastar;
using namespace std::chrono_literals;

class kill_flow : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "killflow";
    }
};

namespace internal {

struct kv_wrapper {
    temporary_buffer<char> roundup_buf;
    size_t actual_length;

    // Create a kv_wrapper from an lvalue reference
    template<typename T>
    kv_wrapper(T& t) {
        actual_length = sizeof(t);
        extendable_buffer eb;
        eb.fill_data(t);
        roundup_buf = eb.get_temp_buffer();
    }

    kv_wrapper(size_t actual_length, temporary_buffer<char> roundup_buf)
        : roundup_buf(std::move(roundup_buf))
        , actual_length(actual_length) {}
};

} // namespace internal

class mica_key {
    internal::kv_wrapper _wrapper;
public:
    template<typename... T>
    mica_key(T&&... args)
        : _wrapper(std::forward<T>(args)...) {}

    temporary_buffer<char> get_roundup_buf(){
        return std::move(_wrapper.roundup_buf);
    }

    size_t get_actual_length() {
        return _wrapper.actual_length;
    }
};

class mica_value {
    internal::kv_wrapper _wrapper;
public:
    template<typename... T>
    mica_value(T&&... args)
        : _wrapper(std::forward<T>(args)...) {}

    temporary_buffer<char> get_roundup_buf(){
        return std::move(_wrapper.roundup_buf);
    }

    size_t get_actual_length() {
        return _wrapper.actual_length;
    }
};

// The response is shared from the received
// response packet. There for mica_response should
// not be held indefinitely. It should be deconstructed
// as soon as the users of the mica_client finishes using
// the result of the response.
class mica_response{
    net::packet _response_pkt;
public:
    mica_response(net::packet p) : _response_pkt(std::move(p)) {}

    mica_response(mica_response&& other) noexcept :
        _response_pkt(std::move(other._response_pkt)) {}

    mica_response& operator=(mica_response&& other) {
        _response_pkt = std::move(other._response_pkt);
        return *this;
    }

    // Get the the size of the key
    size_t get_key_len(){
        auto rh = _response_pkt.get_header<RequestHeader>();
        return (rh->kv_length_vec >> 24);
    }
    // Get the size of the rounded up key len, aka key buffer len
    size_t get_roundup_key_len(){
        return roundup<8>(get_key_len());
    }

    // Get the size of the value
    size_t get_val_len(){
        auto rh = _response_pkt.get_header<RequestHeader>();
        return (rh->kv_length_vec & ((1 << 24) - 1));
    }
    // Get the size of the rounded up value len, aka value buffer len
    size_t get_roundup_val_len(){
       return roundup<8>(get_val_len());
    }

    Operation get_operation(){
        auto rh = _response_pkt.get_header<RequestHeader>();
        return static_cast<Operation>(rh->operation);
    }

    Result get_result(){
        auto rh = _response_pkt.get_header<RequestHeader>();
        return static_cast<Result>(rh->result);
    }

    template<typename T>
    T& get_key(){
        mc_assert(sizeof(T) == get_key_len());
        auto key = _response_pkt.get_header<T>(sizeof(RequestHeader));
        return *key;
    }

    template<typename T>
    T& get_value(){
        mc_assert(sizeof(T) == get_val_len());
        auto value =
                _response_pkt.get_header<T>(
                        sizeof(RequestHeader)+get_roundup_key_len());
        return *value;
    }

    temporary_buffer<char> get_val_tb() {
        temporary_buffer<char> ret;
        auto pkt = _response_pkt.share(sizeof(RequestHeader)+get_roundup_key_len(), get_roundup_val_len());
        mc_assert(pkt.nr_frags() == 1);
        pkt.release_into([&ret] (temporary_buffer<char>&& frag) {
            ret = std::move(frag);
        });
        return ret;
    }
};

class mica_client {
public:
    static constexpr unsigned max_req_len =
            ETHER_MAX_LEN - ETHER_CRC_LEN - sizeof(RequestBatchHeader);

    enum class action {
        recycle_rd,
        resend_rd,
        no_action
    };

    class request_descriptor{
        // index of the request descriptor from the vector.
        const uint16_t _rd_index;
        // record the epoch of the request descriptor,
        // increase by 1 after each recycle
        uint16_t _epoch;

        // record the number of retries performed by the current request
        unsigned _retry_count;
        // a timer that is used to check request timeout
        timer<lowres_clock> _to;

        // the request header
        RequestHeader _rq_hd;

        // record the previous sent key, cleared after each recycle
        size_t _key_len;
        temporary_buffer<char> _key_buf;

        // record the previous sent value, cleared after each recycle
        size_t _val_len;
        temporary_buffer<char> _val_buf;

        // the associated promise with this request
        std::experimental::optional<promise<mica_response>> _pr;

        // the size of the request
        size_t _request_size;

        // maximum number of allowed timeout retries
        static constexpr unsigned max_retries = 5;

        // Initial timeout time in millisecond
        static constexpr unsigned initial_timeout_val = 10;
    public:
        // A series of actions that can be applied to request_descriptor.

        explicit request_descriptor(unsigned rd_index,
                                    std::function<void()> to_handler_fn) :
            _rd_index(rd_index), _epoch(0), _retry_count(0),
            _key_len(0), _key_buf(), _val_len(0), _val_buf(),
            _request_size(sizeof(RequestHeader)){
            _to.set_callback(std::move(to_handler_fn));
        }

        void new_action(Operation op,
                        size_t key_len, temporary_buffer<char> key,
                        size_t val_len, temporary_buffer<char> val){
            // If this method is called, the rd must be popped out from
            // the fifo. When the rd is popped out from the fifo, it is either
            // in initialized state, or be recycled. This means that:
            // 1. _pr contains nothing and associate with no continuation
            // 2. _retry_count is cleared and is zero
            // 3. _to timer is not armed.
            mc_assert(!_pr);
            mc_assert(_retry_count == 0);
            mc_assert(!_to.armed());
            mc_assert(key_len>=8);
            mc_assert((key_len < (1 << 8)));
            mc_assert((val_len >= 8 || val_len == 0));
            mc_assert(roundup<8>(key_len) == key.size());
            mc_assert(roundup<8>(val_len) == val.size());
            mc_assert(ENABLE_MC_ASSERTION == 1);

            _key_len = key_len;
            _key_buf = std::move(key);
            _val_len = val_len;
            _val_buf = std::move(val);
            _request_size = sizeof(RequestHeader)+_key_buf.size()+_val_buf.size();

            // make sure that _request_size is smaller than max_req_len
            mc_assert(_request_size<=max_req_len);

            setup_request_header(op);
        }

        future<mica_response> obtain_future(){
            // This is called after new_action is called. So we
            // perform the same assertion.
            mc_assert(!_pr);
            mc_assert(_retry_count==0);
            mc_assert(!_to.armed());

            _pr = promise<mica_response>();
            return _pr->get_future();
        }

        void append_frags(scattered_message<char>& msg){
            // RequestHeader is a plain old object,
            // doing reinterpret_cast here is fine.
            // Plus we have the guarantee of the lifetime of _rq_hd object.
            msg.append_static(reinterpret_cast<char*>(&_rq_hd),
                           sizeof(RequestHeader));
            msg.append_static(_key_buf.get(), _key_buf.size());
            msg.append_static(_val_buf.get(), _val_buf.size());
        }

        void arm_timer(){
            mc_assert(_retry_count<max_retries);
            mc_assert(!_to.armed());
            // determining how long to timeout before waiting for
            // the response to come back. I test that sometimes,
            // We need to wait for at least 4ms for some requests.
            // Here I'm using a count up method. For the first retry,
            // we wait for s ms, then for each other retry, we increase
            // the timeout time by 1ms.
            // Finally, to fail a message for our current configuration
            // (4 retries, 1ms initial timeout value) a request descriptor
            // will be held for at most 10ms.
            // _to.arm(std::chrono::milliseconds(initial_timeout_val+_retry_count));
            _to.arm(std::chrono::milliseconds(initial_timeout_val));
        }

    public:
        size_t get_request_size(){
            return _request_size;
        }

        uint64_t get_key_hash(){
            return _rq_hd.key_hash;
        }

    public:
        action match_response(RequestHeader& res_hd, net::packet response_pkt){
            auto opaque = res_hd.opaque;
            uint16_t epoch = opaque & ((1 << 16) - 1);
            if(epoch != _epoch){
                // the epoch doesn't match, the response is a late response,
                // ignore it.
                return action::no_action;
            }
            // the epoch matches, we got the response for this request.
            // Here, the timer should be armed and not timed out.
            // The retry count should not exceed the maximum value.
            // the _pr must be associated with some continuations.
            if(!_to.armed()) {
                fprint(std::cout, "_retry_count=%d, _epoch=%d, _rd_index=%d.\n", _retry_count, _epoch, _rd_index);
            }
            mc_assert(_to.armed());
            mc_assert(_retry_count < max_retries);
            mc_assert(_pr);

            _pr->set_value(mica_response(std::move(response_pkt)));
            normal_recycle_prep();
            return action::recycle_rd;
        }

        // The timeout handler
        action timeout_handler(){
            // handle _to timeout
            mc_assert(_pr);
            _retry_count++;
            if(_retry_count == max_retries){
                // we have retried four times without receiving a response,
                // timeout
                _pr->set_exception(kill_flow());
                timeout_recycle_prep();
                return action::recycle_rd;
            }
            else{
                // increase the epoch to discard late responses.
                _epoch++;
                adjust_request_header_opaque();
                return action::resend_rd;
            }
        }

    private:
        void adjust_request_header_opaque(){
            _rq_hd.opaque = (static_cast<uint32_t>(_rd_index) << 16) |
                             static_cast<uint32_t>(_epoch);
        }

        void setup_request_header(Operation& op){
            // set up the request header
            _rq_hd.operation = static_cast<uint8_t>(op);
            _rq_hd.result = static_cast<uint8_t>(Result::kSuccess);
            _rq_hd.key_hash =  mica::util::hash(_key_buf.get(), _key_len);
            _rq_hd.opaque = (static_cast<uint32_t>(_rd_index) << 16) |
                             static_cast<uint32_t>(_epoch);
            _rq_hd.reserved0 = 0;
            _rq_hd.kv_length_vec =
                static_cast<uint32_t>((_key_len << 24) | _val_len);
        }

        void normal_recycle_prep(){
            // Preparation for a normal recycle.
            // This is only triggered by correctly match a response.
            // We have the following assertions to check
            mc_assert(_to.armed());
            mc_assert(_retry_count<max_retries);
            mc_assert(_pr);

            // increase the epoch for a new request_descriptor
            _epoch++;
            // reset the _retyry_count to 0
            _retry_count = 0;
            // cancel the _to timer
            _to.cancel();
            // clear the promise
            _pr = std::experimental::nullopt;
            // no need to touch the _rq_hd
        }

        void timeout_recycle_prep(){
            // Preparation for a timeout recycle.
            // This is only triggered after consecutive timeout is triggered
            // without getting a response.
            mc_assert(_retry_count == max_retries);
            mc_assert(_pr);
            mc_assert(!_to.armed());

            _epoch++;
            _retry_count = 0;
            _pr = std::experimental::nullopt;
        }
    };

    // The request_assembler should be
    // launched for each partition of a server
    class request_assembler{
        endpoint_info _remote_ei;
        endpoint_info _local_ei;
        port& _port;

        // the batch packet header
        net::packet _batch_header_pkt;
        net::fragment _batch_header_frag;

        // remaining request packet size
        unsigned _remaining_size;

        // This vector records the requests that are going
        // to be sent out in a single packet.
        std::vector<unsigned> _rd_idxs;
        // a vector of request descriptors
        std::vector<request_descriptor>& _rds;

        // a stream for holding the rd indexes.
        circular_buffer<unsigned> _send_stream;
    public:
        explicit request_assembler(endpoint_info remote_ei,
                endpoint_info local_ei, port& p,
                std::vector<request_descriptor>& rds) :
                _remote_ei(remote_ei), _local_ei(local_ei),
                _port(p), _remaining_size(max_req_len),
                _rds(rds) {
            _batch_header_pkt = build_requet_batch_header();
            _batch_header_frag = _batch_header_pkt.frag(0);
            _rd_idxs.reserve(10);
            _send_stream.reserve(10);
        }

        void append_new_request_descriptor(unsigned rd_index){
            _send_stream.push_back(rd_index);
            consume_send_stream();
        }

        void consume_send_stream(){
            while(!_send_stream.empty()) {
                auto next_rd_idx = _send_stream.front();
                auto& next_rd = _rds[next_rd_idx];
                auto next_rd_size = next_rd.get_request_size();
                if(_remaining_size < next_rd_size){
                    send_request_packet();
                }
                else{
                    _rd_idxs.push_back(next_rd_idx);
                    _send_stream.pop_front();
                    _remaining_size -= next_rd_size;
                }
            }
        }

        void force_send(){
            if(_rd_idxs.size()>0){
                send_request_packet();
            }
        }

    private:
        void send_request_packet(){
            scattered_message<char> msg;
            msg.reserve(1+3*_rd_idxs.size());

            // First, put the batch_header in to the _frags
            msg.append_static(_batch_header_frag.base,
                              _batch_header_frag.size);

            // Then, for each request descriptor, put the request header
            // , key and value into the _frags.
            for(auto rd_idx : _rd_idxs){
                _rds[rd_idx].append_frags(msg);
            }

            // build up the packet;
            net::packet p(std::move(msg).release());

            // setup the missing header information
            setup_ip_udp_length(p);
            setup_request_num(p);

            // send
            p.linearize();
            _port.send_seastar_packet(std::move(p));

            for(auto rd_idx : _rd_idxs){
                _rds[rd_idx].arm_timer();
            }

            // reset the status of the request_assembler
           _rd_idxs.clear();
           _remaining_size = max_req_len;
        }

        net::packet build_requet_batch_header();

        void setup_ip_udp_length(net::packet& p){
            auto udp_length =
                    static_cast<uint16_t>(
                            p.len()-sizeof(net::eth_hdr)- sizeof(net::ip_hdr));
            auto ip_length =
                    static_cast<uint16_t>(p.len() - sizeof(net::eth_hdr));
            auto ip_hdr = p.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
            ip_hdr->len = net::hton(ip_length);
            auto udp_hdr =
                    p.get_header<net::udp_hdr>(
                            sizeof(net::eth_hdr) + sizeof(net::ip_hdr));
            udp_hdr->len = net::hton(udp_length);
        }
        void setup_request_num(net::packet& p){
            // set up num_requests in the batch header
            auto num_requests = p.get_header<uint8_t>(sizeof(net::eth_hdr)+
                                                      sizeof(net::ip_hdr)+
                                                      sizeof(net::udp_hdr)+1);
            *num_requests = static_cast<uint8_t>(_rd_idxs.size());
        }
    };

private:
    // total_request_descriptor_count must be smaller than the max value
    // that can be represented by uint16_t.
    static constexpr unsigned total_request_descriptor_count = 2048;
    unsigned _port_id;
    std::vector<request_descriptor> _rds;
    std::vector<request_assembler> _ras;
    semaphore _pending_work_queue = {total_request_descriptor_count};
    circular_buffer<unsigned> _recycled_rds;
    timer<steady_clock_type> _check_ras_timer;
    std::experimental::optional<subscription<net::packet>> _sub;

public:
    explicit mica_client(unsigned port_id)
        : _port_id(port_id) {
        fprint(std::cout, "mica_client is created on core %d.\n", engine().cpu_id());
    }

    mica_client(const mica_client& other) = delete;
    mica_client(mica_client&& other)  = delete;
    mica_client& operator=(const mica_client& other) = delete;
    mica_client& operator=(mica_client&& other) = delete;

    future<> stop(){
        return make_ready_future<>();
    }

    void bootup(boost::program_options::variables_map& opts,
                vector<vector<port_pair>>& queue_map){
        // Currently, we only support one port for mica client, and
        // one port for mica server.
        mc_assert(port_manager::get().type(_port_id) == port_type::fdir);

        // prepare all the ingredients for the intialization loop
        net::ethernet_address remote_ei_eth_addr(
                net::parse_ethernet_address(
                        opts["mica-server-mac"].as<std::string>()));
        net::ipv4_address remote_ei_ip_addr(
                opts["mica-server-ip"].as<std::string>());
        uint16_t remote_ei_port_id = opts["mica-server-port-id"].as<uint16_t>();

        net::ethernet_address local_ei_eth_addr(port_manager::get().pOrt(_port_id).get_eth_addr());
        net::ipv4_address local_ei_ip_addr(
                opts["mica-client-ip"].as<std::string>());
        uint16_t local_ei_port_id = 0;

        uint16_t local_ei_core_id = static_cast<uint16_t>(engine().cpu_id());
        auto remote_smp_count = opts["mica-sever-smp-count"].as<uint16_t>();

        // first, create request_assembler for each pair of remote endpoint
        // and local endpoint
        for(uint16_t remote_ei_core_id=0;
            remote_ei_core_id<remote_smp_count;
            remote_ei_core_id++){
            uint16_t remote_ei_udp_port =
                    queue_map[local_ei_core_id][remote_ei_core_id].remote_port;
            endpoint_info remote_ei_info(remote_ei_eth_addr,
                                         remote_ei_ip_addr,
                                         remote_ei_udp_port,
                                         std::make_pair(remote_ei_core_id,
                                                        remote_ei_port_id));

            uint16_t local_ei_udp_port =
                    queue_map[local_ei_core_id][remote_ei_core_id].local_port;
            endpoint_info local_ei_info(local_ei_eth_addr,
                                        local_ei_ip_addr,
                                        local_ei_udp_port,
                                        std::make_pair(local_ei_core_id,
                                                       local_ei_port_id));

            _ras.emplace_back(remote_ei_info, local_ei_info,
                              port_manager::get().pOrt(_port_id), _rds);
        }

        // second, create all the request_descriptor
        for(unsigned rd_idx=0; rd_idx<total_request_descriptor_count; rd_idx++){
            _rds.emplace_back(rd_idx,
                    [this, rd_idx]{check_request_descriptor_timeout(rd_idx);});
            _recycled_rds.emplace_back(rd_idx);
        }

        // finally, set up a timer to regularly force packet out
        // in case there's not enough request put into the request
        // assemblers.
        _check_ras_timer.set_callback([this, which_ra=0] () mutable{
            for(int i=0; i<5; i++) { // why 5?
                check_request_assemblers(which_ra);
                which_ra = (which_ra+1)%(_ras.size());
            }
        });
        _check_ras_timer.arm_periodic(100us);
        fprint(std::cout, "mica_client is booted up on core %d.\n", engine().cpu_id());
    }

    void start_receiving(){
        _sub.emplace(port_manager::get().pOrt(_port_id).receive_seastar_packet([this](net::packet pkt){
            return receive(std::move(pkt));
        }));
        fprint(std::cout, "mica_client starts receiving on core %d.\n", engine().cpu_id());
    }

    future<mica_response> query(Operation op,
               size_t key_len, temporary_buffer<char> key,
               size_t val_len, temporary_buffer<char> val) {
        if(_pending_work_queue.try_wait(1)){
            auto rd_idx = _recycled_rds.front();
            _recycled_rds.pop_front();
            _rds[rd_idx].new_action(op, key_len, std::move(key),
                                    val_len, std::move(val));
            send_request_descriptor(rd_idx);
            return _rds[rd_idx].obtain_future();
        }
        else{
            return _pending_work_queue.wait(1).then(
                    [this, op, key_len, key=std::move(key),
                     val_len, val=std::move(val)] () mutable{
                auto rd_idx = _recycled_rds.front();
                _recycled_rds.pop_front();
                _rds[rd_idx].new_action(op, key_len, std::move(key),
                                        val_len, std::move(val));
                send_request_descriptor(rd_idx);
                return _rds[rd_idx].obtain_future();
            });
        }
    }

    future<mica_response> query(Operation op, mica_key key, mica_value value) {
        return query(op, key.get_actual_length(), key.get_roundup_buf(), value.get_actual_length(), value.get_roundup_buf());
    }

    size_t nr_request_descriptors() {
        return _recycled_rds.size();
    }

private:
    void check_request_assemblers(int which_ra){
        auto& ra = _ras[which_ra];
        ra.consume_send_stream();
        ra.force_send();
    }

    void check_request_descriptor_timeout(unsigned rd_idx){
        auto action_result = _rds[rd_idx].timeout_handler();

        switch (action_result){
        case action::no_action : {
            mc_assert(false);
            break;
        }
        case action::recycle_rd : {
            _recycled_rds.push_back(rd_idx);
            _pending_work_queue.signal(1);
            break;
        }
        case action::resend_rd : {
            send_request_descriptor(rd_idx);
            break;
        }
        default:
            break;
        }
    }

    void send_request_descriptor(unsigned rd_idx){
        // do something to send the request descriptor
        // to the request assembler

        // We don't need to calcualte the server index
        // because we only support a single server curently.

        // Here, each ra in _ras represents a partition.
        auto partition_id = calc_partition_id(_rds[rd_idx].get_key_hash(),
                                              _ras.size());
        _ras[partition_id].append_new_request_descriptor(rd_idx);
    }

    future<> receive(net::packet p){
        if (!is_valid(p) || !is_response(p) || p.nr_frags()!=1){
            return make_ready_future<>();
        }

        size_t offset = sizeof(RequestBatchHeader);
        while(offset < p.len()){
            auto rh = p.get_header<RequestHeader>(offset);

            size_t key_len = (rh->kv_length_vec >> 24);
            size_t roundup_key_len = roundup<8>(key_len);
            size_t val_len = (rh->kv_length_vec & ((1 << 24) - 1));
            size_t roundup_val_len = roundup<8>(val_len);

            // size_t key_offset = offset+sizeof(RequestHeader);
            // size_t val_offset = key_offset + roundup_key_len;
            // size_t next_req_offset = val_offset + roundup_val_len;

            size_t total_reponse_length = sizeof(RequestHeader)+
                    roundup_key_len+roundup_val_len;
            unsigned rd_idx = static_cast<unsigned>(rh->opaque >> 16);
            auto action_res = _rds[rd_idx].match_response(*rh,
                                  p.share(offset, total_reponse_length));

            switch (action_res){
            case action::no_action : {
                break;
            }
            case action::recycle_rd : {
                // recycle the request descriptor.
                _recycled_rds.push_back(rd_idx);
                _pending_work_queue.signal(1);
                break;
            }
            case action::resend_rd : {
                mc_assert(false);
                break;
            }
            default:
                break;
            }

            offset += total_reponse_length;
        }
        mc_assert(offset == p.len());
        return make_ready_future<>();
    }

    uint16_t calc_partition_id(uint64_t key_hash, size_t partition_count) {
      return static_cast<uint16_t>((key_hash >> 48) % partition_count);
    }

    bool is_valid(net::packet& p);

    bool is_response(net::packet& p) const;
};

class mica_manager {
    std::vector<std::vector<mica_client*>> _mica_clients;
    vector<vector<port_pair>> _queue_map;

    mica_manager() {}

    using mica_shard = shard_container_trait<mica_client>;

public:
    static mica_manager& get() {
        static mica_manager mm;
        return mm;
    }

    future<> add_mica_client(boost::program_options::variables_map& opts,
                             uint16_t port_id) {
        assert(_mica_clients.size() == 0);
        assert(port_manager::get().type(port_id) == port_type::fdir);

        _mica_clients.push_back(std::vector<mica_client*>(seastar::smp::count, nullptr));

        auto mica_shard_sptr = std::make_shared<mica_shard::shard_t>();
        return mica_shard_sptr->start(port_id).then([mica_shard_sptr, this]{
            return mica_shard_sptr->invoke_on_all(&mica_shard::instance_t::save_container_ptr,
                                                  &(_mica_clients.at(0)));
        }).then([mica_shard_sptr]{
            return mica_shard_sptr->stop();
        }).then([mica_shard_sptr, this, port_id, &opts]{
            _queue_map = calculate_queue_mapping(opts, port_manager::get().pOrt(port_id));
            auto& queue_map = _queue_map;

            return invoke_on_all([&opts, &queue_map](mica_client& mc){
                mc.bootup(opts, queue_map);
            });
        }).then([this]{
            return invoke_on_all([](mica_client& mc){
                mc.start_receiving();
            });
        });
    }

    mica_client& mc() {
        return *(_mica_clients.at(0).at(engine().cpu_id()));
    }

private:
    template <typename Func>
    future<> invoke_on_all(Func&& func) {
        return parallel_for_each(boost::irange<unsigned>(0, _mica_clients.at(0).size()), [this, &func] (unsigned c) {
            return smp::submit_to(c, [this, func] {
                return func(*(_mica_clients.at(0).at(engine().cpu_id())));
            });
        });
    }
};

} // namespace netstar

#endif // _MICA_CLIENT
