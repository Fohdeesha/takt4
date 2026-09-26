#include "core/output/link_peers.hpp"

#include "core/net/udp.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#elif defined(__linux__)
#include <ifaddrs.h>
#include <net/if.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#endif

namespace takt4::output {
namespace {

/// Where Link announces itself — `ableton::discovery::multicastEndpointV4`.
constexpr const char* kLinkGroup = "224.76.78.75";
constexpr std::uint16_t kLinkPort = 20808;

constexpr std::array<std::uint8_t, 8> kProtocolHeader{'_', 'a', 's', 'd', 'p', '_', 'v', 1};
/// The protocol header, then type, ttl, group and the node id.
constexpr std::size_t kHeaderSize = kProtocolHeader.size() + 1 + 1 + 2 + 8;

constexpr std::uint32_t kTimeline = 0x746d6c6e;  // 'tmln'
constexpr std::uint32_t kSession = 0x73657373;   // 'sess'
constexpr std::uint32_t kStartStop = 0x73747374; // 'stst'

/// Link's own limit on a discovery message (`v1::kMaxMessageSize`), and so all that is read.
constexpr std::size_t kMaxMessage = 512;
/// How many datagrams one `poll` reads at most, so a flood cannot hold the UI thread.
constexpr int kMaxPerPoll = 256;

std::uint32_t read32(const std::uint8_t* at) noexcept {
    return (std::uint32_t{at[0]} << 24) | (std::uint32_t{at[1]} << 16) |
           (std::uint32_t{at[2]} << 8) | std::uint32_t{at[3]};
}

std::int64_t read64(const std::uint8_t* at) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | at[i];
    }
    return static_cast<std::int64_t>(value);
}

} // namespace

bool parseLinkAnnouncement(std::span<const std::uint8_t> datagram, LinkAnnouncement& out) noexcept {
    if (datagram.size() < kHeaderSize || datagram.size() > kMaxMessage ||
        !std::equal(kProtocolHeader.begin(), kProtocolHeader.end(), datagram.begin())) {
        return false;
    }
    LinkAnnouncement parsed;
    const std::uint8_t* at = datagram.data() + kProtocolHeader.size();
    const std::uint8_t type = at[0];
    if (type < 1 || type > 3) {
        return false;
    }
    parsed.type = static_cast<LinkAnnouncement::Type>(type);
    parsed.ttl = at[1];
    // at[2], at[3]: the group, which Link always sends as 0 and no one reads.
    std::copy(at + 4, at + 12, parsed.node.begin());

    std::size_t offset = kHeaderSize;
    while (offset < datagram.size()) {
        if (datagram.size() - offset < 8) {
            return false; // half an entry header
        }
        const std::uint32_t key = read32(datagram.data() + offset);
        const std::uint32_t size = read32(datagram.data() + offset + 4);
        offset += 8;
        if (size > datagram.size() - offset) {
            return false; // an entry longer than what is left
        }
        const std::uint8_t* value = datagram.data() + offset;
        // Each entry read only at the size Link writes it; one that is not is left unread rather
        // than half-read, as Link's own parser would refuse it.
        if (key == kTimeline && size == 24) {
            const std::int64_t microsPerBeat = read64(value);
            if (microsPerBeat > 0) {
                parsed.bpm = 60.0e6 / static_cast<double>(microsPerBeat);
            }
        } else if (key == kSession && size == 8) {
            LinkAnnouncement::Id session{};
            std::copy(value, value + 8, session.begin());
            parsed.session = session;
        } else if (key == kStartStop && size == 17) {
            // Playing, the beat, and when: a state nobody ever stamped is one nobody shares.
            if (read64(value + 9) != 0) {
                parsed.playing = value[0] != 0;
            }
        }
        offset += size;
    }
    out = std::move(parsed);
    return true;
}

// --- the socket --------------------------------------------------------------------------------

struct LinkPeerWatch::Socket {
    net::WinsockGuard winsock;
    net::Socket handle = net::kInvalidSocket;

    ~Socket() {
        if (handle != net::kInvalidSocket) {
            net::closeSocket(handle);
        }
    }
};

namespace {

/// Every IPv4 address of an interface that is up — where Link announces itself from, and so where
/// the group has to be joined for its announcements to arrive.
std::vector<in_addr> interfaceAddresses() {
    std::vector<in_addr> addresses;
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = ::GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) {
        return addresses;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            const sockaddr* address = unicast->Address.lpSockaddr;
            if (address != nullptr && address->sa_family == AF_INET) {
                addresses.push_back(reinterpret_cast<const sockaddr_in*>(address)->sin_addr);
            }
        }
    }
#elif defined(__linux__)
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return addresses;
    }
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr != nullptr && entry->ifa_addr->sa_family == AF_INET &&
            (entry->ifa_flags & IFF_UP) != 0) {
            addresses.push_back(reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr);
        }
    }
    ::freeifaddrs(list);
#endif
    return addresses;
}

bool joinGroup(net::Socket socket, in_addr where) noexcept {
    ip_mreq request{};
    ::inet_pton(AF_INET, kLinkGroup, &request.imr_multiaddr);
    request.imr_interface = where;
    return ::setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                        reinterpret_cast<const char*>(&request), sizeof request) == 0;
}

} // namespace

LinkPeerWatch::LinkPeerWatch(OwnPort ownPort) : ownPort_(std::move(ownPort)) {
    if (!ownPort_) {
        ownPort_ = sentFromHere;
    }
}

LinkPeerWatch::~LinkPeerWatch() = default;

bool LinkPeerWatch::isOpen() const noexcept {
    return socket_ != nullptr;
}

bool LinkPeerWatch::open(std::string& problem) {
    if (socket_ != nullptr) {
        return true;
    }
    if (!sandbox::allowsBind(kLinkPort)) {
        // The test binaries' sandbox: Link's own port on the rig's network. See `sandbox.hpp`.
        sandbox::refuse(sandbox::Refused::Link, kLinkPort);
        problem = "not listening on Link's port in the test sandbox";
        return false;
    }
    auto socket = std::make_unique<Socket>();
    socket->handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket->handle == net::kInvalidSocket) {
        problem = "cannot open a socket (" + std::to_string(net::lastSocketError()) + ")";
        return false;
    }
    // Shared with Link's own socket on the same port, which asks for the same.
    const int reuse = 1;
    ::setsockopt(socket->handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof reuse);
#if defined(_WIN32)
    u_long nonBlocking = 1;
    ::ioctlsocket(socket->handle, FIONBIO, &nonBlocking);
#else
    const int flags = ::fcntl(socket->handle, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(socket->handle, F_SETFL, flags | O_NONBLOCK);
    }
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kLinkPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket->handle, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        problem = "cannot listen on Link's port " + std::to_string(kLinkPort) + " (" +
                  std::to_string(net::lastSocketError()) + ")";
        return false;
    }
    // On every interface, as Link announces on every interface; and on the default one when
    // there is no list to be had.
    int joined = 0;
    for (const in_addr where : interfaceAddresses()) {
        joined += joinGroup(socket->handle, where) ? 1 : 0;
    }
    if (joined == 0) {
        in_addr any{};
        any.s_addr = htonl(INADDR_ANY);
        if (!joinGroup(socket->handle, any)) {
            problem = "cannot join Link's multicast group (" +
                      std::to_string(net::lastSocketError()) + ")";
            return false;
        }
    }
    socket_ = std::move(socket);
    return true;
}

void LinkPeerWatch::close() noexcept {
    socket_.reset();
    nodes_.clear();
}

void LinkPeerWatch::poll(double now) {
    if (socket_ != nullptr) {
        std::array<std::uint8_t, kMaxMessage + 1> buffer{};
        for (int i = 0; i < kMaxPerPoll; ++i) {
            sockaddr_in from{};
            socklen_t fromLength = sizeof from;
#if defined(_WIN32)
            const int capacity = static_cast<int>(buffer.size());
#else
            const std::size_t capacity = buffer.size();
#endif
            const auto received =
                ::recvfrom(socket_->handle, reinterpret_cast<char*>(buffer.data()), capacity, 0,
                           reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (received <= 0) {
                break; // nothing more waiting, or an error that the next poll meets again
            }
            char text[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &from.sin_addr, text, sizeof text);
            take(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received)),
                 text, ntohs(from.sin_port), now);
        }
    }
    std::erase_if(nodes_, [now](const Node& node) { return node.expires <= now; });
}

void LinkPeerWatch::take(std::span<const std::uint8_t> datagram, const std::string& address,
                         std::uint16_t port, double now) {
    LinkAnnouncement announcement;
    if (!parseLinkAnnouncement(datagram, announcement)) {
        return;
    }
    const auto found = std::find_if(nodes_.begin(), nodes_.end(),
                                    [&](const Node& node) { return node.id == announcement.node; });
    if (announcement.type == LinkAnnouncement::Type::ByeBye) {
        if (found != nodes_.end()) {
            nodes_.erase(found);
        }
        return;
    }
    Node* node = nullptr;
    if (found != nodes_.end()) {
        node = &*found;
    } else {
        Node made;
        made.id = announcement.node;
        made.own = ownPort_ && ownPort_(address, port);
        nodes_.push_back(std::move(made));
        node = &nodes_.back();
    }
    if (std::find(node->addresses.begin(), node->addresses.end(), address) ==
        node->addresses.end()) {
        node->addresses.push_back(address);
    }
    if (announcement.bpm) {
        node->bpm = *announcement.bpm;
    }
    if (announcement.session) {
        node->session = announcement.session;
    }
    node->playing = announcement.playing;
    node->expires = now + static_cast<double>(announcement.ttl);
}

std::optional<LinkAnnouncement::Id> LinkPeerWatch::ownSession() const {
    for (const Node& node : nodes_) {
        if (node.own && node.session) {
            return node.session;
        }
    }
    return std::nullopt;
}

std::vector<LinkPeer> LinkPeerWatch::peers() const {
    const std::optional<LinkAnnouncement::Id> own = ownSession();
    std::vector<LinkPeer> peers;
    for (const Node& node : nodes_) {
        if (node.own) {
            continue;
        }
        LinkPeer peer;
        peer.node = node.id;
        peer.addresses = node.addresses;
        peer.bpm = node.bpm;
        peer.sameSession = own && node.session && *own == *node.session;
        peer.playing = node.playing;
        peers.push_back(std::move(peer));
    }
    return peers;
}

// --- whose port --------------------------------------------------------------------------------

bool addressIsHere(const std::string& address) {
    in_addr parsed{};
    if (::inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
        return false;
    }
    if ((ntohl(parsed.s_addr) >> 24) == 127) {
        return true; // loopback, which no interface list need carry
    }
    const std::vector<in_addr> here = interfaceAddresses();
    return std::any_of(here.begin(), here.end(),
                       [parsed](const in_addr& one) { return one.s_addr == parsed.s_addr; });
}

bool sentFromHere(const std::string& address, std::uint16_t port) {
    return addressIsHere(address) && portOwnedHere(port);
}

bool portOwnedHere(std::uint16_t port) {
#if defined(_WIN32)
    // The table of UDP sockets with the process that owns each. It can grow between asking its
    // size and reading it, hence the retries.
    std::vector<DWORD> buffer;
    ULONG size = 0;
    DWORD result = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 4 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        buffer.resize(size / sizeof(DWORD) + 64);
        size = static_cast<ULONG>(buffer.size() * sizeof(DWORD));
        result =
            ::GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    }
    if (result != NO_ERROR) {
        return false;
    }
    const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
    const DWORD self = ::GetCurrentProcessId();
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_UDPROW_OWNER_PID& row = table->table[i];
        if (row.dwOwningPid == self && ntohs(static_cast<u_short>(row.dwLocalPort)) == port) {
            return true;
        }
    }
    return false;
#elif defined(__linux__)
    // The inode of every socket this process holds, from its descriptors; then whether one of
    // them is a UDP socket bound to `port`, from the kernel's table of them.
    std::set<unsigned long> inodes;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", error)) {
        const std::string target = std::filesystem::read_symlink(entry.path(), error).string();
        if (!error && target.rfind("socket:[", 0) == 0 && target.back() == ']') {
            inodes.insert(std::stoul(target.substr(8, target.size() - 9)));
        }
    }
    for (const char* table : {"/proc/net/udp", "/proc/net/udp6"}) {
        std::ifstream in(table);
        std::string line;
        std::getline(in, line); // the column headings
        while (std::getline(in, line)) {
            // sl local_address rem_address st tx_queue:rx_queue tr:tm->when retrnsmt uid timeout
            // inode
            std::istringstream fields(line);
            std::string slot, local, remote, state, queues, timer, retransmits, uid, timeout;
            unsigned long inode = 0;
            if (!(fields >> slot >> local >> remote >> state >> queues >> timer >> retransmits >>
                  uid >> timeout >> inode)) {
                continue;
            }
            const std::size_t colon = local.rfind(':');
            if (colon == std::string::npos || inodes.count(inode) == 0) {
                continue;
            }
            if (std::stoul(local.substr(colon + 1), nullptr, 16) == port) {
                return true;
            }
        }
    }
    return false;
#else
    (void)port;
    return false;
#endif
}

} // namespace takt4::output
