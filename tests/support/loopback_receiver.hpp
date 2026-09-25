#pragma once

// A UDP socket bound to a free port on the loopback interface, so anything that sends OSC
// can be tested against something that really receives. Shared by tests/output/osc_test.cpp
// and tests/output/rule_sink_test.cpp — a rule's address reaching a socket is a different
// claim from a rule's address being built, and only one of them can be checked without one.
//
// REQUIRE is used inside, so this must be included from a Catch2 translation unit.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
// ws2tcpip.h must follow winsock2.h.
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace takt4::testing {

/// A destination every send to fails, on every system CI runs on — the stand-in for a network
/// that is down, since no test can pull a cable. Not 0.0.0.0, which the tests used first:
/// Windows refuses that one, but Linux takes a zero destination to mean this machine and
/// quietly delivers it over loopback, so those tests failed on the Linux runner. Anything else
/// in 0.0.0.0/8 Linux refuses outright (`__mkroute_output` in net/ipv4/route.c), and Windows
/// calls it an unreachable network (measured).
inline constexpr const char* kUnsendableHost = "0.0.0.1";

/// What `net::sendFailure` makes of a send to `kUnsendableHost` here — words, so a test can
/// check the reason an operator is given and not merely that a send failed. Empty where it
/// has not been measured, which any status contains.
#if defined(_WIN32)
inline constexpr const char* kUnsendableReason = "the network is unreachable";
#elif defined(__linux__)
inline constexpr const char* kUnsendableReason = "not an address this machine can send to";
#else
inline constexpr const char* kUnsendableReason = "";
#endif

/// A UDP socket bound to a free port on the loopback interface, so the sender can be
/// tested against something that really receives. Only the tests need one: takt4 does
/// not listen until Phase 5's control input (HANDOFF §5.7).
class LoopbackReceiver {
public:
    LoopbackReceiver() {
#if defined(_WIN32)
        WSADATA data{};
        REQUIRE(::WSAStartup(MAKEWORD(2, 2), &data) == 0);
        started_ = true;
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(socket_ != INVALID_SOCKET);
#else
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(socket_ >= 0);
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0; // let the OS pick
        socklen_t length = static_cast<socklen_t>(sizeof address);
        REQUIRE(::bind(socket_, reinterpret_cast<const sockaddr*>(&address), length) == 0);
        REQUIRE(::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);

        // A test must not hang if a datagram is lost; half a second is far longer than a
        // loopback packet can take.
#if defined(_WIN32)
        const DWORD timeout = 500;
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                     sizeof timeout);
#else
        timeval timeout{};
        timeout.tv_usec = 500000;
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     static_cast<socklen_t>(sizeof timeout));
#endif
    }

    ~LoopbackReceiver() {
#if defined(_WIN32)
        if (socket_ != INVALID_SOCKET) {
            ::closesocket(socket_);
        }
        if (started_) {
            ::WSACleanup();
        }
#else
        if (socket_ >= 0) {
            ::close(socket_);
        }
#endif
    }

    LoopbackReceiver(const LoopbackReceiver&) = delete;
    LoopbackReceiver& operator=(const LoopbackReceiver&) = delete;

    std::uint16_t port() const { return port_; }

    /// The next datagram, or an empty string if none arrived before the timeout.
    std::string receive() {
        char buffer[1024];
#if defined(_WIN32)
        const int length = static_cast<int>(sizeof buffer);
#else
        const std::size_t length = sizeof buffer;
#endif
        const auto got = ::recv(socket_, buffer, length, 0);
        return got > 0 ? std::string(buffer, static_cast<std::size_t>(got)) : std::string();
    }

    /// Whether a datagram is waiting, having waited up to `milliseconds` for one — so a test
    /// holding several receivers can take each datagram from whichever one it reached.
    bool ready(int milliseconds) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket_, &set);
        timeval wait{};
        wait.tv_sec = milliseconds / 1000;
        wait.tv_usec = (milliseconds % 1000) * 1000;
#if defined(_WIN32)
        return ::select(0, &set, nullptr, nullptr, &wait) > 0;
#else
        return ::select(socket_ + 1, &set, nullptr, nullptr, &wait) > 0;
#endif
    }

private:
#if defined(_WIN32)
    SOCKET socket_ = INVALID_SOCKET;
    bool started_ = false;
#else
    int socket_ = -1;
#endif
    std::uint16_t port_ = 0;
};

} // namespace takt4::testing
