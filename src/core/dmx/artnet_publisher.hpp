#pragma once

#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/artnet_sender.hpp"
#include "core/dmx/dmx_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace takt4::dmx {

/// Every Art-Net node takt4 sends to, and the clock that decides when.
///
/// The mirror of `output::OscPublisher`, for the protocol that works the other way round.
/// OSC publishes *events* and a target that is quiet is a target with nothing to say; Art-Net
/// publishes *state* and a target that is quiet is a target that has gone away. So this class
/// is mostly a scheduler: two rules, both from the specification, both of which a rig can
/// actually feel.
///
///   * **Never faster than 44 Hz per universe.** *"For a gateway outputting DMX512, this will
///     always be the maximum rate of 44Hz."* A node handed frames faster drops them, and
///     takt4's output thread comes round every millisecond — a thousand frames a second into
///     a node that wants forty-four is how a fade turns into a stutter.
///   * **Never slower than the keep-alive.** A universe nobody has touched is still re-sent
///     every `kKeepAliveSeconds`, because that is what tells a node the controller is alive.
///     Nodes that stop hearing a source hold, fade or release depending on how they are set,
///     and none of those is what an operator meant by "the lights are not changing right now".
///
/// The pacing is per **target and universe**, not per publisher: two nodes fed the same
/// universe keep their own clocks, so one that was added mid-set gets its first frame at once
/// rather than waiting for the other one's turn.
///
/// On the output thread, like everything it touches. Nothing here throws once it is built —
/// a node that has been unplugged is counted, not raised, because one dead node must not stop
/// the others being fed.
class ArtNetPublisher {
public:
    struct TargetConfig {
        std::string host;
        std::uint16_t port = kArtNetPort;
        /// Which universes this node is fed. **Empty means every universe the patch uses**,
        /// which is the right default for the ordinary rig of one node, and the reason the
        /// field can be left alone until there are two.
        std::vector<PortAddress> universes;
        /// Which bit of a rule's routing mask selects this target. Unused today — a DMX rule
        /// is routed by *fixture*, and a fixture already names its universe — and carried so
        /// that switching a target off is the same gesture here as everywhere else.
        std::size_t bit = 0;
        /// `output::OutputTarget::id`, which is what `setTargets` recognises a node by.
        std::string id;
    };

    /// Adds a node. Throws `std::runtime_error` only when a socket cannot be had at all; a
    /// name that does not resolve is the sender's `problem()`, not an error here.
    void addTarget(const TargetConfig& config);

    /// Replaces the nodes with `configs`, **keeping a node's sender and its pacing while its id
    /// and its host and port are the same** (the audit's H12). Rebuilding them all on every
    /// output edit reset every clock and sequence number, so a dragged slider on another row
    /// sent frames faster than the 44 Hz a node takes. Returns, for each config whose sender
    /// could not be made, its bit and why.
    std::vector<std::pair<std::size_t, std::string>>
    setTargets(const std::vector<TargetConfig>& configs);

    /// Lets every node find its address and open its socket — see `ArtNetSender::ready`.
    void refresh() noexcept;

    /// Which output node `index` is — its routing bit.
    std::size_t outputOf(std::size_t index) const noexcept { return targets_[index].bit; }

    /// Removes every node and forgets every pacing clock, so a node added afterwards is sent
    /// a frame immediately rather than waiting out a keep-alive it was not there for.
    void clearTargets() noexcept;

    std::size_t targetCount() const noexcept { return targets_.size(); }
    const ArtNetSender& target(std::size_t index) const noexcept { return *targets_[index].sender; }
    /// The universes target `index` carries, as configured — empty for "all of them".
    const std::vector<PortAddress>& universesOf(std::size_t index) const noexcept {
        return targets_[index].universes;
    }

    /// Sends every frame that is due, and says how many datagrams left. Call every round.
    std::size_t publish(const DmxEngine& engine, double now);

    /// Sends every universe's current frame to every node **now**, whatever the pacing says —
    /// the last thing takt4 transmits on the way out. A frame changed a millisecond after the
    /// last one sent would otherwise wait out the 44 Hz spacing for a round that never comes,
    /// and a node left holding the frame before the blackout holds the rig lit (the audit's H6).
    std::size_t flush(const DmxEngine& engine, double now);

    /// Datagrams that left, and datagrams a socket refused.
    std::uint64_t sent() const noexcept { return sent_; }
    std::uint64_t failed() const noexcept { return failed_; }

private:
    /// What one target knows about one universe: which frame it last sent and when.
    struct Paced {
        PortAddress universe = 0;
        double lastSentAt = -1.0;
        std::uint64_t lastRevision = 0;
        bool everSent = false;
    };

    struct Target {
        std::unique_ptr<ArtNetSender> sender;
        std::vector<PortAddress> universes;
        std::size_t bit = 0;
        std::string id;
        /// One per universe actually being fed, found or made on the first frame. Held per
        /// target so that two nodes on one universe pace independently.
        std::vector<Paced> paced;
    };

    /// This target's clock for `universe`, made if it is the first frame.
    Paced& pacedFor(Target& target, PortAddress universe);

    std::vector<Target> targets_;
    std::uint64_t sent_ = 0;
    std::uint64_t failed_ = 0;
};

} // namespace takt4::dmx
