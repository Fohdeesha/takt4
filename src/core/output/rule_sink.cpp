#include "core/output/rule_sink.hpp"

#include <algorithm>
#include <array>

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
    if (osc.targetCount() == 0) {
        ++undeliverable_;
        return;
    }
    if (!message.hasArgument) {
        osc.sendAddress(message.address);
    } else {
        // The argument goes out as the type the generator produced. An operator who chose a
        // float generator meant a float: OSC is typed, and a host expecting one and given
        // the other ignores the message rather than guessing.
        switch (message.argument.kind()) {
        case trigger::Value::Kind::Float:
            osc.sendAddress(message.address, message.argument.asFloat());
            break;
        case trigger::Value::Kind::Text:
            osc.sendAddress(message.address, message.argument.text());
            break;
        case trigger::Value::Kind::Int:
        case trigger::Value::Kind::Bool:
            osc.sendAddress(message.address, message.argument.asInt());
            break;
        }
    }
    ++delivered_;
}

void RuleSink::sendMidi(const trigger::Message& message) {
    // §5.6: "Also send configurable note or CC messages on beat and downbeat for MIDI-learn
    // targets." The same port the clock goes to — a second port would be a second setting
    // for no reason anyone has asked for, and a MIDI-learn target is listening to one cable.
    MidiOutput* port = transports_.midiPort();
    if (port == nullptr) {
        ++undeliverable_;
        return;
    }
    // A note with velocity zero is a note-off on every device made since 1983, which is
    // exactly what a follow-up of 0 should be — so the release half of §5.8's press-then-
    // release needs no separate status byte.
    const std::array<unsigned char, 3> bytes{statusFor(message.kind, message.channel),
                                             sevenBits(message.number), sevenBits(message.value)};
    port->send(bytes);
    ++delivered_;
}

} // namespace takt4::output
