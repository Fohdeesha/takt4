#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace takt4::output {

/// One UDP target for OSC, resolved once when it is opened.
///
/// A datagram socket, nothing more: no library, no event loop, no retries. Sending is a
/// single non-blocking `sendto`, so a target that has gone away costs an error return
/// rather than a stall — which matters, because HANDOFF §4.2 puts this on the output
/// thread alongside Link and MIDI, and a blocked socket there would stop the beat.
///
/// Not for the audio thread. Opening one resolves a host name, which can block.
class OscSender {
public:
    /// Resolves `host` (a name or a literal address, IPv4 or IPv6) and opens a socket
    /// for it. Throws std::runtime_error if either fails.
    OscSender(std::string_view host, std::uint16_t port);
    ~OscSender();

    OscSender(const OscSender&) = delete;
    OscSender& operator=(const OscSender&) = delete;
    OscSender(OscSender&& other) noexcept;
    OscSender& operator=(OscSender&& other) noexcept;

    /// Sends one datagram. False when the socket refused it; the counters below say how
    /// often that has happened, and the caller is expected to carry on regardless.
    bool send(std::span<const std::byte> packet) noexcept;

    const std::string& host() const noexcept { return host_; }
    std::uint16_t port() const noexcept { return port_; }
    /// What the host name resolved to, for the UI and for logs.
    const std::string& resolved() const noexcept { return resolved_; }

    std::uint64_t sent() const noexcept { return sent_; }
    std::uint64_t failed() const noexcept { return failed_; }

private:
    struct Impl;
    Impl* impl_ = nullptr; // a socket handle and the resolved address; see the .cpp
    std::string host_;
    std::string resolved_;
    std::uint16_t port_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t failed_ = 0;
};

} // namespace takt4::output
