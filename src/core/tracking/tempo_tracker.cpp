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

/// The frame period every count in `TempoTracker::Options` is stated at: the network's.
constexpr double kReferenceSecondsPerFrame = 0.02;

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
    if (!(options.foldPhaseMemory > 0.0) || !(options.foldPhaseMemory < 1.0)) {
        throw std::invalid_argument(
            "TempoTracker: the fold's sub-grid evidence must decay by some fraction, and "
            "keep some of what it had, from one beat to the next");
    }
    if (!(options.foldPhaseMargin >= 1.0)) {
        throw std::invalid_argument(
            "TempoTracker: a sub-grid may not take the beat from the one in force by "
            "scoring less than it does");
    }
    if (options.foldSupportFrames == 0) {
        throw std::invalid_argument(
            "TempoTracker: believing an octave takes at least one frame of evidence");
    }
    if (!(options.refineSmoothing > 0.0) || !(options.refineSmoothing <= 1.0)) {
        throw std::invalid_argument(
            "TempoTracker: the refinement smoother must move some of the way to a new "
            "measurement and no further than all of it");
    }
    beatFrames_.reserve(options_.refineOverBeats + 1);
    scaleFrameCounts();
    reset();
}

void TempoTracker::scaleFrameCounts() noexcept {
    // Stated at 50 Hz, applied at whatever this tracker runs at: a decoder stepping twice a
    // hop gets twice the frames for the same half second. Never below one frame, and never
    // a full-price relock cheaper than an ordinary lock.
    const double scale = kReferenceSecondsPerFrame / secondsPerFrame_;
    const auto frames = [scale](std::size_t at50) {
        return std::max<std::size_t>(
            1, static_cast<std::size_t>(std::llround(static_cast<double>(at50) * scale)));
    };
    lockAfter_ = frames(options_.lockAfter);
    unlockAfter_ = frames(options_.unlockAfter);
    relockAfter_ = std::max(frames(options_.relockAfter), lockAfter_);
    foldSupportFrames_ = frames(options_.foldSupportFrames);
    confidenceSmoothing_ = std::max(1.0, options_.confidenceSmoothing * scale);
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
    beatOctave_ = 0;
    beatOctaveCandidate_ = 0;
    beatOctaveRun_ = 0;
    filterBeatInBar_ = 0;
    barOffset_ = 0;
    snapPending_ = false;
    snapUnsent_ = false;
    snapAwaitingFilter_ = false;
    sinceSnap_ = 0;
    framesSinceBeat_ = 0;
    beatsSeen_ = 0;
    filterIntervalFrames_ = 0;
    resetFoldPhase(1);
    framesSinceCalled_ = 0;
    beatsCalled_ = 0;
    foldRun_ = 0;
    foldSupported_ = false;
    sinceDownbeat_ = 0;
    anyDownbeat_ = false;
    beatFrames_.clear();
}

std::uint32_t TempoTracker::foldDivisor() const noexcept {
    if (!options_.foldBeats) {
        return 1;
    }
    // How far the published tempo sits below the filter's own grid, as a power of two. The
    // fold's octave and the operator's shift both count: they are the two things that move
    // the published tempo off the cloud's, and `inChosenOctave` applies exactly this sum.
    const std::int64_t shift = foldOctave_ + octaveShift_;
    if (shift >= 0) {
        return 1; // publishing at or above the filter's rate; nothing to divide out
    }
    // The window is a promise about the material and can be wrong for the record playing.
    // Dividing the grid on the automatic fold alone would halve a genuinely fast track, so
    // the cloud has to have shown that the slower octave is a reading of the music. A manual
    // ÷2 is an instruction and needs no evidence. See Options::foldSupportFrames.
    if (octaveShift_ == 0 && !foldSupported_) {
        return 1;
    }
    const std::int64_t steps = std::min<std::int64_t>(-shift, 2);
    return static_cast<std::uint32_t>(1) << steps;
}

void TempoTracker::resetFoldPhase(std::uint32_t divisor) noexcept {
    foldDivisor_ = divisor;
    foldSlot_ = 0;
    foldPhase_ = 0;
    foldAnchored_ = false;
    for (double& score : foldScore_) {
        score = 0.0;
    }
}

bool TempoTracker::onPublishedGrid(const TrackedFrame& frame) noexcept {
    // How many of the filter's beats have gone by since the last one it called — normally
    // one, but the filter drops beats through a quiet passage and a slot counted in *calls*
    // would take the published grid with it every time it did. Counted in periods instead,
    // the grid is anchored to the music's own time and a missed beat costs nothing.
    std::uint32_t slot = foldPhase_;
    if (foldAnchored_) {
        std::uint32_t steps = 1;
        if (filterIntervalFrames_ != 0) {
            const double periods = static_cast<double>(framesSinceCalled_) /
                                   static_cast<double>(filterIntervalFrames_);
            // Bounded by the divisor: past that a published beat is due whatever the phase
            // says, and an unbounded step would let one long gap rotate it anywhere.
            steps = static_cast<std::uint32_t>(
                std::clamp(std::llround(periods), 1LL, static_cast<long long>(foldDivisor_)));
        }
        slot = (foldSlot_ + steps) % foldDivisor_;
    }
    foldSlot_ = slot;
    foldAnchored_ = true;

    // P(this frame is a beat of any kind). The classes are a softmax over beat / downbeat /
    // non-beat, so a bar start reads high on the second and low on the first; either alone
    // would score the downbeat sub-grid as the weak one. See TrackedFrame.
    const double evidence =
        static_cast<double>(frame.beatActivation) + static_cast<double>(frame.downbeatActivation);
    // One average per sub-grid, each stepped only on its own beats, so that two of them are
    // compared at the same point in their cycle. See Options::foldPhaseMemory for what
    // decaying all of them on every beat does instead.
    foldScore_[slot] =
        options_.foldPhaseMemory * foldScore_[slot] + (1.0 - options_.foldPhaseMemory) * evidence;

    // The leader takes the grid only by a margin, as the meter is taken in
    // `ParticleFilter::meterOf` and for the same reason: two sub-grids trading places is two
    // beats in the wrong place every time they do it.
    std::uint32_t leader = foldPhase_;
    for (std::uint32_t candidate = 0; candidate < foldDivisor_; ++candidate) {
        if (foldScore_[candidate] > foldScore_[leader]) {
            leader = candidate;
        }
    }
    if (leader != foldPhase_ &&
        foldScore_[leader] > foldScore_[foldPhase_] * options_.foldPhaseMargin) {
        foldPhase_ = leader;
    }
    return slot == foldPhase_;
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

void TempoTracker::weighBeatOctave(double cloudIntervalFrames) noexcept {
    // This beat's own gap against the cloud's period *now*. Not a median over the last
    // couple of dozen gaps, which is what this was: that window lags a genuine tempo change
    // by half its length, so when the filter goes to double time — as it does on "03 -
    // Fake Sweat", legitimately — the stale median read as "the beats are an octave below
    // the cloud" for eight beats running and this rule fired on a filter that was agreeing
    // with itself. Measured over the 23 tracks of `references/audio` on 2026-09-08, the
    // median rule flagged a false octave on nine of them (Fake Sweat 14 % of beats, eight
    // changes; Jensen 9 %; Alias 8 %) and the per-beat rule on none, while catching Jamie
    // Lidell (89 %) and Moonlake (85 % against 74 %) at least as well. The run below is
    // what makes one gap safe to read: a missed or doubled beat gives a ratio no octave
    // fits and resets it, and nothing moves until `beatOctaveBeats` beats in a row agree.
    const double gap = beatFrames_.size() >= 2
                           ? static_cast<double>(beatFrames_.back() -
                                                 beatFrames_[beatFrames_.size() - 2])
                           : 0.0;
    std::int64_t candidate = 0;
    if (gap > 0.0 && cloudIntervalFrames > 0.0) {
        // A period twice the cloud's is a tempo half of it, so the sign flips on the way from
        // frames to BPM: the beats being *slower* is a *negative* octave.
        const double octaves = std::log2(gap / cloudIntervalFrames);
        const std::int64_t steps = std::llround(octaves);
        if (steps != 0 && std::abs(steps) <= 2 &&
            std::abs(octaves - static_cast<double>(steps)) <= options_.beatOctaveTolerance) {
            candidate = -steps;
        }
    }
    if (candidate != beatOctaveCandidate_) {
        beatOctaveCandidate_ = candidate;
        beatOctaveRun_ = 0;
    }
    ++beatOctaveRun_;
    if (beatOctaveRun_ < options_.beatOctaveBeats || beatOctave_ == beatOctaveCandidate_) {
        return;
    }
    // The beats have moved an octave against the cloud, so the tempo the fold and the lock
    // have been arguing about has moved with them. Ask the fold again about the beats' new
    // tempo, and carry the lock across by whatever the two answers add up to — which is
    // nothing at all when the fold takes back what the beats gave (a 214 cloud calling
    // beats at 107 under a 70–140 window: folded to 107 before, unfolded 107 now), and a
    // clean octave with the fold off. Either way the published number does not move for a
    // reason the operator cannot hear, and the lock is not argued with about it.
    const std::int64_t before = beatOctave_ + foldOctave_;
    beatOctave_ = beatOctaveCandidate_;
    state_.calledBpm = calledBpm(state_.rawBpm);
    chooseOctave(state_.calledBpm);
    const std::int64_t moved = (beatOctave_ + foldOctave_) - before;
    lockedBpm_ = applyShift(lockedBpm_, moved);
    candidate_ = applyShift(candidate_, moved);
    // The refinement described the octave that has just been left, the same reason a lock
    // moving to a different tempo drops it.
    refinedBpm_ = 0.0;
}

double TempoTracker::calledBpm(double cloudBpm) const noexcept {
    return applyShift(cloudBpm, beatOctave_);
}

double TempoTracker::publishedPeriodFrames() const noexcept {
    return static_cast<double>(filterIntervalFrames_) * static_cast<double>(foldDivisor()) *
           applyShift(1.0, -beatOctave_);
}

void TempoTracker::setOptions(const Options& options) noexcept {
    // A window the operator has moved is a new question, so the octave the fold had
    // settled on under the old one is not an answer to it.
    const bool windowMoved = options.octaveFold != options_.octaveFold ||
                             options.foldInDecoder != options_.foldInDecoder ||
                             options.minBpm != options_.minBpm || options.maxBpm != options_.maxBpm;
    options_ = options;
    scaleFrameCounts();
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
        chooseOctave(state_.calledBpm);
        lockedBpm_ = inChosenOctave(state_.calledBpm);
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
    // With the window inside the decoder there is nothing to choose: the tempo arrives in
    // the octave the posterior settled on, window and all. See Options::foldInDecoder.
    if (!options_.octaveFold || options_.foldInDecoder || !(bpm > 0.0)) {
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
    chooseOctave(state_.calledBpm);
    lockedBpm_ = inChosenOctave(state_.calledBpm);
    state_.bpm = lockedBpm_;
    state_.refined = false;
    // And the grid with it, on the press rather than on the next frame: a reader of the
    // state between the two would otherwise see the number halved and the divisor not.
    state_.beatDivisor = foldDivisor();
    candidate_ = lockedBpm_;
    beatFrames_.clear();
}

void TempoTracker::redouble() noexcept {
    ++octaveShift_;
    refinedBpm_ = 0.0;
    lockHeld_ = 0;
    chooseOctave(state_.calledBpm);
    lockedBpm_ = inChosenOctave(state_.calledBpm);
    state_.bpm = lockedBpm_;
    state_.refined = false;
    state_.beatDivisor = foldDivisor();
    candidate_ = lockedBpm_;
    beatFrames_.clear();
}

void TempoTracker::seedTempo(double bpm) noexcept {
    if (!(bpm > 0.0)) {
        return;
    }
    const double before = state_.bpm;
    if (options_.octaveFold) {
        // An octave centred on the tap, so a tap 5 % out — which every human tap is — still
        // lands the tempo it meant squarely inside, and its neighbours squarely outside.
        const double half = std::sqrt(2.0);
        options_.minBpm = bpm / half;
        options_.maxBpm = bpm * half;
        octaveShift_ = 0;
        forgetFold(); // a new window, so a new question about which octave
    } else if (state_.calledBpm > 0.0) {
        // **A tap does not switch the fold on**, and that is the whole of this branch.
        //
        // It used to: `octaveFold = true` sat here, on the reasoning that an operator naming
        // a tempo has named a window too. But takt4 is handed a *set* — one record after
        // another — and the window that a tap set for the record playing then stayed on and
        // silently halved or doubled the next one. Reported from a rig on 2026-09-08:
        // "octave fold keeps getting automatically turned on, which then breaks the next
        // track in the mix because it might not need octave folding". A setting the operator
        // switched off must not come back on because they tapped.
        //
        // So with the fold off, the tap moves the published tempo onto the octave it named
        // using the manual shift — which is exactly what ÷2 and ×2 do, is the only thing
        // "the operator said which octave" can mean without a window, and lasts for as long
        // as the tracker is on this tempo rather than for the rest of the set.
        //
        // Against the tempo the beats are on, not the cloud's: an operator tapping 100 over
        // Moonlake — cloud at 200, beats at 100, readout already 100 — has named the octave
        // being published and asked for nothing to move.
        octaveShift_ = std::llround(std::log2(bpm / state_.calledBpm));
    }

    refinedBpm_ = 0.0;
    lockHeld_ = 0;
    chooseOctave(state_.calledBpm);
    lockedBpm_ = inChosenOctave(state_.calledBpm);
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
    //
    // The period is the published one — the filter's, times whatever the fold divides the
    // grid by and whatever octave the filter is emitting on. The operator is pointing at a
    // beat they can hear, and under a fold that halves the grid every second of the filter's
    // beats is silent.
    const double period = publishedPeriodFrames();
    return 2.0 * static_cast<double>(framesSinceBeat_) < period;
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
        const double weight = static_cast<double>(std::min(agreeing_, lockAfter_));
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
        if (++disagreeing_ < unlockAfter_) {
            return;
        }
        if (lockPinned_) {
            // The whole of the pin, in one branch: the disagreement is measured and simply
            // not acted on. Parked at the threshold rather than left to run, so the count
            // keeps meaning "still disagreeing" and not "for how long", which nothing asks
            // and which would overflow given a long enough set.
            disagreeing_ = unlockAfter_;
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
    std::size_t needed = lockAfter_;
    if (replacing) {
        // A share of how long the incumbent has held, between the two bounds. Charging the
        // full price from birth is what made a bad first lock permanent — see relockAfter.
        needed = std::clamp(lockHeld_ / kDefenceShare, lockAfter_, relockAfter_);
    }
    if (agreeing_ >= needed) {
        if (replacing) {
            refinedBpm_ = 0.0; // the beat spacing under the old tempo says nothing about this one
            lockHeld_ = 0;     // and this tempo has earned nothing yet either
            // Nor does the octave evidence: it was gathered about a tempo that has just been
            // replaced. See Options::foldSupportFrames.
            foldRun_ = 0;
            foldSupported_ = false;
            // **`beatOctave_` is deliberately *not* cleared here**, unlike everything above
            // it. Whether the filter emits on the grid it reports is a property of the
            // filter, not of the tempo it has just moved to, so a lock move says nothing
            // about it — and clearing it would cost `beatOctaveBeats` beats of publishing an
            // octave the beats are not on, every time the lock moved. Nothing is risked by
            // keeping it: the ratio is re-measured against the new interval on the very next
            // called beat and falls back to zero within `beatOctaveBeats` if it no longer
            // holds. (Measured on Moonlake, clearing it and not clearing it give the same
            // trace to the frame, so the argument above is the reason rather than the
            // measurement: that track's cloud sits on interval 15 throughout.)
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
    const double alpha = 1.0 / confidenceSmoothing_;
    smoothedConfidence_ += alpha * (frame.tempoAgreement - smoothedConfidence_);
    state_.confidence = smoothedConfidence_;
    state_.rawBpm = frame.bpm;
    state_.calledBpm = calledBpm(frame.bpm);
    state_.beatsPerBar = frame.beatsPerBar;
    // Where in the beat we are, for a snap arriving between two frames to read. Kept even
    // when the interval is zero, because it is the last one known that says how long a beat
    // lasts, not this frame's absence of one.
    ++framesSinceBeat_;
    ++framesSinceCalled_;
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
    // Both in the octave the filter's *beats* are on rather than the cloud's, because from
    // here down everything is about the beats going out — see Options::beatOctaveBeats for
    // the two tracks published at half their tempo when the fold saw the cloud's octave and
    // the beats' was applied afterwards, on top of it.
    const double beatsBpm = calledBpm(frame.bpm > 0.0 ? frame.bpm : discrete);
    const double beatsDiscrete = calledBpm(discrete);
    const bool confident = smoothedConfidence_ >= options_.confidenceThreshold;
    // Which octave is decided from the cloud's continuous tempo; which tempo interval, from
    // its discrete one. Both used to come from the discrete value, and that is what made a
    // 140 BPM track under a 70-140 window unusable: the cloud reports interval 21 or
    // interval 22 there — 142.9 BPM or 136.4, with nothing between them — so the octave
    // decision was being taken on a number that steps by 6.5 BPM, either side of the edge.
    // The continuous value sits at 140.3 and crosses the edge slowly, which is the only
    // thing a hysteresis can do anything with.
    chooseOctave(beatsBpm);
    if (confident) {
        everConfident_ = true;
        updateLock(inChosenOctave(beatsDiscrete));
    } else if (!everConfident_) {
        // Nothing good has been seen yet, so there is nothing to hold: follow the
        // estimate and let the gate take over once it has been believed once.
        lockedBpm_ = inChosenOctave(beatsDiscrete);
    }
    // Below the gate, state_.bpm keeps whatever it last held (§5.5: "hold the last good
    // tempo, stop emitting new values, and say so").
    state_.holding = !confident && everConfident_;

    // Whether the music supports the octave the window asked for: how much of the recent past
    // the beats' own continuous tempo has spent *at* the published tempo rather than at a
    // multiple of it. See Options::foldSupportFrames — this is the whole of what tells a
    // double-timed 92 BPM track from a 204 BPM one, and the activations cannot.
    const bool atPublished =
        lockedBpm_ > 0.0 && beatsBpm > 0.0 &&
        std::abs(beatsBpm - lockedBpm_) <= options_.sameTempoTolerance * lockedBpm_;
    foldRun_ = atPublished ? foldRun_ + 1 : 0;
    if (foldRun_ >= foldSupportFrames_) {
        foldSupported_ = true;
    }

    // Two different questions from here on: whether the *filter* called a beat, and whether
    // one is *published*. They are the same until the fold divides the grid.
    const bool called = frame.emitted != TrackedFrame::Emitted::None;
    // The refinement measures the filter's own spacing against the cloud's own period and
    // folds the answer afterwards, so it goes on seeing every beat the filter calls. Taking
    // only the published ones would leave it averaging gaps twice the length of the band it
    // accepts them in, and it would reject all of them.
    if (called) {
        rememberBeat(frame.frameIndex);
        // After the gap has been remembered, so this beat counts towards the measurement.
        weighBeatOctave(frame.refinedIntervalFrames > 0.0
                            ? frame.refinedIntervalFrames
                            : static_cast<double>(frame.intervalFrames));
    }

    const std::uint32_t divisor = foldDivisor();
    // Published so the window can say when the fold moved the number and left the beats
    // where they were — see `TempoState::beatDivisor`.
    state_.beatDivisor = divisor;
    if (divisor != foldDivisor_) {
        // Evidence gathered over two sub-grids says nothing about four, and the fold moving
        // octave means the beats being weighed against each other are different beats.
        resetFoldPhase(divisor);
    }
    bool emitted = called;
    bool downbeat = frame.emitted == TrackedFrame::Emitted::Downbeat;
    if (called && divisor > 1) {
        emitted = onPublishedGrid(frame);
        // A filter bar is `beatsPerBar` of the filter's beats, so under a divisor the filter
        // calls a downbeat oftener than one is due. A bar may not be shorter than the meter
        // counted in published beats — which is inert at a divisor of one, and closes for
        // any meter: `divisor` filter bars are `beatsPerBar` published beats. See
        // `sinceDownbeat_`.
        downbeat = emitted && downbeat && anyDownbeat_ &&
                   sinceDownbeat_ + 1 >= std::max<std::uint32_t>(state_.beatsPerBar, 1);
        if (emitted && frame.emitted == TrackedFrame::Emitted::Downbeat && !anyDownbeat_) {
            downbeat = true; // the first bar has nothing to be too short against
        }
    }
    if (called) {
        framesSinceCalled_ = 0;
        ++beatsCalled_;
    }
    if (emitted) {
        framesSinceBeat_ = 0;
        ++beatsSeen_;
        sinceDownbeat_ = downbeat ? 0 : sinceDownbeat_ + 1;
        anyDownbeat_ = anyDownbeat_ || downbeat;
    }

    // Holding below the confidence gate keeps whatever was last published (§5.5: "hold the
    // last good tempo, stop emitting new values, and say so"). Hunting after a lock has
    // been earned does the same, for the same reason: what the cloud's median says between
    // one lock and the next is a whole-frame interval walking around under a passage with
    // nothing percussive in it, and putting that on screen is how a steady track came to
    // read 140 then 90. `locked` going false is what says the tracker is unsure; the tempo
    // saying it too, by wandering, tells the operator nothing they can use.
    if (!state_.holding && (state_.locked || !everLocked_)) {
        // Already in the octave the beats are actually on: the lock was given the beats'
        // tempo, not the cloud's — see `Options::beatOctaveBeats` — so the refinement below
        // works in whichever octave the beats turn out to be in without anything more.
        const double published = lockedBpm_;
        state_.bpm = published;
        state_.refined = false;
        // Once the tempo is locked, the beats themselves say it more precisely than the
        // state space's whole-frame intervals ever can. Only believe that when it agrees
        // with the lock, so a refinement can sharpen the tempo but never change it.
        if (state_.locked) {
            const double tolerance = options_.refineTempoTolerance * published;
            // Only on a frame that brought a beat. Between beats nothing new is known about
            // the spacing — `beatFrames_` has not changed — and yet recomputing gave a
            // different answer every frame anyway, because the band of gaps it accepts is
            // sized from the cloud's period and *that* moves. So a gap would drop in or out
            // of the mean on a frame carrying no new information, and the published tempo
            // would step by a per cent or two for no reason an operator could name. That
            // accounted for 801 of the 1302 jumps left over `references/audio` once the fold
            // was fixed, all of them while locked.
            //
            // Every beat the *filter* called, not every one published: those are the gaps
            // `beatFrames_` holds, and a beat the fold left out still moved it.
            if (called) {
                const double cloudPeriod =
                    frame.refinedIntervalFrames > 0.0
                        ? frame.refinedIntervalFrames
                        : static_cast<double>(frame.intervalFrames);
                // Banded around the period the beats are *arriving* at rather than the one
                // the cloud reports. They are the same number unless the filter is emitting
                // an octave away from its own tempo, and there the cloud's band rejects every
                // gap — so the refinement went silent exactly where the tempo needed it most.
                const double gaps = refinedIntervalFrames(applyShift(cloudPeriod, -beatOctave_));
                if (gaps > 0.0) {
                    // `inChosenOctave` and not `published`: the gaps are already in the beats'
                    // octave, so applying `beatOctave_` again would move it twice.
                    const double refined = inChosenOctave(60.0 / (gaps * secondsPerFrame_));
                    if (std::abs(refined - published) <= tolerance) {
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
            if (refinedBpm_ > 0.0 && std::abs(refinedBpm_ - published) <= tolerance) {
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
    advanceBar(downbeat);
    ++state_.beats;

    BeatEvent event;
    event.frameIndex = frame.frameIndex;
    // Where inside the frame the decoder put the beat, when it can say — see
    // `TrackedFrame::beatOffsetFrames`. Zero for the particle filter.
    event.time = (static_cast<double>(frame.frameIndex) + frame.beatOffsetFrames) * secondsPerFrame_ +
                 options_.latencyOffsetSeconds;
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
