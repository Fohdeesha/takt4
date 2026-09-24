#include "core/output/rule_sink.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takt4::output {

namespace {

/// MIDI status bytes. The low nibble is the channel, counted from zero on the wire and from
/// one by every person who has ever read a manual.
constexpr unsigned char kNoteOff = 0x80;
constexpr unsigned char kNoteOn = 0x90;
constexpr unsigned char kControlChange = 0xB0;
constexpr unsigned char kProgramChange = 0xC0;
constexpr unsigned char kPitchBend = 0xE0;

unsigned char sevenBits(int value) noexcept {
    return static_cast<unsigned char>(std::clamp(value, 0, 127));
}

} // namespace

unsigned char midiStatusFor(trigger::Message::Kind kind, int channel) noexcept {
    const auto wire = static_cast<unsigned char>(std::clamp(channel, 1, 16) - 1);
    unsigned char status = kNoteOn;
    switch (kind) {
    case trigger::Message::Kind::MidiNoteOff:
        status = kNoteOff;
        break;
    case trigger::Message::Kind::MidiCc:
        status = kControlChange;
        break;
    case trigger::Message::Kind::MidiProgramChange:
        status = kProgramChange;
        break;
    case trigger::Message::Kind::MidiPitchBend:
        status = kPitchBend;
        break;
    case trigger::Message::Kind::Osc:
    case trigger::Message::Kind::Dmx:
    case trigger::Message::Kind::MidiNote:
        break;
    }
    return static_cast<unsigned char>(status | wire);
}

std::size_t midiLengthFor(trigger::Message::Kind kind) noexcept {
    return kind == trigger::Message::Kind::MidiProgramChange ? 2 : 3;
}

RuleSink::RuleSink(Transports& transports) noexcept : transports_(transports) {}

void RuleSink::remap(const std::vector<int>& outputs, const std::vector<int>& fixtures) {
    if (!outputs.empty()) {
        std::erase_if(midiQueue_, [&outputs](HeldMidi& held) {
            if (held.target >= outputs.size() || outputs[held.target] < 0) {
                return true; // its output has gone, and so has everywhere to send it
            }
            held.target = static_cast<std::size_t>(outputs[held.target]);
            return false;
        });
    }
    if (!fixtures.empty()) {
        for (HeldDmx& held : dmxQueue_) {
            held.fixtures = trigger::remapBits(held.fixtures, fixtures);
        }
    }
}

void RuleSink::setNow(double now) noexcept {
    now_ = now;
    transports_.osc().setNow(now);
}

void RuleSink::send(const trigger::Message& message) {
    if (message.kind == trigger::Message::Kind::Osc) {
        sendOsc(message);
    } else if (message.kind == trigger::Message::Kind::Dmx) {
        if (!lightingHeld_) {
            sendDmx(message);
        }
    } else {
        sendMidi(message);
    }
}

void RuleSink::startDmx(std::uint64_t fixtures, const dmx::Payload& payload) {
    // So "delivered" means something different for this kind than for the other two: it means
    // the effect reached at least one real channel, not that a datagram left. That is the
    // honest reading — a fade that reaches nothing is exactly as undeliverable as an OSC
    // message routed to a target that is switched off, and looks the same from outside.
    const std::uint64_t before = transports_.dmx().missed();
    transports_.dmx().start(fixtures, payload, now_);
    if (transports_.dmx().missed() != before) {
        ++undeliverable_;
        return;
    }
    ++delivered_;
}

void RuleSink::sendDmx(const trigger::Message& message) {
    // **Nothing goes on a wire here, and that is the whole shape of DMX.** The effect is
    // handed to the engine, which owns the universe buffers and spends the next two bars
    // turning "fade to full" into forty frames a second of slightly different levels.
    // `Transports::advance` is what puts those frames on the network.
    //
    // What can be held is the *start*. An Art-Net node has no delay of its own — see
    // `OutputTarget::delaySeconds` — so only the rig's offset moves it: a beat fired ahead of
    // time on a prediction starts its fade on the beat, or on the beat less a negative offset.
    const double due = message.moment + transports_.latencySeconds();
    if (due <= now_) {
        startDmx(message.fixtures, message.payload);
        return;
    }
    if (dmxQueue_.size() >= kMaxQueued) {
        ++dropped_;
        return;
    }
    dmxQueue_.push_back(HeldDmx{due, message.fixtures, message.payload});
}

void RuleSink::releaseDue(double now) {
    setNow(now);
    // Partitioned rather than erased one at a time, as `OscPublisher::flushDue` does and for
    // the same reason: a round can retire a whole beat's worth.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < midiQueue_.size(); ++i) {
        HeldMidi& held = midiQueue_[i];
        if (held.due > now) {
            if (kept != i) {
                midiQueue_[kept] = held;
            }
            ++kept;
            continue;
        }
        if (MidiOutput* const port = transports_.midiTarget(held.target)) {
            port->send(std::span<const unsigned char>(held.bytes.data(), held.length));
        }
    }
    midiQueue_.resize(kept);

    kept = 0;
    for (std::size_t i = 0; i < dmxQueue_.size(); ++i) {
        HeldDmx& held = dmxQueue_[i];
        if (held.due > now) {
            if (kept != i) {
                dmxQueue_[kept] = held;
            }
            ++kept;
            continue;
        }
        startDmx(held.fixtures, held.payload);
    }
    dmxQueue_.resize(kept);
}

void RuleSink::flushQueued() {
    for (const HeldMidi& held : midiQueue_) {
        if (MidiOutput* const port = transports_.midiTarget(held.target)) {
            port->send(std::span<const unsigned char>(held.bytes.data(), held.length));
        }
    }
    midiQueue_.clear();
    for (const HeldDmx& held : dmxQueue_) {
        startDmx(held.fixtures, held.payload);
    }
    dmxQueue_.clear();
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
    // Spelled as an optional so the overload is chosen by the argument before it, never by a
    // double quietly converting to the int one. The publisher holds each target to its own
    // offset from this moment.
    const std::optional<double> moment{message.moment};
    if (!message.hasArgument) {
        osc.sendAddressTo(message.outputs, message.address, moment);
    } else {
        // The argument goes out as the type the generator produced. An operator who chose a
        // float generator meant a float: OSC is typed, and a host expecting one and given
        // the other ignores the message rather than guessing.
        switch (message.argument.kind()) {
        case trigger::Value::Kind::Float:
            osc.sendAddressTo(message.outputs, message.address, message.argument.asFloat(), moment);
            break;
        case trigger::Value::Kind::Text:
            osc.sendAddressTo(message.outputs, message.address, message.argument.text(), moment);
            break;
        case trigger::Value::Kind::Int:
        case trigger::Value::Kind::Bool:
            osc.sendAddressTo(message.outputs, message.address, message.argument.asInt(), moment);
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
    // **Not every message is three bytes**, which is why this builds a length rather than a
    // fixed array. Program change is two. And the release half of §5.8's press-then-release
    // is a real Note Off: this file used to claim "a note with velocity zero is a note-off on
    // every device made since 1983", and the operator's laser controller is the counter
    // example that cost a show — it holds the clip until `0x80` arrives. See
    // `trigger::Message::Kind`.
    std::array<unsigned char, 3> bytes{midiStatusFor(message.kind, message.channel), 0, 0};
    const std::size_t length = midiLengthFor(message.kind);
    if (message.kind == trigger::Message::Kind::MidiProgramChange) {
        bytes[1] = sevenBits(message.number);
    } else if (message.kind == trigger::Message::Kind::MidiPitchBend) {
        // 14 bits, low seven first. 8192 is centre; the generator's range is the operator's.
        const int bend = std::clamp(message.value, 0, 16383);
        bytes[1] = static_cast<unsigned char>(bend & 0x7F);
        bytes[2] = static_cast<unsigned char>((bend >> 7) & 0x7F);
    } else {
        bytes[1] = sevenBits(message.number);
        bytes[2] = sevenBits(message.value);
    }
    bool sent = false;
    const std::vector<OutputTarget>& targets = transports_.outputs();
    const double latency = transports_.latencySeconds();
    for (std::size_t i = 0; i < targets.size() && i < kMaxRoutableTargets; ++i) {
        if ((message.outputs & (std::uint64_t{1} << i)) == 0) {
            continue; // routed away from this one
        }
        MidiOutput* const port = transports_.midiTarget(i);
        if (port == nullptr) {
            continue;
        }
        sent = true;
        // A MIDI target's own delay, which the window has always offered on its row and which
        // nothing applied until the audit (M9): the desk on the end of a MIDI cable has a lag of
        // its own exactly as a media server on OSC does.
        const double delay =
            std::clamp(targets[i].delaySeconds, kMinOutputDelaySeconds, kMaxOutputDelaySeconds);
        const double due = message.moment + latency + delay;
        if (due <= now_) {
            port->send(std::span<const unsigned char>(bytes.data(), length));
            continue;
        }
        if (midiQueue_.size() >= kMaxQueued) {
            ++dropped_;
            continue;
        }
        midiQueue_.push_back(HeldMidi{due, i, bytes, length});
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
