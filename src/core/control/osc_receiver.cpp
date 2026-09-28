#include "core/control/osc_receiver.hpp"

#include "core/net/udp.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace takt4::control {
namespace {

using net::closeSocket;
using net::describe;
using net::kInvalidSocket;
using net::lastSocketError;
using net::Socket;
using net::WinsockGuard;

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
    // IPV6_V6ONLY, and every control surface §5.7 names speaks IPv4.
    impl->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->socket == kInvalidSocket) {
        throw std::runtime_error("OSC control: cannot open a socket (" +
                                 std::to_string(lastSocketError()) + ")");
    }

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
