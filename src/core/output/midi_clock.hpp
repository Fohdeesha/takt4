#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::output {

/// Where MIDI bytes go. Real hardware behind MidiOutput; a recorder in the tests.
class MidiSink {
public:
    virtual ~MidiSink() = default;
    /// One complete MIDI message. Called from the clock thread, never the audio thread.
    virtual void send(std::span<const unsigned char> message) noexcept = 0;

protected:
    MidiSink() = default;
    MidiSink(const MidiSink&) = default;
    MidiSink& operator=(const MidiSink&) = default;
};

/// One RtMidi output port, opened by name.
class MidiOutput final : public MidiSink {
public:
    /// Opens the first port whose name contains `portName` (case-sensitive), or the port
    /// at that index if `portName` is a number. Throws std::runtime_error if there is no
    /// such port or RtMidi cannot open it.
    explicit MidiOutput(std::string_view portName);
    ~MidiOutput() override;

    MidiOutput(const MidiOutput&) = delete;
    MidiOutput& operator=(const MidiOutput&) = delete;

    void send(std::span<const unsigned char> message) noexcept override;

    const std::string& portName() const noexcept { return portName_; }
    std::uint64_t sent() const noexcept { return sent_; }
    std::uint64_t failed() const noexcept { return failed_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string portName_;
    std::uint64_t sent_ = 0;
    std::uint64_t failed_ = 0;
};

/// MIDI beat clock: 24 pulses per quarter note, plus Start, Stop and Continue.
///
/// HANDOFF §5.6: "This is the transport nobody in this niche ships and it is cheap.
/// Lighting desks, hardware sequencers and older software speak MIDI clock and do not
/// speak Link."
///
/// The clock keeps no timer of its own. `advance(now)` is given a time in seconds and
/// emits every tick that has come due, so the scheduling is a pure function of the times
/// it is called with — which is what makes it testable without hardware and without
/// waiting. Whoever owns it calls that from a thread, or from a test, or from a file
/// replay. `syncToBeat` puts a tick exactly on a beat the tracker just called, which is
/// what keeps the clock in phase with the audio rather than free-running from a tempo.
class MidiClock {
public:
    static constexpr std::size_t kPulsesPerQuarterNote = 24;
    static constexpr unsigned char kTick = 0xF8;
    static constexpr unsigned char kStart = 0xFA;
    static constexpr unsigned char kContinue = 0xFB;
    static constexpr unsigned char kStop = 0xFC;

    /// A burst longer than this many ticks is not sent: something stalled, and flooding
    /// the port with catch-up ticks would be worse than skipping them. Two seconds at
    /// 120 BPM.
    static constexpr std::size_t kMaxBurst = 96;

    /// `sink` has to outlive the clock. The tempo can be anything positive; it is
    /// normally the tracker's, set again on every beat.
    MidiClock(MidiSink& sink, double bpm);

    /// Sends Start (or Continue) and begins ticking at `now`. The first tick goes out on
    /// the next advance() at or after `now`.
    void start(double now, bool asContinue = false) noexcept;
    /// Sends Stop. advance() emits nothing until start() is called again.
    void stop() noexcept;
    bool running() const noexcept { return running_; }

    /// Emits every tick due at or before `now`. Returns how many went out.
    std::size_t advance(double now) noexcept;

    /// The tempo the ticks are spaced by. Takes effect from the next tick, so the
    /// interval already in flight is not retimed underneath the receiver.
    void setTempo(double bpm) noexcept;
    double tempo() const noexcept { return bpm_; }

    /// Puts a tick on `now` and starts the quarter note there. Called on each beat the
    /// tracker reports, so the clock follows the audio rather than drifting from it.
    void syncToBeat(double now) noexcept;

    std::uint64_t ticksSent() const noexcept { return ticks_; }
    std::uint64_t ticksSkipped() const noexcept { return skipped_; }
    /// Where in the quarter note the next tick falls, 0 to 23.
    std::size_t pulseInQuarter() const noexcept { return pulse_; }

private:
    double tickSeconds() const noexcept;
    /// Tick times are counted from `origin_` rather than accumulated, so an hour of
    /// ticking does not walk away from the tempo one rounding at a time. Every start,
    /// tempo change and beat sync re-anchors it.
    double nextTick() const noexcept;
    void anchor(double at) noexcept;
    void emit(unsigned char status) noexcept;

    MidiSink* sink_;
    double bpm_;
    bool running_ = false;
    double origin_ = 0.0;
    std::uint64_t sinceOrigin_ = 0;
    std::size_t pulse_ = 0;
    std::uint64_t ticks_ = 0;
    std::uint64_t skipped_ = 0;
};

} // namespace takt4::output
