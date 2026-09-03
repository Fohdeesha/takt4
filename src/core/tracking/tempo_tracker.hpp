#pragma once

#include "core/tracking/particle_filter.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace takt4::tracking {

/// What the tracker is currently saying, whether or not a beat just happened.
struct TempoState {
    double bpm = 0.0;    ///< after octave folding, and held while unconfident
    double rawBpm = 0.0; ///< what the particle filter's cloud says, unfolded
    /// True once `bpm` is coming from the spacing of the beats themselves rather than
    /// from the particle cloud's tempo interval. See TempoTracker's header.
    bool refined = false;
    bool locked = false;           ///< the tempo has agreed with itself long enough
    bool holding = false;          ///< confidence is below the gate; bpm is the last good one
    double confidence = 0.0;       ///< 0 to 1; see TempoTracker's header for what it measures
    std::uint32_t beatsPerBar = 0; ///< from the filter's downbeat stage, never assumed
    std::uint32_t beatInBar = 0;   ///< 1 on the downbeat, counting up; 0 before the first beat
    std::uint64_t bars = 0;        ///< downbeats seen since the last reset
    std::uint64_t beats = 0;       ///< beats seen since the last reset, downbeats included
};

/// One beat, as the output transports want it.
struct BeatEvent {
    std::uint64_t frameIndex = 0; ///< the activation frame it was called on
    double time = 0.0;            ///< seconds since the stream started, latency offset applied
    bool downbeat = false;
    std::uint32_t beatInBar = 0;
    std::uint32_t beatsPerBar = 0;
    double bpm = 0.0;
    bool locked = false;
    double confidence = 0.0;
};

/// HANDOFF §5.5, the layer between the particle filter and the outputs.
///
/// "The tracker is perhaps 40% of perceived quality. This layer is most of the rest, and
/// it is where the existing tools are weakest." It does five things, all of them
/// user-configurable:
///
///   * **Octave fold.** The filter tracks 55-215 BPM; an operator knows their material
///     sits in, say, 70-140. An estimate outside that is halved or doubled into it
///     before anything else looks at it, which is what stops a house set reading 170.
///   * **Lock and hysteresis.** A tempo has to agree with itself for `lockAfter` frames
///     before it is called locked, and disagree for `unlockAfter` frames before that is
///     given up — never on one frame.
///   * **Confidence gate.** Below the threshold the last good tempo is held and `holding`
///     is set, rather than a wrong number being published. Silence beats wrong.
///   * **Latency offset.** Every beat's timestamp is moved by a fixed number of
///     milliseconds so downstream fires early enough to be in time.
///   * **Meter.** Taken from the filter's downbeat stage. Nothing here hardcodes 4.
///
/// It also fixes the one number the state space cannot give. madmom's tempo intervals
/// are whole 20 ms frames, so around 130 BPM the only values that exist are 130.43 and
/// 125.00 — a 5.4 BPM step, and 14 BPM up at 214. Nothing inside the filter can do
/// better than the nearer of the two. But the beats it calls are spaced 23, 24, 23, 23
/// frames apart, and the mean of those over a few bars resolves the tempo to a fraction
/// of a BPM. So once the tempo is locked, the published value comes from the beat
/// spacing, and the cloud's interval is used only to decide *which* tempo is being
/// tracked. On the synthetic excerpt that is the difference between reporting 130.4 and
/// reporting 128.0, which is what the drum machine actually plays.
///
/// Confidence is how much of the beat particle cloud agrees with the tempo its own
/// median reports, smoothed over about a second. Upstream publishes no confidence at
/// all; this is the natural one, because it is exactly what falls apart when the tracker
/// is lost — the cloud spreads across tempi before the reported tempo starts jumping.
///
/// Deterministic, allocation-free and independent of wall-clock time: everything is
/// derived from the frame index the filter reports.
class TempoTracker {
public:
    struct Options {
        /// The octave-fold window. Estimates are doubled or halved into it. Set
        /// `octaveFold` false to publish whatever the filter says.
        double minBpm = 70.0;
        double maxBpm = 140.0;
        bool octaveFold = true;

        /// Frames of agreement before the tempo is called locked, and of disagreement
        /// before that is given up. At 50 Hz these are 0.5 s and 1.5 s.
        std::size_t lockAfter = 25;
        std::size_t unlockAfter = 75;
        /// How close two estimates have to be to count as agreeing. The state space is
        /// discrete, so in practice this means "the same tempo interval".
        double lockToleranceBpm = 0.5;

        /// Below this the published tempo is held and `holding` is set.
        double confidenceThreshold = 0.15;
        /// Time constant of the confidence smoother, in frames. 25 is half a second.
        double confidenceSmoothing = 25.0;

        /// Added to every beat's timestamp, to compensate for what happens downstream.
        /// Negative fires early, which is the useful direction.
        double latencyOffsetSeconds = 0.0;

        /// How many beat-to-beat gaps the refined tempo is averaged over, and how many of
        /// them have to survive outlier rejection before it is published at all. Eight
        /// gaps is about two bars of 4/4.
        std::size_t refineOverBeats = 8;
        std::size_t refineNeedsBeats = 4;
        /// How far a gap may be from the cloud's period and still be counted, and how far
        /// the refined tempo may be from the locked one before it is disbelieved. The
        /// first throws out a missed or doubled beat; the second stops a refinement ever
        /// silently becoming a different tempo.
        double refineGapTolerance = 0.25;
        double refineTempoTolerance = 0.10;
    };

    explicit TempoTracker(double secondsPerFrame, Options options = {});

    /// Feeds one frame from the particle filter. Returns a beat if one was called.
    std::optional<BeatEvent> process(const TrackedFrame& frame) noexcept;

    void reset() noexcept;

    const TempoState& state() const noexcept { return state_; }
    const Options& options() const noexcept { return options_; }

    /// Changing the window or the gate takes effect on the next frame; the lock is kept
    /// unless the published tempo no longer folds into the new window.
    void setOptions(const Options& options) noexcept;

    /// Halves or doubles the published tempo, and the fold window with it if that is the
    /// only way the new tempo can survive folding. §5.5's manual octave shift.
    void halve() noexcept;
    void redouble() noexcept;

    /// The octave fold on its own, for tests and for the UI to preview.
    double fold(double bpm) const noexcept;

private:
    void updateLock(double folded) noexcept;
    void rememberBeat(std::uint64_t frameIndex) noexcept;
    /// The mean beat-to-beat gap in frames, over the gaps within `refineGapTolerance` of
    /// `cloudIntervalFrames`. Zero when there are not enough of them.
    double refinedIntervalFrames(double cloudIntervalFrames) const noexcept;

    double secondsPerFrame_;
    Options options_;
    TempoState state_;

    double lockedBpm_ = 0.0;   ///< the discrete tempo the lock is on; state_.bpm may refine it
    double candidate_ = 0.0;   ///< the tempo currently being agreed with
    std::size_t agreeing_ = 0; ///< frames it has been agreed with for
    std::size_t disagreeing_ = 0;
    double smoothedConfidence_ = 0.0;
    bool everConfident_ = false;
    std::int64_t octaveShift_ = 0; ///< manual ×2 (+1) and ÷2 (-1) steps, applied after folding

    /// The frames the last few beats were called on, oldest first. Bounded by
    /// `refineOverBeats + 1`, so this allocates once and never grows.
    std::vector<std::uint64_t> beatFrames_;
};

} // namespace takt4::tracking
