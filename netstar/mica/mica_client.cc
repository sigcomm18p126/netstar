#include "netstar/mica/mica_client.hh"

namespace netstar{

net::packet mica_client::request_assembler::build_requet_batch_header(){
    net::packet pkt;

    // reserved0
    pkt.prepend_header<uint32_t>();

    // num_requests
    pkt.prepend_header<uint8_t>();

    // magic number
    auto magic_hdr = pkt.prepend_header<uint8_t>();
    *magic_hdr = 0x78;

    // udp header
    auto uhdr = pkt.prepend_header<net::udp_hdr>();
    uhdr->src_port = _local_ei.udp_port;
    uhdr->dst_port = _remote_ei.udp_port;
    uhdr->len = 0;
    uhdr->cksum = 0;
    *uhdr = net::hton(*uhdr);

    // ip header
    auto iph = pkt.prepend_header<net::ip_hdr>();
    iph->ihl = sizeof(*iph) / 4;
    iph->ver = 4;
    iph->dscp = 0;
    iph->ecn = 0;
    iph->len = pkt.len();
    iph->id = 0;
    iph->frag = 0;
    iph->ttl = 64;
    iph->ip_proto = (uint8_t)net::ip_protocol_num::udp;
    iph->csum = 0;
    iph->src_ip = _local_ei.ip_addr;
    iph->dst_ip = _remote_ei.ip_addr;
    *iph = net::hton(*iph);

    // ethernet header
    auto eh = pkt.prepend_header<net::eth_hdr>();
    net::ethernet_address eth_src = _local_ei.eth_addr;
    net::ethernet_address eth_dst = _remote_ei.eth_addr;
    eh->dst_mac = eth_dst;
    eh->src_mac = eth_src;
    eh->eth_proto = uint16_t(net::eth_protocol_num::ipv4);
    *eh = net::hton(*eh);

    // set up the offload information here
    // I don't whether I really need this
    net::offload_info oi;
    oi.needs_csum = false;
    oi.protocol = net::ip_protocol_num::udp;
    pkt.set_offload_info(oi);

    pkt.linearize();
    assert(pkt.nr_frags() == 1);
    return pkt;
}

bool mica_client::is_valid(net::packet& p){
    auto eth_hdr = p.get_header<net::eth_hdr>();
    auto ip_hdr = p.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
    auto udp_hdr = p.get_header<net::udp_hdr>(sizeof(net::eth_hdr) + sizeof(net::ip_hdr));
    auto rbh = p.get_header<RequestBatchHeader>();

    if (p.len() < sizeof(RequestBatchHeader)) {
        // printf("too short packet length\n");
        return false;
    }

    if (net::ntoh(eth_hdr->eth_proto) != uint16_t(net::eth_protocol_num::ipv4)) {
        // printf("invalid network layer protocol\n");
        return false;
    }

    if (ip_hdr->ihl != 5 && ip_hdr->ver != 4) {
        // printf("invalid IP layer protocol\n");
        return false;
    }

    if (ip_hdr->id != 0 || ip_hdr->frag != 0) {
        // printf("ignoring fragmented IP packet\n");
        return false;
    }


    if (net::ntoh(ip_hdr->len) != p.len() - sizeof(net::eth_hdr)) {
        // printf("invalid IP packet length\n");
        return false;
    }

    if (ip_hdr->ip_proto != (uint8_t)net::ip_protocol_num::udp) {
        // printf("invalid transport layer protocol\n");
        return false;
    }

    if (net::ntoh(udp_hdr->len) != p.len() - sizeof(net::eth_hdr) - sizeof(net::ip_hdr)) {
        // printf("invalid UDP datagram length\n");
        return false;
    }

    if(udp_hdr->cksum != 0 || ip_hdr->csum != 0){
        // printf("We only accept packet with zero checksums\n");
        return false;
    }

    if (rbh->magic != 0x78 && rbh->magic != 0x79) {
        printf("invalid magic\n");
        return false;
    }

    return true;
}

bool mica_client::is_response(net::packet& p) const {
    auto rbh = p.get_header<RequestBatchHeader>();
    return rbh->magic == 0x79;
}

} // namespace nestar
