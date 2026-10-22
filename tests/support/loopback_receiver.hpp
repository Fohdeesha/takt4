#pragma once

// A UDP socket bound to a free port on the loopback interface, so anything that sends OSC
// can be tested against something that really receives. Shared by tests/output/osc_test.cpp
// and tests/output/rule_sink_test.cpp — a rule's address reaching a socket is a different
// claim from a rule's address being built, and only one of them can be checked without one.
//
// REQUIRE is used inside, so this must be included from a Catch2 translation unit.

#include "core/sandbox.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
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
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace takt4::testing {

/// A destination a send to fails — the stand-in for a network that is down, since no test can
/// pull a cable. Windows calls 0.0.0.1 an unreachable network (measured). Not 0.0.0.0, which
/// the tests used first: Linux takes a zero destination to mean this machine.
///
/// **And not on Linux at all** (`kUnsendableFails`): this said Linux refuses the rest of
/// 0.0.0.0/8, which no run had checked, and the first linux-tsan job that reached the tests
/// (2026-09-30, Ubuntu's kernel) sent to 0.0.0.1 without complaint — five tests failed. Their
/// point is what takt4 says when a send fails, which only a failing send can show, so they skip
/// there and run on Windows, which is where takt4 ships.
inline constexpr const char* kUnsendableHost = "0.0.0.1";

/// Whether a send to `kUnsendableHost` fails on this system. See there.
#if defined(__linux__)
inline constexpr bool kUnsendableFails = false;
#else
inline constexpr bool kUnsendableFails = true;
#endif

/// The reason a test gives when `kUnsendableFails` is false.
inline constexpr const char* kUnsendableSkip =
    "sends to 0.0.0.1 go through on this system (see kUnsendableHost)";

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
/// tested against something that really receives — and the only kind of destination the test
/// binaries' sandbox lets anything be sent to (`src/core/sandbox.hpp`).
class LoopbackReceiver {
public:
    /// `allowed` false only for the sandbox's own test: a real socket the sandbox has not been
    /// told about, which nothing in takt4 may send to.
    explicit LoopbackReceiver(bool allowed = true) { open(INADDR_LOOPBACK, 0, allowed); }

    /// Bound to `ipv4` (in host order) and `port`, rather than 127.0.0.1 and a port the system
    /// picks: a second receiver at another loopback address on the same port as a first, for an
    /// output whose name moves from the one to the other. The whole of 127/8 is the loopback.
    LoopbackReceiver(std::uint32_t ipv4, std::uint16_t port) { open(ipv4, port, true); }

    /// Whether a socket can be bound to `ipv4` (in host order) here. Linux and Windows answer for
    /// the whole of 127/8; macOS has 127.0.0.1 alone unless another is aliased, so a test that
    /// moves an output from one loopback address to another skips there.
    static bool canBind(std::uint32_t ipv4) {
#if defined(_WIN32)
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
        const SOCKET probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const bool opened = probe != INVALID_SOCKET;
#else
        const int probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const bool opened = probe >= 0;
#endif
        bool bound = false;
        if (opened) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(ipv4);
            bound = ::bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
#if defined(_WIN32)
            ::closesocket(probe);
#else
            ::close(probe);
#endif
        }
#if defined(_WIN32)
        ::WSACleanup();
#endif
        return bound;
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

    /// A datagram and when it reached this machine, on `std::chrono::steady_clock`.
    struct Stamped {
        std::string datagram;
        std::chrono::steady_clock::time_point at;
    };

    /// The next datagram and when it arrived — **the kernel's own stamp** where there is one
    /// (SO_TIMESTAMP, macOS and Linux), so a test measuring when takt4 sent something does not
    /// also measure how soon its own reading thread, at the default priority, was let run: on the
    /// first macOS CI runs a thread at that priority woke 154 times a second when asked for a
    /// thousand. Where there is none (Windows), the moment `recv` returned. An empty datagram for
    /// a timeout.
    Stamped receiveStamped() {
#if defined(_WIN32)
        Stamped out;
        out.datagram = receive();
        out.at = std::chrono::steady_clock::now();
        return out;
#else
        char buffer[1024];
        alignas(cmsghdr) char control[256];
        iovec part{buffer, sizeof buffer};
        msghdr message{};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof control;
        Stamped out;
        const auto got = ::recvmsg(socket_, &message, 0);
        const auto steadyNow = std::chrono::steady_clock::now();
        const auto systemNow = std::chrono::system_clock::now();
        out.at = steadyNow;
        if (got <= 0) {
            return out;
        }
        out.datagram.assign(buffer, static_cast<std::size_t>(got));
        for (cmsghdr* item = CMSG_FIRSTHDR(&message); item != nullptr;
             item = CMSG_NXTHDR(&message, item)) {
            if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_TIMESTAMP) {
                timeval stamp{};
                std::memcpy(&stamp, CMSG_DATA(item), sizeof stamp);
                // The kernel stamps on the wall clock: carried onto the steady one across the
                // moment just read on both, which the two cannot drift apart in.
                const auto arrived = std::chrono::system_clock::time_point(
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::seconds(stamp.tv_sec) +
                        std::chrono::microseconds(stamp.tv_usec)));
                out.at =
                    steadyNow - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    systemNow - arrived);
            }
        }
        return out;
#endif
    }

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

    /// `bytes` to `port` on 127.0.0.1, **from this socket** — so whatever answers the sender
    /// answers here, as a show controller sends and is answered on its one socket.
    bool send(std::uint16_t port, std::span<const std::byte> bytes) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(port);
#if defined(_WIN32)
        const int length = static_cast<int>(bytes.size());
#else
        const std::size_t length = bytes.size();
#endif
        const auto wrote =
            ::sendto(socket_, reinterpret_cast<const char*>(bytes.data()), length, 0,
                     reinterpret_cast<const sockaddr*>(&to), static_cast<socklen_t>(sizeof to));
        return wrote >= 0 && static_cast<std::size_t>(wrote) == bytes.size();
    }

private:
    void open(std::uint32_t ipv4, std::uint16_t port, bool allowed) {
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
        address.sin_addr.s_addr = htonl(ipv4);
        address.sin_port = htons(port); // 0: let the OS pick
        socklen_t length = static_cast<socklen_t>(sizeof address);
        REQUIRE(::bind(socket_, reinterpret_cast<const sockaddr*>(&address), length) == 0);
        REQUIRE(::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        // A port the system gave this process, so the sandbox lets takt4 send here — and bind
        // here, for the tests that free one and ask takt4 to listen on it. See `sandbox.hpp`.
        if (allowed) {
            takt4::sandbox::allowPort(port_);
        }

#if !defined(_WIN32)
        // The kernel's own moment of arrival on every datagram: see `receiveStamped`.
        const int stamp = 1;
        ::setsockopt(socket_, SOL_SOCKET, SO_TIMESTAMP, &stamp,
                     static_cast<socklen_t>(sizeof stamp));
#endif
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

#if defined(_WIN32)
    SOCKET socket_ = INVALID_SOCKET;
    bool started_ = false;
#else
    int socket_ = -1;
#endif
    std::uint16_t port_ = 0;
};

} // namespace takt4::testing
