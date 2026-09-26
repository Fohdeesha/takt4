#include "core/dmx/artnet_sender.hpp"

#include "core/net/resolver.hpp"
#include "core/net/udp.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
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
    /// Where the address comes from. See `net::AsyncAddress`.
    std::optional<net::AsyncAddress> where;
    /// Set when the address was known and a socket for it still could not be had.
    int socketError = 0;
    /// Why the last send failed, and zero once one succeeds. A network that is down or a cable
    /// that is out fails every send, and before this nothing said so: the target simply went
    /// quiet (the audit's T3 and M12).
    int sendError = 0;

    ~Impl() {
        if (socket != kInvalidSocket) {
            closeSocket(socket);
        }
    }
};

ArtNetSender::ArtNetSender(std::string_view host, std::uint16_t port) : host_(host), port_(port) {
    auto impl = std::make_unique<Impl>();
    impl->where.emplace(std::string(host), port);
    impl_ = impl.release();
    // A literal address has its socket now; one that cannot get one never will, and says so.
    if (!ready() && impl_->socketError != 0) {
        const int error = impl_->socketError;
        delete std::exchange(impl_, nullptr);
        throw std::runtime_error("Art-Net node " + std::string(host) + ":" + std::to_string(port) +
                                 ": cannot open a UDP socket (" + std::to_string(error) + ")");
    }
}

bool ArtNetSender::ready() noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    if (impl_->socket != kInvalidSocket) {
        return true;
    }
    try {
        const std::optional<net::AsyncAddress::Address> known = impl_->where->address();
        if (!known) {
            return false;
        }
        const Socket handle = ::socket(known->family, SOCK_DGRAM, IPPROTO_UDP);
        if (handle == kInvalidSocket) {
            impl_->socketError = lastSocketError();
            return false;
        }
        impl_->socket = handle;
        impl_->socketError = 0;
        std::memcpy(&impl_->address, known->storage, static_cast<std::size_t>(known->length));
        impl_->addressLength = static_cast<socklen_t>(known->length);
        // Non-blocking, and allowed to broadcast: see `net::prepareSender`.
        net::prepareSender(handle);
        resolved_ = describe(reinterpret_cast<const sockaddr*>(&impl_->address),
                             impl_->addressLength);
        return true;
    } catch (...) {
        return false; // an allocation on the way to a look-up; the next frame tries again
    }
}

std::string ArtNetSender::problem() const {
    if (impl_ == nullptr) {
        return "not open";
    }
    if (impl_->socket != kInvalidSocket) {
        return impl_->sendError == 0 ? std::string()
                                     : "sends are failing: " + net::sendFailure(impl_->sendError);
    }
    if (impl_->socketError != 0) {
        return "cannot open a UDP socket (" + std::to_string(impl_->socketError) + ")";
    }
    return impl_->where->problem();
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
    if (!ready()) {
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

    if (!net::sandboxLets(impl_->address)) {
        // The test binaries' sandbox, and nowhere else: gone, as far as the sender can tell.
        impl_->sendError = 0;
        ++sent_;
        return true;
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
        impl_->sendError = written < 0 ? lastSocketError() : -1;
        ++failed_;
        return false;
    }
    impl_->sendError = 0;
    ++sent_;
    return true;
}

} // namespace takt4::dmx
