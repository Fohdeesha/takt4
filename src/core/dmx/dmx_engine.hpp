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
    /// **Levels survive it where they still mean the same thing.** A channel that is patched
    /// before and after — same position in its fixture, same role, same parked level — keeps the
    /// byte it had, which is what lets an operator add a fixture or rename one during a set
    /// without the rig blinking: re-patching is an edit to the map, not an instruction to reset
    /// the lights.
    ///
    /// Everything else is put right (the audit's H8). A channel newly patched, or whose role or
    /// parked level changed — a head re-moded from 8-bit to 16-bit, a parked shutter edited —
    /// takes its fixture's parked level; it used to keep the old byte until a restart, so the
    /// re-moded head inherited a closed shutter. And a channel no enabled fixture covers any
    /// more goes to **zero**: a fixture switched off or deleted used to go on transmitting its
    /// last levels, out of reach of every rule, Blackout included.
    ///
    /// **So does a whole universe the patch no longer uses** (the audit of 2026-09-25, H4). A
    /// rig with one par on a node — an ordinary small rig — lost that universe from the patch
    /// when the par was switched off or deleted, and nothing ever sent it again: the node held
    /// its last frame, and the lamp stayed lit through Stop, quit and Blackout. Such a universe
    /// is `released()`: sent all zeros for `kReleasedSeconds`, a fresh frame at the 44 Hz
    /// ceiling so one dropped datagram costs nothing, and then left alone — the operator's
    /// answer to the audit's Q2, so that anything else that takes the universe over is not
    /// fought for it.
    ///
    /// Running effects **carry on**, re-aimed at the same fixture — by `Fixture::id`, so a
    /// rename is not a different fixture — in the new patch; they were all cancelled, so every
    /// fade, strobe and path froze where it was whenever a name was typed. An effect on a fixture that has gone, or cannot take it any more, stops
    /// there. A channel TEST hold is let go, and its channel put back where it was.
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
    void start(const FixtureSet& fixtures, const Payload& payload, double now);

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
    /// must not leave a channel somewhere the operator has to remember to undo. Nor is it held
    /// across a re-patch: `setPatch` lets go of it and puts the channel back first — a hold left
    /// running then was a TEST stuck on (the audit's H8).
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

    /// Every effect stopped and every light-emitting channel of every fixture taken to zero —
    /// what the Blackout effect does, to the whole patch at once, and nothing that configures a
    /// fixture: a shutter or a head's position stays where it is. **Stop and quit**, the
    /// operator's call of 2026-09-23 (the audit's Q3); PANIC is `cancelAll`, which freezes.
    void blackout(double now);

    /// Every level back to its fixture's parked value, and every effect stopped. **Only the
    /// tests call it**, to start a case from the rig as patched: a preset load goes through
    /// `setPatch`, and panic is `cancelAll` (the audit of 2026-09-25's stale-comment list).
    void reset();

    /// The universes the patch uses, ascending. Stable between `setPatch` calls.
    const std::vector<PortAddress>& universes() const noexcept { return universes_; }

    /// How long a universe that has left the patch goes on being sent zeros. Three seconds is
    /// about 130 frames: any node that is listening hears one. See `setPatch`.
    static constexpr double kReleasedSeconds = 3.0;
    /// The universes that have left the patch and are still being sent zeros — see `setPatch`.
    /// `levels` and `revision` answer for them as for any other; `tick` lets each go
    /// `kReleasedSeconds` after the first tick that saw it released.
    const std::vector<PortAddress>& released() const noexcept { return releasedUniverses_; }

    /// One universe's 512 levels — all zero for a released one — or an empty span when the patch
    /// does not use it.
    std::span<const std::uint8_t> levels(PortAddress universe) const noexcept;

    /// How many times this universe's levels have changed. What tells a sender "this frame is
    /// new, send it now" from "nothing has moved, send it again when the keep-alive is due" —
    /// see `kKeepAliveSeconds`. A released universe's moves on every tick, so it is sent at
    /// the 44 Hz ceiling while it is being released.
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
    /// A fixture index that is not one — a TEST hold, which is addressed by channel.
    static constexpr std::uint16_t kNoFixture = 0xFFFF;
    /// `Track::component` for a virtual dimmer's intensity; a color component is 1 + its
    /// index in `kEmitters`.
    static constexpr std::uint8_t kIntensity = 0;

    /// One channel an effect is driving, in unit terms so that 8-bit and 16-bit are the same
    /// arithmetic everywhere but the write — or one half of a **virtual** fixture's state.
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
        /// Which fixture of the patch it was built for, and the role it addressed there — what
        /// lets `setPatch` re-aim it at the same fixture in a new patch. `kNoFixture` for a
        /// channel TEST hold.
        std::uint16_t fixture = kNoFixture;
        Role source = Role::Unused;
        /// Which of the fixture's channels carrying `source` this is (`channelOf`'s `nth`): a
        /// bar of cells has a track per cell.
        std::uint8_t nth = 0;
        /// Whether this drives a virtual fixture's state (`Virtual`) rather than a channel, and
        /// which part of it: `kIntensity`, or 1 + a `kEmitters` index.
        bool isVirtual = false;
        std::uint8_t component = 0;
    };

    /// A pan and tilt pair, driven together. Movement cannot be expressed as two independent
    /// tracks: a circle is one figure in two channels, and interpolating them separately would
    /// make it a diagonal line.
    struct Move {
        /// See `Track::fixture`.
        std::uint16_t fixture = kNoFixture;
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

    /// What the patch says a channel is: its place in its fixture, its role and its parked
    /// level. A channel whose meaning moved is re-parked by `setPatch`; one that kept it keeps
    /// its level.
    struct Meaning {
        bool covered = false;
        std::uint16_t position = 0;
        Role role = Role::Unused;
        std::uint8_t parked = 0;
        friend bool operator==(const Meaning&, const Meaning&) = default;
    };

    struct Buffer {
        PortAddress universe = 0;
        std::array<std::uint8_t, kChannelsPerUniverse> levels{};
        /// What the patch makes of each channel — see `Meaning` and `setPatch`.
        std::array<Meaning, kChannelsPerUniverse> meaning{};
        std::uint64_t revision = 0;
    };

    /// **A fixture with no dimmer channel that emits light** — the LED par — has its brightness
    /// kept here, apart from its color, and its emitter channels written as color × intensity.
    ///
    /// It used to take the hue for a dimmer effect from the channels' *current* levels, so once
    /// a flash had decayed to zero the fixture read as dark and was taken as white: a color rule
    /// and a beat flash gave the chosen color on the first flash and white on every one after
    /// (the audit's H9). A color effect now moves the color and a dimmer effect the intensity,
    /// and neither forgets the other.
    struct Virtual {
        bool active = false;
        double intensity = 1.0;
        std::array<double, kEmitters.size()> color{};
        /// Whether the channels need writing again from the two above.
        bool dirty = false;
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
    /// Fills `running`'s tracks and moves for one fixture — the `index`th of the patch. False
    /// when this fixture has none of the channels the payload needs.
    bool buildFor(std::size_t index, const Payload& payload, Running& running);
    /// Applies one running effect at `now`.
    void evaluate(const Running& running, double now);
    /// Writes one track's value, to its channel or to its virtual fixture's state.
    void writeTrack(const Track& track, double unit) noexcept;
    /// Writes every virtual fixture whose state moved to its channels, as color × intensity.
    void composeVirtuals() noexcept;
    /// A virtual fixture's state, read from its channels as they stand — for a new patch.
    Virtual virtualFromLevels(const Fixture& fixture) const;
    /// Drops the channels `running` is about to drive from every effect already driving them.
    void preempt(const Running& running);
    /// Lets go of every channel TEST hold (`holdChannel`), each channel put back where its
    /// hold found it. A patch change, PANIC and Stop all do this first.
    void releaseTestHolds() noexcept;
    /// `start`, on the fixtures `fixtures` names — or, with `everyFixture`, on every enabled
    /// fixture of the patch, past the 64 a mask can name. `blackout`'s.
    void launch(const Payload& payload, double now, const FixtureSet& fixtures, bool everyFixture);
    /// Re-aims every running effect at the new patch, by fixture — see `setPatch`.
    /// `mapped[i]` is where the old patch's fixture `i` is in the new one, or `kNoFixture`.
    void retarget(const std::vector<std::uint16_t>& mapped);

    std::vector<Fixture> patch_;
    /// One per fixture of `patch_`; see `Virtual`.
    std::vector<Virtual> virtuals_;
    std::vector<Buffer> buffers_;
    std::vector<PortAddress> universes_;
    /// A universe that has left the patch, being sent zeros — see `released`.
    struct Released {
        PortAddress universe = 0;
        /// When it stops being sent; negative until the first `tick` after it was released,
        /// which is the first moment this object knows the time.
        double until = -1.0;
        std::uint64_t revision = 0;
    };
    std::vector<Released> released_;
    /// `released_`'s universes, in the same order, for `released()` to hand out whole.
    std::vector<PortAddress> releasedUniverses_;
    std::vector<Running> running_;
    /// Where `start` builds an effect's tracks before deciding whether it reached anything.
    /// Kept as a member so that the *building* reuses one buffer; the effect that survives is
    /// copied into `running_`, which is the one allocation a fire costs.
    Running staging_;
    std::uint64_t started_ = 0;
    std::uint64_t missed_ = 0;
};

} // namespace takt4::dmx
