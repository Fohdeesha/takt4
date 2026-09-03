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

    downMax_ = mode(downParticles_, model_->downbeat().numStates());
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
    frame.beatsPerBar = down.intervals()[down.intervalOf(downMax_)];
    ++counter_;
    return frame;
}

} // namespace takt4::tracking
