#pragma once

#include "core/tracking/tempo_tracker.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace takt4::engine {

/// One live change to a running tracker (HANDOFF §5.5).
///
/// `TempoTracker` belongs to the inference thread and nothing about it is thread-safe, so
/// a UI button, an inbound OSC message (§5.7) or a keyboard shortcut cannot simply call
/// it. Nor can they stop the engine first: `BeatEngine::start()` reseeds the filter, so an
/// operator nudging the latency slider mid-set would lose the lock and several seconds of
/// tracking. They post one of these instead, and the inference thread applies it between
/// frames.
///
/// A tagged struct rather than a variant because these travel through a fixed-capacity
/// queue and are copied, not owned.
struct Command {
    enum class Kind : std::uint8_t {
        /// §5.5's adjustable settings, whole: the octave-fold window, the confidence
        /// gate, the latency offset, the lock timings. Idempotent — a second one says
        /// everything the first did — so the queue keeps only the newest.
        SetTempoOptions,
        Halve,    ///< §5.5's ÷2.
        Redouble, ///< §5.5's ×2.
        /// §5.5's manual downbeat: the beat nearest the press starts the bar. The one
        /// command the handoff calls non-negotiable, and the one an operator reaches for
        /// first.
        SnapDownbeat,
        /// §5.5's tap tempo, already measured: `tracking::TapTempo` counts the taps on
        /// whichever thread they arrive on, and only the tempo it worked out travels here.
        SeedTempo,
        /// §5.7's `/ctl/lock <0|1>`. Deliberately *not* superseded the way SetTempoOptions
        /// is: a pin followed by a release is not a pin, so these have to arrive in the
        /// order they were sent.
        ///
        /// On a decoder that can hold a tempo (`BeatDecoder::canHoldTempo`) the pin also
        /// holds the tempo being published, so the decoder tracks phase and nothing else,
        /// and the release lets it go — the *tempo hold*, behind
        /// the control the window already has. On one that cannot, the pin is the
        /// tracker's alone, as it always was.
        SetLockPinned,
        /// The tempo hold, named directly: pin the decoder to `bpm` and track phase
        /// only, or release it with zero. Ignored by a decoder that cannot.
        HoldTempo,
    };

    Kind kind;
    /// Read for SetTempoOptions and ignored otherwise.
    tracking::TempoTracker::Options tempo;
    /// Read for SeedTempo and HoldTempo and ignored otherwise.
    double bpm = 0.0;
    /// Read for SetLockPinned and ignored otherwise.
    bool pinned = false;

    static Command setTempoOptions(const tracking::TempoTracker::Options& options) noexcept {
        return Command{Kind::SetTempoOptions, options, 0.0, false};
    }
    static Command halve() noexcept { return Command{Kind::Halve, {}, 0.0, false}; }
    static Command redouble() noexcept { return Command{Kind::Redouble, {}, 0.0, false}; }
    static Command snapDownbeat() noexcept { return Command{Kind::SnapDownbeat, {}, 0.0, false}; }
    static Command seedTempo(double bpm) noexcept {
        return Command{Kind::SeedTempo, {}, bpm, false};
    }
    static Command setLockPinned(bool pinned) noexcept {
        return Command{Kind::SetLockPinned, {}, 0.0, pinned};
    }
    static Command holdTempo(double bpm) noexcept { return Command{Kind::HoldTempo, {}, bpm, false}; }
};

/// **A setting, not a press**: what the operator has set up rather than something done to the
/// tracker at a moment — the settings (`SetTempoOptions`) and the tempo hold named directly
/// (`HoldTempo`, which the console sets before it starts). The rest — ÷2, ×2, DOWNBEAT, a tap, a
/// pin — act on what the tracker is hearing at the press, and mean nothing to a tracker that is
/// stopped.
constexpr bool isSetting(Command::Kind kind) noexcept {
    return kind == Command::Kind::SetTempoOptions || kind == Command::Kind::HoldTempo;
}

/// The road into the inference thread: many writers, one reader, bounded, no allocation
/// once it is warm.
///
/// A mutex, deliberately. §4.2 forbids allocating and locking on the *audio* thread and
/// says nothing of the kind about the inference thread, which is where this is drained —
/// so the lock-free machinery `rt::SpscRing` needs would buy nothing and cost a great
/// deal, and unlike that ring this has to accept several producers at once. A UI thread,
/// an OSC receiver and a MIDI receiver all post here.
///
/// **Never post from the audio thread.** That one does forbid locking, and it has nothing
/// to say here anyway.
class ControlQueue {
public:
    /// Commands that may be waiting at once. The consumer takes every one of them each
    /// frame, so at 50 Hz this is 3200 posts a second before anything is refused —
    /// far past a dragged slider, and reaching it means a producer is looping.
    static constexpr std::size_t kCapacity = 64;

    ControlQueue();

    /// Any thread but the audio one. False when the queue is full, and then `dropped()`
    /// counts it. A SetTempoOptions supersedes any already waiting rather than queueing
    /// behind it, so a slider dragged while nothing is draining cannot fill this.
    ///
    /// **A setting is never refused** (`isSetting`): a queue full of presses — 64 of them posted
    /// while nothing drained, a stopped tracker and a control surface — gives up the oldest press
    /// for it, counted as dropped. It used to refuse the setting, so the latency and BPM window
    /// sliders moved on screen and reached nothing.
    bool post(const Command& command);

    /// Drops every press waiting (`isSetting`), keeping the settings in their order. What START
    /// does before it applies what was posted while the tracker was stopped: a ÷2, a pin or a
    /// DOWNBEAT pressed on a control surface between sets would otherwise land on the next run.
    void dropPresses();

    /// The consumer. Takes everything waiting, in the order it was posted, and leaves the
    /// queue empty. `out` is cleared first. Keep the same vector across calls, **reserved to
    /// `kCapacity` before the first**, and this allocates nothing: the queue swaps it in and
    /// sizes what it is given, so an empty one costs an allocation on the first call.
    void drain(std::vector<Command>& out);

    std::size_t pending() const;
    std::uint64_t dropped() const;

private:
    mutable std::mutex mutex_;
    std::vector<Command> pending_;
    std::uint64_t dropped_ = 0;
};

} // namespace takt4::engine
