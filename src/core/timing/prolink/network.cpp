#include "core/timing/prolink/network.hpp"

#include "core/net/udp.hpp"
#include "core/sandbox.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/select.h>
#if defined(__APPLE__)
#include <net/if_dl.h>
#elif defined(__linux__)
#include <netpacket/packet.h>
#endif
#endif

namespace takt4::timing::prolink {

namespace {

/// Datagrams one socket is read for in a round, so a flood cannot hold the thread.
constexpr int kMaxReadsPerRound = 256;
/// How often the window's snapshot is rebuilt.
constexpr std::int64_t kPublishMicros = 50'000;
/// How long after a join that could not be made — nothing heard yet, a port taken — it is tried
/// again. Each try lists the machine's cards, which is not free.
constexpr std::int64_t kRetryJoinMicros = 250'000;
/// A device that sent a beat this recently is beating, for the screen.
constexpr std::int64_t kBeatingMicros = 2'000'000;

#if defined(_WIN32)
/// Windows gives a card's network as a prefix length; the others as a mask.
std::uint32_t maskOfPrefix(unsigned prefix) noexcept {
    return prefix == 0 ? 0u : prefix >= 32 ? 0xffffffffu : ~((1u << (32 - prefix)) - 1u);
}

std::string narrow(const wchar_t* wide) {
    if (wide == nullptr || *wide == L'\0') {
        return {};
    }
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string text(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), length, nullptr, nullptr);
    text.resize(static_cast<std::size_t>(length - 1));
    return text;
}
#endif

bool lastErrorIsReset() noexcept {
    const int error = net::lastSocketError();
#if defined(_WIN32)
    // Windows reports an earlier send's "port unreachable" on the next read of the socket.
    return error == WSAECONNRESET;
#else
    return error == ECONNREFUSED;
#endif
}

} // namespace

std::string addressText(std::uint32_t address) {
    return std::to_string((address >> 24) & 0xff) + "." + std::to_string((address >> 16) & 0xff) +
           "." + std::to_string((address >> 8) & 0xff) + "." + std::to_string(address & 0xff);
}

std::optional<std::uint32_t> parseAddress(const std::string& text) {
    in_addr parsed{};
    if (text.empty() || ::inet_pton(AF_INET, text.c_str(), &parsed) != 1) {
        return std::nullopt;
    }
    return ntohl(parsed.s_addr);
}

std::vector<Interface> listInterfaces() {
    std::vector<Interface> found;
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
        return found;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        Mac mac{};
        if (static_cast<std::size_t>(adapter->PhysicalAddressLength) == mac.size()) {
            std::copy_n(adapter->PhysicalAddress, mac.size(), mac.begin());
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            const sockaddr* address = unicast->Address.lpSockaddr;
            if (address == nullptr || address->sa_family != AF_INET) {
                continue;
            }
            Interface card;
            card.name = narrow(adapter->FriendlyName);
            card.ip = ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr);
            card.mask = maskOfPrefix(unicast->OnLinkPrefixLength);
            card.broadcast = card.ip | ~card.mask;
            card.mac = mac;
            found.push_back(std::move(card));
        }
    }
#else
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return found;
    }
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        Interface card;
        card.name = entry->ifa_name != nullptr ? entry->ifa_name : "";
        card.ip = ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr);
        card.mask =
            entry->ifa_netmask != nullptr
                ? ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask)->sin_addr.s_addr)
                : 0xffffff00u;
        card.broadcast = card.ip | ~card.mask;
        // The card's hardware address is a separate entry of the same name.
        for (const ifaddrs* link = list; link != nullptr; link = link->ifa_next) {
            if (link->ifa_addr == nullptr || link->ifa_name == nullptr ||
                std::strcmp(link->ifa_name, card.name.c_str()) != 0) {
                continue;
            }
#if defined(__APPLE__)
            if (link->ifa_addr->sa_family == AF_LINK) {
                const auto* dl = reinterpret_cast<const sockaddr_dl*>(link->ifa_addr);
                if (static_cast<std::size_t>(dl->sdl_alen) == card.mac.size()) {
                    // What `LLADDR` gives, spelled out: the address follows the name.
                    std::memcpy(card.mac.data(), dl->sdl_data + dl->sdl_nlen, card.mac.size());
                }
            }
#elif defined(__linux__)
            if (link->ifa_addr->sa_family == AF_PACKET) {
                const auto* ll = reinterpret_cast<const sockaddr_ll*>(link->ifa_addr);
                if (static_cast<std::size_t>(ll->sll_halen) == card.mac.size()) {
                    std::memcpy(card.mac.data(), ll->sll_addr, card.mac.size());
                }
            }
#endif
        }
        found.push_back(std::move(card));
    }
    ::freeifaddrs(list);
#endif
    return found;
}

std::optional<Interface> interfaceFor(const std::vector<Interface>& interfaces,
                                      std::uint32_t address) {
    for (const Interface& card : interfaces) {
        if (card.mask != 0 && (card.ip & card.mask) == (address & card.mask)) {
            return card;
        }
    }
    return std::nullopt;
}

// --- the sockets -------------------------------------------------------------------------------

struct Network::Sockets {
    net::WinsockGuard winsock;
    net::Socket announce = net::kInvalidSocket;
    net::Socket beat = net::kInvalidSocket;
    net::Socket status = net::kInvalidSocket;

    static void shut(net::Socket& socket) noexcept {
        if (socket != net::kInvalidSocket) {
            net::closeSocket(socket);
            socket = net::kInvalidSocket;
        }
    }
    ~Sockets() {
        shut(announce);
        shut(beat);
        shut(status);
    }
};

namespace {

/// A non-blocking datagram socket bound to `ip`:`port`, allowed to broadcast — or invalid, with
/// why. Not shared with anyone: a port another program holds is that program's.
net::Socket bound(std::uint32_t ip, std::uint16_t port, std::string& problem) {
    if (!sandbox::allowsBind(port)) {
        sandbox::refuse(sandbox::Refused::Bind, port);
        problem = "not listening for CDJs in the test sandbox";
        return net::kInvalidSocket;
    }
    const net::Socket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == net::kInvalidSocket) {
        problem = "cannot open a socket (" + std::to_string(net::lastSocketError()) + ")";
        return net::kInvalidSocket;
    }
    // Non-blocking, and allowed to broadcast: everything takt4 sends goes from these.
    net::prepareSender(socket);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(ip);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        const int error = net::lastSocketError();
        net::closeSocket(socket);
        problem = net::bindFailure(error, port);
#if defined(_WIN32)
        const bool inUse = error == WSAEADDRINUSE || error == WSAEACCES;
#else
        const bool inUse = error == EADDRINUSE;
#endif
        if (inUse) {
            // The one program that is always there beside a CDJ, and holds these very ports.
            problem += " \xE2\x80\x94 is rekordbox running on this computer?";
        }
        return net::kInvalidSocket;
    }
    return socket;
}

} // namespace

Network::Network(Config config) : config_(std::move(config)), player_(config_.player) {
    if (!config_.interfaces) {
        config_.interfaces = listInterfaces;
    }
    interfaceWanted_ = config_.interfaceAddress;
}

Network::~Network() {
    close();
}

void Network::setProblem(std::string problem) {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.problem = std::move(problem);
}

void Network::setInterface(std::optional<std::uint32_t> address) {
    const std::lock_guard<std::mutex> lock(mutex_);
    interfaceWanted_ = address;
}

bool Network::open(std::string& problem) {
    if (isOpen()) {
        return true;
    }
    try {
        sockets_ = std::make_unique<Sockets>();
    } catch (const std::exception& e) {
        problem = std::string("cannot use the network: ") + e.what();
        setProblem(problem);
        return false;
    }
    std::string why;
    sockets_->announce = bound(0, config_.own.announce, why);
    if (sockets_->announce == net::kInvalidSocket) {
        sockets_.reset();
        problem = "cannot listen for CDJs: " + why;
        setProblem(problem);
        return false;
    }
    player_ = VirtualPlayer(config_.player);
    player_.listen(config_.clock());
    interface_.reset();
    joinedSockets_ = false;
    beatsHeard_ = beatsDropped_ = datagramsRead_ = sendsFailed_ = 0;
    publishedAt_ = 0;
    retryAt_ = 0;
    joinFailed_ = false;
    VirtualPlayer::Heard stale;
    while (beats_.tryPop(stale)) {
    }
    setProblem({});
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void Network::close() noexcept {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    sockets_.reset();
    joinedSockets_ = false;
    interface_.reset();
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = Snapshot{};
}

Network::Snapshot Network::snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void Network::run() noexcept {
    try {
        while (running_.load(std::memory_order_acquire)) {
            round();
        }
    } catch (const std::exception& e) {
        // Nothing here throws by design; if something does, the network says why and stops.
        setProblem(std::string("the CDJ network stopped: ") + e.what());
    }
    if (joinedSockets_) {
        player_.leave();
        closeJoined();
    }
}

void Network::readAll(int which, std::int64_t& now) {
    const net::Socket socket = which == 0   ? sockets_->announce
                               : which == 1 ? sockets_->beat
                                            : sockets_->status;
    const VirtualPlayer::Channel channel = which == 0   ? VirtualPlayer::Channel::Announce
                                           : which == 1 ? VirtualPlayer::Channel::Beat
                                                        : VirtualPlayer::Channel::Status;
    std::array<std::uint8_t, 2048> buffer{};
    for (int i = 0; i < kMaxReadsPerRound; ++i) {
        sockaddr_in from{};
        socklen_t fromLength = sizeof from;
#if defined(_WIN32)
        const int capacity = static_cast<int>(buffer.size());
#else
        const std::size_t capacity = buffer.size();
#endif
        const auto received = ::recvfrom(socket, reinterpret_cast<char*>(buffer.data()), capacity,
                                         0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received < 0 && lastErrorIsReset()) {
            continue; // an earlier send's refusal, reported here: not this datagram
        }
        if (received <= 0) {
            return;
        }
        // Stamped as it is read, before anything else is done with it.
        now = config_.clock();
        ++datagramsRead_;
        player_.receive(
            channel,
            std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received)),
            ntohl(from.sin_addr.s_addr), now);
    }
}

bool Network::openJoined(std::int64_t now) {
    // The card: the one asked for, or the one the first device heard is on.
    std::optional<std::uint32_t> wanted;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        wanted = interfaceWanted_;
    }
    const std::vector<Interface> cards = config_.interfaces();
    std::optional<Interface> card;
    if (wanted) {
        const auto match = std::find_if(cards.begin(), cards.end(),
                                        [&](const Interface& c) { return c.ip == *wanted; });
        if (match == cards.end()) {
            setProblem("no network card here has the address " + addressText(*wanted) +
                       " picked for the CDJs");
            return false;
        }
        card = *match;
    } else {
        const std::vector<VirtualPlayer::Device>& devices = player_.devices();
        if (devices.empty()) {
            return false; // nothing heard yet to join: waited for, said by the snapshot
        }
        card = interfaceFor(cards, devices.front().ip);
        if (!card) {
            setProblem("no network card here is on the CDJs' network (" +
                       addressText(devices.front().ip) + ")");
            return false;
        }
    }
    std::string why;
    sockets_->beat = bound(0, config_.own.beat, why);
    if (sockets_->beat != net::kInvalidSocket) {
        sockets_->status = bound(card->ip, config_.own.status, why);
    }
    if (sockets_->beat == net::kInvalidSocket || sockets_->status == net::kInvalidSocket) {
        Sockets::shut(sockets_->beat);
        Sockets::shut(sockets_->status);
        setProblem("cannot join the CDJs: " + why);
        return false;
    }
    interface_ = card;
    VirtualPlayer::Identity identity;
    identity.ip = card->ip;
    identity.broadcast = card->broadcast;
    identity.mac = card->mac;
    player_.join(identity, now);
    joinedSockets_ = true;
    setProblem({});
    return true;
}

void Network::closeJoined() noexcept {
    if (sockets_) {
        Sockets::shut(sockets_->beat);
        Sockets::shut(sockets_->status);
    }
    joinedSockets_ = false;
}

void Network::sendOutbox() {
    std::vector<VirtualPlayer::Datagram>& outbox = player_.outbox();
    for (const VirtualPlayer::Datagram& datagram : outbox) {
        const net::Socket from =
            sockets_->status != net::kInvalidSocket ? sockets_->status : sockets_->announce;
        sockaddr_storage to{};
        sockaddr_in v4{};
        v4.sin_family = AF_INET;
        v4.sin_port = htons(config_.peerAnnounce);
        v4.sin_addr.s_addr = htonl(datagram.to);
        std::memcpy(&to, &v4, sizeof v4);
        if (!net::sandboxLets(to)) {
            continue; // dropped in the test binaries' sandbox, and taken as sent
        }
        const std::span<const std::uint8_t> bytes = datagram.packet.view();
#if defined(_WIN32)
        const int length = static_cast<int>(bytes.size());
#else
        const std::size_t length = bytes.size();
#endif
        const auto sent = ::sendto(from, reinterpret_cast<const char*>(bytes.data()), length, 0,
                                   reinterpret_cast<const sockaddr*>(&v4), sizeof v4);
        if (sent < 0) {
            ++sendsFailed_;
        }
    }
    outbox.clear();
}

void Network::round() {
    fd_set readable;
    FD_ZERO(&readable);
    net::Socket highest = sockets_->announce;
    FD_SET(sockets_->announce, &readable);
    for (const net::Socket socket : {sockets_->beat, sockets_->status}) {
        if (socket != net::kInvalidSocket) {
            FD_SET(socket, &readable);
            highest = std::max(highest, socket);
        }
    }
    timeval wait{};
    wait.tv_sec = 0;
    wait.tv_usec = static_cast<decltype(wait.tv_usec)>(config_.roundMillis * 1000);
#if defined(_WIN32)
    (void)highest;
    const int ready = ::select(0, &readable, nullptr, nullptr, &wait);
#else
    const int ready = ::select(highest + 1, &readable, nullptr, nullptr, &wait);
#endif
    std::int64_t now = config_.clock();
    if (ready > 0) {
        if (FD_ISSET(sockets_->announce, &readable)) {
            readAll(0, now);
        }
        if (sockets_->beat != net::kInvalidSocket && FD_ISSET(sockets_->beat, &readable)) {
            readAll(1, now);
        }
        if (sockets_->status != net::kInvalidSocket && FD_ISSET(sockets_->status, &readable)) {
            readAll(2, now);
        }
    }

    // The join asked for, on this thread.
    const bool wanted = wantJoined_.load(std::memory_order_acquire);
    if (wanted && !joinedSockets_) {
        now = config_.clock();
        if (now >= retryAt_ && !openJoined(now)) {
            retryAt_ = now + kRetryJoinMicros;
            joinFailed_ = true;
        }
    } else if (!wanted && joinedSockets_) {
        player_.leave();
        closeJoined();
        interface_.reset();
    }
    // Why a join could not be made is said for as long as it is wanted, and no longer.
    if (!wanted && joinFailed_) {
        joinFailed_ = false;
        retryAt_ = 0;
        setProblem({});
    }

    player_.setFollow(follow_.load(std::memory_order_relaxed));
    now = config_.clock();
    player_.advance(now);
    if (!player_.outbox().empty()) {
        sendOutbox();
    }
    std::vector<VirtualPlayer::Heard>& heard = player_.heard();
    for (const VirtualPlayer::Heard& beat : heard) {
        ++beatsHeard_;
        if (!beats_.tryPush(beat)) {
            ++beatsDropped_;
        }
    }
    heard.clear();
    if (now - publishedAt_ >= kPublishMicros) {
        publish(now);
    }
}

void Network::publish(std::int64_t now) {
    publishedAt_ = now;
    Snapshot made;
    made.open = true;
    made.phase = player_.phase();
    made.number = player_.number();
    made.master = player_.master(now);
    made.followed = player_.followed(now);
    made.follow = player_.follow();
    for (const VirtualPlayer::Device& device : player_.devices()) {
        DeviceView view;
        view.number = device.number;
        view.name = nameText(device.name);
        view.kind = device.kind;
        view.ip = device.ip;
        view.hasStatus = device.hasStatus;
        if (device.hasStatus) {
            view.playing = device.status.playing();
            view.master = device.status.master();
            view.synced = device.status.synced();
            view.onAir = device.status.onAir();
            view.cueing = device.status.cueing();
            view.masterWithoutBeats = device.status.master() && !device.status.mixer &&
                                      device.status.masterMeaningful == 2;
            view.bpm = device.status.bpm();
        }
        view.beating = device.heardBeat && now - device.beatAt <= kBeatingMicros;
        made.devices.push_back(std::move(view));
    }
    std::sort(made.devices.begin(), made.devices.end(),
              [](const DeviceView& a, const DeviceView& b) { return a.number < b.number; });
    if (interface_) {
        made.interfaceName = interface_->name;
        made.interfaceIp = interface_->ip;
    }
    made.beatsHeard = beatsHeard_;
    made.beatsDropped = beatsDropped_;
    made.cueBeatsDropped = player_.cueBeatsDropped();
    made.numbersLost = player_.numbersLost();
    made.datagramsRead = datagramsRead_;
    made.sendsFailed = sendsFailed_;
    const std::lock_guard<std::mutex> lock(mutex_);
    made.problem = std::move(snapshot_.problem);
    snapshot_ = std::move(made);
}

} // namespace takt4::timing::prolink
