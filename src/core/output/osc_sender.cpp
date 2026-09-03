#include "core/output/osc_sender.hpp"

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

#if defined(_WIN32)
#include <winsock2.h>
// ws2tcpip.h must follow winsock2.h.
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace takt4::output {

namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;

void closeSocket(Socket socket) noexcept {
    ::closesocket(socket);
}

/// Winsock has to be started before any socket call and stopped after the last one.
/// Every sender holds one of these, so the library is up exactly while one exists.
class WinsockGuard {
public:
    WinsockGuard() {
        if (count_.fetch_add(1, std::memory_order_acq_rel) == 0) {
            WSADATA data{};
            const int error = ::WSAStartup(MAKEWORD(2, 2), &data);
            if (error != 0) {
                count_.fetch_sub(1, std::memory_order_acq_rel);
                throw std::runtime_error("WSAStartup failed with " + std::to_string(error));
            }
        }
    }
    ~WinsockGuard() {
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            ::WSACleanup();
        }
    }
    WinsockGuard(const WinsockGuard&) : WinsockGuard() {}
    WinsockGuard& operator=(const WinsockGuard&) = delete;

private:
    static std::atomic<int> count_;
};

std::atomic<int> WinsockGuard::count_{0};

int lastSocketError() noexcept {
    return ::WSAGetLastError();
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;

void closeSocket(Socket socket) noexcept {
    ::close(socket);
}

struct WinsockGuard {};

int lastSocketError() noexcept {
    return errno;
}
#endif

std::string describe(const sockaddr* address, socklen_t length) {
    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    if (::getnameinfo(address, length, host, sizeof host, service, sizeof service,
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "?";
    }
    return std::string(host) + ":" + service;
}

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
