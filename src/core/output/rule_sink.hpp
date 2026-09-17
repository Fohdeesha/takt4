#pragma once

#include "core/output/transports.hpp"
#include "core/trigger/rule.hpp"

#include <cstddef>
#include <cstdint>

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
class RuleSink final : public trigger::Sink {
public:
    /// The transports must outlive this.
    explicit RuleSink(Transports& transports) noexcept;

    void send(const trigger::Message& message) override;

    /// The output thread's clock, for the one kind of message that needs to know when it is.
    ///
    /// OSC and MIDI are instants: a message is sent and that is the whole of it. A lighting
    /// effect **starts** at an instant and runs for a duration, so the engine has to be told
    /// where on its clock the fade begins — and a `trigger::Sink` deliberately does not carry
    /// a time, because the two kinds that existed before this one had no use for one.
    ///
    /// Set by `OutputRunner` at the top of every round, before any rule is evaluated, so a
    /// fire in this round starts on this round's clock.
    void setNow(double now) noexcept { now_ = now; }

    /// Messages handed to a transport that was actually switched on. A rule firing into an
    /// app with no OSC target and no MIDI port sends nothing, and says so here rather than
    /// looking like a rule that never fired.
    std::uint64_t delivered() const noexcept { return delivered_; }
    /// Messages with nowhere to go — no OSC target, or no MIDI port open.
    std::uint64_t undeliverable() const noexcept { return undeliverable_; }

private:
    void sendOsc(const trigger::Message& message);
    void sendMidi(const trigger::Message& message);
    void sendDmx(const trigger::Message& message);

    Transports& transports_;
    /// See `setNow`. Zero until the first round, which only matters offline: an effect started
    /// at zero runs its duration from zero, and the first `tick` is at zero too.
    double now_ = 0.0;
    std::uint64_t delivered_ = 0;
    std::uint64_t undeliverable_ = 0;
};

} // namespace takt4::output
