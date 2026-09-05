#include "core/output/osc_sender.hpp"

#include "core/net/udp.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
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

    ~Impl() {
        if (socket != kInvalidSocket) {
            closeSocket(socket);
        }
    }
};

OscSender::OscSender(std::string_view host, std::uint16_t port) : host_(host), port_(port) {
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
        throw std::runtime_error("OSC target " + node + ":" + service + ": cannot resolve (" +
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
        throw std::runtime_error("OSC target " + node + ":" + service +
                                 ": cannot open a UDP socket (" +
                                 std::to_string(lastSocketError()) + ")");
    }
    resolved_ = resolved;
    impl_ = impl.release();
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
    if (impl_ == nullptr || packet.empty()) {
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
