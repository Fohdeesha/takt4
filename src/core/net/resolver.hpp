#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace takt4::net {

/// A host and port turned into an address **without blocking the thread that asks.**
///
/// An OSC target or an Art-Net node used to be resolved when it was opened, and it is opened
/// on the output thread — the one that also runs the MIDI clock, Link, Art-Net's keep-alive and
/// every rule. A target typed as a name while the venue's DNS was down stopped all of it for the
/// resolver's timeout, and again for every edit queued behind it (the audit's H12).
///
/// So a numeric address — an IPv4 or IPv6 literal, which is what nearly every rig types — is
/// taken apart here and now, which costs nothing and asks no one. A name is looked up on a
/// thread of its own; until it answers there is no address, and a failed look-up is tried again
/// in the background `kRetrySeconds` after it failed, so a target whose DNS comes back later
/// starts working without anybody touching it.
///
/// Where a name has both, an IPv4 address is preferred: a receiver on "localhost" is nearly
/// always listening on 127.0.0.1, and `::1` reaches nothing there (the audit's M20).
class AsyncAddress {
public:
    /// How long after a failed look-up the next one starts, at the earliest.
    static constexpr double kRetrySeconds = 5.0;

    /// One resolved UDP address, in the form `sendto` takes.
    struct Address {
        /// `sockaddr_storage`'s bytes. Held opaquely so this header does not pull in the
        /// platform's socket headers; `core/net/udp.hpp` is where those live.
        alignas(8) unsigned char storage[128]{};
        int length = 0;
        /// `AF_INET` or `AF_INET6`, for the socket that will send to it.
        int family = 0;
    };

    AsyncAddress(std::string host, std::uint16_t port);
    ~AsyncAddress();

    AsyncAddress(const AsyncAddress&) = delete;
    AsyncAddress& operator=(const AsyncAddress&) = delete;

    /// The address once it is known, or nothing. Never blocks; starts a retry when the last
    /// look-up failed long enough ago.
    std::optional<Address> address();

    /// Empty once there is an address; otherwise what is happening instead — still looking the
    /// name up, or why that failed — worded for a status line.
    std::string problem() const;

    const std::string& host() const noexcept { return host_; }
    std::uint16_t port() const noexcept { return port_; }

private:
    struct State;
    void lookUp();

    std::string host_;
    std::uint16_t port_ = 0;
    /// Shared with the look-up thread, which may outlive this object: a target removed while
    /// its name is still being resolved leaves the thread to finish into a state nobody reads.
    std::shared_ptr<State> state_;
};

} // namespace takt4::net
