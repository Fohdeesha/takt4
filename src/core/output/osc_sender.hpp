#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace takt4::output {

/// One UDP target for OSC.
///
/// A datagram socket, nothing more: no library, no event loop. Sending is a single
/// non-blocking `sendto`, so a target that has gone away costs an error return rather than a
/// stall — which matters, because HANDOFF §4.2 puts this on the output thread alongside Link
/// and MIDI, and a blocked socket there would stop the beat.
///
/// **Opening one never blocks either.** A literal address is used at once; a name is looked up
/// on a thread of its own (`net::AsyncAddress`) and the socket opens on the first send after it
/// answers. Until then a send fails, is counted, and `problem()` says why. It used to resolve
/// here, on the output thread, and a name with the venue's DNS down stopped the MIDI clock and
/// every rule for the resolver's timeout (the audit's H12).
class OscSender {
public:
    /// `host` is a name or a literal address, IPv4 or IPv6. Throws std::runtime_error only
    /// when a socket cannot be had at all for a literal one; a name that does not resolve is
    /// not an error here but a `problem()`, because nobody can know that yet.
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
    /// What the host name resolved to, for the UI and for logs. Empty until it has.
    const std::string& resolved() const noexcept { return resolved_; }
    /// Empty when this target can be sent to; otherwise why not — a name still being looked
    /// up, one that would not resolve, or a socket that would not open.
    std::string problem() const;

    std::uint64_t sent() const noexcept { return sent_; }
    std::uint64_t failed() const noexcept { return failed_; }

    /// Opens the socket if the address has become known, and starts another look-up if the
    /// last one failed long enough ago. True when there is a socket to send on. A send does
    /// this itself; it is public so that a target nothing is being sent to still finds its
    /// address, and still says when it cannot.
    bool ready() noexcept;

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
