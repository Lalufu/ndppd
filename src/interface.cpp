// ndppd - NDP Proxy Daemon
// Copyright (C) 2011-2018  Daniel Adolfsson <daniel@priv.nu>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <netinet/ether.h>
#include <netpacket/packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <linux/filter.h>
#include <errno.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#include "ndppd.h"
#include "interface.h"
#include "proxy.h"
#include "rule.h"
#include "address.h"
#include "netlink.h"

NDPPD_NS_BEGIN

namespace {
std::list<std::weak_ptr<Interface>> interfaces;
}

std::shared_ptr<Interface> Interface::get_or_create(unsigned int index)
{
    char name[IF_NAMESIZE];

    if (if_indextoname(index, name) == nullptr)
        throw std::system_error(errno, std::generic_category(), "Interface::get_or_create(index)");

    return std::move(get_or_create(index, name));
}

std::shared_ptr<Interface> Interface::get_or_create(const std::string& name)
{
    auto index = if_nametoindex(name.c_str());

    if (index == 0)
        throw std::system_error(errno, std::generic_category(), "Interface::get_or_create(name)");

    return std::move(get_or_create(index, name));
}

std::shared_ptr<Interface> Interface::get_or_create(unsigned int index, const std::string& name)
{
    auto it = std::find_if(interfaces.cbegin(), interfaces.cend(),
            [&](const std::weak_ptr<Interface>& ptr) {
                auto iface = ptr.lock();
                return iface != nullptr && (iface->_index == index || iface->_name == name);
            });

    if (it != interfaces.cend())
        // TODO: Make sure both index and name matches!
        return it->lock();

    auto iface = std::make_shared<Interface>(index, name);
    interfaces.push_back(iface);
    return std::move(iface);
}

Interface::Interface(unsigned int index, const std::string& name)
        : _index(index), _name(name), _icmp6_socket {}, _packet_socket {}
{
    Logger::debug() << "Interface::Interface()";
}

Interface::~Interface()
{
    Logger::debug() << "Interface::~Interface()";

    if (_packet_socket) {
        if (_prev_allmulti >= 0)
            allmulti(_prev_allmulti != 0);
        if (_prev_promisc >= 0)
            promisc(_prev_promisc != 0);
    }
}

void Interface::ensure_packet_socket(bool promisc)
{
    if (_packet_socket)
        return;

    ensure_icmp6_socket();

    auto socket = std::make_unique<Socket>(PF_PACKET, SOCK_RAW, htons(ETH_P_IPV6));
    socket->handler = std::bind(&Interface::packet_handler, this, std::placeholders::_1);
    socket->bind((sockaddr_ll) { AF_PACKET, htons(ETH_P_IPV6), _index });

    // Set up filter.

    static struct sock_filter filter[] = {
            // Load the ether_type.
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, offsetof(ether_header, ether_type)),
            // Bail if it's* not* ETHERTYPE_IPV6.
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETHERTYPE_IPV6, 0, 5),
            // Load the next header type.
            BPF_STMT(BPF_LD | BPF_B | BPF_ABS, sizeof(ether_header) + offsetof(ip6_hdr, ip6_nxt)),
            // Bail if it's* not* IPPROTO_ICMPV6.
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_ICMPV6, 0, 3),
            // Load the ICMPv6 type.
            BPF_STMT(BPF_LD | BPF_B | BPF_ABS, sizeof(ether_header) + sizeof(ip6_hdr) +
                                               offsetof(icmp6_hdr, icmp6_type)),
            // Bail if it's* not* ND_NEIGHBOR_SOLICIT.
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ND_NEIGHBOR_SOLICIT, 0, 1),
            // Keep packet.
            BPF_STMT(BPF_RET | BPF_K, (u_int32_t) -1),
            // Drop packet.
            BPF_STMT(BPF_RET | BPF_K, 0)
    };

    static struct sock_fprog fprog = {
            8,
            filter
    };

    socket->setsockopt(SOL_SOCKET, SO_ATTACH_FILTER, fprog);

    _packet_socket = std::move(socket);
    _prev_allmulti = allmulti();
    _prev_promisc = promisc ? this->promisc() : -1;
}

void Interface::ensure_icmp6_socket()
{
    if (_icmp6_socket)
        return;

    auto socket = std::make_unique<Socket>(PF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    socket->handler = std::bind(&Interface::icmp6_handler, this, std::placeholders::_1);

    // Bind to the specified interface.

    ifreq ifr {};
    strncpy(ifr.ifr_name, _name.c_str(), IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    socket->setsockopt(SOL_SOCKET, SO_BINDTODEVICE, ifr);

    // Detect the link-layer address.

    ifr = {};
    strncpy(ifr.ifr_name, _name.c_str(), IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    socket->ioctl(SIOCGIFHWADDR, &ifr);

    Logger::debug() << "hwaddr=" << ether_ntoa(reinterpret_cast<ether_addr*>(&ifr.ifr_hwaddr.sa_data));

    // Set max hops.

    socket->setsockopt(IPPROTO_IPV6, IPV6_MULTICAST_HOPS, 255);
    socket->setsockopt(IPPROTO_IPV6, IPV6_UNICAST_HOPS, 255);

    // Set up filter.

    icmp6_filter filter {};
    ICMP6_FILTER_SETBLOCKALL(&filter);
    ICMP6_FILTER_SETPASS(ND_NEIGHBOR_ADVERT, &filter);

    socket->setsockopt(IPPROTO_ICMPV6, ICMP6_FILTER, filter);

    hwaddr = *reinterpret_cast<ether_addr*>(ifr.ifr_hwaddr.sa_data);
    _icmp6_socket = std::move(socket);
}

ssize_t Interface::read_solicit(Address& saddr, Address& daddr, Address& taddr)
{
    sockaddr_ll t_saddr {};
    uint8_t msg[256];
    ssize_t len;

    if ((len = _packet_socket->recvmsg(t_saddr, msg, sizeof(msg))) < 0) {
        if (errno != EAGAIN)
            Logger::warning() << "Interface::read_solicit() failed: " << Logger::err();
        return -1;
    }

    auto& ip6h = *reinterpret_cast<ip6_hdr*>(msg + ETH_HLEN);
    auto& ns = *reinterpret_cast<nd_neighbor_solicit*>(msg + ETH_HLEN + sizeof(ip6_hdr));

    taddr = Address(ns.nd_ns_target);
    daddr = Address(ip6h.ip6_dst);
    saddr = Address(ip6h.ip6_src);

    // Ignore packets sent from this machine
    if (Interface::is_local(saddr))
        return 0;

    Logger::debug() << "Interface::read_solicit() saddr=" << saddr.to_string()
                    << ", daddr=" << daddr.to_string() << ", taddr=" << taddr.to_string() << ", len=" << len;

    return len;
}

ssize_t Interface::write_solicit(const Address& taddr)
{
    char buf[128] = {};

    auto& ns = *reinterpret_cast<nd_neighbor_solicit*>(buf);
    ns.nd_ns_type = ND_NEIGHBOR_SOLICIT;
    ns.nd_ns_target = taddr.c_addr();

    auto& opt = *reinterpret_cast<nd_opt_hdr*>(buf + sizeof(nd_neighbor_solicit));
    opt.nd_opt_type = ND_OPT_SOURCE_LINKADDR;
    opt.nd_opt_len = 1;

    memcpy(buf + sizeof(nd_neighbor_solicit) + sizeof(nd_opt_hdr), &hwaddr, 6);

    // FIXME: Alright, I'm lazy.
    static Address multicast("ff02::1:ff00:0000");

    Address daddr;

    daddr = multicast;

    daddr.addr().s6_addr[13] = taddr.c_addr().s6_addr[13];
    daddr.addr().s6_addr[14] = taddr.c_addr().s6_addr[14];
    daddr.addr().s6_addr[15] = taddr.c_addr().s6_addr[15];

    Logger::debug() << "iface::write_solicit() taddr=" << taddr.to_string() << ", daddr=" << daddr.to_string();

    return _icmp6_socket->sendmsg(daddr, buf, sizeof(nd_neighbor_solicit) + sizeof(nd_opt_hdr) + 6);
}

ssize_t Interface::write_advert(const Address& daddr, const Address& taddr, bool router)
{
    char buf[128] {};

    auto& na = *(nd_neighbor_advert*) &buf[0];
    na.nd_na_type = ND_NEIGHBOR_ADVERT;
    na.nd_na_flags_reserved = static_cast<uint32_t>((daddr.is_multicast() ? 0 : ND_NA_FLAG_SOLICITED) |
                                                    (router ? ND_NA_FLAG_ROUTER : 0));
    na.nd_na_target = taddr.c_addr();

    auto& opt = *(nd_opt_hdr*) &buf[sizeof(nd_neighbor_advert)];
    opt.nd_opt_type = ND_OPT_TARGET_LINKADDR;
    opt.nd_opt_len = 1;

    *(ether_addr*) (buf + sizeof(nd_neighbor_advert) + sizeof(nd_opt_hdr)) = hwaddr;

    Logger::debug() << "iface::write_advert() daddr=" << daddr.to_string()
                    << ", taddr=" << taddr.to_string();

    return _icmp6_socket->sendmsg(daddr, buf, sizeof(nd_neighbor_advert) + sizeof(nd_opt_hdr) + 6);
}

ssize_t Interface::read_advert(Address& saddr, Address& taddr)
{
    sockaddr_in6 t_saddr {};
    uint8_t msg[256];
    ssize_t len;

    t_saddr.sin6_family = AF_INET6;
    t_saddr.sin6_port = htons(IPPROTO_ICMPV6); // Needed?

    if ((len = _icmp6_socket->recvmsg(t_saddr, msg, sizeof(msg))) < 0)
        return -1;

    saddr = Address(t_saddr.sin6_addr);

    // Ignore packets sent from this machine
    if (Interface::is_local(saddr)) {
        return 0;
    }

    if (((icmp6_hdr*) msg)->icmp6_type != ND_NEIGHBOR_ADVERT)
        return -1;

    taddr = Address(((nd_neighbor_solicit*) msg)->nd_ns_target);

    Logger::debug() << "iface::read_advert() saddr=" << saddr.to_string() << ", taddr=" << taddr.to_string()
                    << ", len=" << len;

    return len;
}

bool Interface::is_local(const Address& addr)
{
    return Netlink::is_local(addr);
}

bool Interface::handle_local(const Address& source, const Address& target)
{
    for (const auto& nla : Netlink::local_addresses()) {
        if (nla.address() != target)
            continue;

        for (const auto& proxy : Proxy::proxies()) {
            if (proxy->iface().get() != this)
                continue;

            for (const auto& rule : proxy->rules()) {
                if (rule->iface()->_index == nla.index()) {
                    Logger::debug() << "Interface::handle_local() found local taddr=" << target;
                    write_advert(source, target, false);
                    return true;
                }
            }
        }
    }

    return false;
}

void Interface::handle_reverse_advert(const Address& saddr, const std::string& ifname)
{
    if (!saddr.is_unicast())
        return;

    Logger::debug() << "Interface::handle_reverse_advert()";

    // Loop through all the parents that forward new NDP soliciation requests to this interface
    for (Proxy& proxy : _parents) {
        if (proxy.iface())
            continue;

        // Setup the reverse path on any proxies that are dealing
        // with the reverse direction (this helps improve connectivity and
        // latency in a full duplex setup)
        for (auto& rule : proxy.rules()) {
            if (rule->cidr() % saddr && rule->iface()->name() == ifname) {
                Logger::debug() << " - generating artifical advertisement: " << ifname;
                proxy.handle_stateless_advert(saddr, saddr, ifname, rule->autovia);
            }
        }
    }
}

void Interface::packet_handler(Socket&)
{
    for (;;) {
        Address saddr, daddr, taddr;

        if (read_solicit(saddr, daddr, taddr) < 0)
            break;

        // Process any local addresses for interfaces that we are proxying
        if (handle_local(saddr, taddr))
            continue;

        // We have to handle all the parents who may be interested in
        // the reverse path towards the one who sent this solicit.
        // In fact, the parent need to know the source address in order
        // to respond to NDP Solicitations
        handle_reverse_advert(saddr, _name);

        // Loop through all the proxies that are using this iface to respond to NDP solicitation requests
        auto handled = false;

        for (Proxy& proxy : _serves) {
            // Process the solicitation request by relating it to other
            // interfaces or lookup up any statics routes we have configured
            handled = true;
            proxy.handle_solicit(saddr, taddr, _name);
        }

        // If it was not handled then write an error message
        if (!handled)
            Logger::debug() << " - solicit was ignored";
    }
}

void Interface::icmp6_handler(Socket&)
{
    for (;;) {
        Address saddr, taddr;

        if (read_advert(saddr, taddr) < 0)
            break;

        bool handled = false;

        for (Proxy& proxy : _parents) {
            if (!proxy.iface())
                return;

            // The proxy must have a rule for this interface or it is not meant to receive
            // any notifications and thus they must be ignored
            bool autovia = false;
            bool is_relevant = false;

            for (const auto& rule : proxy.rules()) {
                if (rule->cidr() % taddr && rule->iface() && rule->iface()->name() == _name) {
                    is_relevant = true;
                    autovia = rule->autovia;
                    break;
                }
            }

            if (!is_relevant) {
                Logger::debug() << "Interface::read_advert() advert is not for " << _name << "...skipping";
                continue;
            }

            // Process the NDP advertisement
            handled = true;
            proxy.handle_advert(saddr, taddr, _name, autovia);
        }

        // If it was not handled then write an error message
        if (!handled)
            Logger::debug() << " - advert was ignored";
    }
}

int Interface::allmulti(bool state)
{
    Logger::debug() << "Interface::allmulti() state=" << state << ", _name=\"" << _name << "\"";

    ifreq ifr {};
    strncpy(ifr.ifr_name, _name.c_str(), IFNAMSIZ);

    _packet_socket->ioctl(SIOCGIFFLAGS, &ifr);

    auto old_state = (ifr.ifr_flags & IFF_ALLMULTI) != 0;

    if (state == old_state)
        return old_state;

    if (state)
        ifr.ifr_flags |= IFF_ALLMULTI;
    else
        ifr.ifr_flags &= ~IFF_ALLMULTI;

    _packet_socket->ioctl(SIOCSIFFLAGS, &ifr);

    return old_state;
}

int Interface::promisc(bool state)
{
    Logger::debug() << "Interface::promisc() state=" << state << ", _name=\"" << _name << "\"";

    ifreq ifr {};
    strncpy(ifr.ifr_name, _name.c_str(), IFNAMSIZ);

    _packet_socket->ioctl(SIOCGIFFLAGS, &ifr);

    int old_state = (ifr.ifr_flags & IFF_PROMISC) != 0;

    if (state == old_state)
        return old_state;

    if (state)
        ifr.ifr_flags |= IFF_PROMISC;
    else
        ifr.ifr_flags &= ~IFF_PROMISC;

    _packet_socket->ioctl(SIOCSIFFLAGS, &ifr);

    return old_state;
}

const std::string& Interface::name() const
{
    return _name;
}

void Interface::add_serves(Proxy& proxy)
{
    _serves.push_back(std::ref(proxy));
}

void Interface::add_parent(Proxy& proxy)
{
    _parents.push_back(std::ref(proxy));
}

const Range<std::list<std::reference_wrapper<Proxy>>::const_iterator> Interface::parents() const
{
    return { _parents.cbegin(), _parents.cend() };
}

const Range<std::list<std::reference_wrapper<Proxy>>::const_iterator> Interface::serves() const
{
    return { _serves.cbegin(), _serves.cend() };
}

NDPPD_NS_END