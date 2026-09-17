#include "core/dmx/artnet_sender.hpp"

#include "core/net/udp.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace takt4::dmx {
namespace {

using takt4::net::closeSocket;
using takt4::net::describe;
using takt4::net::kInvalidSocket;
using takt4::net::lastSocketError;
using takt4::net::Socket;
using takt4::net::WinsockGuard;

} // namespace

struct ArtNetSender::Impl {
    WinsockGuard winsock;
    Socket socket = kInvalidSocket;
    sockaddr_storage address{};
    socklen_t addressLength = 0;
    /// One sequence counter per universe — see `sendDmx`. A map rather than an array of
    /// 32,768 bytes, because a rig uses a handful of universes and the whole Port-Address
    /// space is addressable.
    std::unordered_map<PortAddress, std::uint8_t> sequence;
    /// The encode buffer, reused: one ArtDmx packet at a time, on one thread.
    std::array<std::byte, kArtDmxMaxSize> packet{};

    ~Impl() {
        if (socket != kInvalidSocket) {
            closeSocket(socket);
        }
    }
};

ArtNetSender::ArtNetSender(std::string_view host, std::uint16_t port) : host_(host), port_(port) {
    auto impl = std::make_unique<Impl>();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* results = nullptr;
    const std::string node(host);
    const std::string service = std::to_string(port);
    const int error = ::getaddrinfo(node.c_str(), service.c_str(), &hints, &results);
    if (error != 0 || results == nullptr) {
        throw std::runtime_error("Art-Net node " + node + ":" + service + ": cannot resolve (" +
                                 std::to_string(error) + ")");
    }

    for (const addrinfo* candidate = results; candidate != nullptr;
         candidate = candidate->ai_next) {
        const Socket handle =
            ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (handle == kInvalidSocket) {
            continue;
        }
        impl->socket = handle;
        std::memcpy(&impl->address, candidate->ai_addr, candidate->ai_addrlen);
        impl->addressLength = static_cast<socklen_t>(candidate->ai_addrlen);
        break;
    }
    const std::string resolved =
        impl->addressLength > 0
            ? describe(reinterpret_cast<const sockaddr*>(&impl->address), impl->addressLength)
            : std::string();
    ::freeaddrinfo(results);

    if (impl->socket == kInvalidSocket) {
        throw std::runtime_error("Art-Net node " + node + ":" + service +
                                 ": cannot open a UDP socket (" +
                                 std::to_string(lastSocketError()) + ")");
    }

    // A rig built around a broadcast address is a rig somebody already has, and the socket
    // has to be told before it will carry one. Asked for unconditionally and ignored when it
    // fails: a platform that refuses the option still sends unicast perfectly well, and the
    // operator who typed a unicast address is not affected either way.
    const int broadcast = 1;
#if defined(_WIN32)
    ::setsockopt(impl->socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
                 sizeof broadcast);
#else
    ::setsockopt(impl->socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof broadcast);
#endif

    resolved_ = resolved;
    impl_ = impl.release();
}

ArtNetSender::~ArtNetSender() {
    delete impl_;
}

ArtNetSender::ArtNetSender(ArtNetSender&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)), host_(std::move(other.host_)),
      resolved_(std::move(other.resolved_)), port_(other.port_), sent_(other.sent_),
      failed_(other.failed_) {}

ArtNetSender& ArtNetSender::operator=(ArtNetSender&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = std::exchange(other.impl_, nullptr);
        host_ = std::move(other.host_);
        resolved_ = std::move(other.resolved_);
        port_ = other.port_;
        sent_ = other.sent_;
        failed_ = other.failed_;
    }
    return *this;
}

bool ArtNetSender::sendDmx(PortAddress universe, std::span<const std::uint8_t> levels) noexcept {
    if (impl_ == nullptr) {
        ++failed_;
        return false;
    }
    std::uint8_t& sequence = impl_->sequence[universe];
    sequence = nextSequence(sequence);

    const std::size_t length = writeArtDmx(impl_->packet, universe, levels, sequence);
    if (length == 0) {
        ++failed_;
        return false;
    }

#if defined(_WIN32)
    const int size = static_cast<int>(length);
    const auto* data = reinterpret_cast<const char*>(impl_->packet.data());
#else
    const std::size_t size = length;
    const void* data = impl_->packet.data();
#endif
    const auto written =
        ::sendto(impl_->socket, data, size, 0, reinterpret_cast<const sockaddr*>(&impl_->address),
                 impl_->addressLength);
    if (written < 0 || static_cast<std::size_t>(written) != length) {
        ++failed_;
        return false;
    }
    ++sent_;
    return true;
}

} // namespace takt4::dmx
