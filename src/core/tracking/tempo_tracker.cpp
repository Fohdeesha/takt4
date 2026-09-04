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

TempoTracker::TempoTracker(double secondsPerFrame) : TempoTracker(secondsPerFrame, Options{}) {}

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
    if (options.refineNeedsBeats < 2 || options.refineOverBeats < options.refineNeedsBeats) {
        throw std::invalid_argument(
            "TempoTracker: refining the tempo needs at least two beat-to-beat gaps, and no "
            "more required than kept");
    }
    beatFrames_.reserve(options_.refineOverBeats + 1);
    reset();
}

void TempoTracker::reset() noexcept {
    state_ = TempoState{};
    lockedBpm_ = 0.0;
    candidate_ = 0.0;
    agreeing_ = 0;
    disagreeing_ = 0;
    smoothedConfidence_ = 0.0;
    everConfident_ = false;
    octaveShift_ = 0;
    filterBeatInBar_ = 0;
    barOffset_ = 0;
    snapPending_ = false;
    snapAwaitingFilter_ = false;
    sinceSnap_ = 0;
    beatFrames_.clear();
}

void TempoTracker::rememberBeat(std::uint64_t frameIndex) noexcept {
    if (beatFrames_.size() > options_.refineOverBeats) {
        beatFrames_.erase(beatFrames_.begin());
    }
    beatFrames_.push_back(frameIndex);
}

double TempoTracker::refinedIntervalFrames(double cloudIntervalFrames) const noexcept {
    if (beatFrames_.size() < options_.refineNeedsBeats + 1 || !(cloudIntervalFrames > 0.0)) {
        return 0.0;
    }
    // A missed beat leaves a gap of twice the period and a spurious one leaves half; both
    // would wreck a mean, so only gaps near the period the cloud believes are counted.
    const double low = cloudIntervalFrames * (1.0 - options_.refineGapTolerance);
    const double high = cloudIntervalFrames * (1.0 + options_.refineGapTolerance);
    double total = 0.0;
    std::size_t counted = 0;
    for (std::size_t i = 1; i < beatFrames_.size(); ++i) {
        const double gap = static_cast<double>(beatFrames_[i] - beatFrames_[i - 1]);
        if (gap >= low && gap <= high) {
            total += gap;
            ++counted;
        }
    }
    return counted >= options_.refineNeedsBeats ? total / static_cast<double>(counted) : 0.0;
}

void TempoTracker::setOptions(const Options& options) noexcept {
    options_ = options;
    // A window that no longer holds the locked tempo invalidates the lock; anything else
    // leaves it alone, so nudging a slider does not drop sync.
    if (state_.locked && options_.octaveFold &&
        (lockedBpm_ < options_.minBpm || lockedBpm_ >= options_.maxBpm)) {
        state_.locked = false;
        lockedBpm_ = fold(state_.rawBpm);
        state_.bpm = lockedBpm_;
        state_.refined = false;
        candidate_ = lockedBpm_;
        agreeing_ = 0;
        disagreeing_ = 0;
        beatFrames_.clear();
    }
}

double TempoTracker::fold(double bpm) const noexcept {
    const double folded =
        options_.octaveFold ? foldInto(bpm, options_.minBpm, options_.maxBpm) : bpm;
    return applyShift(folded, octaveShift_);
}

void TempoTracker::halve() noexcept {
    --octaveShift_;
    lockedBpm_ = fold(state_.rawBpm);
    state_.bpm = lockedBpm_;
    state_.refined = false;
    candidate_ = lockedBpm_;
    beatFrames_.clear();
}

void TempoTracker::redouble() noexcept {
    ++octaveShift_;
    lockedBpm_ = fold(state_.rawBpm);
    state_.bpm = lockedBpm_;
    state_.refined = false;
    candidate_ = lockedBpm_;
    beatFrames_.clear();
}

void TempoTracker::seedTempo(double bpm) noexcept {
    if (!(bpm > 0.0)) {
        return;
    }
    // An octave centred on the tap, so a tap 5 % out — which every human tap is — still
    // lands the tempo it meant squarely inside, and its neighbours squarely outside.
    const double half = std::sqrt(2.0);
    const double before = state_.bpm;
    options_.minBpm = bpm / half;
    options_.maxBpm = bpm * half;
    options_.octaveFold = true;
    octaveShift_ = 0;

    lockedBpm_ = fold(state_.rawBpm);
    state_.bpm = lockedBpm_;
    state_.refined = false;
    candidate_ = lockedBpm_;
    if (std::abs(state_.bpm - before) > options_.lockToleranceBpm) {
        // The tap really did move the tempo, so what was locked is no longer what is
        // published and the hunt starts again. A tap that agrees keeps the lock, as
        // nudging the window by hand does.
        state_.locked = false;
        agreeing_ = 0;
        disagreeing_ = 0;
        beatFrames_.clear();
    }
}

void TempoTracker::snapDownbeat() noexcept {
    snapPending_ = true;
}

void TempoTracker::advanceBar(bool filterCalledDownbeat) noexcept {
    const std::uint32_t meter = state_.beatsPerBar;

    // Where the filter says we are, on its own terms and untouched by any snap.
    if (filterCalledDownbeat) {
        filterBeatInBar_ = 1;
    } else if (filterBeatInBar_ != 0 && meter > 0) {
        filterBeatInBar_ = filterBeatInBar_ % meter + 1;
    }

    const bool haveFilterBar = filterBeatInBar_ != 0;

    if (snapPending_) {
        snapPending_ = false;
        snapAwaitingFilter_ = !(haveFilterBar && meter > 0);
        if (snapAwaitingFilter_) {
            // Nothing to rotate yet. Count from here, and turn the count into an offset
            // the moment the filter first has an opinion.
            sinceSnap_ = 0;
        } else {
            // Rotate so this beat comes out as 1. Reducing the position first keeps this
            // right through a meter change that leaves the filter past the end of a bar.
            barOffset_ = (meter - (filterBeatInBar_ - 1) % meter) % meter;
        }
    } else if (snapAwaitingFilter_ && meter > 0) {
        sinceSnap_ = (sinceSnap_ + 1) % meter;
        if (haveFilterBar) {
            // The filter has just called its first downbeat, so filterBeatInBar_ is 1 and
            // the offset that keeps the operator's count going is the count itself.
            barOffset_ = sinceSnap_;
            snapAwaitingFilter_ = false;
        }
    }

    if (haveFilterBar) {
        // Without a meter there is nothing to rotate within, so the filter's own position
        // stands — which for the only case that reaches here, a downbeat, is beat 1.
        state_.beatInBar =
            meter > 0 ? (filterBeatInBar_ - 1 + barOffset_) % meter + 1 : filterBeatInBar_;
    } else if (snapAwaitingFilter_) {
        // Between the snap and the filter's first downbeat the operator is the only one
        // who knows where the bar is, and they said it starts here.
        state_.beatInBar = sinceSnap_ + 1;
    } else {
        // A beat before anything has an opinion on the bar: say so with 0 rather than
        // guessing 1.
        state_.beatInBar = 0;
    }
    // A bar starts where the published one does, which without a snap is where the
    // filter's does. Without a meter the position cannot advance, so beat 1 would repeat
    // on every beat and count a bar each time: there, only the filter calling a downbeat
    // is a new bar.
    if (state_.beatInBar == 1 && (meter > 0 || filterCalledDownbeat)) {
        ++state_.bars;
    }
}

void TempoTracker::updateLock(double folded) noexcept {
    const double tolerance = options_.lockToleranceBpm;
    if (state_.locked) {
        if (std::abs(folded - lockedBpm_) <= tolerance) {
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
            lockedBpm_ = candidate_;
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
    lockedBpm_ = candidate_;
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

    // The lock is decided on the state space's own whole-frame interval, never on the
    // refined tempo. The refinement moves by tenths of a BPM from one frame to the next,
    // which is finer than the lock tolerance, so locking against it would break the lock
    // on every other frame — the discrete value is what says *which* tempo is being
    // tracked, and that is the only question the lock asks.
    const double discrete =
        frame.intervalFrames > 0
            ? 60.0 / (static_cast<double>(frame.intervalFrames) * secondsPerFrame_)
            : frame.bpm;
    const bool confident = smoothedConfidence_ >= options_.confidenceThreshold;
    if (confident) {
        everConfident_ = true;
        updateLock(fold(discrete));
    } else if (!everConfident_) {
        // Nothing good has been seen yet, so there is nothing to hold: follow the
        // estimate and let the gate take over once it has been believed once.
        lockedBpm_ = fold(discrete);
    }
    // Below the gate, state_.bpm keeps whatever it last held (§5.5: "hold the last good
    // tempo, stop emitting new values, and say so").
    state_.holding = !confident && everConfident_;

    const bool emitted = frame.emitted != TrackedFrame::Emitted::None;
    if (emitted) {
        rememberBeat(frame.frameIndex);
    }

    if (!state_.holding) {
        state_.bpm = lockedBpm_;
        state_.refined = false;
        // Once the tempo is locked, the beats themselves say it more precisely than the
        // state space's whole-frame intervals ever can. Only believe that when it agrees
        // with the lock, so a refinement can sharpen the tempo but never change it.
        if (state_.locked) {
            const double gaps = refinedIntervalFrames(
                frame.refinedIntervalFrames > 0.0 ? frame.refinedIntervalFrames
                                                  : static_cast<double>(frame.intervalFrames));
            if (gaps > 0.0) {
                const double refined = fold(60.0 / (gaps * secondsPerFrame_));
                if (std::abs(refined - lockedBpm_) <= options_.refineTempoTolerance * lockedBpm_) {
                    state_.bpm = refined;
                    state_.refined = true;
                }
            }
        }
    }

    if (!emitted) {
        return std::nullopt;
    }

    const bool snapped = snapPending_;
    advanceBar(frame.emitted == TrackedFrame::Emitted::Downbeat);
    ++state_.beats;

    BeatEvent event;
    event.frameIndex = frame.frameIndex;
    event.time =
        static_cast<double>(frame.frameIndex) * secondsPerFrame_ + options_.latencyOffsetSeconds;
    // What the operator sees as the downbeat, which without a snap is the filter's.
    event.downbeat = state_.beatInBar == 1;
    event.snapped = snapped;
    event.beatInBar = state_.beatInBar;
    event.beatsPerBar = state_.beatsPerBar;
    event.bpm = state_.bpm;
    event.locked = state_.locked;
    event.confidence = state_.confidence;
    return event;
}

} // namespace takt4::tracking
