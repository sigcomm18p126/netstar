#ifndef _IO_CONNECTOR_HH
#define _IO_CONNECTOR_HH

#include "core/stream.hh"

#include "netstar/rte_packet.hh"

#include <experimental/optional>

namespace netstar {

namespace internal {

template<typename FlowKeyType>
struct io_connector {
    std::experimental::optional<seastar::stream<rte_packet, FlowKeyType*>> send_stream;
    std::experimental::optional<seastar::subscription<rte_packet>> recv_sub;

    io_connector() {}

};

} // namespace internal

} // namespace netstar

#endif // _IO_CONNECTOR_HH
