#ifndef _MICA_DEF
#define _MICA_DEF

#include <array>
#include <cstdint>
#include <utility>
#include <vector>
#include <boost/program_options/variables_map.hpp>
#include <string>

#include "net/packet.hh"
#include "net/udp.hh"
#include "net/ip_checksum.hh"
#include "net/ip.hh"
#include "net/net.hh"
#include "net/byteorder.hh"

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

#include "netstar/port.hh"

using namespace seastar;
using namespace std;

namespace netstar{

#define ENABLE_MC_ASSERTION 0

#if ENABLE_MC_ASSERTION
#define mc_assert(condition) assert(condition)
#else
#define mc_assert(condition) ((void)0)
#endif

#define MICA_USE_CB 0

#if MICA_USE_CB
static_assert(ENABLE_MC_ASSERTION==0, "We must disable the mica assertion if we use mica callback mode");
#endif

struct endpoint_info{
    net::ethernet_address eth_addr;
    net::ipv4_address ip_addr;
    uint16_t udp_port;
    std::pair<uint16_t, uint16_t> lcore_port_pair;

    explicit endpoint_info(net::ethernet_address eaddr,
                           net::ipv4_address ipaddr,
                           uint16_t u_port,
                           std::pair<uint16_t, uint16_t> lp_pair) :
            eth_addr(eaddr),
            ip_addr(ipaddr),
            udp_port(u_port),
            lcore_port_pair(lp_pair) {}
};

struct RequestBatchHeader {
    // 0
    uint8_t header[sizeof(ether_hdr) + sizeof(ipv4_hdr) + sizeof(udp_hdr)];
    // 42
    uint8_t magic;  // 0x78 for requests; 0x79 for responses.
    uint8_t num_requests;
    // 44
    uint32_t reserved0;
    // 48
    // KeyValueRequestHeader
};

struct RequestHeader {
    // 0
    uint8_t operation;  // ::mica::processor::Operation
    uint8_t result;     // ::mica::table::Result
    // 2
    uint16_t reserved0;
    // 4
    uint32_t kv_length_vec;  // key_length: 8, value_length: 24
    // 8
    uint64_t key_hash;
    // 16
    uint32_t opaque;
    uint32_t reserved1;
    // 24
    // Key-value data
};

enum class Operation : uint8_t{
    kReset = 0,
    kNoopRead,
    kNoopWrite,
    kAdd,
    kSet,
    kGet,
    kTest,
    kDelete,
    kIncrement,
};

enum class Result : uint8_t {
    kSuccess = 0,
    kError,
    kInsufficientSpace,
    kExists,
    kNotFound,
    kPartialValue,
    kNotProcessed,
    kNotSupported,
    kTimedOut,
    kRejected,
};

// The following definitions and functions are used to initialize
// a so-called "queue mapping". This is used to direct packet from
// one core on the mica client to a specific core on the mica server.
// If the fdir functionality works correctly on our x710 NIC, we wouldn't
// need to calculate the queue mapping in such a complicated manner.

struct port_pair{
    uint16_t local_port;
    uint16_t remote_port;
};

vector<vector<port_pair>>
calculate_queue_mapping(boost::program_options::variables_map& opts,
                        port& pt);
} // namespace netstar

#endif // _MICA_CLIENT_DEF
