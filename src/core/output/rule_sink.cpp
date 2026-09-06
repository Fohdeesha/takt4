#include "core/output/rule_sink.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace takt4::output {

namespace {

/// MIDI status bytes. The low nibble is the channel, counted from zero on the wire and from
/// one by every person who has ever read a manual.
constexpr unsigned char kNoteOn = 0x90;
constexpr unsigned char kControlChange = 0xB0;

unsigned char statusFor(trigger::Message::Kind kind, int channel) noexcept {
    const auto wire = static_cast<unsigned char>(std::clamp(channel, 1, 16) - 1);
    return static_cast<unsigned char>(kind == trigger::Message::Kind::MidiCc ? kControlChange
                                                                             : kNoteOn) |
           wire;
}

unsigned char sevenBits(int value) noexcept {
    return static_cast<unsigned char>(std::clamp(value, 0, 127));
}

} // namespace

RuleSink::RuleSink(Transports& transports) noexcept : transports_(transports) {}

void RuleSink::send(const trigger::Message& message) {
    if (message.kind == trigger::Message::Kind::Osc) {
        sendOsc(message);
    } else {
        sendMidi(message);
    }
}

void RuleSink::sendOsc(const trigger::Message& message) {
    OscPublisher& osc = transports_.osc();
    // §5.6's rule subset. A rule routed to a target that is switched off, or named one this
    // rig does not have, reaches nothing — which is *undeliverable* rather than sent, and is
    // the difference between "your routing is wrong" and "your rule never fired".
    if (!osc.anyTargetIn(message.outputs)) {
        ++undeliverable_;
        return;
    }
    if (!message.hasArgument) {
        osc.sendAddressTo(message.outputs, message.address);
    } else {
        // The argument goes out as the type the generator produced. An operator who chose a
        // float generator meant a float: OSC is typed, and a host expecting one and given
        // the other ignores the message rather than guessing.
        switch (message.argument.kind()) {
        case trigger::Value::Kind::Float:
            osc.sendAddressTo(message.outputs, message.address, message.argument.asFloat());
            break;
        case trigger::Value::Kind::Text:
            osc.sendAddressTo(message.outputs, message.address, message.argument.text());
            break;
        case trigger::Value::Kind::Int:
        case trigger::Value::Kind::Bool:
            osc.sendAddressTo(message.outputs, message.address, message.argument.asInt());
            break;
        }
    }
    ++delivered_;
}

void RuleSink::sendMidi(const trigger::Message& message) {
    // §5.6: "Also send configurable note or CC messages on beat and downbeat for MIDI-learn
    // targets." Down whichever of §5.6's targets the rule named, which may be several cables
    // — a rig with a lighting desk and a hardware sequencer on it is two — and which may be
    // the same device the 24 PPQN clock uses. `Transports` opens each device once.
    //
    // A note with velocity zero is a note-off on every device made since 1983, which is
    // exactly what a follow-up of 0 should be, so the release half of §5.8's press-then-
    // release needs no separate status byte.
    const std::array<unsigned char, 3> bytes{statusFor(message.kind, message.channel),
                                             sevenBits(message.number), sevenBits(message.value)};
    bool sent = false;
    const std::vector<OutputTarget>& targets = transports_.outputs();
    for (std::size_t i = 0; i < targets.size() && i < kMaxRoutableTargets; ++i) {
        if ((message.outputs & (std::uint64_t{1} << i)) == 0) {
            continue; // routed away from this one
        }
        if (MidiOutput* const port = transports_.midiTarget(i)) {
            port->send(bytes);
            sent = true;
        }
    }
    if (sent) {
        ++delivered_;
    } else {
        // No MIDI target on this rig, or none the rule is routed to. Counted rather than
        // silent: a rule firing into nothing looks exactly like a rule that never fires.
        ++undeliverable_;
    }
}

} // namespace takt4::output
