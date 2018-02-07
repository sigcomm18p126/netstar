/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 */

#ifndef ARP_HH_
#define ARP_HH_

#include "net.hh"
#include "core/reactor.hh"
#include "core/byteorder.hh"
#include "ethernet.hh"
#include "core/print.hh"
#include <unordered_map>

namespace seastar {

namespace net {

class arp;
class arp_for_protocol;
template <typename L3>
class arp_for;

class arp_for_protocol {
protected:
    arp& _arp;
    uint16_t _proto_num;
public:
    arp_for_protocol(arp& a, uint16_t proto_num);
    virtual ~arp_for_protocol();
    virtual future<> received(packet p) = 0;
    virtual bool forward(forward_hash& out_hash_data, packet& p, size_t off) { return false; }
};

class arp {
    interface* _netif;
    l3_protocol _proto;
    subscription<packet, ethernet_address> _rx_packets;
    std::unordered_map<uint16_t, arp_for_protocol*> _arp_for_protocol;
    circular_buffer<l3_protocol::l3packet> _packetq;
private:
    struct arp_hdr {
        uint16_t htype;
        uint16_t ptype;

        static arp_hdr read(const char* p) {
            arp_hdr ah;
            ah.htype = consume_be<uint16_t>(p);
            ah.ptype = consume_be<uint16_t>(p);
            return ah;
        }
        static constexpr size_t size() { return 4; }
    };
public:
    explicit arp(interface* netif);
    void add(uint16_t proto_num, arp_for_protocol* afp);
    void del(uint16_t proto_num);
private:
    ethernet_address l2self() { return _netif->hw_address(); }
    future<> process_packet(packet p, ethernet_address from);
    bool forward(forward_hash& out_hash_data, packet& p, size_t off);
    std::experimental::optional<l3_protocol::l3packet> get_packet();
    template <class l3_proto>
    friend class arp_for;
};

template <typename L3>
class arp_for : public arp_for_protocol {
public:
    using l2addr = ethernet_address;
    using l3addr = typename L3::address_type;
private:
    static constexpr auto max_waiters = 512;
    enum oper {
        op_request = 1,
        op_reply = 2,
    };
// patch by djp
// Expose arp_hdr to public.
public:
    struct arp_hdr {
        uint16_t htype;
        uint16_t ptype;
        uint8_t hlen;
        uint8_t plen;
        uint16_t oper;
        l2addr sender_hwaddr;
        l3addr sender_paddr;
        l2addr target_hwaddr;
        l3addr target_paddr;

        static arp_hdr read(const char* p) {
            arp_hdr ah;
            ah.htype = consume_be<uint16_t>(p);
            ah.ptype = consume_be<uint16_t>(p);
            ah.hlen = consume_be<uint8_t>(p);
            ah.plen = consume_be<uint8_t>(p);
            ah.oper = consume_be<uint16_t>(p);
            ah.sender_hwaddr = l2addr::consume(p);
            ah.sender_paddr = l3addr::consume(p);
            ah.target_hwaddr = l2addr::consume(p);
            ah.target_paddr = l3addr::consume(p);
            return ah;
        }
        void write(char* p) const {
            produce_be<uint16_t>(p, htype);
            produce_be<uint16_t>(p, ptype);
            produce_be<uint8_t>(p, hlen);
            produce_be<uint8_t>(p, plen);
            produce_be<uint16_t>(p, oper);
            sender_hwaddr.produce(p);
            sender_paddr.produce(p);
            target_hwaddr.produce(p);
            target_paddr.produce(p);
        }
        static constexpr size_t size() {
            return 8 + 2 * (l2addr::size() + l3addr::size());
        }
    };
    struct resolution {
        std::vector<promise<l2addr>> _waiters;
        timer<> _timeout_timer;
    };
private:
    l3addr _l3self = L3::broadcast_address();
    std::unordered_map<l3addr, l2addr> _table;
    std::unordered_map<l3addr, resolution> _in_progress;
private:
    packet make_query_packet(l3addr paddr);
    virtual future<> received(packet p) override;
    future<> handle_request(arp_hdr* ah);
    l2addr l2self() { return _arp.l2self(); }
    void send(l2addr to, packet p);
public:
    future<> send_query(const l3addr& paddr);
    explicit arp_for(arp& a) : arp_for_protocol(a, L3::arp_protocol_type()) {
        _table[L3::broadcast_address()] = ethernet::broadcast_address();
    }
    future<ethernet_address> lookup(const l3addr& addr);
    void learn(l2addr l2, l3addr l3);
    void run();
    void set_self_addr(l3addr addr) {
        _table.erase(_l3self);
        _table[addr] = l2self();
        _l3self = addr;

        // patch by djp
        // A hack to enable port 0 on r1 to connect to port 1 on r3.
        // First, a summary about the mac address of the port on each server.
        // r1: p0 3c:fd:fe:06:08:00, p1 3c:fd:fe:06:08:02
        // r2: p0 3c:fd:fe:06:09:60, p1 3c:fd:fe:06:09:62
        // r3: p0 3c:fd:fe:06:07:80, p1 3c:fd:fe:06:07:82
        // The planned testers for TCP-based, bidirection connection.
        // r1: Sender, bound to p0, the IP address of p0 is 10.10.0.1
        // r2: Middlebox, bound to p0 and p1.
        // r3: Receiver, bound to p1, the IP address of p1 is 10.10.0.3

        // Here, we perform two hacks.
        // First, on the sender, 10.10.0.3 is statically written to the
        // arp table, with hardware address 3c:fd:fe:06:09:60.

        // Then, all the connections generated by the sender will be received
        // by p0 on r2. r2 will perform mac address translation and send
        // the packet out from p1.

        // Finally, on the receiver, 10.10.0.1 is statically written to the
        // arp table, with mac address 3c:fd:fe:06:09:62.

        // The middlebox should perform the following mac address translation.

        // For IP/TCP packets received from port 0, it should replace the
        // source mac to 3c:fd:fe:06:09:62 and destination mac to 3c:fd:fe:06:07:82

        // For IP/TCP packets received from port 1, it should replace the
        // source mac to 3c:fd:fe:06:09:60 and destination mac to 3c:fd:fe:06:08:00

        if(addr == l3addr("10.10.0.1")) {
            auto receiver_ip = l3addr("10.10.0.3");
            auto receiver_mac = l2addr{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x60};
            _table[receiver_ip] = receiver_mac;
        }

        if(addr == l3addr("10.10.0.3")) {
            auto sender_ip = l3addr("10.10.0.1");
            auto sender_mac = l2addr{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x62};
            _table[sender_ip] = sender_mac;
        }
    }
    friend class arp;
    // patch by djp
    // Add a way to hijack ARP
private:
    std::vector<arp_for<L3>*> _other_arp_fors;
public:
    void set_other_arp_fors(std::vector<arp_for<L3>*> other_arp_fors){
        fprint(std::cout, "Thread %d: arp_for instance setting other_arp_fors with size %zu\n",
                engine().cpu_id(), other_arp_fors.size());
        _other_arp_fors = std::move(other_arp_fors);
    }
};

template <typename L3>
packet
arp_for<L3>::make_query_packet(l3addr paddr) {
    arp_hdr hdr;
    hdr.htype = ethernet::arp_hardware_type();
    hdr.ptype = L3::arp_protocol_type();
    hdr.hlen = sizeof(l2addr);
    hdr.plen = sizeof(l3addr);
    hdr.oper = op_request;
    hdr.sender_hwaddr = l2self();
    hdr.sender_paddr = _l3self;
    hdr.target_hwaddr = ethernet::broadcast_address();
    hdr.target_paddr = paddr;
    auto p = packet();
    p.prepend_uninitialized_header(hdr.size());
    hdr.write(p.get_header(0, hdr.size()));
    return p;
}

template <typename L3>
void arp_for<L3>::send(l2addr to, packet p) {
    _arp._packetq.push_back(l3_protocol::l3packet{eth_protocol_num::arp, to, std::move(p)});
}

template <typename L3>
future<>
arp_for<L3>::send_query(const l3addr& paddr) {
    send(ethernet::broadcast_address(), make_query_packet(paddr));
    return make_ready_future<>();
}

class arp_error : public std::runtime_error {
public:
    arp_error(const std::string& msg) : std::runtime_error(msg) {}
};

class arp_timeout_error : public arp_error {
public:
    arp_timeout_error() : arp_error("ARP timeout") {}
};

class arp_queue_full_error : public arp_error {
public:
    arp_queue_full_error() : arp_error("ARP waiter's queue is full") {}
};

template <typename L3>
future<ethernet_address>
arp_for<L3>::lookup(const l3addr& paddr) {
    auto i = _table.find(paddr);
    if (i != _table.end()) {
        return make_ready_future<ethernet_address>(i->second);
    }
    auto j = _in_progress.find(paddr);
    auto first_request = j == _in_progress.end();
    auto& res = first_request ? _in_progress[paddr] : j->second;

    if (first_request) {
        res._timeout_timer.set_callback([paddr, this, &res] {
            send_query(paddr);
            for (auto& w : res._waiters) {
                w.set_exception(arp_timeout_error());
            }
            res._waiters.clear();
        });
        res._timeout_timer.arm_periodic(std::chrono::seconds(1));
        send_query(paddr);
    }

    if (res._waiters.size() >= max_waiters) {
        return make_exception_future<ethernet_address>(arp_queue_full_error());
    }

    res._waiters.emplace_back();
    return res._waiters.back().get_future();
}

template <typename L3>
void
arp_for<L3>::learn(l2addr hwaddr, l3addr paddr) {
    _table[paddr] = hwaddr;
    auto i = _in_progress.find(paddr);
    if (i != _in_progress.end()) {
        auto& res = i->second;
        res._timeout_timer.cancel();
        for (auto &&pr : res._waiters) {
            pr.set_value(hwaddr);
        }
        _in_progress.erase(i);
    }
}

template <typename L3>
future<>
arp_for<L3>::received(packet p) {
    auto ah = p.get_header(0, arp_hdr::size());
    if (!ah) {
        return make_ready_future<>();
    }
    auto h = arp_hdr::read(ah);
    if (h.hlen != sizeof(l2addr) || h.plen != sizeof(l3addr)) {
        return make_ready_future<>();
    }
    switch (h.oper) {
    case op_request:
        return handle_request(&h);
    case op_reply:
        // arp_learn(h.sender_hwaddr, h.sender_paddr);
        // patch by djp
        // Add a way to hijack ARP.
        if(_other_arp_fors.size()==0){
            arp_learn(h.sender_hwaddr, h.sender_paddr);
        }
        else{
            if(_other_arp_fors.size()==smp::count){
                for(unsigned i=0; i<smp::count; i++){
                    smp::submit_to(i,
                            [other_arp=_other_arp_fors.at(i), l2=h.sender_hwaddr, l3=h.sender_paddr]{
                        other_arp->learn(l2, l3);
                    });
                }
            }
        }
        return make_ready_future<>();
    default:
        return make_ready_future<>();
    }
}

template <typename L3>
future<>
arp_for<L3>::handle_request(arp_hdr* ah) {
    if (ah->target_paddr == _l3self
            && _l3self != L3::broadcast_address()) {
        ah->oper = op_reply;
        ah->target_hwaddr = ah->sender_hwaddr;
        ah->target_paddr = ah->sender_paddr;
        ah->sender_hwaddr = l2self();
        ah->sender_paddr = _l3self;
        auto p = packet();
        ah->write(p.prepend_uninitialized_header(ah->size()));
        send(ah->target_hwaddr, std::move(p));
    }
    return make_ready_future<>();
}

}

}

#endif /* ARP_HH_ */
