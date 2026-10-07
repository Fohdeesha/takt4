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
    /// Whether the port is open, as far as this object has opened and closed it. **A send on a
    /// port that is not open is a failure, whatever the port says** (the audit of 2026-09-25,
    /// C1): RtMidi's WinMM port swallows a send once it is closed, so after one reopen that
    /// failed every send "worked", `lost()` went false, the window said the device was back, and
    /// nothing looked for it again.
    bool open_ = false;
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
///
/// **Start goes out on a downbeat, not on a press** (the audit's M19). A receiver starts its
/// song on the first tick after Start, and counts its bars from there, so a Start sent when
/// the operator pressed START put every drum machine's and sequencer's bar 1 wherever the
/// press happened to fall. `startTicking` begins the ticks — a receiver can read the tempo off
/// them while it waits — and `startOnDownbeat` sends Song Position 0 and Start just ahead of
/// the tick that lands on a downbeat.
class MidiClock {
public:
    static constexpr std::size_t kPulsesPerQuarterNote = 24;
    static constexpr unsigned char kTick = 0xF8;
    static constexpr unsigned char kStart = 0xFA;
    static constexpr unsigned char kContinue = 0xFB;
    static constexpr unsigned char kStop = 0xFC;
    /// Song Position Pointer, followed by a 14-bit position in sixteenths, low seven bits
    /// first. Sent as 0 ahead of Start, for a receiver that follows the position rather than
    /// taking Start to mean the top of the song.
    static constexpr unsigned char kSongPosition = 0xF2;

    /// A burst longer than this many ticks is not sent: something stalled, and flooding
    /// the port with catch-up ticks would be worse than skipping them. Two seconds at
    /// 120 BPM. What is skipped is whole quarter notes, so the receiver's count of pulses and
    /// this clock's stay one.
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
    /// Begins ticking at `now` and sends nothing else: the receiver hears a tempo and is not
    /// told to play. See `startOnDownbeat`.
    void startTicking(double now) noexcept;
    /// Sends Song Position 0 and Start ahead of the pulse 0 that lands on a downbeat: the
    /// first one at or after `downbeat` of the grid `downbeat + k * barSeconds`, on the clock
    /// `advance` is given. They go out straight after the tick before it, so the tick on the
    /// downbeat is the first one the receiver counts.
    ///
    /// Only a pulse 0 within `kStartWindow` of a beat of the tempo is taken as that downbeat.
    /// A clock that has not pulled in on the beats yet waits a bar rather than starting a
    /// receiver's bar off the music. Called again, it replaces the grid, which is how a caller
    /// keeps it up to date with the tempo. Does nothing unless the clock is ticking and has
    /// not been started.
    void startOnDownbeat(double downbeat, double barSeconds) noexcept;
    /// Keeps a receiver's bars on the tracker's once it has been started — what a DOWNBEAT press
    /// is for (the 2026-09-22 audit's M19, the half it left).
    ///
    /// A receiver counts its bars from the first tick after Start, and Song Position only moves
    /// it while it is stopped, so a DOWNBEAT pressed after Start — or a bar the tracker has found
    /// again somewhere else after a break — moved the lights and Link and left every drum machine
    /// and sequencer a beat or three off for the rest of the run. The clock knows where the
    /// receiver's bar is: it has counted every tick since Start. Given each locked beat's place in
    /// the bar — `beatInBar` of `beatsPerBar`, at `beatTime` on the clock `advance` is given —
    /// it compares; when two beats in a row say the receiver's bar is somewhere else, it sends
    /// Stop, Song Position 0 and Start between the tick before the tracker's next downbeat and
    /// the downbeat's own, so the receiver's bar 1 lands on it. Two, so a beat the tracker called
    /// once in the wrong place costs nothing. Does nothing unless the receiver has been started.
    void followBar(double beatTime, std::uint32_t beatInBar, std::uint32_t beatsPerBar) noexcept;
    /// How many times `followBar` has put a receiver's bar back — Stop and Start again.
    std::uint64_t barsRealigned() const noexcept { return realigned_; }

    /// Sends Stop, if the receiver was told to play. advance() emits nothing until the clock is
    /// started again.
    void stop() noexcept;
    /// Whether ticks are going out.
    bool running() const noexcept { return running_; }
    /// Whether the receiver has been told to play: Start or Continue sent, Stop not.
    bool started() const noexcept { return started_; }
    /// Ticking, and not yet started: what `startOnDownbeat` is for.
    bool waitingToStart() const noexcept { return running_ && !started_; }

    /// How far a pulse 0 may be from the downbeat it is to start on, in beats.
    static constexpr double kStartWindow = 0.25;

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
    /// **Before Start**, with nobody counting the pulses, the next pulse 0 is put on the beat in
    /// one step — the ticks before it renumbered, and respaced by no more than half a tick each —
    /// so Start lands on its downbeat.
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
    /// Sends Song Position 0 and Start if one is waiting and the next tick is the pulse 0 on
    /// its downbeat. See `startOnDownbeat`.
    void startIfDue() noexcept;
    /// `syncToBeat` before Start: the next pulse 0 put on the beat in one step.
    void aimBeforeStart(double beatTime, double tick, double beat) noexcept;

    MidiSink* sink_;
    double bpm_;
    bool running_ = false;
    bool started_ = false;
    /// A Start waiting for its downbeat: the grid `startAt_ + k * startBar_`.
    bool startPending_ = false;
    /// And the one waiting is a restart: Stop goes out ahead of it. See `followBar`.
    bool restart_ = false;
    double startAt_ = 0.0;
    double startBar_ = 0.0;
    /// The tick the receiver counts as the first of bar 1: `ticks_` when Start went out, which
    /// is the tick that followed it. What `followBar` counts the receiver's bars from.
    std::uint64_t startTick_ = 0;
    /// Beats in a row whose place in the bar the receiver's count disagrees with.
    std::uint32_t barMismatches_ = 0;
    std::uint64_t realigned_ = 0;
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
