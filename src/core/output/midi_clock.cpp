#include "core/output/midi_clock.hpp"

#include "core/output/midi_ports.hpp"

#include <RtMidi.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::output {

namespace {

/// A port specification is either an index or part of a name. Returns the port number,
/// or throws with the list of what was actually there — the useful message when a
/// device is unplugged or named differently than the operator expected.
unsigned int findPort(RtMidiOut& out, std::string_view spec) {
    const unsigned int count = out.getPortCount();
    std::vector<std::string> names;
    names.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        names.push_back(out.getPortName(i));
    }

    const bool numeric = !spec.empty() && std::all_of(spec.begin(), spec.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
    if (numeric) {
        const unsigned long index = std::stoul(std::string(spec));
        if (index < count) {
            return static_cast<unsigned int>(index);
        }
    } else {
        for (unsigned int i = 0; i < count; ++i) {
            if (names[i].find(spec) != std::string::npos) {
                return i;
            }
        }
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
            // here it has to become an error the caller can print.
            throw std::runtime_error("MIDI output: no usable MIDI API on this machine (" +
                                     error.getMessage() + ")");
        }
    }

    std::string open(std::string_view spec) override {
        try {
            // Looked up afresh every time: a device that went away and came back can come
            // back at another index, and RtMidi enumerates again on every count.
            const unsigned int index = findPort(*out_, spec);
            std::string name = out_->getPortName(index);
            out_->openPort(index, "takt4");
            return name;
        } catch (const RtMidiError& error) {
            throw std::runtime_error("MIDI output: " + error.getMessage());
        }
    }

    void close() noexcept override {
        try {
            out_->closePort();
        } catch (...) {
            // A port that is already gone cannot be closed any more gone.
        }
    }

    void send(std::span<const unsigned char> message) override {
        out_->sendMessage(message.data(), message.size());
    }

private:
    std::unique_ptr<RtMidiOut> out_;
};

} // namespace

MidiOutput::MidiOutput(std::string_view portName)
    : MidiOutput(portName, std::make_unique<RtMidiPort>()) {}

MidiOutput::MidiOutput(std::string_view portName, std::unique_ptr<MidiPort> port)
    : port_(std::move(port)), requested_(portName) {
    portName_ = port_->open(portName); // throws: no such port
}

MidiOutput::~MidiOutput() {
    if (port_) {
        port_->close();
    }
}

void MidiOutput::send(std::span<const unsigned char> message) noexcept {
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
        portName_ = port_->open(requested_);
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
}

double MidiClock::tickSeconds() const noexcept {
    return 60.0 / (bpm_ * static_cast<double>(kPulsesPerQuarterNote));
}

void MidiClock::emit(unsigned char status) noexcept {
    const unsigned char message[1] = {status};
    sink_->send(std::span<const unsigned char>(message, 1));
}

double MidiClock::nextTick() const noexcept {
    return origin_ + static_cast<double>(sinceOrigin_) * tickSeconds();
}

void MidiClock::anchor(double at) noexcept {
    origin_ = at;
    sinceOrigin_ = 0;
}

void MidiClock::start(double now, bool asContinue) noexcept {
    emit(asContinue ? kContinue : kStart);
    running_ = true;
    anchor(now);
    pulse_ = 0;
}

void MidiClock::stop() noexcept {
    if (running_) {
        emit(kStop);
    }
    running_ = false;
}

void MidiClock::setTempo(double bpm) noexcept {
    if (!(bpm > 0.0)) {
        return;
    }
    // Re-anchor on the tick that was already scheduled, so the interval in flight is not
    // retimed underneath the receiver.
    const double pending = nextTick();
    bpm_ = bpm;
    anchor(pending);
}

void MidiClock::syncToBeat(double now) noexcept {
    if (!running_) {
        return;
    }
    // A beat is pulse 0 of a quarter note. Putting the next tick here rather than
    // wherever the free-running schedule had reached is what stops the clock drifting
    // away from the audio between tempo updates.
    anchor(now);
    pulse_ = 0;
}

std::size_t MidiClock::advance(double now) noexcept {
    if (!running_) {
        return 0;
    }
    const double interval = tickSeconds();
    std::size_t emitted = 0;
    while (nextTick() <= now) {
        if (emitted >= kMaxBurst) {
            // Something stalled for longer than the burst allows. Skip the backlog and
            // start again from here rather than flooding the port.
            const auto behind = static_cast<std::uint64_t>((now - nextTick()) / interval) + 1;
            skipped_ += behind;
            pulse_ = (pulse_ + behind) % kPulsesPerQuarterNote;
            anchor(now + interval);
            break;
        }
        emit(kTick);
        ++ticks_;
        ++emitted;
        ++sinceOrigin_;
        pulse_ = (pulse_ + 1) % kPulsesPerQuarterNote;
    }
    return emitted;
}

} // namespace takt4::output
