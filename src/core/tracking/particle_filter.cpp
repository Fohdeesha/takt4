#include "core/tracking/particle_filter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace takt4::tracking {

namespace {

// Upstream's constants, from particle_filtering_cascade.particle_filter_cascade. The
// names are its names; tools/pf_reference.py states the same set on the Python side.
constexpr double kInformationGate = 0.4; ///< below this an activation is flattened...
constexpr double kFloor = 0.03;          ///< ...to this, which is also the non-beat weight
constexpr double kResampleThreshold = 0.1;
constexpr double kInjectThreshold = 0.8;
constexpr double kDownbeatInjectThreshold = 0.7;
constexpr double kEmitThreshold = 0.4;
constexpr std::size_t kInjectStride = 6;     ///< every 6th tempo...
constexpr std::size_t kInjectPhases = 4;     ///< ...starting at a random one of the first 4
constexpr double kGatherSeconds = 0.07;      ///< how near the beat the cloud has to be
constexpr double kMinimumBeatFraction = 0.4; ///< of a beat period, between emissions

/// **Not upstream's, and not part of any gate** — see `meterOf`.
///
/// How much of the meter evidence survives each time the downbeat stage runs, which is
/// about once a beat. 0.98 averages over roughly fifty beats.
///
/// Chosen by measuring both things it trades off, over the eighteen excerpts in
/// `tests/data/features` and the 698 Ballroom clips:
///
/// | memory | meter changes | downbeat F-measure |
/// |---|---|---|
/// | mode, as upstream reads it | 166 | 0.8575 |
/// | mass, no memory | 167 | 0.8579 |
/// | 0.8 | 71 | 0.8504 |
/// | 0.9 | 57 | 0.8519 |
/// | 0.95 | 47 | 0.8535 |
/// | **0.98** | **41** | 0.8531 |
///
/// **This was 0.95, and the note here used to say 0.98 bought six fewer changes and no
/// accuracy — which was true of the excerpts and false of real tracks.** Measured again
/// over the seventeen full tracks in `references/audio`, 68 minutes of the material the
/// operator's report came from, and with `kMeterMargin` in place:
///
/// | memory | meter changes in 68 min |
/// |---|---|
/// | 0.95 | 129 |
/// | **0.98** | **51** |
///
/// Thirty-second excerpts cannot show this. They are shorter than the memory itself, so
/// most of what a long memory buys happens after the clip has ended.
///
/// The cost is the one Ballroom cannot show either, because every clip in it holds one
/// meter throughout: how long a *genuine* change takes to follow. With the margin at 1.5,
/// a cloud that switches wholesale needs 0.98^n < 0.4, so 45 beats — about 22 seconds at
/// 120 BPM, against 9 at 0.95. Four-to-the-bar to four-to-the-bar, which is nearly every
/// track change, costs nothing either way. **If an operator reports the meter too slow to
/// follow a real change, this is the constant to lower, not the margin** — the margin adds
/// no time at all.
constexpr double kMeterMemory = 0.98;

/// **Not upstream's either** — how far ahead of the meter in force another one has to be
/// before it takes over, as a ratio of the remembered evidence.
///
/// The memory above made the evidence steady; it did not make the *argmax* of it steady,
/// and those are different things. Two meters that the material genuinely supports about
/// equally — which is most of any bar-length phrase, and every passage where a four could
/// be read as two twos — leave their scores within a per cent of each other, so the leader
/// still changes whenever the cloud breathes. Measured over the seventeen full tracks in
/// `references/audio`, 68 minutes: 322 meter changes with the memory alone, one every
/// thirteen seconds, which is what the report "the time signature still changes more than
/// it should" was about.
///
/// So the meter in force keeps it until something is ahead by this much:
///
/// | margin | meter changes in 68 min | downbeat F-measure |
/// |---|---|---|
/// | 1.0, as it was | 322 | 0.8535 |
/// | 1.25 | 179 | 0.8522 |
/// | **1.5** | **129** | 0.8515 |
///
/// — all at memory 0.95; with the 0.98 above, that last row is **51** and 0.8502.
///
/// Note what this is *not*: it is not a delay. The version measured and rejected before the
/// memory existed held a new meter for N frames before publishing it, and lost downbeat
/// F-measure doing it — 0.8575 to 0.8143 at half a second — because the meter and the
/// filter's own downbeat calls come out of this one cloud, and delaying one desynchronises
/// it from the other. A margin costs no time at all: the frame the evidence becomes
/// decisive is the frame the meter changes. It only declines to change on evidence that is
/// not decisive, which is what "two hypotheses within a per cent of each other, and one
/// particle moved" is.
constexpr double kMeterMargin = 1.5;

/// How many particles an injection can add, over every phase it might start at.
constexpr std::size_t injectionSize(std::size_t intervals, std::size_t stride) {
    return (intervals + stride - 1) / stride;
}

} // namespace

ParticleFilter::ParticleFilter(const StateSpaceModel& model) : ParticleFilter(model, Options{}) {}

ParticleFilter::ParticleFilter(const StateSpaceModel& model, Options options)
    : model_(&model), options_(options), rng_(options.seed) {
    if (options_.particles == 0 || options_.downbeatParticles == 0) {
        throw std::invalid_argument("ParticleFilter: both stages need at least one particle");
    }
    const StateSpace& beat = model.beat();
    const StateSpace& down = model.downbeat();
    // Upstream seeds particles over `arange(0, num_states - 1)`, so a state space of one
    // state would have nothing to draw from.
    if (beat.numStates() < 2 || down.numStates() < 2) {
        throw std::invalid_argument("ParticleFilter: the state space is too small to seed");
    }

    const std::size_t beatCapacity =
        options_.particles + injectionSize(beat.numIntervals(), kInjectStride);
    const std::size_t downCapacity = options_.downbeatParticles + down.numIntervals();
    const std::size_t working = std::max(beatCapacity, downCapacity);

    particles_.reserve(beatCapacity);
    downParticles_.reserve(downCapacity);
    moved_.reserve(working);
    wrapped_.reserve(working);
    resampled_.resize(working);
    sorted_.resize(working);
    cumulative_.resize(working);
    counts_.resize(std::max(beat.numStates(), down.numStates()));
    meterScores_.assign(down.numIntervals(), 0.0);
    dropped_.reserve(maxInjection());

    // Only the beat states' weights change from frame to frame; the rest sit at the
    // floor for the life of the filter.
    beatWeights_.assign(beat.numStates(), kFloor);
    downWeights_.assign(down.numStates(), kFloor);

    gatherWindow_ = static_cast<std::size_t>(kGatherSeconds / model.secondsPerFrame()) + 1;
    reset();
}

std::size_t ParticleFilter::maxInjection() const noexcept {
    return std::max(injectionSize(model_->beat().numIntervals(), kInjectStride),
                    model_->downbeat().numIntervals());
}

void ParticleFilter::reset() noexcept {
    rng_.reseed(options_.seed);
    const StateSpace& beat = model_->beat();
    const StateSpace& down = model_->downbeat();

    // Upstream's `np.sort(np.random.choice(np.arange(0, num_states - 1), size))`: the
    // very last state is left out, and the cloud starts sorted.
    particles_.clear();
    for (std::size_t i = 0; i < options_.particles; ++i) {
        particles_.push_back(static_cast<std::uint32_t>(rng_.bounded(beat.numStates() - 1)));
    }
    std::sort(particles_.begin(), particles_.end());

    downParticles_.clear();
    for (std::size_t i = 0; i < options_.downbeatParticles; ++i) {
        downParticles_.push_back(static_cast<std::uint32_t>(rng_.bounded(down.numStates() - 1)));
    }
    std::sort(downParticles_.begin(), downParticles_.end());

    std::fill(meterScores_.begin(), meterScores_.end(), 0.0);
    downMax_ = mode(downParticles_, model_->downbeat().numStates());
    meterNow_ = meterOf(downParticles_);
    counter_ = 0;
    lastEmitTime_ = 0.0;
    lastEmitted_ = TrackedFrame::Emitted::None;
}

void ParticleFilter::moveAlong(std::vector<std::uint32_t>& particles, const StateSpace& space,
                               bool downbeat) noexcept {
    moved_.clear();
    wrapped_.clear();
    for (const std::uint32_t particle : particles) {
        if (space.isLastState(particle)) {
            wrapped_.push_back(particle);
        } else {
            moved_.push_back(particle + 1);
        }
    }
    // A particle at the end of its interval draws a new one: the exponential tempo
    // distribution for the beat stage, the meter transitions for the downbeat stage.
    for (const std::uint32_t particle : wrapped_) {
        const std::size_t interval = space.intervalOf(particle);
        const std::span<const double> probabilities =
            downbeat ? model_->meterTransitions(interval) : model_->tempoProbabilities(interval);
        const std::span<const std::uint32_t> destinations =
            downbeat ? std::span<const std::uint32_t>{} : model_->tempoDestinations(interval);

        const double u = rng_.nextDouble();
        double cumulative = 0.0;
        std::size_t chosen = probabilities.size() - 1;
        for (std::size_t k = 0; k < probabilities.size(); ++k) {
            cumulative += probabilities[k];
            if (u < cumulative) {
                chosen = k;
                break;
            }
        }
        const std::size_t destination = downbeat ? chosen : destinations[chosen];
        moved_.push_back(space.firstStates()[destination]);
    }
    // assign rather than swap: the two clouds are different sizes, and swapping would
    // leave the scratch buffer too small for the other one.
    particles.assign(moved_.begin(), moved_.end());
}

void ParticleFilter::resample(std::vector<std::uint32_t>& particles,
                              const double* weightPerState) noexcept {
    const std::size_t count = particles.size();
    double running = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        running += weightPerState[particles[i]];
        cumulative_[i] = running;
    }
    // Every hypothesis is impossible; there is nothing to redistribute towards.
    if (!(running > 0.0)) {
        return;
    }
    // Systematic resampling: one stratified draw per particle, walked in order. The
    // cumulative sum is compared against `u * total` rather than the weights being
    // normalised first, because numpy sums pairwise and this does not — see
    // tools/pf_reference.py, which does the same so the two agree to the last bit.
    const double step = 1.0 / static_cast<double>(count);
    std::size_t at = 0;
    for (std::size_t j = 0; j < count; ++j) {
        const double target = (rng_.nextDouble() * step + static_cast<double>(j) * step) * running;
        while (at + 1 < count && cumulative_[at] < target) {
            ++at;
        }
        resampled_[j] = particles[at];
    }
    std::copy(resampled_.begin(), resampled_.begin() + static_cast<std::ptrdiff_t>(count),
              particles.begin());
}

void ParticleFilter::dropAtRandom(std::vector<std::uint32_t>& particles,
                                  std::size_t count) noexcept {
    dropped_.clear();
    while (dropped_.size() < count) {
        const std::uint64_t index = rng_.bounded(particles.size());
        if (std::find(dropped_.begin(), dropped_.end(), index) == dropped_.end()) {
            dropped_.push_back(index);
        }
    }
    std::size_t out = 0;
    for (std::size_t i = 0; i < particles.size(); ++i) {
        if (std::find(dropped_.begin(), dropped_.end(), static_cast<std::uint64_t>(i)) ==
            dropped_.end()) {
            particles[out++] = particles[i];
        }
    }
    particles.resize(out);
}

std::uint32_t ParticleFilter::median(const std::vector<std::uint32_t>& particles) noexcept {
    const std::size_t count = particles.size();
    const auto begin = sorted_.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(count);
    std::copy(particles.begin(), particles.end(), begin);
    const std::size_t half = count / 2;
    std::nth_element(begin, begin + static_cast<std::ptrdiff_t>(half), end);
    if (count % 2 == 1) {
        return sorted_[half];
    }
    // numpy's median of an even count is the mean of the two middle values, and upstream
    // truncates it back to a state index.
    const std::uint32_t upper = sorted_[half];
    const std::uint32_t lower = *std::max_element(begin, begin + static_cast<std::ptrdiff_t>(half));
    return static_cast<std::uint32_t>((static_cast<double>(lower) + static_cast<double>(upper)) /
                                      2.0);
}

std::uint32_t ParticleFilter::mode(const std::vector<std::uint32_t>& particles,
                                   std::size_t numStates) noexcept {
    std::fill_n(counts_.begin(), numStates, 0u);
    for (const std::uint32_t particle : particles) {
        ++counts_[particle];
    }
    // The first state holding the largest count, as numpy's argmax over a bincount is.
    std::uint32_t best = 0;
    std::uint32_t at = 0;
    for (std::uint32_t state = 0; state < numStates; ++state) {
        if (counts_[state] > best) {
            best = counts_[state];
            at = state;
        }
    }
    return at;
}

std::uint32_t ParticleFilter::meterOf(const std::vector<std::uint32_t>& particles) noexcept {
    const StateSpace& down = model_->downbeat();

    // The meter the *whole cloud* is on, rather than the meter of the single commonest
    // particle.
    //
    // `downMax_` is one state, and the downbeat space holds a state per (position in bar,
    // meter) pair. When the cloud is genuinely split — a passage that could be three or
    // four to the bar, which is most of a bar's worth of every phrase — the mode sits in
    // whichever hypothesis is ahead by a single particle, and crosses back the moment one
    // particle moves. Measured over the eighteen excerpts in `tests/data/features`, the
    // meter read off it changed up to **1.8 times a second**: a bar indicator nobody can
    // read, and a downbeat snap nobody can aim, because the rotation it sets up is
    // computed against a meter that has already moved.
    //
    // Summing the mass on each meter answers the question actually being asked — "how many
    // beats to the bar does the cloud believe?" — and 250 particles' worth of evidence
    // does not cross over because one of them moved.
    //
    // **This deliberately does not touch `downMax_`.** That one decides `isBeatState`, and
    // so decides whether a beat is emitted as a downbeat; it is column 1 of the Phase 4
    // parity gate and the emit decision is column 2. Both stay exactly as they were, which
    // is why that gate still passes bit for bit. Only the meter — which the gate does not
    // carry — is derived differently.
    // Mass alone is a better estimate but not a steadier one: a particle redraws its meter
    // from `meterTransitions` whenever it wraps, so the cloud genuinely migrates every
    // beat and the argmax still crosses over about every other one. So the mass is
    // *remembered* — each meter's evidence decays by `kMeterMemory` and this beat's is
    // added, which averages over roughly five beats.
    //
    // The decay goes here, on the evidence, and not downstream on the answer. Smoothing
    // the published meter in `TempoTracker` was tried first and measurably lost downbeat
    // F-measure (0.8575 -> 0.8340 at a two-second hold, 0.8143 at half a second), because
    // the meter and the filter's own downbeat *calls* come from this one cloud: delaying
    // one desynchronises it from the other, and a lagged meter is worse than a jumpy one.
    // Averaging the evidence keeps them the same estimate.
    for (double& score : meterScores_) {
        score *= kMeterMemory;
    }
    for (const std::uint32_t particle : particles) {
        meterScores_[down.intervalOf(particle)] += 1.0;
    }

    // The first interval holding the largest score, as `mode` above breaks its ties: an
    // arbitrary but fixed rule, so the filter stays deterministic.
    double best = -1.0;
    std::size_t at = 0;
    double holding = -1.0;
    for (std::size_t interval = 0; interval < meterScores_.size(); ++interval) {
        if (meterScores_[interval] > best) {
            best = meterScores_[interval];
            at = interval;
        }
        if (down.intervals()[interval] == meterNow_) {
            holding = meterScores_[interval];
        }
    }
    // Incumbent advantage: see kMeterMargin. With nothing in force yet the leader takes it.
    if (meterNow_ != 0 && holding >= 0.0 && best <= holding * kMeterMargin) {
        return meterNow_;
    }
    return down.intervals()[at];
}

TrackedFrame ParticleFilter::process(float beatActivation, float downbeatActivation) noexcept {
    const StateSpace& beat = model_->beat();
    const StateSpace& down = model_->downbeat();
    const double beatProbability = static_cast<double>(beatActivation);
    const double downProbability = static_cast<double>(downbeatActivation);
    const double T = model_->secondsPerFrame();
    const double now = static_cast<double>(counter_) * T;

    // The information gate: anything below the threshold is flattened to the floor, so a
    // quiet passage neither drags the cloud around nor emits.
    double gated = std::max(beatProbability, downProbability);
    if (gated < kInformationGate) {
        gated = kFloor;
    }

    TrackedFrame frame;
    frame.frameIndex = counter_;
    // Carried, not used: see TrackedFrame. Taken before the gate above is applied, because
    // what the layer above weighs beats against each other with is the network's reading,
    // not the floor a quiet frame is flattened to.
    frame.beatActivation = beatActivation;
    frame.downbeatActivation = downbeatActivation;
    frame.gathering = median(particles_);
    const std::size_t intervalIndex = beat.intervalOf(frame.gathering);
    frame.intervalFrames = beat.intervals()[intervalIndex];
    frame.phase = beat.statePositions()[frame.gathering];

    // Two things from one pass over the cloud that produced this median. How much of it
    // agrees with the median's tempo — nothing upstream reads that, but §5.5's
    // confidence gate does — and the mean period over the particles on that tempo or
    // either neighbour, which is the tempo without the state space's integer steps.
    std::size_t agreeing = 0;
    std::size_t nearby = 0;
    double periodTotal = 0.0;
    for (const std::uint32_t particle : particles_) {
        const std::size_t interval = beat.intervalOf(particle);
        if (interval == intervalIndex) {
            ++agreeing;
        }
        const std::size_t distance =
            interval > intervalIndex ? interval - intervalIndex : intervalIndex - interval;
        if (distance <= 1) {
            ++nearby;
            periodTotal += static_cast<double>(beat.stateIntervals()[particle]);
        }
    }
    frame.tempoAgreement = static_cast<double>(agreeing) / static_cast<double>(particles_.size());
    frame.refinedIntervalFrames = nearby > 0 ? periodTotal / static_cast<double>(nearby)
                                             : static_cast<double>(frame.intervalFrames);
    frame.bpm = 60.0 / (frame.refinedIntervalFrames * T);

    // A beat can only be here if the cloud has gathered at the start of a beat, and only
    // if enough of one has passed since the last beat was called.
    if (beat.phaseOf(frame.gathering) < gatherWindow_ &&
        now - lastEmitTime_ >
            kMinimumBeatFraction * T * static_cast<double>(frame.intervalFrames)) {
        moveAlong(downParticles_, down, true);

        std::size_t injected = 0;
        if (downProbability > kDownbeatInjectThreshold) {
            for (const std::uint32_t first : down.firstStates()) {
                downParticles_.push_back(first);
            }
            injected = down.firstStates().size();
        }
        // madmom's pointer 2 selects the downbeat density, 0 the plain beat one.
        for (std::size_t state = 0; state < down.numStates(); ++state) {
            downWeights_[state] = down.isBeatState(state) ? downProbability : beatProbability;
        }
        resample(downParticles_, downWeights_.data());
        if (injected != 0) {
            dropAtRandom(downParticles_, injected);
        }
        downMax_ = mode(downParticles_, model_->downbeat().numStates());
        // Beside `downMax_`, and for the same reason it lives here: both read the downbeat
        // cloud, and the cloud only moves on the frames this block runs — about one in
        // twenty-three. Decaying the meter evidence out here rather than once per frame is
        // what makes `kMeterMemory` mean "about five beats" instead of a tenth of a second.
        meterNow_ = meterOf(downParticles_);

        if (down.isBeatState(downMax_) && lastEmitted_ != TrackedFrame::Emitted::Downbeat &&
            downProbability > kEmitThreshold) {
            frame.emitted = TrackedFrame::Emitted::Downbeat;
        } else if (gated > kEmitThreshold) {
            frame.emitted = TrackedFrame::Emitted::Beat;
        }
        if (frame.emitted != TrackedFrame::Emitted::None) {
            lastEmitTime_ = now;
            lastEmitted_ = frame.emitted;
        }
    }

    moveAlong(particles_, beat, false);
    if (gated > kResampleThreshold) {
        std::size_t injected = 0;
        // A strong activation is a chance to plant fresh hypotheses on the beat, spread
        // across the tempo range so a tempo the cloud has abandoned can be found again.
        if (gated > kInjectThreshold) {
            for (std::size_t i = rng_.bounded(kInjectPhases); i < beat.numIntervals();
                 i += kInjectStride) {
                particles_.push_back(beat.firstStates()[i]);
                ++injected;
            }
        }
        for (const std::uint32_t first : beat.firstStates()) {
            beatWeights_[first] = gated;
        }
        resample(particles_, beatWeights_.data());
        if (injected != 0) {
            dropAtRandom(particles_, injected);
        }
    }

    frame.downMax = downMax_;
    frame.beatsPerBar = meterNow_;
    ++counter_;
    return frame;
}

} // namespace takt4::tracking
