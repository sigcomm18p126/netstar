#include "netstar/preprocessor/tcp_ppr.hh"

#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/sleep.hh"

#include "net/packet-data-source.hh"

#include "netstar/port_manager.hh"
#include "netstar/stack/stack_manager.hh"
#include "netstar/hookpoint/hook_manager.hh"
#include "netstar/mica/mica_client.hh"
#include "netstar/asyncflow/sd_async_flow.hh"
#include "netstar/asyncflow/async_flow.hh"
#include "netstar/preprocessor/udp_ppr.hh"

#include "http/request_parser.hh"
#include "http/http_response_parser.hh"
#include "http/request.hh"

#include "netstar/aho-corasick/aho.h"
#include "netstar/aho-corasick/ds_queue.h"
#include "netstar/aho-corasick/fpp.h"

using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

#define MAX_MATCH 8192

class IPS{
public:
    IPS(){

        int num_patterns, i;

        int num_threads = 1;
        assert(num_threads >= 1 && num_threads <= AHO_MAX_THREADS);

        stats =(struct stat_t*)malloc(num_threads * sizeof(struct stat_t));
        for(i = 0; i < num_threads; i++) {
            stats[i].tput = 0;
        }
        struct aho_pattern *patterns;
        /* Thread structures */
        //pthread_t worker_threads[AHO_MAX_THREADS];


        red_printf("State size = %lu\n", sizeof(struct aho_state));

        /* Initialize the shared DFAs */
        for(i = 0; i < AHO_MAX_DFA; i++) {
            printf("Initializing DFA %d\n", i);
            aho_init(&dfa_arr[i], i);
        }

        red_printf("Adding patterns to DFAs\n");
        patterns = aho_get_patterns(AHO_PATTERN_FILE,
            &num_patterns);

        for(i = 0; i < num_patterns; i++) {
            int dfa_id = patterns[i].dfa_id;
            aho_add_pattern(&dfa_arr[dfa_id], &patterns[i], i);
        }

        red_printf("Building AC failure function\n");
        for(i = 0; i < AHO_MAX_DFA; i++) {
            aho_build_ff(&dfa_arr[i]);
            aho_preprocess_dfa(&dfa_arr[i]);
        }
    }
    struct aho_dfa dfa_arr[AHO_MAX_DFA];
    struct stat_t *stats;

};

class forwarder;
distributed<forwarder> forwarders;

class ids {
    enum class ids_state_code {
        abnormal_tcp_flow,
        intrusion_detected,
        parser_error,
        normal_detection
    };

    sd_async_flow<tcp_reorder_ppr> _af;
    ids_state_code _state_code;
    http_request_parser _parser;
    input_stream<char> _cur_istream;
    net::packet _cur_payload_pkt;

    // Xiaodong's IPS code.
    struct ips_flow_state{
        uint8_t tag;
        uint32_t _state;
        uint32_t _dfa_id;
        bool _alert;
    };

    struct mp_list_t {
        int num_match;
        uint16_t ptrn_id[MAX_MATCH];
    };

    IPS& _ac_sm;
    ips_flow_state _ac_state;

    void init_automataState(struct ips_flow_state& state){
          srand((unsigned)time(NULL));
          state._state=0;
          state._alert=false;
          state._dfa_id=rand()%AHO_MAX_DFA;
      }
    void parse_pkt(net::packet *rte_pkt, struct ips_flow_state* state,struct aho_pkt*  aho_pkt){
        assert(rte_pkt->nr_frags() == 1);
        aho_pkt->content= reinterpret_cast<uint8_t*>(rte_pkt->frag(0).base);
        // memcpy(aho_pkt->content,reinterpret_cast<uint8_t*>(rte_pkt->get_header(0,sizeof(char))),rte_pkt->len()-1);
        aho_pkt->dfa_id=state->_dfa_id;
        aho_pkt->len=rte_pkt->frag(0).size;
    }
    bool state_updated(struct ips_flow_state* old_,struct ips_flow_state* new_){
        if(old_->_alert==new_->_alert&&old_->_dfa_id==new_->_dfa_id&&old_->_state==new_->_state){
            return false;
        }
        return true;
    }

    void process_batch(const struct aho_dfa *dfa_arr,
        const struct aho_pkt *pkts, struct mp_list_t *mp_list, struct ips_flow_state* ips_state)
    {
        int I, j;

        for(I = 0; I < BATCH_SIZE; I++) {
            int dfa_id = pkts[I].dfa_id;
            int len = pkts[I].len;
            struct aho_state *st_arr = dfa_arr[dfa_id].root;

            int state = ips_state->_state;
          if(state>=dfa_arr[dfa_id].num_used_states){
              ips_state->_alert=false;
              ips_state->_state=state;
              return;
          }


            for(j = 0; j < len; j++) {

                int count = st_arr[state].output.count;

                if(count != 0) {
                    /* This state matches some patterns: copy the pattern IDs
                      *  to the output */
                    int offset = mp_list[I].num_match;
                    memcpy(&mp_list[I].ptrn_id[offset],
                        st_arr[state].out_arr, count * sizeof(uint16_t));
                    mp_list[I].num_match += count;
                    ips_state->_alert=true;
                    ips_state->_state=state;
                    return;

                }
                int inp = pkts[I].content[j];
                state = st_arr[state].G[inp];
            }
            ips_state->_state=state;
        }


    }
    void ids_func(struct aho_ctrl_blk *cb,struct ips_flow_state* state)
    {
        int i, j;



        struct aho_dfa *dfa_arr = cb->dfa_arr;
        struct aho_pkt *pkts = cb->pkts;
        int num_pkts = cb->num_pkts;

        /* Per-batch matched patterns */
        struct mp_list_t mp_list[BATCH_SIZE];
        for(i = 0; i < BATCH_SIZE; i++) {
            mp_list[i].num_match = 0;
        }

        /* Being paranoid about GCC optimization: ensure that the memcpys in
          *  process_batch functions don't get optimized out */


        //int tot_proc = 0;     /* How many packets did we actually match ? */
        //int tot_success = 0;  /* Packets that matched a DFA state */
        // tot_bytes = 0;       /* Total bytes matched through DFAs */

        for(i = 0; i < num_pkts; i += BATCH_SIZE) {
            process_batch(dfa_arr, &pkts[i], mp_list,state);

            for(j = 0; j < BATCH_SIZE; j++) {
                int num_match = mp_list[j].num_match;
                assert(num_match < MAX_MATCH);


                mp_list[j].num_match = 0;
            }
        }

    }
    void ips_detect(net::packet *rte_pkt, struct ips_flow_state* state){

        struct aho_pkt pkts;
        parse_pkt(rte_pkt, state, &pkts);
        struct aho_ctrl_blk worker_cb;
        worker_cb.stats = _ac_sm.stats;
        worker_cb.tot_threads = 1;
        worker_cb.tid = 0;
        worker_cb.dfa_arr = _ac_sm.dfa_arr;
        worker_cb.pkts = &pkts;
        worker_cb.num_pkts = 1;

        ids_func(&worker_cb,state);
        // free(pkts.content);
    }
    // Xiaodong's IPS code ends here.

public:
    ids(sd_async_flow<tcp_reorder_ppr>&& af, IPS& ac_sm)
        : _af(std::move(af))
        , _state_code(ids_state_code::normal_detection)
        , _cur_payload_pkt(net::packet::make_null_packet())
        , _ac_sm(ac_sm) {
    }

    future<> configure() {
        _af.register_events(tcp_reorder_events::new_data);
        _af.register_events(tcp_reorder_events::abnormal_connection);
        _state_code = ids_state_code::normal_detection;
        _parser.init();
        return _af.run_async_loop([this](){
            if(_state_code == ids_state_code::abnormal_tcp_flow) {
                return handle_abnormal_tcp_flow();
            }
            else if(_state_code == ids_state_code::intrusion_detected) {
                return handle_intrusion_detected();
            }
            else if(_state_code == ids_state_code::parser_error) {
                return handle_parser_error();
            }
            else{
                return handle_normal_detection();
            }
        });
    }

private:

    // The major detection procedure.
    future<af_action> handle_normal_detection() {
        if(_af.cur_event().on_close_event()) {
            if(_af.cur_event().on_event(tcp_reorder_events::abnormal_connection)) {
                return make_ready_future<af_action>(af_action::close_drop);
            }

            if(_af.cur_event().on_event(tcp_reorder_events::new_data)) {
                auto pkt = _af.get_ppr()->read_reordering_buffer();
                _cur_istream = as_input_stream(std::move(pkt));
                return detect().then([this](af_action a){
                    if(a==af_action::forward) {
                        return af_action::close_forward;
                    }
                    else{
                        return af_action::close_drop;
                    }
                });
            }

            return make_ready_future<af_action>(af_action::close_forward);
        }

        if(_af.cur_event().on_event(tcp_reorder_events::abnormal_connection)) {
            _state_code = ids_state_code::abnormal_tcp_flow;
            return make_ready_future<af_action>(af_action::drop);
        }
        else{
            auto pkt = _af.get_ppr()->read_reordering_buffer();
            assert(!_cur_payload_pkt);
            _cur_payload_pkt = std::move(pkt.share());
            _cur_istream = as_input_stream(std::move(pkt));
            return detect();
        }
    }

    future<af_action> detect() {
        return _cur_istream.consume(_parser).then([this]{
            if(_parser.is_done()) {
                init_automataState(_ac_state);
                ips_detect(&_cur_payload_pkt, &_ac_state);
                if(_ac_state._alert) {
                    _parser.init();
                    _cur_istream = input_stream<char>();
                    _cur_payload_pkt = net::packet::make_null_packet();
                    _state_code = ids_state_code::intrusion_detected;
                    _af.register_events(tcp_reorder_events::pkt_in);
                    return make_ready_future<af_action>(af_action::drop);
                }

                // fprint(std::cout, "Get a new HTTP request.\n");
                _parser.init();

                if(_cur_istream.eof()) {
                    _cur_payload_pkt = net::packet::make_null_packet();
                    return make_ready_future<af_action>(af_action::forward);
                }
                else {
                    return detect();
                }
            }
            else if(_parser.is_error()) {
                // fprint(std::cout, "The parser generates an error.\n");
                _parser.init();
                _cur_istream = input_stream<char>();
                _cur_payload_pkt = net::packet::make_null_packet();
                _state_code = ids_state_code::parser_error;
                _af.register_events(tcp_reorder_events::pkt_in);
                return make_ready_future<af_action>(af_action::drop);
            }
            else {
                // Parser wants more.
                assert(_cur_istream.eof());
                _cur_payload_pkt = net::packet::make_null_packet();
                return make_ready_future<af_action>(af_action::forward);
            }
        });
    }

    // If abnormal_connection event is raised, the TCP flow is identified
    // as abnormal. There will only be abnormal_connection events raised
    // for each received packets in the future, and we should drop all the packets.
    future<af_action> handle_abnormal_tcp_flow() {
        if(_af.cur_event().on_close_event()) {
            return make_ready_future<af_action>(af_action::close_drop);
        }
        else {
            return make_ready_future<af_action>(af_action::drop);
        }
    }

    // The HTTP payload doesn't pass the AC atomaton. In this case,
    // we should first register pkt_in events. Then we should
    // retrieve all the connection data, and drop all the packets.
    future<af_action> handle_intrusion_detected() {
        if(_af.cur_event().on_close_event()) {
            return make_ready_future<af_action>(af_action::close_drop);
        }
        else {
            if(_af.cur_event().on_event(tcp_reorder_events::new_data)) {
                auto pkt = _af.get_ppr()->read_reordering_buffer();
            }
            return make_ready_future<af_action>(af_action::drop);
        }
    }

    // The connection is not transmitting valid HTTP request. Just
    // apply the same operation as if an intrusion has been detected.
    future<af_action> handle_parser_error() {
        return handle_intrusion_detected();
    }
};

class forwarder {
    sd_async_flow_manager<tcp_reorder_ppr> _tcp_forward;
    sd_async_flow_manager<tcp_ppr> _tcp_reverse;
    sd_async_flow_manager<udp_ppr> _udp_forward;
    sd_async_flow_manager<udp_ppr> _udp_reverse;
    IPS ac_automaton;

public:
    forwarder() {
        hook_manager::get().hOok(0).send_to_sdaf_manager(_tcp_forward);
        hook_manager::get().hOok(0).receive_from_sdaf_manager(_tcp_reverse);

        hook_manager::get().hOok(1).receive_from_sdaf_manager(_tcp_forward);
        hook_manager::get().hOok(1).send_to_sdaf_manager(_tcp_reverse);

        hook_manager::get().hOok(0).send_to_sdaf_manager(_udp_forward);
        hook_manager::get().hOok(0).receive_from_sdaf_manager(_udp_reverse);

        hook_manager::get().hOok(1).receive_from_sdaf_manager(_udp_forward);
        hook_manager::get().hOok(1).send_to_sdaf_manager(_udp_reverse);
    }

    future<> stop() {
        return make_ready_future<>();
    }

    void run_udp_manager(int) {
        repeat([this]{
            return _tcp_forward.on_new_initial_context().then([this]() mutable {
                auto ic = _tcp_forward.get_initial_context();

                do_with(ids(ic.get_sd_async_flow(), std::ref(ac_automaton)), [](ids& instance){
                    return instance.configure();
                });

                return stop_iteration::no;
            });
        });

        repeat([this]{
            return _tcp_reverse.on_new_initial_context().then([this]() mutable {
                auto ic = _tcp_reverse.get_initial_context();
                return stop_iteration::no;
            });
        });
    }

    struct info {
        uint64_t ingress_received;
        uint64_t ingress_send;
        uint64_t egress_received;
        uint64_t egress_send;
        size_t active_flow_num;
        size_t reverse_flow_num;
        void operator+=(const info& o) {
            ingress_received += o.ingress_received;
            ingress_send += o.ingress_send;
            egress_received += o.egress_received;
            egress_send += o.egress_send;
            active_flow_num += o.active_flow_num;
            reverse_flow_num += o.reverse_flow_num;
        }
    };
    info _old{0,0,0,0,0,0};

    future<info> get_info() {
        return make_ready_future<info>(info{port_manager::get().pOrt(0).rx_pkts(),
                                            port_manager::get().pOrt(0).tx_pkts(),
                                            port_manager::get().pOrt(1).rx_pkts(),
                                            port_manager::get().pOrt(1).tx_pkts(),
                                            _tcp_forward.peek_active_flow_num(),
                                            _tcp_reverse.peek_active_flow_num()});
    }
    void collect_stats(int) {
        repeat([this]{
            return forwarders.map_reduce(adder<info>(), &forwarder::get_info).then([this](info i){
                fprint(std::cout, "p0_recv=%d, p0_send=%d, p1_recv=%d, p1_send=%d, fwd_tcp_num=%d, reverse_tcp_num=%d.\n",
                        i.ingress_received-_old.ingress_received,
                        i.ingress_send-_old.ingress_send,
                        i.egress_received - _old.egress_received,
                        i.egress_send - _old.egress_send,
                        i.active_flow_num,
                        i.reverse_flow_num);
                _old = i;
            }).then([]{
                return seastar::sleep(1s).then([]{
                    return stop_iteration::no;
                });
            });
        });
    }
};

int main(int ac, char** av) {
    app_template app;
    sd_async_flow_manager<tcp_ppr> m1;
    sd_async_flow_manager<udp_ppr> m2;
    async_flow_manager<tcp_ppr> m3;
    async_flow_manager<udp_ppr> m4;

    return app.run_deprecated(ac, av, [&app] {

        auto& opts = app.configuration();

        port_manager::get().add_port(opts, 0, port_type::standard).then([&opts]{
            return port_manager::get().add_port(opts, 1, port_type::standard);
        }).then([]{
            return hook_manager::get().add_hook_point(hook_type::sd_async_flow, 0);
        }).then([]{
            return hook_manager::get().add_hook_point(hook_type::sd_async_flow, 1);
        }).then([]{
            return forwarders.start();
        }).then([]{
            return hook_manager::get().invoke_on_all(0, &hook::check_and_start);
        }).then([]{
            return hook_manager::get().invoke_on_all(1, &hook::check_and_start);
        }).then([]{
            return forwarders.invoke_on_all(&forwarder::run_udp_manager, 1);
        }).then([]{
            return forwarders.invoke_on(0, &forwarder::collect_stats, 1);
        });
    });
}
