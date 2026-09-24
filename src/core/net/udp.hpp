#pragma once

/// The platform's UDP sockets, spelled the same way on all three.
///
/// Internal to `core`: this is a shim over `<winsock2.h>` and `<sys/socket.h>`, not an
/// abstraction, and it pulls the platform's networking headers into whatever includes it.
/// Nothing outside `core/output` and `core/control` should want it.
///
/// It exists because OSC now goes both ways — `output::OscSender` writes datagrams and
/// `control::OscReceiver` reads them — and the second copy of a reference-counted
/// `WSAStartup` guard is the sort of duplication that is right until one of them is
/// fixed and the other is not.

#include <atomic>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
// ws2tcpip.h must follow winsock2.h.
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <stdexcept>

namespace takt4::net {

#if defined(_WIN32)

using Socket = SOCKET;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;

inline void closeSocket(Socket socket) noexcept {
    ::closesocket(socket);
}

inline int lastSocketError() noexcept {
    return ::WSAGetLastError();
}

/// Winsock has to be started before any socket call and stopped after the last one.
/// Everything holding a socket holds one of these, so the library is up exactly while at
/// least one exists. Windows reference-counts `WSAStartup` itself; the counter here is so
/// that `WSACleanup` is not called while another socket in this process is still open.
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
    /// Copying takes its own reference rather than sharing one.
    WinsockGuard(const WinsockGuard&) : WinsockGuard() {}
    WinsockGuard& operator=(const WinsockGuard&) = delete;

private:
    inline static std::atomic<int> count_{0};
};

#else

using Socket = int;
inline constexpr Socket kInvalidSocket = -1;

inline void closeSocket(Socket socket) noexcept {
    ::close(socket);
}

inline int lastSocketError() noexcept {
    return errno;
}

/// Nothing to start anywhere but Windows.
struct WinsockGuard {};

#endif

/// Makes a fresh datagram socket into one the output thread can send on. A new socket is
/// neither of these, and both used to be claimed of it (the audit's M20):
///
/// - **Non-blocking.** A blocking `sendto` waits whenever the socket's send buffer is full — a
///   link slower than the traffic, an adapter that is busy or going away — and on the output
///   thread that wait stops the MIDI clock and every rule until the buffer drains. This way
///   the send fails at once with `WSAEWOULDBLOCK` / `EAGAIN`, the sender counts it, and
///   `sendFailure` calls it "the network is not keeping up". A dropped datagram is a missed
///   cue; a stalled output thread is every cue.
/// - **Allowed to broadcast.** A rig built around a broadcast address is one somebody
///   already has, and a socket refuses to send to one until it is told it may.
///
/// Both are asked for and a refusal is ignored. A platform that will not broadcast still
/// sends unicast perfectly well, and a socket left blocking still sends — it can only stall
/// as it always could.
inline void prepareSender(Socket socket) noexcept {
    const int broadcast = 1;
#if defined(_WIN32)
    u_long nonBlocking = 1;
    ::ioctlsocket(socket, FIONBIO, &nonBlocking);
    ::setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
                 sizeof broadcast);
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(socket, F_SETFL, flags | O_NONBLOCK);
    }
    ::setsockopt(socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof broadcast);
#endif
}

/// Why a datagram could not be sent, in words an operator can act on, with the code after
/// it for anyone who has to look it up. For the errors a network that is down, unplugged or
/// wrongly addressed gives; anything else is its code.
inline std::string sendFailure(int error) {
    std::string why;
#if defined(_WIN32)
    switch (error) {
    case WSAENETUNREACH:
        why = "the network is unreachable";
        break;
    case WSAEHOSTUNREACH:
        why = "there is no route to that address";
        break;
    case WSAENETDOWN:
        why = "the network is down";
        break;
    case WSAEADDRNOTAVAIL:
        why = "that is not an address this machine can send to";
        break;
    case WSAEACCES:
        why = "not permitted (a broadcast address?)";
        break;
    case WSAENOBUFS:
    case WSAEWOULDBLOCK:
        why = "the network is not keeping up";
        break;
    default:
        why = "error";
        break;
    }
#else
    switch (error) {
    case ENETUNREACH:
        why = "the network is unreachable";
        break;
    case EHOSTUNREACH:
        why = "there is no route to that address";
        break;
    case ENETDOWN:
        why = "the network is down";
        break;
    case EADDRNOTAVAIL:
        why = "that is not an address this machine can send to";
        break;
    case EACCES:
        why = "not permitted (a broadcast address?)";
        break;
    case ENOBUFS:
    case EAGAIN:
        why = "the network is not keeping up";
        break;
    default:
        why = "error";
        break;
    }
#endif
    return why + " (" + std::to_string(error) + ")";
}

/// "host:port" for an address, numeric, or "?" when it cannot be named. For logs and for
/// a UI that has to say where a message came from.
inline std::string describe(const sockaddr* address, socklen_t length) {
    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    if (::getnameinfo(address, length, host, sizeof host, service, sizeof service,
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "?";
    }
    return std::string(host) + ":" + service;
}

} // namespace takt4::net
