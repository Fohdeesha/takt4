#include "core/output/midi_clock.hpp"

#include "core/output/midi_ports.hpp"
#include "core/sandbox.hpp"

#include <RtMidi.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::output {

namespace {

/// A port specification is an index or a name — see `findMidiPort`. Returns the port number,
/// or throws with the list of what was actually there — the useful message when a
/// device is unplugged or named differently than the operator expected.
unsigned int findPort(RtMidiOut& out, std::string_view spec) {
    const unsigned int count = out.getPortCount();
    std::vector<std::string> names;
    names.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        names.push_back(out.getPortName(i));
    }
    if (const std::optional<std::size_t> found = findMidiPort(names, spec)) {
        return static_cast<unsigned int>(*found);
    }

    std::string message = "MIDI output: no port matching \"" + std::string(spec) + "\"; ";
    if (names.empty()) {
        message += "there are none";
    } else {
        message += "there are " + std::to_string(names.size()) + ":";
        for (std::size_t i = 0; i < names.size(); ++i) {
            message += "\n  " + std::to_string(i) + ": " + names[i];
        }
    }
    throw std::runtime_error(message);
}

} // namespace

namespace {

/// RtMidi's output port, as a `MidiPort`.
class RtMidiPort final : public MidiPort {
public:
    RtMidiPort() {
        try {
            out_ = std::make_unique<RtMidiOut>();
        } catch (const RtMidiError& error) {
            // A machine can have no usable MIDI API at all — a headless Linux box without an
            // ALSA sequencer is the ordinary case, and CI runs on one — and RtMidi reports
            // that by throwing from its own constructor rather than offering an empty port
            // list. listMidiOutputPorts() swallows the same thing to keep a listing simple;
            // here it has to become an error the caller can print, and it is kept for
            // `open` so that the error can say which port was wanted.
            unusable_ = error.getMessage();
        }
    }

    std::string open(std::string_view spec) override {
        if (!out_) {
            throw std::runtime_error("MIDI output: cannot open \"" + std::string(spec) +
                                     "\": no usable MIDI API on this machine (" + unusable_ +
                                     ")");
        }
        try {
            // Looked up afresh every time: a device that went away and came back can come
            // back at another index, and RtMidi enumerates again on every count.
            const unsigned int index = findPort(*out_, spec);
            std::string name = out_->getPortName(index);
            if (sandbox::active()) {
                // The test binaries' sandbox: this port is one of the rig's. See `sandbox.hpp`.
                sandbox::refuse(sandbox::Refused::Midi);
                throw std::runtime_error("MIDI output: \"" + name +
                                         "\" is not opened in the test sandbox");
            }
            out_->openPort(index, "takt4");
            return name;
        } catch (const RtMidiError& error) {
            throw std::runtime_error("MIDI output: " + error.getMessage());
        }
    }

    void close() noexcept override {
        if (!out_) {
            return;
        }
        try {
            out_->closePort();
        } catch (...) {
            // A port that is already gone cannot be closed any more gone.
        }
    }

    void send(std::span<const unsigned char> message) override {
        if (!out_) {
            throw std::runtime_error("MIDI output: no usable MIDI API on this machine");
        }
        // RtMidi's Windows port returns without a word when it is not open (C1), so the question
        // is asked here: a message that goes nowhere has failed.
        if (!out_->isPortOpen()) {
            throw std::runtime_error("MIDI output: the port is not open");
        }
        out_->sendMessage(message.data(), message.size());
    }

private:
    std::unique_ptr<RtMidiOut> out_;
    /// RtMidi's reason, when it could not start at all and `out_` is empty.
    std::string unusable_;
};

} // namespace

MidiOutput::MidiOutput(std::string_view portName)
    : MidiOutput(portName, std::make_unique<RtMidiPort>()) {}

MidiOutput::MidiOutput(std::string_view portName, std::unique_ptr<MidiPort> port)
    : port_(std::move(port)), requested_(portName) {
    portName_ = port_->open(portName); // throws: no such port
    open_ = true;
}

MidiOutput::~MidiOutput() {
    if (port_) {
        port_->close();
    }
}

void MidiOutput::send(std::span<const unsigned char> message) noexcept {
    if (!open_) {
        // Closed by a reconnect that could not open it again. Nothing reaches the device, and
        // counting that as a send is what kept an unplugged interface "back" for good (C1).
        ++failed_;
        failedInARow_ = kLostAfterFailures;
        return;
    }
    try {
        port_->send(message);
        ++sent_;
        failedInARow_ = 0;
    } catch (...) {
        // RtMidi throws on a port that has gone away. A dropped clock tick is not worth
        // taking the tracker down for; the counters are what say it is happening, and a run of
        // them is what `lost()` — and so the reconnect — is keyed on.
        ++failed_;
        if (failedInARow_ < kLostAfterFailures) {
            ++failedInARow_;
        }
    }
}

bool MidiOutput::reconnect() noexcept {
    try {
        port_->close();
        open_ = false;
        portName_ = port_->open(requested_);
        open_ = true;
        failedInARow_ = 0;
        return true;
    } catch (...) {
        // Not back yet. Still lost, so the next look tries again.
        failedInARow_ = kLostAfterFailures;
        return false;
    }
}

MidiClock::MidiClock(MidiSink& sink, double bpm) : sink_(&sink), bpm_(bpm) {
    if (!(bpm > 0.0)) {
        throw std::invalid_argument("MidiClock: the tempo must be positive");
    }
    spacing_ = tickSeconds();
}

double MidiClock::tickSeconds() const noexcept {
    return 60.0 / (bpm_ * static_cast<double>(kPulsesPerQuarterNote));
}

void MidiClock::emit(unsigned char status) noexcept {
    const unsigned char message[1] = {status};
    sink_->send(std::span<const unsigned char>(message, 1));
}

double MidiClock::nextTick() const noexcept {
    return origin_ + static_cast<double>(sinceOrigin_) * spacing_;
}

void MidiClock::anchor(double at, double spacing) noexcept {
    origin_ = at;
    sinceOrigin_ = 0;
    spacing_ = spacing;
}

void MidiClock::start(double now, bool asContinue) noexcept {
    emit(asContinue ? kContinue : kStart);
    startTicking(now);
    started_ = true;
}

void MidiClock::startTicking(double now) noexcept {
    running_ = true;
    started_ = false;
    startPending_ = false;
    steering_ = 0;
    anchor(now, tickSeconds());
    pulse_ = 0;
}

void MidiClock::startOnDownbeat(double downbeat, double barSeconds) noexcept {
    if (!waitingToStart() || !std::isfinite(downbeat) || !std::isfinite(barSeconds) ||
        !(barSeconds > 0.0)) {
        return;
    }
    startPending_ = true;
    startAt_ = downbeat;
    startBar_ = barSeconds;
    // The tick before the downbeat may already have gone, leaving the downbeat's own tick next.
    startIfDue();
}

void MidiClock::startIfDue() noexcept {
    if (!startPending_ || pulse_ != 0) {
        return;
    }
    const double beat = tickSeconds() * static_cast<double>(kPulsesPerQuarterNote);
    const double window = kStartWindow * beat;
    const double at = nextTick();
    // A downbeat already behind this pulse 0 by more than the window is gone; the grid runs
    // on, and the first downbeat that is not is the one to aim at.
    if (at > startAt_ + window) {
        startAt_ += std::ceil((at - startAt_ - window) / startBar_) * startBar_;
    }
    if (std::abs(at - startAt_) > window) {
        return; // a pulse 0 on another beat, or off the beat altogether: not yet
    }
    const unsigned char position[3] = {kSongPosition, 0x00, 0x00};
    sink_->send(std::span<const unsigned char>(position, 3));
    emit(kStart);
    started_ = true;
    startPending_ = false;
}

void MidiClock::stop() noexcept {
    if (started_) {
        emit(kStop);
    }
    running_ = false;
    started_ = false;
    startPending_ = false;
}

void MidiClock::setTempo(double bpm) noexcept {
    if (!(bpm > 0.0)) {
        return;
    }
    bpm_ = bpm;
    if (steering_ == 0) {
        // Re-anchored on the tick that was already scheduled, so the interval in flight is
        // not retimed underneath the receiver. A steered stretch keeps its spacing: it was
        // worked out to land a pulse 0 on a beat, and the next sync works from the new tempo.
        anchor(nextTick(), tickSeconds());
    }
}

void MidiClock::syncToBeat(double beatTime) noexcept {
    if (!running_) {
        return;
    }
    const double tick = tickSeconds();
    const double beat = tick * static_cast<double>(kPulsesPerQuarterNote);
    // The pulse 0 to steer: the next one at least a tick away. When the next tick *is* a
    // pulse 0 it is left alone — it is due within one tick, and moving it would mean moving
    // a tick that is about to go — and the one after it is steered instead.
    const std::size_t remaining = kPulsesPerQuarterNote - pulse_; // 24 when pulse_ is 0
    const double next = nextTick();
    // Where that pulse 0 lands at the tempo's own spacing, which is the question — not where
    // a spacing steered for the last beat would have put it.
    const double unsteered = next + static_cast<double>(remaining) * tick;
    // The nearest beat of the grid the tracker's beat sits on, which is what the pulse 0 is
    // pulled towards. Past or future, it makes no difference: the grid runs both ways.
    const double target = beatTime + std::round((unsteered - beatTime) / beat) * beat;
    const double error = unsteered - target; // positive: the clock is late
    const double landing = unsteered - kSteerGain * error;
    const double spacing = std::clamp((landing - next) / static_cast<double>(remaining),
                                      tick * kSteerMin, tick * kSteerMax);
    // From the tick already scheduled, which stays where it is.
    anchor(next, spacing);
    steering_ = remaining;
}

std::size_t MidiClock::advance(double now) noexcept {
    if (!running_) {
        return 0;
    }
    std::size_t emitted = 0;
    while (nextTick() <= now) {
        if (emitted >= kMaxBurst) {
            // Something stalled for longer than the burst allows. Skip the backlog and
            // start again from here rather than flooding the port. The only place a tick is
            // ever dropped, and it is counted.
            const double interval = tickSeconds();
            const auto behind = static_cast<std::uint64_t>((now - nextTick()) / interval) + 1;
            skipped_ += behind;
            pulse_ = (pulse_ + behind) % kPulsesPerQuarterNote;
            steering_ = 0;
            anchor(now + interval, interval);
            break;
        }
        emit(kTick);
        ++ticks_;
        ++emitted;
        ++sinceOrigin_;
        pulse_ = (pulse_ + 1) % kPulsesPerQuarterNote;
        if (steering_ != 0 && --steering_ == 0) {
            // The steered pulse 0 has just gone. From here the tempo's own spacing again, until
            // the next beat asks for a correction — so a clock left without beats (a quiet
            // passage, an unlocked tracker) runs on at the tempo rather than at a correction.
            anchor(nextTick(), tickSeconds());
        }
        // After the tick before a pulse 0, so a Start lands between the two and the pulse 0
        // is the first tick the receiver counts.
        startIfDue();
    }
    return emitted;
}

} // namespace takt4::output
