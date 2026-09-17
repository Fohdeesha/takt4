#pragma once

#include "core/dmx/artnet_packet.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace takt4::dmx {

/// One Art-Net node, resolved once when it is opened.
///
/// A datagram socket and an ArtDmx encoder, nothing more: no discovery, no event loop, no
/// retries. Sending is a single non-blocking `sendto`, so a node that has been unplugged costs
/// an error return rather than a stall — which matters, because this sits on §4.2's output
/// thread beside Link and MIDI and a blocked socket there would stop the beat.
///
/// **takt4 does not discover nodes, and that is a decision rather than a gap.** Art-Net 4 has
/// a controller broadcast ArtPoll every 2.5 to 3 seconds and build its list of destinations
/// from the ArtPollReply packets that come back. takt4 asks the operator for an IP instead,
/// for two reasons. A show rig is known in advance — the node has a fixed address written on a
/// label on the back of it — so discovery answers a question nobody was asking. And ArtPoll is
/// a broadcast, on a laptop that is usually also on a venue's wifi or a tour network; takt4
/// putting 20 broadcasts a minute onto a network it was not invited to is worse manners than
/// asking for an address once.
///
/// The one thing that follows from it: **the unicast rule cuts both ways.** The specification
/// says *"ArtDmx packets must be unicast to subscribers of the specific universe ... There are
/// no conditions in which broadcast is allowed"*, and since takt4 has no subscriber list, the
/// address the operator typed *is* the subscription. Point it at a node's own IP and it is
/// exactly what the specification asks for. Point it at a broadcast address and it will do
/// that too, because some rigs are built that way and refusing would help nobody — but it is
/// the operator's instruction and not takt4's default.
///
/// Not for the audio thread. Opening one resolves a host name, which can block.
class ArtNetSender {
public:
    /// Resolves `host` (a name or a literal address) and opens a socket for it. Throws
    /// `std::runtime_error` if either fails.
    ///
    /// `port` is almost always `kArtNetPort`; it is settable because a few software nodes
    /// listen elsewhere and because two nodes behind one NAT have to be told apart somehow.
    ArtNetSender(std::string_view host, std::uint16_t port = kArtNetPort);
    ~ArtNetSender();

    ArtNetSender(const ArtNetSender&) = delete;
    ArtNetSender& operator=(const ArtNetSender&) = delete;
    ArtNetSender(ArtNetSender&& other) noexcept;
    ArtNetSender& operator=(ArtNetSender&& other) noexcept;

    /// Encodes one ArtDmx packet and sends it. False when the socket refused it or the frame
    /// could not be encoded; the counters below say how often, and the caller carries on —
    /// one node that has gone away must not stop the others being fed.
    ///
    /// `levels` is 1 to 512 channels. The sequence number is this sender's own, per universe:
    /// the specification has the receiver use it to re-order packets, and two universes share
    /// no ordering, so a single counter across all of them would make every universe's stream
    /// look like it had gaps in it.
    bool sendDmx(PortAddress universe, std::span<const std::uint8_t> levels) noexcept;

    const std::string& host() const noexcept { return host_; }
    std::uint16_t port() const noexcept { return port_; }
    /// What the host name resolved to, for the UI and for logs.
    const std::string& resolved() const noexcept { return resolved_; }

    std::uint64_t sent() const noexcept { return sent_; }
    std::uint64_t failed() const noexcept { return failed_; }

private:
    struct Impl;
    Impl* impl_ = nullptr; // a socket handle, the resolved address and the sequence counters
    std::string host_;
    std::string resolved_;
    std::uint16_t port_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t failed_ = 0;
};

} // namespace takt4::dmx
