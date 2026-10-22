#include "core/control/osc_receiver.hpp"

#include "core/net/udp.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <iphlpapi.h>
#include <mswsock.h> // SIO_UDP_CONNRESET

#include <vector>
#pragma comment(lib, "iphlpapi.lib")
#endif

namespace takt4::control {
namespace {

using net::closeSocket;
using net::describe;
using net::kInvalidSocket;
using net::lastSocketError;
using net::Socket;
using net::WinsockGuard;

#if defined(_WIN32)
/// Whether another socket on this machine already takes loopback datagrams sent to `port`: one
/// bound to every address, or to 127.0.0.1. Windows lets a bind to 127.0.0.1 succeed beside a
/// socket on every address — exclusive or not, measured — and then hands it every loopback
/// datagram to the port: the operator types Resolume's port as the control port, takt4 says it
/// is listening, and from then on its own OSC to Resolume lands in its control socket. From the
/// system's table of UDP sockets; a table that cannot be read says nothing is there.
bool loopbackTaken(std::uint16_t port) {
    std::vector<DWORD> buffer;
    ULONG size = 16 * 1024;
    DWORD result = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 4 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        buffer.resize(size / sizeof(DWORD) + 64);
        size = static_cast<ULONG>(buffer.size() * sizeof(DWORD));
        result = ::GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_BASIC, 0);
    }
    if (result != NO_ERROR) {
        return false;
    }
    const auto* table = reinterpret_cast<const MIB_UDPTABLE*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_UDPROW& row = table->table[i];
        const std::uint32_t address = ntohl(row.dwLocalAddr);
        if (ntohs(static_cast<u_short>(row.dwLocalPort)) == port &&
            (address == INADDR_ANY || address == INADDR_LOOPBACK)) {
            return true;
        }
    }
    return false;
}
#endif

} // namespace

struct OscReceiver::Impl {
    WinsockGuard winsock;
    Socket socket = kInvalidSocket;
    std::array<std::byte, kMaxDatagram> buffer{};
    /// Replies go from the window's thread and the receiving one's both.
    std::mutex sendMutex;

    ~Impl() {
        if (socket != kInvalidSocket) {
            closeSocket(socket);
        }
    }
};

OscReceiver::OscReceiver(std::uint16_t port, bool localOnly) : port_(port) {
    if (!sandbox::allowsBind(port)) {
        // The test binaries' sandbox: a port a program on the rig may hold — this one is
        // likely takt4's own, running the show. See `sandbox.hpp`.
        sandbox::refuse(sandbox::Refused::Bind, port);
        throw std::runtime_error("OSC control: cannot listen on port " + std::to_string(port) +
                                 " (the test sandbox binds only ports its receivers were given)");
    }
    auto impl = std::make_unique<Impl>();

    // IPv4 only, deliberately. A dual-stack socket would need per-platform handling of
    // IPV6_V6ONLY, and every control surface the design names speaks IPv4.
    impl->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->socket == kInvalidSocket) {
        throw std::runtime_error("OSC control: cannot open a socket (" +
                                 std::to_string(lastSocketError()) + ")");
    }
    // The port this socket's alone: a tool binding 127.0.0.1 to it later, with this one on every
    // address ("allow other machines"), took the Stream Deck's traffic from this machine — PANIC
    // included — without a word to either.
    net::bindExclusively(impl->socket);
#if defined(_WIN32)
    // And the other way round, which no flag of this socket's can stop: a port another program
    // holds on every address is refused here rather than shared.
    if (localOnly && port != 0 && loopbackTaken(port)) {
        throw std::runtime_error("OSC control: cannot listen: " +
                                 net::bindFailure(WSAEADDRINUSE, port));
    }
#endif

    // Unqualified: `htons` and `htonl` are functions on Winsock but *macros* on glibc and
    // on Darwin, and `::htons` asks a macro for a namespace it does not have. MSVC
    // compiles the qualified form happily, which is why this only showed up on CI.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr =
        htonl(static_cast<std::uint32_t>(localOnly ? INADDR_LOOPBACK : INADDR_ANY));
    if (::bind(impl->socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        throw std::runtime_error("OSC control: cannot listen: " +
                                 net::bindFailure(lastSocketError(), port));
    }
#if defined(_WIN32)
    // A reply sent to a port nobody listens on makes Windows fail this socket's *next* receive
    // with WSAECONNRESET (10054, measured): a monitor that went away would cost the next control
    // message. Told not to.
    BOOL report = FALSE;
    DWORD returned = 0;
    (void)::WSAIoctl(impl->socket, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &returned,
                     nullptr, nullptr);
    // And never blocking, so a reply cannot wait on a full buffer (`sendTo`), as MSG_DONTWAIT
    // does it a send at a time elsewhere. A receive waits in `select`, not in the call.
    u_long nonBlocking = 1;
    (void)::ioctlsocket(impl->socket, FIONBIO, &nonBlocking);
#endif

    // Asked for any free port: say which one it got, so a caller can tell somebody.
    if (port_ == 0) {
        sockaddr_in bound{};
        socklen_t boundLength = sizeof bound;
        if (::getsockname(impl->socket, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0) {
            port_ = ntohs(bound.sin_port);
        }
        // This process's own, so a test may send to it (see `sandbox.hpp`).
        sandbox::allowPort(port_);
    }

    impl_ = std::move(impl);
}

OscReceiver::~OscReceiver() = default;

std::span<const std::byte> OscReceiver::receive(std::chrono::milliseconds timeout) noexcept {
    if (!impl_ || impl_->socket == kInvalidSocket) {
        return {};
    }

    // select rather than SO_RCVTIMEO: the timeout means the same thing on every platform
    // here, and a socket closed under a waiting thread wakes it either way.
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(impl_->socket, &readable);
    timeval wait{};
    wait.tv_sec = static_cast<decltype(wait.tv_sec)>(timeout.count() / 1000);
    wait.tv_usec = static_cast<decltype(wait.tv_usec)>((timeout.count() % 1000) * 1000);

#if defined(_WIN32)
    const int ready = ::select(0, &readable, nullptr, nullptr, &wait);
#else
    const int ready = ::select(impl_->socket + 1, &readable, nullptr, nullptr, &wait);
#endif
    if (ready <= 0) {
        return {};
    }

    sockaddr_storage from{};
    socklen_t fromLength = sizeof from;
    // Winsock's `recvfrom` takes an `int` length and POSIX's takes a `size_t`, so the
    // cast has to differ or one of them is a signedness conversion `-Wconversion` refuses.
#if defined(_WIN32)
    const int capacity = static_cast<int>(impl_->buffer.size());
#else
    const std::size_t capacity = impl_->buffer.size();
#endif
    const auto received = ::recvfrom(impl_->socket, reinterpret_cast<char*>(impl_->buffer.data()),
                                     capacity, 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
    if (received <= 0) {
        return {};
    }

    lastSender_ = describe(reinterpret_cast<const sockaddr*>(&from), fromLength);
    lastFrom_ = {};
    if (from.ss_family == AF_INET) {
        sockaddr_in v4{};
        std::memcpy(&v4, &from, sizeof v4);
        lastFrom_.ip = ntohl(v4.sin_addr.s_addr);
        lastFrom_.port = ntohs(v4.sin_port);
    }
    ++datagrams_;
    return std::span<const std::byte>(impl_->buffer.data(), static_cast<std::size_t>(received));
}

bool OscReceiver::sendTo(const remote::Endpoint& to, std::span<const std::byte> packet) noexcept {
    if (!impl_ || impl_->socket == kInvalidSocket || packet.empty() || to.port == 0) {
        sendFailures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    sockaddr_storage address{};
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(to.port);
    v4.sin_addr.s_addr = htonl(to.ip);
    std::memcpy(&address, &v4, sizeof v4);
    // In the test binaries, only to a port a test's own receiver was given (`sandbox.hpp`).
    if (!net::sandboxLets(address)) {
        sendFailures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
#if defined(_WIN32)
    const int length = static_cast<int>(packet.size());
    constexpr int flags = 0;
#else
    const std::size_t length = packet.size();
    constexpr int flags = MSG_DONTWAIT;
#endif
    const std::lock_guard<std::mutex> lock(impl_->sendMutex);
    const auto wrote = ::sendto(impl_->socket, reinterpret_cast<const char*>(packet.data()), length,
                                flags, reinterpret_cast<const sockaddr*>(&v4), sizeof v4);
    if (wrote < 0 || static_cast<std::size_t>(wrote) != packet.size()) {
        sendFailures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace takt4::control
