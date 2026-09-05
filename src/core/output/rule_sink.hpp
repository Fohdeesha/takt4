#pragma once

#include "core/output/transports.hpp"
#include "core/trigger/rule.hpp"

#include <cstdint>

namespace takt4::output {

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

    /// Messages handed to a transport that was actually switched on. A rule firing into an
    /// app with no OSC target and no MIDI port sends nothing, and says so here rather than
    /// looking like a rule that never fired.
    std::uint64_t delivered() const noexcept { return delivered_; }
    /// Messages with nowhere to go — no OSC target, or no MIDI port open.
    std::uint64_t undeliverable() const noexcept { return undeliverable_; }

private:
    void sendOsc(const trigger::Message& message);
    void sendMidi(const trigger::Message& message);

    Transports& transports_;
    std::uint64_t delivered_ = 0;
    std::uint64_t undeliverable_ = 0;
};

} // namespace takt4::output
