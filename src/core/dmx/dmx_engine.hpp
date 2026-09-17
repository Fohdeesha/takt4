#pragma once

#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/effect.hpp"
#include "core/dmx/fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace takt4::dmx {

/// The universe buffers and the effects running against them.
///
/// **This is the piece that makes DMX different from everything else takt4 sends, and it is
/// worth being blunt about why it exists.** OSC and MIDI are *events*: a rule fires, one
/// datagram or three bytes leave, and nothing is owed afterwards. DMX is *state*: a universe
/// is 512 levels that a controller re-sends continuously, and a fixture's brightness is
/// whatever the last frame said it was. There is no "send a fade" on the wire — a fade is the
/// controller sending a slightly different frame forty times a second until it arrives.
///
/// So a rule cannot hand a fade to a socket the way it hands an address to `OscPublisher`.
/// Something has to hold the levels between frames, advance every running effect on a clock,
/// and hand out finished frames; that something is this. `output::Transports` ticks it every
/// round and `ArtNetSender` sends what comes out.
///
/// Not thread-safe, and on the output thread by construction — the same rule `trigger::
/// TriggerEngine` states, and for the same reason: the runner owns that thread, the rules and
/// the transports, and this joins them. A patch edited from a window goes through the runner's
/// command queue exactly as a rule does.
///
/// It allocates when an effect starts (one small vector of tracks) and never in `tick`, which
/// is the call that happens a thousand times a second. §4.2 forbids allocation on the audio
/// thread and nowhere else, so this is a cost decision rather than a correctness one.
class DmxEngine {
public:
    /// A channel index that is not a channel — what `Track::fine` holds on an 8-bit fixture.
    static constexpr std::uint16_t kNoChannel = 0xFFFF;

    /// Replaces the patch.
    ///
    /// **Levels survive it where they can.** A universe that is still patched keeps the bytes
    /// it had, and only channels the *previous* patch did not cover are set to their new
    /// fixture's parked levels. That is what lets an operator add a fixture or fix a channel
    /// map during a set without the rig blinking: re-patching is an edit to the map, not an
    /// instruction to reset the lights. Channels that have stopped being patched are left
    /// where they were and simply stop being sent, if their universe has gone with them.
    ///
    /// Running effects are **cancelled**, because their tracks point at buffers and channels
    /// that may no longer mean the same thing. Cancelling holds levels rather than clearing
    /// them, so this is a pause in the animation and not a blackout.
    void setPatch(std::vector<Fixture> patch);
    const std::vector<Fixture>& patch() const noexcept { return patch_; }

    /// Starts `payload` on every fixture `fixtures` selects, at `now`.
    ///
    /// **Last takes precedence, per channel.** A new effect that writes a channel removes that
    /// channel from whatever was already animating it, and an effect left with no channels is
    /// dropped. Highest-takes-precedence is what a lighting desk does between two *operators*;
    /// between two rules of one operator's own show, "the thing I just fired wins" is the only
    /// behaviour that is predictable from reading the rules — under HTP a fade to 20% would
    /// simply not happen while anything brighter was still running, and nothing on screen
    /// would say why.
    ///
    /// A payload aimed at channels no selected fixture has — a pan on a wash, a color on a
    /// dimmer — starts nothing and is counted in `missed()`. That is not an error: one rule
    /// aimed at a group holding both kinds of fixture is an ordinary thing to want.
    void start(std::uint64_t fixtures, const Payload& payload, double now);

    /// Advances every running effect and writes the frames. Call every round, effect or no
    /// effect — that is what makes a duration mean seconds rather than rounds.
    void tick(double now);

    /// Holds one raw channel at `level` for `seconds`, then puts it back where it was.
    ///
    /// **Addressed by channel and not by role**, which is what makes this the patch editor's
    /// and not a rule's. What an operator is checking from the truss is that *channel 72 is
    /// this fixture's blue* — and two channels of one fixture can carry the same role, or
    /// none at all, and `Role::Unused` is exactly the channel somebody wants to poke at to
    /// find out what it does. A role could not name any of those.
    ///
    /// It goes back on its own, which is the whole point of a test: pressing it during a set
    /// must not leave a channel somewhere the operator has to remember to undo. Nothing is
    /// held across a re-patch — `setPatch` cancels everything, as it does for every effect.
    void holdChannel(PortAddress universe, std::uint16_t channel, std::uint8_t level,
                     double seconds, double now);

    /// Stops every effect and **holds the levels where they are** — §5.8's PANIC, as the
    /// operator asked for it on 2026-09-16: the lights freeze rather than going dark.
    ///
    /// The reasoning is theirs and it is a live-show one. takt4 may be one source among
    /// several on a rig, and a node may be merging it with a desk; a panic button that drove
    /// every channel to zero would fight the desk down and black out a stage that was not
    /// takt4's to black out. Freezing gives back control without taking anything away. A
    /// blackout is available as an effect a rule can fire, which is where a deliberate one
    /// belongs.
    void cancelAll() noexcept;

    /// Every level back to its fixture's parked value, and every effect stopped. For a preset
    /// load, not for panic.
    void reset();

    /// The universes the patch uses, ascending. Stable between `setPatch` calls, which is what
    /// lets `ArtNetSender` keep a per-universe sequence counter by index.
    const std::vector<PortAddress>& universes() const noexcept { return universes_; }

    /// One universe's 512 levels, or an empty span when the patch does not use it.
    std::span<const std::uint8_t> levels(PortAddress universe) const noexcept;

    /// How many times this universe's levels have changed. What tells a sender "this frame is
    /// new, send it now" from "nothing has moved, send it again when the keep-alive is due" —
    /// see `kKeepAliveSeconds`.
    std::uint64_t revision(PortAddress universe) const noexcept;

    /// Effects currently animating.
    std::size_t running() const noexcept { return running_.size(); }
    /// Effects started since construction.
    std::uint64_t started() const noexcept { return started_; }
    /// Effects that reached no channel — aimed at fixtures that have not got them, or at no
    /// fixture at all. Counted rather than silent, for the reason `RuleSink::undeliverable`
    /// gives: a rule firing into nothing looks exactly like a rule that never fires.
    std::uint64_t missed() const noexcept { return missed_; }

private:
    /// One channel an effect is driving, in unit terms so that 8-bit and 16-bit are the same
    /// arithmetic everywhere but the write.
    struct Track {
        std::uint32_t buffer = 0;
        /// 0-based within the universe. `channelOf` hands out 1-based DMX numbers; this is
        /// the one place they are turned into indices.
        std::uint16_t channel = 0;
        /// The low byte of a 16-bit pair, or `kNoChannel`.
        std::uint16_t fine = kNoChannel;
        double from = 0.0;
        double to = 0.0;
        /// Which color component this track carries, for `HueSweep` — which computes a
        /// color per round rather than interpolating three independent numbers.
        Role role = Role::Unused;
    };

    /// A pan and tilt pair, driven together. Movement cannot be expressed as two independent
    /// tracks: a circle is one figure in two channels, and interpolating them separately would
    /// make it a diagonal line.
    struct Move {
        std::uint32_t buffer = 0;
        std::uint16_t pan = kNoChannel;
        std::uint16_t panFine = kNoChannel;
        std::uint16_t tilt = kNoChannel;
        std::uint16_t tiltFine = kNoChannel;
        /// Where the head was when the effect started, as a fraction of its **whole** travel.
        double fromPan = 0.0;
        double fromTilt = 0.0;
        /// Where it is going, or the centre a path orbits — same units.
        double toPan = 0.0;
        double toTilt = 0.0;
        /// This fixture's own movement window, same units. A path's radius is scaled by it, so
        /// one rule aimed at six heads with six different limits stays inside all six.
        double panLow = 0.0;
        double panHigh = 1.0;
        double tiltLow = 0.0;
        double tiltHigh = 1.0;
    };

    struct Running {
        Payload payload;
        double start = 0.0;
        double duration = 0.0;
        std::vector<Track> tracks;
        std::vector<Move> moves;
    };

    struct Buffer {
        PortAddress universe = 0;
        std::array<std::uint8_t, kChannelsPerUniverse> levels{};
        /// Whether the patch reaches each channel. Only patched channels are given parked
        /// levels, and only they survive a re-patch — see `setPatch`.
        std::array<bool, kChannelsPerUniverse> covered{};
        std::uint64_t revision = 0;
    };

    /// The index of `universe` in `buffers_`, or `npos`.
    std::size_t bufferOf(PortAddress universe) const noexcept;
    /// Writes one channel, 8-bit or 16-bit, from a unit value. Bumps the buffer's revision
    /// only when a byte really moved.
    void writeChannel(std::uint32_t buffer, std::uint16_t channel, std::uint16_t fine,
                      double unit) noexcept;
    /// Reads one channel back as a unit value — where a fade starts from.
    double readChannel(std::uint32_t buffer, std::uint16_t channel,
                       std::uint16_t fine) const noexcept;
    /// Fills `running`'s tracks and moves for one fixture. False when this fixture has none
    /// of the channels the payload needs.
    bool buildFor(const Fixture& fixture, const Payload& payload, Running& running);
    /// Applies one running effect at `now`.
    void evaluate(const Running& running, double now);
    /// Drops the channels `running` is about to drive from every effect already driving them.
    void preempt(const Running& running);

    std::vector<Fixture> patch_;
    std::vector<Buffer> buffers_;
    std::vector<PortAddress> universes_;
    std::vector<Running> running_;
    /// Where `start` builds an effect's tracks before deciding whether it reached anything.
    /// Kept as a member so that the *building* reuses one buffer; the effect that survives is
    /// copied into `running_`, which is the one allocation a fire costs.
    Running staging_;
    std::uint64_t started_ = 0;
    std::uint64_t missed_ = 0;
};

} // namespace takt4::dmx
