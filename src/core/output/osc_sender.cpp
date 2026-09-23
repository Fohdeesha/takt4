#include "core/output/osc_sender.hpp"

#include "core/net/resolver.hpp"
#include "core/net/udp.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace takt4::output {
namespace {

using takt4::net::closeSocket;
using takt4::net::describe;
using takt4::net::kInvalidSocket;
using takt4::net::lastSocketError;
using takt4::net::Socket;
using takt4::net::WinsockGuard;

} // namespace

struct OscSender::Impl {
    WinsockGuard winsock;
    Socket socket = kInvalidSocket;
    sockaddr_storage address{};
    socklen_t addressLength = 0;
    /// Where the address comes from — at once for a literal, from a thread of its own for a
    /// name. See `net::AsyncAddress`.
    std::optional<net::AsyncAddress> where;
    /// Set when the address was known and a socket for it still could not be had.
    int socketError = 0;

    ~Impl() {
        if (socket != kInvalidSocket) {
            closeSocket(socket);
        }
    }
};

OscSender::OscSender(std::string_view host, std::uint16_t port) : host_(host), port_(port) {
    auto impl = std::make_unique<Impl>();
    impl->where.emplace(std::string(host), port);
    impl_ = impl.release();
    // A literal address has its socket now, as it always did — and a literal that cannot get
    // one is the one failure worth throwing for, since no amount of waiting will fix it.
    if (!ready() && impl_->socketError != 0) {
        const int error = impl_->socketError;
        delete std::exchange(impl_, nullptr);
        throw std::runtime_error("OSC target " + std::string(host) + ":" + std::to_string(port) +
                                 ": cannot open a UDP socket (" + std::to_string(error) + ")");
    }
}

bool OscSender::ready() noexcept {
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
        resolved_ = describe(reinterpret_cast<const sockaddr*>(&impl_->address),
                             impl_->addressLength);
        return true;
    } catch (...) {
        return false; // an allocation on the way to a look-up; the next send tries again
    }
}

std::string OscSender::problem() const {
    if (impl_ == nullptr) {
        return "not open";
    }
    if (impl_->socket != kInvalidSocket) {
        return {};
    }
    if (impl_->socketError != 0) {
        return "cannot open a UDP socket (" + std::to_string(impl_->socketError) + ")";
    }
    const std::string why = impl_->where->problem();
    // Known but not yet opened: the next send opens it, so there is nothing wrong to report.
    return why;
}

OscSender::~OscSender() {
    delete impl_;
}

OscSender::OscSender(OscSender&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)), host_(std::move(other.host_)),
      resolved_(std::move(other.resolved_)), port_(other.port_), sent_(other.sent_),
      failed_(other.failed_) {}

OscSender& OscSender::operator=(OscSender&& other) noexcept {
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

bool OscSender::send(std::span<const std::byte> packet) noexcept {
    if (packet.empty() || !ready()) {
        ++failed_;
        return false;
    }
#if defined(_WIN32)
    const int length = static_cast<int>(packet.size());
    const auto* data = reinterpret_cast<const char*>(packet.data());
#else
    const std::size_t length = packet.size();
    const void* data = packet.data();
#endif
    const auto written =
        ::sendto(impl_->socket, data, length, 0, reinterpret_cast<const sockaddr*>(&impl_->address),
                 impl_->addressLength);
    if (written < 0 || static_cast<std::size_t>(written) != packet.size()) {
        ++failed_;
        return false;
    }
    ++sent_;
    return true;
}

} // namespace takt4::output
