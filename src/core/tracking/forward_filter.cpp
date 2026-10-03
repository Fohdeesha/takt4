#include "core/tracking/forward_filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace takt4::tracking {

namespace {

/// madmom's `exponential_transition` zeroes anything at or below machine epsilon before
/// normalising; kept so the rows are the rows it would build.
constexpr double kTransitionThreshold = std::numeric_limits<double>::epsilon();
/// The prototype turned a log density of −inf into −30 before exponentiating
/// (`decoders.py`, `densities`): an activation of exactly zero is very unlikely, not
/// impossible.
const double kDensityFloor = std::exp(-30.0);
/// A softmax over three classes sums to one; the two carried here to at most 0.999 so the
/// non-beat density stays positive, as the prototype clipped them.
constexpr double kActivationCeiling = 0.999;

} // namespace

ForwardFilter::ForwardFilter() : ForwardFilter(Options{}) {}

ForwardFilter::ForwardFilter(Options options) : options_(options) {
    if (options_.fps == 0) {
        throw std::invalid_argument("ForwardFilter: the frame rate must be positive");
    }
    if (!(options_.minBpm > 0.0) || !(options_.maxBpm > options_.minBpm)) {
        throw std::invalid_argument("ForwardFilter: the tempo range must be a positive range");
    }
    if (!(options_.transitionLambda > 0.0)) {
        throw std::invalid_argument("ForwardFilter: the tempo-change lambda must be positive");
    }
    if (options_.observationLambda < 2) {
        throw std::invalid_argument(
            "ForwardFilter: a beat has to be split into at least two parts");
    }
    if (!(options_.windowPenalty > 0.0) || !(options_.windowPenalty <= 1.0) ||
        !(options_.holdPenalty > 0.0) || !(options_.holdPenalty <= 1.0)) {
        throw std::invalid_argument("ForwardFilter: a penalty is a weight in (0, 1]");
    }
    if (!(options_.meterChangeProbability >= 0.0) || !(options_.meterChangeProbability < 1.0) ||
        !(options_.floor >= 0.0) || !(options_.floor < 1.0) || !(options_.meterFloor >= 0.0) ||
        !(options_.meterFloor < 0.5)) {
        throw std::invalid_argument(
            "ForwardFilter: the meter-change probability, the floor and the meter floor are "
            "shares of the posterior, below one");
    }
    if (!(options_.minimumBeatFraction >= 0.0) || !(options_.minimumBeatFraction < 1.0)) {
        throw std::invalid_argument("ForwardFilter: the minimum beat spacing is in [0, 1)");
    }
    if (!(options_.predictFrames >= 0.0)) {
        throw std::invalid_argument("ForwardFilter: a beat cannot be predicted a negative time ahead");
    }
    if (!(options_.meterMargin >= 1.0)) {
        throw std::invalid_argument("ForwardFilter: the meter margin is at least one");
    }
    if (!(options_.coastContrast >= 0.0) || !(options_.coastContrast <= 1.0)) {
        throw std::invalid_argument("ForwardFilter: the coasting contrast is an activation, 0 to 1");
    }
    buildStateSpace();
    buildTransitions();

    const std::size_t n = intervals_.size();
    windowWeight_.assign(n, 1.0);
    holdWeight_.assign(n, 1.0);
    intervalWeight_.assign(n, 1.0);
    posterior_.assign(numStates_, 0.0);
    next_.assign(numStates_, 0.0);
    evidencePosterior_.assign(numStates_, 0.0);
    recentLevel_.assign(std::max<std::size_t>(1, intervals_.back() / 2), 0.0);
    lastMass_.assign(n, 0.0);
    incoming_.assign(n, 0.0);
    barIncoming_.assign(patterns_.size() * n, 0.0);
    intervalMass_.assign(n, 0.0);
    patternMass_.assign(patterns_.size(), 0.0);
    beatMarginal_.assign(statesPerBeat_, 0.0);
    reset();
}

void ForwardFilter::buildStateSpace() {
    // madmom's `BeatStateSpace`: every whole-frame interval from round(min) to round(max).
    const double fps = static_cast<double>(options_.fps);
    const double minInterval = 60.0 * fps / options_.maxBpm;
    const double maxInterval = 60.0 * fps / options_.minBpm;
    const auto first = static_cast<std::uint32_t>(std::lround(minInterval));
    const auto last = static_cast<std::uint32_t>(std::lround(maxInterval));
    if (first < 1 || last < first) {
        throw std::invalid_argument("ForwardFilter: the tempo range holds no whole-frame interval");
    }
    intervals_.clear();
    firstOfInterval_.clear();
    statesPerBeat_ = 0;
    for (std::uint32_t interval = first; interval <= last; ++interval) {
        intervals_.push_back(interval);
        firstOfInterval_.push_back(statesPerBeat_);
        statesPerBeat_ += interval;
    }
    if (intervals_.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("ForwardFilter: too many tempo intervals");
    }

    patterns_.clear();
    numStates_ = 0;
    for (const std::uint8_t meter : options_.meters) {
        if (meter == 0) {
            break;
        }
        patterns_.push_back(Pattern{meter, numStates_});
        numStates_ += static_cast<std::size_t>(meter) * statesPerBeat_;
    }
    if (patterns_.empty()) {
        throw std::invalid_argument("ForwardFilter: at least one meter is needed");
    }
    if (patterns_.size() > std::numeric_limits<std::uint8_t>::max()) {
        throw std::invalid_argument("ForwardFilter: too many meters");
    }

    stateInterval_.assign(numStates_, 0);
    statePattern_.assign(numStates_, 0);
    stateBeat_.assign(numStates_, 0);
    statePointer_.assign(numStates_, 0);
    statePhase_.assign(numStates_, 0.0f);
    phaseCos_.assign(numStates_, 0.0f);
    phaseSin_.assign(numStates_, 0.0f);
    // madmom's `RNNDownBeatTrackingObservationModel`: the first `border` of every beat is
    // a beat state, of the bar's first beat a downbeat state.
    const double border = 1.0 / static_cast<double>(options_.observationLambda);
    for (std::size_t p = 0; p < patterns_.size(); ++p) {
        const Pattern& pattern = patterns_[p];
        for (std::uint32_t beat = 0; beat < pattern.meter; ++beat) {
            std::size_t state = pattern.offset + static_cast<std::size_t>(beat) * statesPerBeat_;
            for (std::size_t i = 0; i < intervals_.size(); ++i) {
                const std::uint32_t interval = intervals_[i];
                for (std::uint32_t phase = 0; phase < interval; ++phase, ++state) {
                    // linspace(0, 1, interval, endpoint=False)
                    const double position = static_cast<double>(phase) / static_cast<double>(interval);
                    stateInterval_[state] = static_cast<std::uint16_t>(i);
                    statePattern_[state] = static_cast<std::uint8_t>(p);
                    stateBeat_[state] = static_cast<std::uint8_t>(beat);
                    statePointer_[state] =
                        position < border ? static_cast<std::uint8_t>(beat == 0 ? 2 : 1)
                                          : static_cast<std::uint8_t>(0);
                    statePhase_[state] = static_cast<float>(position);
                    const double angle = 2.0 * std::numbers::pi * position;
                    phaseCos_[state] = static_cast<float>(std::cos(angle));
                    phaseSin_[state] = static_cast<float>(std::sin(angle));
                }
            }
        }
    }
}

void ForwardFilter::buildTransitions() {
    // madmom's `exponential_transition`: rows are the interval a beat ends on, columns the
    // one the next beat starts on, exp(−λ · |to / from − 1|), thresholded, then normalised
    // over the row.
    const std::size_t n = intervals_.size();
    tempoTransition_.assign(n * n, 0.0);
    for (std::size_t from = 0; from < n; ++from) {
        double sum = 0.0;
        for (std::size_t to = 0; to < n; ++to) {
            const double ratio = static_cast<double>(intervals_[to]) / static_cast<double>(intervals_[from]);
            double probability = std::exp(-options_.transitionLambda * std::abs(ratio - 1.0));
            if (probability <= kTransitionThreshold) {
                probability = 0.0;
            }
            tempoTransition_[from * n + to] = probability;
            sum += probability;
        }
        for (std::size_t to = 0; to < n; ++to) {
            tempoTransition_[from * n + to] /= sum;
        }
    }
    // The same numbers, a column to a row, for `transition` to read in order.
    tempoTransposed_.assign(n * n, 0.0);
    for (std::size_t from = 0; from < n; ++from) {
        for (std::size_t to = 0; to < n; ++to) {
            tempoTransposed_[to * n + from] = tempoTransition_[from * n + to];
        }
    }
}

double ForwardFilter::bpmOfInterval(std::size_t interval) const noexcept {
    return 60.0 * static_cast<double>(options_.fps) / static_cast<double>(intervals_[interval]);
}

std::span<const double> ForwardFilter::tempoTransitions(std::size_t from) const noexcept {
    const std::size_t n = intervals_.size();
    return std::span<const double>(tempoTransition_).subspan(from * n, n);
}

void ForwardFilter::reset() noexcept {
    const double uniform = 1.0 / static_cast<double>(numStates_);
    std::fill(posterior_.begin(), posterior_.end(), uniform);
    std::fill(patternMass_.begin(), patternMass_.end(), 0.0);
    std::fill(intervalMass_.begin(), intervalMass_.end(), 0.0);
    counter_ = 0;
    armed_ = false;
    lastEvidence_ = 0;
    evidenceFrame_ = 0;
    evidenceInterval_ = 0;
    std::fill(recentLevel_.begin(), recentLevel_.end(), 0.0);
    recentAt_ = 0;
    everHeard_ = false;
    coasting_ = false;
    everEmitted_ = false;
    lastEmitFrame_ = 0.0;
    lastBeatIndex_ = -1;
    lastPosition_ = 0.0;
    lastMeanPhase_ = 0.0;
    haveMeanPhase_ = false;
    meterNow_ = 0;
    inRange_ = false;
    rangeEmitted_ = false;
    rangeBeatIndex_ = 0;
    rangeBestActivation_ = -1.0;
    rangeBestFrame_ = 0;
    lastActivation_ = 0.0;
    lastActivation2_ = 0.0;
    // The window and the hold are settings, not state: they survive a reset as
    // `TempoTracker`'s options survive its own.
}

void ForwardFilter::silence() noexcept {
    // `reset`, less three things that carry on: the frame count, which every frame index and
    // beat time downstream is measured in; the recent levels, which the next beat's contrast is
    // judged against; and when the last beat went out, so the spacing rule holds across the gap.
    const double uniform = 1.0 / static_cast<double>(numStates_);
    std::fill(posterior_.begin(), posterior_.end(), uniform);
    std::fill(patternMass_.begin(), patternMass_.end(), 0.0);
    std::fill(intervalMass_.begin(), intervalMass_.end(), 0.0);
    armed_ = false;
    lastEvidence_ = 0;
    evidenceFrame_ = 0;
    evidenceInterval_ = 0;
    everHeard_ = false;
    coasting_ = false;
    lastBeatIndex_ = -1;
    lastPosition_ = 0.0;
    lastMeanPhase_ = 0.0;
    haveMeanPhase_ = false;
    inRange_ = false;
    rangeEmitted_ = false;
    rangeBeatIndex_ = 0;
    rangeBestActivation_ = -1.0;
    rangeBestFrame_ = 0;
}

void ForwardFilter::combineWeights() noexcept {
    anyWeight_ = false;
    anyHold_ = false;
    for (std::size_t i = 0; i < intervalWeight_.size(); ++i) {
        intervalWeight_[i] = windowWeight_[i] * holdWeight_[i];
        anyWeight_ = anyWeight_ || intervalWeight_[i] != 1.0;
        anyHold_ = anyHold_ || holdWeight_[i] != 1.0;
    }
}

void ForwardFilter::setTempoWindow(double minBpm, double maxBpm, bool enabled) noexcept {
    for (std::size_t i = 0; i < intervals_.size(); ++i) {
        const double bpm = bpmOfInterval(i);
        const bool inside = !enabled || (bpm >= minBpm && bpm <= maxBpm);
        windowWeight_[i] = inside ? 1.0 : options_.windowPenalty;
    }
    combineWeights();
}

void ForwardFilter::holdTempo(double bpm) noexcept {
    heldBpm_ = bpm > 0.0 ? bpm : 0.0;
    if (heldBpm_ <= 0.0) {
        std::fill(holdWeight_.begin(), holdWeight_.end(), 1.0);
        combineWeights();
        return;
    }
    // The interval nearest the held tempo, and one either side of it: the state space is
    // whole frames, so a tempo between two intervals needs both to keep its phase right.
    const double wanted = 60.0 * static_cast<double>(options_.fps) / heldBpm_;
    std::size_t nearest = 0;
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < intervals_.size(); ++i) {
        const double distance = std::abs(static_cast<double>(intervals_[i]) - wanted);
        if (distance < best) {
            best = distance;
            nearest = i;
        }
    }
    for (std::size_t i = 0; i < intervals_.size(); ++i) {
        const std::size_t distance = i > nearest ? i - nearest : nearest - i;
        holdWeight_[i] = distance <= 1 ? 1.0 : options_.holdPenalty;
    }
    combineWeights();
}

void ForwardFilter::transition(bool keepTempo) noexcept {
    const std::size_t n = intervals_.size();
    // --- transition ---------------------------------------------------------------------
    // Within a beat every state steps to the next: the posterior shifts by one. At the
    // first state of each beat, what arrives is the mass that ended the previous beat on
    // every tempo, redistributed by the tempo-change rows — or, with `keepTempo`, on the
    // tempo it ended on, which is what coasting does so the tempo cannot wander while
    // there is nothing to hold it.
    for (std::size_t p = 0; p < patterns_.size(); ++p) {
        const Pattern& pattern = patterns_[p];
        for (std::uint32_t b = 0; b < pattern.meter; ++b) {
            const std::uint32_t previous = b == 0 ? pattern.meter - 1 : b - 1;
            const std::size_t fromBase = pattern.offset + static_cast<std::size_t>(previous) * statesPerBeat_;
            for (std::size_t i = 0; i < n; ++i) {
                lastMass_[i] = posterior_[fromBase + firstOfInterval_[i] + intervals_[i] - 1];
            }
            if (keepTempo) {
                std::copy(lastMass_.begin(), lastMass_.end(), incoming_.begin());
            } else {
                // Along a row of the transposed matrix: the same terms in the same order as
                // walking down a column of `tempoTransition_`, so the same sum to the bit, and
                // read from consecutive memory rather than one element a row apart (the
                // audit's Low items).
                for (std::size_t to = 0; to < n; ++to) {
                    const double* const into = tempoTransposed_.data() + to * n;
                    double sum = 0.0;
                    for (std::size_t from = 0; from < n; ++from) {
                        sum += into[from] * lastMass_[from];
                    }
                    incoming_[to] = sum;
                }
            }
            const std::size_t toBase = pattern.offset + static_cast<std::size_t>(b) * statesPerBeat_;
            if (b == 0) {
                // Kept aside: a bar boundary may also be where the meter changes, and the
                // other patterns' arrivals are not known until every pattern has been
                // walked.
                std::copy(incoming_.begin(), incoming_.end(), barIncoming_.begin() + static_cast<std::ptrdiff_t>(p * n));
            }
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t first = toBase + firstOfInterval_[i];
                next_[first] = incoming_[i];
                for (std::uint32_t phase = 1; phase < intervals_[i]; ++phase) {
                    next_[first + phase] = posterior_[first + phase - 1];
                }
            }
        }
    }
    if (options_.meterChangeProbability > 0.0 && patterns_.size() > 1 && !keepTempo) {
        const double stay = 1.0 - options_.meterChangeProbability;
        const double leave = options_.meterChangeProbability / static_cast<double>(patterns_.size() - 1);
        for (std::size_t p = 0; p < patterns_.size(); ++p) {
            const std::size_t base = patterns_[p].offset;
            for (std::size_t i = 0; i < n; ++i) {
                double mass = stay * barIncoming_[p * n + i];
                for (std::size_t q = 0; q < patterns_.size(); ++q) {
                    if (q != p) {
                        mass += leave * barIncoming_[q * n + i];
                    }
                }
                next_[base + firstOfInterval_[i]] = mass;
            }
        }
    }
}

void ForwardFilter::weigh(const double* density) noexcept {
    // --- observation, and the operator's evidence ---------------------------------------
    double total = 0.0;
    std::fill(intervalMass_.begin(), intervalMass_.end(), 0.0);
    std::fill(patternMass_.begin(), patternMass_.end(), 0.0);
    // Coasting there is no `density`: the audio says nothing, so it weighs nothing — **and nor
    // does the window**. The window is a preference argued against the music, and with no music
    // to argue with it wins by default: a tempo the music had held outside it slid to the
    // window's nearest edge, 0.97 a frame — measured, 128 under a 50-100 window was 99.8 after
    // twenty seconds of silence, a tempo nobody was playing (the audit of 2026-09-25, L41). The
    // hold is an instruction, and still holds.
    const bool coasting = density == nullptr;
    const std::vector<double>& weights = coasting ? holdWeight_ : intervalWeight_;
    const bool weighed = coasting ? anyHold_ : anyWeight_;
    for (std::size_t s = 0; s < numStates_; ++s) {
        double mass = coasting ? next_[s] : next_[s] * density[statePointer_[s]];
        if (weighed) {
            mass *= weights[stateInterval_[s]];
        }
        next_[s] = mass;
        total += mass;
    }
    if (!(total > 0.0) || !std::isfinite(total)) {
        // Every hypothesis is impossible, which only rounding can bring about: start again
        // from nothing rather than divide by it.
        const double uniform = 1.0 / static_cast<double>(numStates_);
        std::fill(next_.begin(), next_.end(), uniform);
        total = 1.0;
    }
    const double scale = 1.0 / total;
    const double keep = 1.0 - options_.floor;
    const double spread = options_.floor / static_cast<double>(numStates_);
    for (std::size_t s = 0; s < numStates_; ++s) {
        const double mass = next_[s] * scale * keep + spread;
        next_[s] = mass;
        patternMass_[statePattern_[s]] += mass;
    }
    posterior_.swap(next_);
}

void ForwardFilter::rewindToEvidence() noexcept {
    // Back to the posterior as it stood on the last frame a beat was heard, and forward
    // again over every frame since as the coasting frames they turned out to be. The
    // frames in between were decoded as music whose beat had not come yet, and a beat that
    // never comes is strong evidence against the tempo that expected it: with exact zeros
    // a 128 BPM posterior was at 106 before the silence was a second old. Replayed, it is
    // as if the filter had coasted from the last beat on.
    std::copy(evidencePosterior_.begin(), evidencePosterior_.end(), posterior_.begin());
    for (std::uint64_t f = evidenceFrame_ + 1; f < counter_; ++f) {
        transition(true);
        weigh(nullptr);
    }
}

TrackedFrame ForwardFilter::process(float beatActivation, float downbeatActivation) noexcept {
    const std::size_t n = intervals_.size();
    const double now = static_cast<double>(counter_);

    TrackedFrame frame;
    frame.frameIndex = counter_;
    frame.beatActivation = beatActivation;
    frame.downbeatActivation = downbeatActivation;

    // The observation densities, madmom's three: what is left for a non-beat state, the
    // beat probability, the downbeat probability.
    double beat = std::clamp(static_cast<double>(beatActivation), 0.0, 1.0);
    double down = std::clamp(static_cast<double>(downbeatActivation), 0.0, 1.0);
    const double both = beat + down;
    if (both > kActivationCeiling) {
        beat *= kActivationCeiling / both;
        down *= kActivationCeiling / both;
    }
    const double density[3] = {
        std::max((1.0 - beat - down) / static_cast<double>(options_.observationLambda - 1),
                 kDensityFloor),
        std::max(beat, kDensityFloor),
        std::max(down, kDensityFloor),
    };
    // A beat heard is the activation up at the threshold *and* well clear of the lowest it has
    // been over half a slow beat — the top of a peak, not a plateau. See Options::coastContrast.
    const double level = std::max(beat, down);
    const double trough = *std::min_element(recentLevel_.begin(), recentLevel_.end());
    recentLevel_[recentAt_] = level;
    recentAt_ = (recentAt_ + 1) % recentLevel_.size();
    if (level >= options_.armThreshold) {
        armed_ = true;
    }
    const bool heard = level >= options_.armThreshold && level - trough >= options_.coastContrast;
    // **Nothing is evidence until a beat has been heard** — see the class note. The posterior is
    // left as it is, which before the first beat is the uniform prior, and the frame says no
    // tempo and no agreement: there is no beat yet to have a tempo of.
    if (!everHeard_ && !heard) {
        frame.beatsPerBar = meterNow_;
        lastActivation2_ = lastActivation_;
        lastActivation_ = beat + down;
        ++counter_;
        return frame;
    }
    // A beat that was due and never came: the music, or at least its beat, has stopped, and
    // nothing is taken from it until it comes back — see Options::coastAfterFrames.
    const std::uint64_t coastAfter =
        options_.coastAfterFrames > 0
            ? options_.coastAfterFrames
            : static_cast<std::uint64_t>(std::ceil(1.5 * static_cast<double>(evidenceInterval_)));
    const bool coasting = armed_ && !heard && everHeard_ && counter_ - lastEvidence_ > coastAfter;
    if (coasting && !coasting_) {
        rewindToEvidence();
    }
    coasting_ = coasting;

    transition(coasting);
    weigh(coasting ? nullptr : density);
    if (heard) {
        lastEvidence_ = counter_;
        everHeard_ = true;
    }
    // Kept for `rewindToEvidence`: the posterior as the last beat heard left it — through the
    // half beat after it too, in which nothing is overdue yet, so the replay keeps what the
    // music said between that beat and the next. Measured over the 23 electronic tracks, a
    // copy taken at the beat's peak alone (with peaks alone as beats heard) fell below the
    // old baseline on nine figures where this falls on one, and lost Alias's octave.
    if (everHeard_ && !coasting && counter_ - lastEvidence_ <= evidenceInterval_ / 2) {
        std::copy(posterior_.begin(), posterior_.end(), evidencePosterior_.begin());
        evidenceFrame_ = counter_;
    }

    // --- the meter, and everything else read off its chain ------------------------------
    // The pattern with the most mass, held by the incumbent until another is ahead by the
    // margin — `ParticleFilter::meterOf`'s rule. Then the beat, the tempo and the phase are
    // read off *that* chain alone, so that what is published as the meter and what calls
    // the downbeats can never be two different bars: a chain within the margin of the
    // leader is an explanation nearly as good, and its bar is the one in force.
    std::size_t leader = 0;
    for (std::size_t p = 1; p < patterns_.size(); ++p) {
        if (patternMass_[p] > patternMass_[leader]) {
            leader = p;
        }
    }
    // A chain that has lost the argument is kept at the floor with the leader's own beat —
    // its distribution over tempo and phase within the beat, laid into every beat of the
    // losing chain's bar — so that it neither underflows to nothing nor comes back with a
    // tempo of its own invention. See Options::meterFloor.
    if (options_.meterFloor > 0.0 && patterns_.size() > 1 && patternMass_[leader] > 0.0) {
        bool marginalReady = false;
        for (std::size_t p = 0; p < patterns_.size(); ++p) {
            if (p == leader || patternMass_[p] >= options_.meterFloor) {
                continue;
            }
            if (!marginalReady) {
                const Pattern& from = patterns_[leader];
                std::fill(beatMarginal_.begin(), beatMarginal_.end(), 0.0);
                for (std::uint32_t b = 0; b < from.meter; ++b) {
                    const std::size_t base = from.offset + static_cast<std::size_t>(b) * statesPerBeat_;
                    for (std::size_t k = 0; k < statesPerBeat_; ++k) {
                        beatMarginal_[k] += posterior_[base + k];
                    }
                }
                const double marginalScale = 1.0 / patternMass_[leader];
                for (double& mass : beatMarginal_) {
                    mass *= marginalScale;
                }
                marginalReady = true;
            }
            const Pattern& to = patterns_[p];
            const double perBeat = options_.meterFloor / static_cast<double>(to.meter);
            for (std::uint32_t b = 0; b < to.meter; ++b) {
                const std::size_t base = to.offset + static_cast<std::size_t>(b) * statesPerBeat_;
                for (std::size_t k = 0; k < statesPerBeat_; ++k) {
                    posterior_[base + k] = perBeat * beatMarginal_[k];
                }
            }
            patternMass_[p] = options_.meterFloor;
        }
    }
    std::size_t chosen = leader;
    for (std::size_t p = 0; p < patterns_.size(); ++p) {
        if (patterns_[p].meter == meterNow_ && meterNow_ != 0 &&
            patternMass_[leader] <= patternMass_[p] * options_.meterMargin) {
            chosen = p;
        }
    }
    meterNow_ = patterns_[chosen].meter;
    frame.beatsPerBar = meterNow_;

    const Pattern& mapPattern = patterns_[chosen];
    const std::size_t chainBegin = mapPattern.offset;
    const std::size_t chainEnd = chainBegin + static_cast<std::size_t>(mapPattern.meter) * statesPerBeat_;
    std::size_t map = chainBegin;
    double best = -1.0;
    // The circular mean of the phase is `Emission::MeanPhase`'s alone. Summed on every frame
    // whatever the rule, it was two multiplies a state for nothing under the default (the
    // audit's Low items); what else this loop adds up is summed in the same order either way.
    const bool meanWanted = options_.emission == Emission::MeanPhase;
    double cosSum = 0.0;
    double sinSum = 0.0;
    for (std::size_t s = chainBegin; s < chainEnd; ++s) {
        const double mass = posterior_[s];
        intervalMass_[stateInterval_[s]] += mass;
        if (meanWanted) {
            cosSum += mass * static_cast<double>(phaseCos_[s]);
            sinSum += mass * static_cast<double>(phaseSin_[s]);
        }
        if (mass > best) {
            best = mass;
            map = s;
        }
    }
    // Within the chain, so a chain holding a tenth of the mass still reads as a
    // distribution over its own states.
    const double chainMass = patternMass_[chosen] > 0.0 ? patternMass_[chosen] : 1.0;

    // --- what the posterior says --------------------------------------------------------
    const std::size_t mapInterval = stateInterval_[map];
    frame.gathering = static_cast<std::uint32_t>(map);
    frame.intervalFrames = intervals_[mapInterval];
    if (heard) {
        evidenceInterval_ = frame.intervalFrames; // how long until the next one is overdue
    }
    // The tempo without the whole-frame quantisation: the posterior mean over the MAP
    // interval and its two neighbours, which is the particle filter's `refinedIntervalFrames`
    // read off a distribution rather than a cloud — and the mass there is the confidence.
    double nearMass = 0.0;
    double nearPeriod = 0.0;
    for (std::size_t i = mapInterval > 0 ? mapInterval - 1 : 0; i < n && i <= mapInterval + 1; ++i) {
        nearMass += intervalMass_[i];
        nearPeriod += intervalMass_[i] * static_cast<double>(intervals_[i]);
    }
    frame.refinedIntervalFrames = nearMass > 0.0 ? nearPeriod / nearMass
                                                 : static_cast<double>(frame.intervalFrames);
    frame.bpm = 60.0 * static_cast<double>(options_.fps) / frame.refinedIntervalFrames;
    // Coasting, the tempo is carried, not measured, and saying so is what makes `TempoTracker`
    // hold it and report `holding` rather than present a guess as a reading.
    frame.tempoAgreement = coasting ? 0.0 : std::clamp(nearMass / chainMass, 0.0, 1.0);

    // --- the beat -----------------------------------------------------------------------
    const double position = static_cast<double>(stateBeat_[map]) + static_cast<double>(statePhase_[map]);
    const auto beatIndex = static_cast<std::int64_t>(stateBeat_[map]);
    const double spacing = options_.minimumBeatFraction * static_cast<double>(frame.intervalFrames);
    const bool spaced = !everEmitted_ || now - lastEmitFrame_ >= spacing;
    const double meanPhase = [&] {
        if (!meanWanted) {
            return 0.0;
        }
        const double angle = std::atan2(sinSum, cosSum) / (2.0 * std::numbers::pi);
        return angle < 0.0 ? angle + 1.0 : angle;
    }();

    const double anyBeat = beat + down; // P(beat of any kind), for the peak rule

    if (options_.emission == Emission::MapCrossing) {
        frame.phase = static_cast<double>(statePhase_[map]);
        // The prototype's rule: the MAP state has entered another beat of the bar, or has
        // gone backwards within one (a tempo or meter jump landing earlier in the beat).
        const bool crossed = lastBeatIndex_ >= 0 &&
                             (beatIndex != lastBeatIndex_ || position < lastPosition_);
        if (armed_ && crossed && spaced) {
            frame.emitted = beatIndex == 0 ? TrackedFrame::Emitted::Downbeat
                                           : TrackedFrame::Emitted::Beat;
            frame.beatOffsetFrames = 0.0;
        }
    } else if (options_.emission == Emission::MeanPhase) {
        frame.phase = meanPhase;
        // Where the mean phase, advancing at the MAP tempo, will reach the boundary; when
        // that is within the horizon the beat is committed now, that far ahead. Beyond the
        // horizon nothing is; and once committed the spacing rule keeps the crossing itself,
        // and the network's rising edge pulling the mean past it, from emitting a second
        // beat. The beat about to happen is the one after the MAP state's, which is how the
        // downbeat is named.
        const double rate = 1.0 / static_cast<double>(frame.intervalFrames);
        const double untilCrossing = (1.0 - meanPhase) / rate;
        const bool ahead = haveMeanPhase_ && untilCrossing <= options_.predictFrames;
        if (armed_ && ahead && spaced) {
            const std::uint32_t nextBeat =
                (static_cast<std::uint32_t>(beatIndex) + 1) % mapPattern.meter;
            frame.emitted = nextBeat == 0 ? TrackedFrame::Emitted::Downbeat
                                          : TrackedFrame::Emitted::Beat;
            frame.beatOffsetFrames = untilCrossing;
        }
    } else {
        frame.phase = static_cast<double>(statePhase_[map]);
        // The beat range: the MAP state is in the first 1/lambda of a beat. Entering one
        // starts a beat; the beat itself is the activation's peak inside it, known a frame
        // late — the previous frame was the peak when this one is lower — or, if the range
        // ends before the activation turns, the loudest frame the range had.
        const bool inRange = statePointer_[map] != 0;
        if (inRange && !inRange_) {
            rangeEmitted_ = false;
            rangeBeatIndex_ = beatIndex;
            rangeBestActivation_ = -1.0;
        }
        if (inRange && anyBeat > rangeBestActivation_) {
            rangeBestActivation_ = anyBeat;
            rangeBestFrame_ = counter_;
        }
        if (armed_ && !rangeEmitted_ && spaced && inRange_) {
            const bool peaked = lastActivation_ >= anyBeat && lastActivation_ > lastActivation2_;
            if (peaked) {
                frame.beatOffsetFrames = -1.0;
            } else if (!inRange && rangeBestActivation_ >= 0.0) {
                frame.beatOffsetFrames =
                    -static_cast<double>(counter_ - rangeBestFrame_);
            }
            if (peaked || (!inRange && rangeBestActivation_ >= 0.0)) {
                frame.emitted = rangeBeatIndex_ == 0 ? TrackedFrame::Emitted::Downbeat
                                                     : TrackedFrame::Emitted::Beat;
                rangeEmitted_ = true;
            }
        }
        inRange_ = inRange;
    }
    if (frame.emitted != TrackedFrame::Emitted::None) {
        lastEmitFrame_ = now + frame.beatOffsetFrames;
        everEmitted_ = true;
    }
    lastBeatIndex_ = beatIndex;
    lastPosition_ = position;
    lastMeanPhase_ = meanPhase;
    haveMeanPhase_ = true;
    lastActivation2_ = lastActivation_;
    lastActivation_ = anyBeat;

    ++counter_;
    return frame;
}

} // namespace takt4::tracking
