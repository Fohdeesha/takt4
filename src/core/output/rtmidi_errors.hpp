#pragma once

// RtMidi's errors, kept off stderr and thrown where the caller asked.
//
// Left to itself RtMidi writes every error to `std::cerr` and then throws it — and takt4's stderr
// is `takt4.log` beside the executable, opened unbuffered, on whatever disk takt4 runs from: on
// the rig this was found on, a network share. A MIDI interface pulled out mid-set failed every
// send like that, three lines written straight to the share, on the output thread, every round
// until the device was marked lost — and again once a second while it stayed busy. Given an
// error callback RtMidi writes nothing and throws nothing; this keeps what it said and throws it
// after the call, as RtMidi would have, so every caller's `catch` goes on working.

#include <RtMidi.h>

#include <mutex>
#include <optional>
#include <string>

namespace takt4::output {

class RtMidiErrors {
public:
    /// From here on, `midi`'s errors come here rather than to stderr. It must not outlive this:
    /// declare this first. Warnings — a port name asked for past the end of the list, a callback
    /// set twice — are dropped; they were only ever printed.
    template <typename Midi>
    void watch(Midi& midi) {
        midi.setErrorCallback(&RtMidiErrors::onError, this);
    }

    /// Throws the error RtMidi reported since the last look, if it reported one — as the
    /// `RtMidiError` it would have thrown itself.
    void raise() {
        std::optional<RtMidiError> error;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            error.swap(error_);
        }
        if (error) {
            throw *error;
        }
    }

private:
    // RtMidi's callback type. An input reports from its own thread as well as the caller's, so
    // what it says is kept under a lock.
    static void onError(RtMidiError::Type type, const std::string& text, void* self) {
        if (type == RtMidiError::WARNING || type == RtMidiError::DEBUG_WARNING) {
            return;
        }
        auto* const errors = static_cast<RtMidiErrors*>(self);
        const std::lock_guard<std::mutex> lock(errors->mutex_);
        if (!errors->error_) {
            errors->error_.emplace(text, type);
        }
    }

    std::mutex mutex_;
    std::optional<RtMidiError> error_;
};

} // namespace takt4::output
