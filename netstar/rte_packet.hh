#ifndef _RTE_PACKET_HH
#define _RTE_PACKET_HH

#include "net/packet.hh"

#include <rte_mbuf.h>

namespace seastar{
namespace standard_device {
template <bool HugetlbfsMemBackend>
class dpdk_qp;
}
namespace fdir_device {
template <bool HugetlbfsMemBackend>
class dpdk_qp;
}
}

namespace netstar{

class rte_packet {
#ifdef HAVE_DPDK

   rte_mbuf* _mbuf;

public:
   // Explicit constructors.
   rte_packet()
       : _mbuf(nullptr) {}

   // Deconstructors
   ~rte_packet() {
       if(_mbuf) {
           rte_pktmbuf_free(_mbuf);
       }
   }

   // Copy construct/assign
   rte_packet(const rte_packet& other) = delete;
   rte_packet& operator=(const rte_packet& other) = delete;

   // Move construct/asign
   rte_packet(rte_packet&& other) noexcept
       : _mbuf(other._mbuf) {
       other._mbuf = nullptr;
   }
   rte_packet& operator=(rte_packet&& other) noexcept {
       if(this != &other) {
           this->~rte_packet();
           new (this) rte_packet(std::move(other));
       }
       return *this;
   }

   // Boolean operator overloads
   explicit operator bool() {
       return bool(_mbuf);
   }

   // Get a header pointer.
   template <typename Header>
   Header* get_header(size_t offset = 0) {
       assert(_mbuf);
       if(offset+sizeof(Header) > rte_pktmbuf_pkt_len(_mbuf)) {
           return nullptr;
       }
       return reinterpret_cast<Header*>(rte_pktmbuf_mtod_offset(_mbuf, void*, offset));
   }

   char* get_header(size_t offset, size_t size) {
       if(offset+size > rte_pktmbuf_pkt_len(_mbuf)) {
           return nullptr;
       }

       return reinterpret_cast<char*>(rte_pktmbuf_mtod_offset(_mbuf, void*, offset));
   }

   // Trim some payload from front of the packet
   void trim_front(size_t how_much) {
       assert(_mbuf);
       assert(how_much <= rte_pktmbuf_pkt_len(_mbuf));
       rte_pktmbuf_adj(_mbuf, how_much);
   }

   // Trim some payload from the back of the packet
   void trim_back(size_t how_much) {
       assert(_mbuf);
       assert(how_much <= rte_pktmbuf_pkt_len(_mbuf));
       rte_pktmbuf_trim(_mbuf, how_much);
   }

   // Append some content to the back of the packet
   void append(size_t how_much) {
       assert(_mbuf);
       assert(how_much <= rte_pktmbuf_tailroom(_mbuf));
       rte_pktmbuf_append(_mbuf, how_much);
   }

   // Prepend a header to the front of the packet.
   template <typename Header>
   Header* prepend_header(size_t extra_size = 0) {
       assert(_mbuf);
       assert(sizeof(Header)+extra_size <= rte_pktmbuf_headroom(_mbuf));
       auto h = rte_pktmbuf_prepend(_mbuf, sizeof(Header) + extra_size);
       return new (h) Header{};
   }

   // Obtain the length of the packet.
   unsigned len() const {
       assert(_mbuf);
       return rte_pktmbuf_pkt_len(_mbuf);
   }

   // Get copy of the packet represented in net::packet
   std::experimental::optional<seastar::net::packet>
   get_packet() {
       // Fast path, consider removing it for stable code.
       assert(_mbuf);

       auto len = rte_pktmbuf_data_len(_mbuf);
       char* buf = (char*)malloc(len);

       if (!buf) {
           // Return nullopt if allocation fails.
           return std::experimental::nullopt;
       } else {
           // Build up the packet.
           rte_memcpy(buf, rte_pktmbuf_mtod(_mbuf, char*), len);
           seastar::net::packet pkt(seastar::net::fragment{buf, len},
                                    seastar::make_free_deleter(buf));

           // Set up theoffload info.
           seastar::net::offload_info oi;
           if (_mbuf->ol_flags & PKT_RX_VLAN_PKT) {
               oi.vlan_tci = _mbuf->vlan_tci;
           }
           pkt.set_offload_info(oi);

           if (_mbuf->ol_flags & PKT_RX_RSS_HASH) {
               pkt.set_rss_hash(_mbuf->hash.rss);
           }

           return std::move(pkt);
       }
   }

private:
   friend class seastar::standard_device::dpdk_qp<true>;
   friend class seastar::standard_device::dpdk_qp<false>;
   friend class seastar::fdir_device::dpdk_qp<true>;
   friend class seastar::fdir_device::dpdk_qp<false>;

   // Explicitly invalidate _mbuf and return the original
   // _mbuf.
   // Be extra careful!! This is used internally by different
   // devices to directly send an rte_packet. And I don't know
   // hide it from public access. User should not
   // call this function by any means.
   rte_mbuf* release_mbuf() {
       rte_mbuf* tmp = _mbuf;
       _mbuf = nullptr;
       return tmp;
   }

   // How are you going to call this constructor, if you
   // can't build a mbuf from the rte_mpool?
   rte_packet(rte_mbuf* mbuf) {
       assert(mbuf);
       assert(rte_pktmbuf_is_contiguous(mbuf));
       _mbuf = mbuf;
   }

#endif // HAVE_DPDK
};

} // namespace netstar

#endif // _RTE_PACKET_HH
