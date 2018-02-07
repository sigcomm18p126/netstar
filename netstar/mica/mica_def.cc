#include "netstar/mica/mica_def.hh"
#include "core/print.hh"
#include "core/reactor.hh"
#include "net/toeplitz.hh"

namespace netstar{

// arguments: program options, number of local cores, number of remote
//            cores, local IPv4 address in string, remote IPv4 address
//            in string, the port used by the mica client
// return:    ret[x][y].local_port,
//            local port that maps local queue x to remote queue y;
//            ret[x][y].remote_port,
//            remote port that maps local queue x to remote queue y
vector<vector<port_pair>>
do_calculate_queue_mapping(boost::program_options::variables_map& opts,
                        unsigned local_smp_count,
                        unsigned remote_smp_count,
                        std::string local_ip_addr_str,
                        std::string remote_ip_addr_str,
                        port& pt){
    // res[x][y].local_port:
    // local port that maps local queue x to remote queue y
    // res[x][y].remote_port:
    // remote port that maps local queue x to remote queue y
    vector<vector<port_pair>> res;
    res.resize(local_smp_count);
    for(auto& v : res){
        v.resize(remote_smp_count);
    }

    // record whether a position in res is used
    vector<vector<bool>> res_pos_flag;
    res_pos_flag.resize(local_smp_count);
    for(auto& v : res_pos_flag){
        v.resize(remote_smp_count, false);
    }

    unsigned total = local_smp_count * remote_smp_count;
    net::ipv4_address local_ip_addr(local_ip_addr_str);
    net::ipv4_address remote_ip_addr(remote_ip_addr_str);

    // manually build a redirection table for remote side.
    // We assume that the size of the redirection table is 512
    vector<uint8_t> remote_redir_table(512);
    unsigned i = 0;
    for (auto& r : remote_redir_table) {
        r = i++ % remote_smp_count;
    }

    // iterate through each of the local and remote port
    for(uint16_t local_port = 10240; local_port < 65535; local_port ++){
        for(uint16_t remote_port = 10240; remote_port < 65535; remote_port ++){

            net::l4connid<net::ipv4_traits>
            to_local{local_ip_addr, remote_ip_addr, local_port, remote_port};

            net::l4connid<net::ipv4_traits>
            to_remote{remote_ip_addr, local_ip_addr, remote_port, local_port};

            const rss_key_type& rss_key = (seastar::smp::count==1)?
                                          (seastar::default_rsskey_52bytes) :
                                          (pt.get_rss_key());

            unsigned local_queue = (seastar::smp::count==1)?
                                    0 :
                                    pt.hash2cpu(to_local.hash(rss_key));
            unsigned remote_queue =
                    remote_redir_table[
                                       to_remote.hash(rss_key) &
                                       (remote_redir_table.size() - 1)
                                       ];

            if(!res_pos_flag[local_queue][remote_queue]){
                res_pos_flag[local_queue][remote_queue] = true;
                total--;
                res[local_queue][remote_queue].local_port = local_port;
                res[local_queue][remote_queue].remote_port = remote_port;
            }

            if(total == 0){
                return res;
            }
        }
    }

    if(total!=0) {
        printf("Fail to find a valid queue mapping. \n");
        assert(false);
    }

    return res;
}

vector<vector<port_pair>>
calculate_queue_mapping(boost::program_options::variables_map& opts,
                        port& pt){
    return do_calculate_queue_mapping(
            opts,
            smp::count,
            static_cast<unsigned>(opts["mica-sever-smp-count"].as<uint16_t>()),
            opts["mica-client-ip"].as<std::string>(),
            opts["mica-server-ip"].as<std::string>(),
            pt);
}

} // namespace netstar
