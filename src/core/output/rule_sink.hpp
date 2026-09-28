#pragma once

#include "core/output/transports.hpp"
#include "core/trigger/rule.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace takt4::output {

/// The status byte a kind and a channel put on the wire: `0x80` note off, `0x90` note on,
/// `0xB0` control change, `0xC0` program change, `0xE0` pitch bend, with the channel counted
/// from one by people and from zero on the wire.
///
/// Out here rather than inside the .cpp so a test can state the mapping directly. It is the
/// half of the note-off fix that a counter cannot see: a rule can decide to release and
/// still put `0x90` on the cable, which is exactly the bug that left the operator's laser
/// clip latched on.
unsigned char midiStatusFor(trigger::Message::Kind kind, int channel) noexcept;

/// How many bytes that kind occupies. Program change is two; everything else is three.
std::size_t midiLengthFor(trigger::Message::Kind kind) noexcept;

/// The adapter between §5.8's rules and §5.6's transports: one `trigger::Message` becomes
/// one OSC datagram or one MIDI message.
///
/// **It exists so that neither side has to know the other.** `trigger` builds messages and
/// has no business owning a socket; `Transports` sends §5.6's namespace and has no business
/// knowing what a rule is. This is the one file that includes both, which is what keeps
/// `core/output` out of `core/trigger`'s headers and lets a test drive the whole rule engine
/// against a recording sink instead.
///
/// On the output thread, like everything it touches. Nothing here throws: an OSC target that
/// has gone away or a MIDI port that has been unplugged is counted, not raised, because a
/// rule failing to reach one host must not stop it reaching the others.
///
/// **When each message goes is decided here, and only from `Message::moment`.** Every target
/// has a message at its moment plus §5.5's rig-wide offset plus the target's own delay, or at
/// once if that has already gone. OSC holds its datagrams in `OscPublisher`; MIDI and the start
/// of a lighting effect are held here, in `releaseDue`. The output thread fires a beat early —
/// on a prediction, by as much as the earliest target needs — so a negative offset lands before
/// the beat it belongs to rather than before the one after (the audit's H4).
class RuleSink final : public trigger::Sink {
public:
    /// The transports must outlive this.
    explicit RuleSink(Transports& transports) noexcept;

    void send(const trigger::Message& message) override;

    /// The output thread's clock, which is what every hold is measured against — and where on
    /// it a lighting effect starts, since an effect **starts** at an instant and runs for a
    /// duration.
    ///
    /// Set by `OutputRunner` at the top of every round and before every command it applies,
    /// before any rule is evaluated. The OSC publisher is told the same, so a datagram and a
    /// note sent in one round agree about what "now" is.
    void setNow(double now) noexcept;

    /// Sends every held MIDI message, and starts every held lighting effect, whose time has
    /// come. Every round, after the rules.
    void releaseDue(double now);
    /// Sends and starts everything held, now, whatever it was waiting for. For a PANIC and a
    /// stop, where a release held back for a delayed target must still go; and before the
    /// targets or the patch are replaced, which would change what a held message's output bit
    /// or fixture mask names.
    void flushQueued();
    /// Moves what is held after the outputs (`outputs`) or the patch (`fixtures`) changed —
    /// see `trigger::remapBits`. A held MIDI message follows its output to where it now sits,
    /// and is dropped with it when it is gone; a held effect's fixtures likewise. Empty leaves
    /// that half alone. It used to be flushed instead, so a note off owed a beat later went out
    /// the moment an output was renamed.
    void remap(const std::vector<int>& outputs, const std::vector<int>& fixtures);
    /// Forgets every held lighting effect without starting it. For a PANIC, which freezes the
    /// lights where they are: an effect started only to be frozen on its first frame would be a
    /// flash held at full.
    void dropQueuedLighting() noexcept { dmxQueue_.clear(); }
    /// While true, lighting is not sent: for the moment a PANIC pays out every release it owes,
    /// which it does so that a clip or a laser note lets go. The lights are to be frozen where
    /// they are (the operator's call, 2026-09-16), and a lighting release is the fired effect
    /// again at the release level — on a rule that snaps, a lamp turned off (the audit's M6).
    /// Most are held here until their moment and dropped with `dropQueuedLighting`; this is
    /// for the rest — a release owed a little ahead on a rig whose offset is further ahead
    /// still, whose moment has already passed and which would otherwise be started at once.
    void holdLighting(bool held) noexcept { lightingHeld_ = held; }
    /// MIDI messages and lighting effects waiting for their time.
    std::size_t queued() const noexcept { return midiQueue_.size() + dmxQueue_.size(); }

    /// Messages handed to a transport that was actually switched on. A rule firing into an
    /// app with no OSC target and no MIDI port sends nothing, and says so here rather than
    /// looking like a rule that never fired.
    std::uint64_t delivered() const noexcept { return delivered_; }
    /// Messages with nowhere to go — no OSC target, or no MIDI port open.
    std::uint64_t undeliverable() const noexcept { return undeliverable_; }
    /// Held messages dropped because too many were already waiting. See `kMaxQueued`.
    std::uint64_t dropped() const noexcept { return dropped_; }

    /// How many held messages may wait at once, of each kind. A second of delay on a rule
    /// firing every 32nd note at 215 BPM is 115; past this something upstream is wrong, and
    /// dropping the newest with a count beats growing a queue on the thread with a MIDI clock.
    static constexpr std::size_t kMaxQueued = 512;

private:
    void sendOsc(const trigger::Message& message);
    void sendMidi(const trigger::Message& message);
    void sendDmx(const trigger::Message& message);
    /// Starts one effect now, counting whether it reached a real channel.
    void startDmx(const dmx::FixtureSet& fixtures, const dmx::Payload& payload);

    Transports& transports_;
    /// See `setNow`. Zero until the first round, which only matters offline: an effect started
    /// at zero runs its duration from zero, and the first `tick` is at zero too.
    double now_ = 0.0;
    std::uint64_t delivered_ = 0;
    std::uint64_t undeliverable_ = 0;
    std::uint64_t dropped_ = 0;
    bool lightingHeld_ = false;

    /// One MIDI message held for one target. The target's index rather than its port: the
    /// port is looked up again when it goes, and `flushQueued` runs before the list changes,
    /// so the index still names the device it was meant for.
    struct HeldMidi {
        double due = 0.0;
        std::size_t target = 0;
        std::array<unsigned char, 3> bytes{};
        std::size_t length = 0;
    };
    /// One lighting effect held until its start.
    struct HeldDmx {
        double due = 0.0;
        dmx::FixtureSet fixtures{};
        dmx::Payload payload;
    };
    /// In the order queued, and released in that order among whatever has come due — the rule
    /// `OscPublisher` keeps for its own queue, for the same reason.
    std::vector<HeldMidi> midiQueue_;
    std::vector<HeldDmx> dmxQueue_;
};

} // namespace takt4::output
