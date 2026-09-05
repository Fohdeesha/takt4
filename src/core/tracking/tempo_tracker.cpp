#include "core/tracking/tempo_tracker.hpp"

#include <algorithm>
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

/// What fraction of the incumbent's tenure a challenger has to be agreed with for, before
/// it may replace it — an eighth, bounded by `lockAfter` and `relockAfter`. So a lock held
/// for twelve seconds is defended for one and a half, and anything past twenty-four seconds
/// is defended for the full three. Eight rather than four or sixteen because it is the
/// value at which a lock reaches full defence in about the time a phrase takes, which is
/// the shortest stretch over which "this really is the tempo" means anything.
constexpr std::size_t kDefenceShare = 8;

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
    if (options.lockAfter == 0 || options.unlockAfter == 0 || options.relockAfter == 0) {
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
    if (!(options.sameTempoTolerance > 0.0) || !(options.sameTempoTolerance < 1.0)) {
        throw std::invalid_argument(
            "TempoTracker: two tempi have to differ by some fraction, and by less than all "
            "of one, to be different tempi");
    }
    if (!(options.refineSmoothing > 0.0) || !(options.refineSmoothing <= 1.0)) {
        throw std::invalid_argument(
            "TempoTracker: the refinement smoother must move some of the way to a new "
            "measurement and no further than all of it");
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
    everLocked_ = false;
    refinedBpm_ = 0.0;
    lockHeld_ = 0;
    forgetFold();
    // A reseed leaves nothing for a pin to hold, so it goes with everything else rather
    // than surviving as a pin on a tempo that no longer exists.
    lockPinned_ = false;
    octaveShift_ = 0;
    filterBeatInBar_ = 0;
    barOffset_ = 0;
    snapPending_ = false;
    snapUnsent_ = false;
    snapAwaitingFilter_ = false;
    sinceSnap_ = 0;
    framesSinceBeat_ = 0;
    beatsSeen_ = 0;
    filterIntervalFrames_ = 0;
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
    // A window the operator has moved is a new question, so the octave the fold had
    // settled on under the old one is not an answer to it.
    const bool windowMoved = options.octaveFold != options_.octaveFold ||
                             options.minBpm != options_.minBpm || options.maxBpm != options_.maxBpm;
    options_ = options;
    if (windowMoved) {
        forgetFold();
    }
    // A window that no longer holds the locked tempo invalidates the lock; anything else
    // leaves it alone, so nudging a slider does not drop sync.
    if (state_.locked && options_.octaveFold &&
        (lockedBpm_ < options_.minBpm || lockedBpm_ >= options_.maxBpm)) {
        state_.locked = false;
        // The operator has excluded what was being published, so there is nothing left
        // worth holding on to: hunt from what is playing now, following the cloud as
        // during a first acquisition.
        everLocked_ = false;
        refinedBpm_ = 0.0;
        lockHeld_ = 0;
        chooseOctave(state_.rawBpm);
        lockedBpm_ = inChosenOctave(state_.rawBpm);
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

void TempoTracker::forgetFold() noexcept {
    foldChosen_ = false;
    foldOctave_ = 0;
}

double TempoTracker::inChosenOctave(double bpm) const noexcept {
    return applyShift(applyShift(bpm, foldOctave_), octaveShift_);
}

void TempoTracker::chooseOctave(double bpm) noexcept {
    if (!options_.octaveFold || !(bpm > 0.0)) {
        forgetFold();
        return;
    }
    const bool inWindow = bpm >= options_.minBpm && bpm < options_.maxBpm;
    // The window widened by the hysteresis at both ends: more than an octave wide by
    // construction, so inside the overlap an estimate is legitimately in range in two
    // octaves at once and something has to choose between them. Three rules, in order.
    const double margin = 1.0 + std::max(0.0, options_.foldHysteresis);
    const double low = options_.minBpm / margin;
    const double high = options_.maxBpm * margin;

    // 1. Continuity, but only where the answer is genuinely ambiguous — an estimate that
    //    has left the operator's actual window and is out among the hysteresis. There the
    //    octave already in force stands, which is what stops a track sitting *on* an edge
    //    flipping between octaves. Under the default 70-140, a 140 BPM track is exactly on
    //    one: its cloud reports interval 21 or interval 22, 142.9 BPM or 136.4 with nothing
    //    in between because the state space holds nothing in between, and a plain fold
    //    folds the first and not the second — an octave apart, twice a second, measured
    //    fifty times in 213 seconds on `references/audio`'s "01 - Pirates".
    //
    //    Not before a lock, though: the octave on offer then is whatever the cloud's
    //    opening guesses folded to, and being sticky about that is how a 133 BPM track came
    //    out at 66.7 ("1 - Jungle - GOOD TIMES", measured). A hysteresis holds a position
    //    that was arrived at; it does not freeze the first noise that came along.
    if (!inWindow && foldChosen_ && everLocked_) {
        const double kept = applyShift(bpm, foldOctave_);
        if (kept >= low && kept < high) {
            return;
        }
    }
    // 2. No correction unless one is needed. An estimate inside the window — or inside the
    //    hysteresis with nothing to be continuous with — is published as it stands.
    //
    //    Guarding rule 1 on `inWindow` is what makes this reachable again after an excursion,
    //    and it has to be: the widened window spans 1.27 octaves, so from one octave down
    //    the *whole* of the identity's range still satisfies rule 1. Without the guard a
    //    single passage where the cloud ran to double time left the tempo halved for the
    //    rest of the track — which is exactly what "1 - Jungle - GOOD TIMES" did, reading
    //    66.7 for a cloud whose median was 133.7.
    if (bpm >= low && bpm < high) {
        foldOctave_ = 0;
        foldChosen_ = true;
        return;
    }
    // 3. Out of range in both directions: fold, as the operator asked.
    const double folded = foldInto(bpm, options_.minBpm, options_.maxBpm);
    // Recovered rather than counted, because foldInto gives up when the window is narrower
    // than an octave and the number of steps it took is not the number that lands there.
    foldOctave_ = std::llround(std::log2(folded / bpm));
    foldChosen_ = true;
}

void TempoTracker::halve() noexcept {
    --octaveShift_;
    // The operator's shift moves the tempo now, whether or not the tracker is locked: this
    // is the one case where a hunting tracker's published tempo is allowed to move without
    // a lock behind it, because it moved because they asked.
    refinedBpm_ = 0.0;
    lockHeld_ = 0;
    chooseOctave(state_.rawBpm);
    lockedBpm_ = inChosenOctave(state_.rawBpm);
    state_.bpm = lockedBpm_;
    state_.refined = false;
    candidate_ = lockedBpm_;
    beatFrames_.clear();
}

void TempoTracker::redouble() noexcept {
    ++octaveShift_;
    refinedBpm_ = 0.0;
    lockHeld_ = 0;
    chooseOctave(state_.rawBpm);
    lockedBpm_ = inChosenOctave(state_.rawBpm);
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
    forgetFold(); // a new window, so a new question about which octave

    refinedBpm_ = 0.0;
    lockHeld_ = 0;
    chooseOctave(state_.rawBpm);
    lockedBpm_ = inChosenOctave(state_.rawBpm);
    state_.bpm = lockedBpm_;
    state_.refined = false;
    candidate_ = lockedBpm_;
    if (std::abs(state_.bpm - before) > options_.lockToleranceBpm) {
        // The tap really did move the tempo, so what was locked is no longer what is
        // published and the hunt starts again. A tap that agrees keeps the lock, as
        // nudging the window by hand does.
        state_.locked = false;
        everLocked_ = false; // and the tempo it would have held is the one just replaced
        agreeing_ = 0;
        disagreeing_ = 0;
        beatFrames_.clear();
    }
}

bool TempoTracker::lastBeatIsNearer() const noexcept {
    if (beatsSeen_ == 0 || filterIntervalFrames_ == 0) {
        return false; // no beat behind, or no idea when the next one is due
    }
    // Nearer than the next beat, whose best estimate is one period after the last one. The
    // comparison is doubled rather than halved so it stays in whole frames.
    return 2 * framesSinceBeat_ < filterIntervalFrames_;
}

void TempoTracker::snapDownbeat() noexcept {
    // Either way a correction is now owed to the outputs, and the next beat carries it.
    snapUnsent_ = true;
    if (!lastBeatIsNearer()) {
        snapPending_ = true; // the beat being pointed at has not happened yet
        return;
    }
    // The operator is pointing at the beat just called, so the bar starts there and this
    // beat's successor is beat 2 — not beat 1, which is what made every snap read a beat
    // late. Nothing is left pending: the rotation is known now and takes effect now.
    snapPending_ = false;
    const bool wasBarStart = state_.beatInBar == 1;
    startBarHere();
    state_.beatInBar = 1;
    if (!wasBarStart) {
        // A bar the operator has just declared, which the count above did not make. Had it
        // already been beat 1 the bar was counted when it was called, and counting it twice
        // would make a snap that changed nothing move the bar number.
        ++state_.bars;
    }
}

void TempoTracker::startBarHere() noexcept {
    const std::uint32_t meter = state_.beatsPerBar;
    snapAwaitingFilter_ = !(filterBeatInBar_ != 0 && meter > 0);
    if (snapAwaitingFilter_) {
        // Nothing to rotate yet. Count from here, and turn the count into an offset the
        // moment the filter first has an opinion.
        sinceSnap_ = 0;
        return;
    }
    // Rotate so this beat comes out as 1. Reducing the position first keeps this right
    // through a meter change that leaves the filter past the end of a bar.
    barOffset_ = (meter - (filterBeatInBar_ - 1) % meter) % meter;
}

void TempoTracker::setLockPinned(bool pinned) noexcept {
    if (pinned == lockPinned_) {
        return; // idempotent: a control surface may resend its state at will
    }
    lockPinned_ = pinned;
    state_.pinned = pinned;
    if (pinned) {
        // Pin what is showing. `lockedBpm_` is the tempo being published whether or not
        // the lock has been earned, so this locks to what the operator is looking at.
        // With nothing tracked there is nothing to pin, and acquisition — which the pin
        // does not touch — raises the flag in its own time.
        if (lockedBpm_ > 0.0) {
            state_.locked = true;
            disagreeing_ = 0;
        }
        return;
    }
    // Released: the lock goes too, and the beat spacing collected under it with it, so
    // what comes back is a genuine re-acquisition rather than the old tempo wearing a
    // new lock. That includes not *holding* the released tempo while hunting, which an
    // ordinary unlock does: the operator letting go is saying "that was wrong, look
    // again", and holding it would be the one thing they were trying to shed.
    state_.locked = false;
    everLocked_ = false;
    refinedBpm_ = 0.0;
    lockHeld_ = 0;
    state_.refined = false;
    agreeing_ = 0;
    disagreeing_ = 0;
    beatFrames_.clear();
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
        // A snap that named a beat still to come: this is it, so the bar starts here. The
        // one that named the beat just gone rotated in snapDownbeat() and left nothing
        // pending, which is why it falls through to the ordinary count below.
        snapPending_ = false;
        startBarHere();
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
    // "Is this a different tempo?" — never "is this a different number?". The state space's
    // neighbouring intervals are 4.3 % apart at 130 BPM, so a cloud whose median hops
    // between interval 22 and interval 23 is not changing tempo, and counting that as
    // disagreement is why no lock survived a long passage. See Options::sameTempoTolerance.
    const auto sameTempo = [this](double a, double b) noexcept {
        return b > 0.0 && std::abs(a - b) <= options_.sameTempoTolerance * b;
    };

    // Whichever tempo is currently being argued for, tracked whether or not there is a lock
    // — so the frames that unwound one already count towards whatever replaces it, and a
    // genuine change costs `unlockAfter + relockAfter` rather than the two added up twice.
    if (agreeing_ != 0 && sameTempo(folded, candidate_)) {
        ++agreeing_;
        // The band is six per cent wide and a run can start anywhere in it, so let the
        // candidate settle on the middle of its own evidence rather than on its first
        // frame. Capped, or a candidate agreed with for a minute would stop moving at all.
        const double weight = static_cast<double>(std::min(agreeing_, options_.lockAfter));
        candidate_ += (folded - candidate_) / weight;
    } else {
        candidate_ = folded;
        agreeing_ = 1;
    }

    if (state_.locked) {
        ++lockHeld_;
        if (sameTempo(folded, lockedBpm_)) {
            disagreeing_ = 0;
            return;
        }
        // Disagreement has to be sustained.
        if (++disagreeing_ < options_.unlockAfter) {
            return;
        }
        if (lockPinned_) {
            // The whole of the pin, in one branch: the disagreement is measured and simply
            // not acted on. Parked at the threshold rather than left to run, so the count
            // keeps meaning "still disagreeing" and not "for how long", which nothing asks
            // and which would overflow given a long enough set.
            disagreeing_ = options_.unlockAfter;
            return;
        }
        state_.locked = false;
        // `lockedBpm_` deliberately stays where it is. An unlock says the tracker is no
        // longer sure, not that it has a better answer, and moving the published tempo here
        // was the loudest half of "140 to 90 in seconds": the candidate that unwound the
        // lock was published the instant the count ran out, before anything had agreed with
        // it. Now it has to earn a lock of its own first, and a transient excursion — the
        // cloud wandering off through a breakdown and coming back — costs nothing at all,
        // because what it comes back to is the tempo still being published.
        disagreeing_ = 0;
        return;
    }

    // Not locked, pinned or not: a pin holds a flag up, it does not raise one.
    if (!everLocked_) {
        // Nothing to hold yet, so the published tempo follows the cloud and there is
        // something to look at while it hunts. After the first lock it holds — see process().
        lockedBpm_ = candidate_;
    }
    // Replacing an established tempo is a different question from finding one, and costs
    // more agreement: see Options::relockAfter. Coming back to what is already being
    // published is not a replacement, so it re-locks at the ordinary price and the operator
    // never sees the number move.
    const bool replacing = everLocked_ && !sameTempo(candidate_, lockedBpm_);
    std::size_t needed = options_.lockAfter;
    if (replacing) {
        // A share of how long the incumbent has held, between the two bounds. Charging the
        // full price from birth is what made a bad first lock permanent — see relockAfter.
        needed = std::clamp(lockHeld_ / kDefenceShare, options_.lockAfter, options_.relockAfter);
    }
    if (agreeing_ >= needed) {
        if (replacing) {
            refinedBpm_ = 0.0; // the beat spacing under the old tempo says nothing about this one
            lockHeld_ = 0;     // and this tempo has earned nothing yet either
        }
        state_.locked = true;
        everLocked_ = true;
        lockedBpm_ = candidate_;
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
    // Where in the beat we are, for a snap arriving between two frames to read. Kept even
    // when the interval is zero, because it is the last one known that says how long a beat
    // lasts, not this frame's absence of one.
    ++framesSinceBeat_;
    if (frame.intervalFrames > 0) {
        filterIntervalFrames_ = frame.intervalFrames;
    }

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
    // Which octave is decided from the cloud's continuous tempo; which tempo interval, from
    // its discrete one. Both used to come from the discrete value, and that is what made a
    // 140 BPM track under a 70-140 window unusable: the cloud reports interval 21 or
    // interval 22 there — 142.9 BPM or 136.4, with nothing between them — so the octave
    // decision was being taken on a number that steps by 6.5 BPM, either side of the edge.
    // The continuous value sits at 140.3 and crosses the edge slowly, which is the only
    // thing a hysteresis can do anything with.
    chooseOctave(frame.bpm > 0.0 ? frame.bpm : discrete);
    if (confident) {
        everConfident_ = true;
        updateLock(inChosenOctave(discrete));
    } else if (!everConfident_) {
        // Nothing good has been seen yet, so there is nothing to hold: follow the
        // estimate and let the gate take over once it has been believed once.
        lockedBpm_ = inChosenOctave(discrete);
    }
    // Below the gate, state_.bpm keeps whatever it last held (§5.5: "hold the last good
    // tempo, stop emitting new values, and say so").
    state_.holding = !confident && everConfident_;

    const bool emitted = frame.emitted != TrackedFrame::Emitted::None;
    if (emitted) {
        rememberBeat(frame.frameIndex);
        framesSinceBeat_ = 0;
        ++beatsSeen_;
    }

    // Holding below the confidence gate keeps whatever was last published (§5.5: "hold the
    // last good tempo, stop emitting new values, and say so"). Hunting after a lock has
    // been earned does the same, for the same reason: what the cloud's median says between
    // one lock and the next is a whole-frame interval walking around under a passage with
    // nothing percussive in it, and putting that on screen is how a steady track came to
    // read 140 then 90. `locked` going false is what says the tracker is unsure; the tempo
    // saying it too, by wandering, tells the operator nothing they can use.
    if (!state_.holding && (state_.locked || !everLocked_)) {
        state_.bpm = lockedBpm_;
        state_.refined = false;
        // Once the tempo is locked, the beats themselves say it more precisely than the
        // state space's whole-frame intervals ever can. Only believe that when it agrees
        // with the lock, so a refinement can sharpen the tempo but never change it.
        if (state_.locked) {
            const double tolerance = options_.refineTempoTolerance * lockedBpm_;
            // Only on a frame that brought a beat. Between beats nothing new is known about
            // the spacing — `beatFrames_` has not changed — and yet recomputing gave a
            // different answer every frame anyway, because the band of gaps it accepts is
            // sized from the cloud's period and *that* moves. So a gap would drop in or out
            // of the mean on a frame carrying no new information, and the published tempo
            // would step by a per cent or two for no reason an operator could name. That
            // accounted for 801 of the 1302 jumps left over `references/audio` once the fold
            // was fixed, all of them while locked.
            if (emitted) {
                const double gaps = refinedIntervalFrames(
                    frame.refinedIntervalFrames > 0.0 ? frame.refinedIntervalFrames
                                                      : static_cast<double>(frame.intervalFrames));
                if (gaps > 0.0) {
                    const double refined = inChosenOctave(60.0 / (gaps * secondsPerFrame_));
                    if (std::abs(refined - lockedBpm_) <= tolerance) {
                        refinedBpm_ = refinedBpm_ > 0.0 ? refinedBpm_ + options_.refineSmoothing *
                                                                            (refined - refinedBpm_)
                                                        : refined;
                    }
                }
            }
            // A frame with too few gaps near the cloud's period — which is every frame in
            // a bar the drums sat out — used to fall back to `lockedBpm_`, and the two
            // differ by up to 5.4 BPM at 130 because one is a whole-frame interval and the
            // other is not. Alternating between them *is* a jump, so the last refinement
            // is kept until the lock itself moves away from it.
            if (refinedBpm_ > 0.0 && std::abs(refinedBpm_ - lockedBpm_) <= tolerance) {
                state_.bpm = refinedBpm_;
                state_.refined = true;
            }
        }
    }

    if (!emitted) {
        return std::nullopt;
    }

    const bool snapped = snapUnsent_;
    snapUnsent_ = false;
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
