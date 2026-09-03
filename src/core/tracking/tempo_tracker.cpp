#include "core/tracking/tempo_tracker.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace takt4::tracking {

namespace {

/// Doubling or halving `bpm` until it lands in [low, high), if it can. A window narrower
/// than an octave has gaps no folding reaches, so this gives up rather than looping.
double foldInto(double bpm, double low, double high) noexcept {
    if (!(bpm > 0.0) || !(low > 0.0) || !(high > low)) {
        return bpm;
    }
    double folded = bpm;
    for (int step = 0; step < 16 && folded < low; ++step) {
        folded *= 2.0;
    }
    for (int step = 0; step < 16 && folded >= high; ++step) {
        folded *= 0.5;
    }
    // Overshot: the window is narrower than an octave and nothing lands inside it. Take
    // whichever of the two neighbouring octaves is nearer the window.
    if (folded < low) {
        const double up = folded * 2.0;
        folded = (low - folded) <= (up - high) ? folded : up;
    }
    return folded;
}

double applyShift(double bpm, std::int64_t shift) noexcept {
    return bpm * std::pow(2.0, static_cast<double>(shift));
}

} // namespace

TempoTracker::TempoTracker(double secondsPerFrame, Options options)
    : secondsPerFrame_(secondsPerFrame), options_(options) {
    if (!(secondsPerFrame > 0.0)) {
        throw std::invalid_argument("TempoTracker: the frame period must be positive");
    }
    if (!(options.maxBpm > options.minBpm) || !(options.minBpm > 0.0)) {
        throw std::invalid_argument(
            "TempoTracker: the octave-fold window must be a positive range");
    }
    if (options.lockAfter == 0 || options.unlockAfter == 0) {
        throw std::invalid_argument("TempoTracker: locking needs at least one frame");
    }
    if (!(options.confidenceSmoothing >= 1.0)) {
        throw std::invalid_argument(
            "TempoTracker: the confidence smoother needs at least one frame");
    }
    reset();
}

void TempoTracker::reset() noexcept {
    state_ = TempoState{};
    candidate_ = 0.0;
    agreeing_ = 0;
    disagreeing_ = 0;
    smoothedConfidence_ = 0.0;
    everConfident_ = false;
    octaveShift_ = 0;
}

void TempoTracker::setOptions(const Options& options) noexcept {
    options_ = options;
    // A window that no longer holds the published tempo invalidates the lock; anything
    // else leaves it alone, so nudging a slider does not drop sync.
    if (state_.locked && options_.octaveFold &&
        (state_.bpm < options_.minBpm || state_.bpm >= options_.maxBpm)) {
        state_.locked = false;
        state_.bpm = fold(state_.rawBpm);
        candidate_ = state_.bpm;
        agreeing_ = 0;
        disagreeing_ = 0;
    }
}

double TempoTracker::fold(double bpm) const noexcept {
    const double folded =
        options_.octaveFold ? foldInto(bpm, options_.minBpm, options_.maxBpm) : bpm;
    return applyShift(folded, octaveShift_);
}

void TempoTracker::halve() noexcept {
    --octaveShift_;
    state_.bpm = fold(state_.rawBpm);
    candidate_ = state_.bpm;
}

void TempoTracker::redouble() noexcept {
    ++octaveShift_;
    state_.bpm = fold(state_.rawBpm);
    candidate_ = state_.bpm;
}

void TempoTracker::updateLock(double folded) noexcept {
    const double tolerance = options_.lockToleranceBpm;
    if (state_.locked) {
        if (std::abs(folded - state_.bpm) <= tolerance) {
            disagreeing_ = 0;
            candidate_ = folded;
            return;
        }
        // Disagreement has to be sustained. Follow the new value as the candidate so the
        // lock can move straight to it when the window runs out.
        if (std::abs(folded - candidate_) > tolerance) {
            candidate_ = folded;
        }
        if (++disagreeing_ >= options_.unlockAfter) {
            state_.locked = false;
            state_.bpm = candidate_;
            agreeing_ = 1;
            disagreeing_ = 0;
        }
        return;
    }

    if (agreeing_ != 0 && std::abs(folded - candidate_) <= tolerance) {
        ++agreeing_;
    } else {
        candidate_ = folded;
        agreeing_ = 1;
    }
    state_.bpm = candidate_;
    if (agreeing_ >= options_.lockAfter) {
        state_.locked = true;
        disagreeing_ = 0;
    }
}

std::optional<BeatEvent> TempoTracker::process(const TrackedFrame& frame) noexcept {
    // One-pole smoothing of the cloud's agreement with its own median. The filter's
    // per-frame value is far too jumpy to gate on directly.
    const double alpha = 1.0 / options_.confidenceSmoothing;
    smoothedConfidence_ += alpha * (frame.tempoAgreement - smoothedConfidence_);
    state_.confidence = smoothedConfidence_;
    state_.rawBpm = frame.bpm;
    state_.beatsPerBar = frame.beatsPerBar;

    const bool confident = smoothedConfidence_ >= options_.confidenceThreshold;
    if (confident) {
        everConfident_ = true;
        updateLock(fold(frame.bpm));
    } else if (!everConfident_) {
        // Nothing good has been seen yet, so there is nothing to hold: publish the
        // estimate as it comes and let the gate take over once it has been believed once.
        state_.bpm = fold(frame.bpm);
    }
    // Below the gate, state_.bpm keeps whatever it last held (§5.5: "hold the last good
    // tempo, stop emitting new values, and say so").
    state_.holding = !confident && everConfident_;

    if (frame.emitted == TrackedFrame::Emitted::None) {
        return std::nullopt;
    }

    const bool downbeat = frame.emitted == TrackedFrame::Emitted::Downbeat;
    if (downbeat) {
        state_.beatInBar = 1;
        ++state_.bars;
    } else if (state_.beatInBar == 0) {
        // A beat before any downbeat has been called: the bar phase is not known yet, so
        // say so with 0 rather than guessing 1.
        state_.beatInBar = 0;
    } else if (state_.beatsPerBar > 0) {
        state_.beatInBar = state_.beatInBar % state_.beatsPerBar + 1;
    }
    ++state_.beats;

    BeatEvent event;
    event.frameIndex = frame.frameIndex;
    event.time =
        static_cast<double>(frame.frameIndex) * secondsPerFrame_ + options_.latencyOffsetSeconds;
    event.downbeat = downbeat;
    event.beatInBar = state_.beatInBar;
    event.beatsPerBar = state_.beatsPerBar;
    event.bpm = state_.bpm;
    event.locked = state_.locked;
    event.confidence = state_.confidence;
    return event;
}

} // namespace takt4::tracking
