#include "core/control/osc_receiver.hpp"

#include "core/net/udp.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <iphlpapi.h>

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
    ++datagrams_;
    return std::span<const std::byte>(impl_->buffer.data(), static_cast<std::size_t>(received));
}

} // namespace takt4::control
