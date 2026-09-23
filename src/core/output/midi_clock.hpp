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

/// The device behind a `MidiOutput`: RtMidi's port in the application, a stand-in in the
/// tests — which is the only way to test a device being unplugged and plugged back in on a
/// machine that has nothing to unplug.
class MidiPort {
public:
    virtual ~MidiPort() = default;
    /// Opens the first port matching `spec` — part of a name, or an index — and returns the
    /// port's full name. Throws std::runtime_error when there is no such port.
    virtual std::string open(std::string_view spec) = 0;
    virtual void close() noexcept = 0;
    /// One message. Throws when the port has gone away.
    virtual void send(std::span<const unsigned char> message) = 0;

protected:
    MidiPort() = default;
    MidiPort(const MidiPort&) = default;
    MidiPort& operator=(const MidiPort&) = default;
};

/// One MIDI output port, opened by name — and **opened again by name when it comes back**.
///
/// A USB MIDI interface pulled out mid-show used to stay dead for the rest of the show: every
/// send failed and was counted, nothing ever reopened the port, and re-picking the same port
/// in the window did nothing because it was "already open" (the audit's H11a). Now a run of
/// failures marks it `lost()`, and whoever owns it calls `reconnect()` now and then — the
/// output thread does, once a second — until the device is back.
class MidiOutput final : public MidiSink {
public:
    /// Sends in a row that fail before the port counts as lost. One failure can be a driver
    /// hiccup; three in a row, with a clock sending 24 a beat, is a device that has gone.
    static constexpr std::uint32_t kLostAfterFailures = 3;

    /// Opens the first port whose name contains `portName` (case-sensitive), or the port
    /// at that index if `portName` is a number. Throws std::runtime_error if there is no
    /// such port or RtMidi cannot open it.
    explicit MidiOutput(std::string_view portName);
    /// The same through any port — how a test supplies one it can unplug.
    MidiOutput(std::string_view portName, std::unique_ptr<MidiPort> port);
    ~MidiOutput() override;

    MidiOutput(const MidiOutput&) = delete;
    MidiOutput& operator=(const MidiOutput&) = delete;

    void send(std::span<const unsigned char> message) noexcept override;

    /// The port's full name as opened — which is what the window shows.
    const std::string& portName() const noexcept { return portName_; }
    /// The name it was asked for, which is what it is looked for by again.
    const std::string& requested() const noexcept { return requested_; }
    std::uint64_t sent() const noexcept { return sent_; }
    std::uint64_t failed() const noexcept { return failed_; }

    /// Whether the port has stopped taking messages: `kLostAfterFailures` sends in a row failed,
    /// and no reconnect has worked since.
    bool lost() const noexcept { return failedInARow_ >= kLostAfterFailures; }

    /// Closes the port and opens it again by `requested()`. True when that worked; false, and
    /// still lost, when the device is not back yet. Never throws.
    bool reconnect() noexcept;

private:
    std::unique_ptr<MidiPort> port_;
    std::string requested_;
    std::string portName_;
    std::uint64_t sent_ = 0;
    std::uint64_t failed_ = 0;
    std::uint32_t failedInARow_ = 0;
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
/// replay.
///
/// **It is a phase-locked loop, and it never adds or drops a tick to stay in phase.** A
/// receiver counting ticks — a drum machine, a sequencer, a lighting desk — counts 24 to a
/// beat and nothing else, so a clock that re-anchored pulse 0 on every beat the tracker called
/// sent it 25 to 28 ticks a beat whenever a beat was drained late or the latency offset was
/// negative, and it walked off the music (the audit's C2, measured: 128 BPM gained 1.8 %, and
/// −40 ms sent 26 to 28 on every beat in zero-gap bursts). `syncToBeat` now *steers*: it
/// spreads the ticks up to the next pulse 0 a little wider or a little closer so that pulse 0
/// lands half-way nearer the beat, and every quarter note is still exactly 24 ticks.
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

    /// How much of the phase error one beat's steering takes out. A half: a clock 40 ms off
    /// is 20 ms off a beat later, 10 the beat after, and inside a millisecond within six
    /// beats — while a beat whose timestamp jittered by 10 ms moves the clock by 5 and no more.
    static constexpr double kSteerGain = 0.5;
    /// How far a steered tick spacing may stray from the tempo's own, either way. A receiver
    /// reads its tempo off the spacing, so this is also how far its tempo display can swing
    /// while the clock pulls in — and it is what stops a wild beat from sending ticks in a
    /// burst or leaving a gap.
    static constexpr double kSteerMin = 0.8;
    static constexpr double kSteerMax = 1.25;

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

    /// Steers the clock towards a beat at `beatTime` — past or future, on the clock `advance`
    /// is given — at the current tempo. Called on each beat the tracker reports, so the clock
    /// follows the audio rather than drifting from it.
    ///
    /// The pulse 0 that is steered is the next one at least a tick away, and it is moved
    /// `kSteerGain` of the way to the nearest beat of the grid `beatTime` sits on. Nothing is
    /// emitted and nothing is skipped: the ticks in between are respaced, within `kSteerMin`
    /// to `kSteerMax` of the tempo's own spacing, and the spacing goes back to the tempo's own
    /// once that pulse 0 has gone.
    void syncToBeat(double beatTime) noexcept;

    std::uint64_t ticksSent() const noexcept { return ticks_; }
    std::uint64_t ticksSkipped() const noexcept { return skipped_; }
    /// Where in the quarter note the next tick falls, 0 to 23.
    std::size_t pulseInQuarter() const noexcept { return pulse_; }
    /// When the next tick is due, on the clock `advance` is given.
    double nextTickAt() const noexcept { return nextTick(); }

private:
    /// The tick spacing the tempo asks for, before any steering.
    double tickSeconds() const noexcept;
    /// Tick times are counted from `origin_` rather than accumulated, so an hour of
    /// ticking does not walk away from the tempo one rounding at a time. Every start,
    /// change of spacing and skipped stall re-anchors it.
    double nextTick() const noexcept;
    void anchor(double at, double spacing) noexcept;
    void emit(unsigned char status) noexcept;

    MidiSink* sink_;
    double bpm_;
    bool running_ = false;
    double origin_ = 0.0;
    std::uint64_t sinceOrigin_ = 0;
    /// The spacing in force from `origin_`: the tempo's own, or a steered one.
    double spacing_ = 0.0;
    /// Ticks left to emit before a steered spacing gives way to the tempo's own again — the
    /// ones before the pulse 0 being steered, whose own time the last of them fixes. Zero when
    /// nothing is being steered.
    std::size_t steering_ = 0;
    std::size_t pulse_ = 0;
    std::uint64_t ticks_ = 0;
    std::uint64_t skipped_ = 0;
};

} // namespace takt4::output
