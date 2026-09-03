#pragma once

#include "core/tracking/random.hpp"
#include "core/tracking/state_space.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace takt4::tracking {

/// What the tracker made of one 50 Hz activation frame.
struct TrackedFrame {
    enum class Emitted : std::uint8_t { None = 0, Downbeat = 1, Beat = 2 };

    std::uint64_t frameIndex = 0; ///< frames since the filter was reset
    Emitted emitted = Emitted::None;

    /// The median of the beat particles, taken before this frame's motion. Every
    /// decision the filter makes rests on it; the reference calls it `gathering`.
    std::uint32_t gathering = 0;
    /// The commonest downbeat particle, updated only on frames that could carry a beat.
    std::uint32_t downMax = 0;

    std::uint32_t intervalFrames = 0; ///< the cloud's beat period, in whole frames
    /// The same period without the state space's integer quantisation: the mean over the
    /// particles sitting on the median's tempo or either neighbour of it. madmom's
    /// intervals are whole frames, so at 130 BPM the nearest two are 130.43 and 125.00
    /// and there is nothing in between; a real tempo lands between them and the cloud
    /// straddles both, which is what this reads.
    double refinedIntervalFrames = 0.0;
    double bpm = 0.0;              ///< from refinedIntervalFrames, so it is continuous
    double phase = 0.0;            ///< how far through the beat the cloud is, 0 to 1
    std::uint32_t beatsPerBar = 0; ///< the meter the downbeat cloud settled on
    /// How much of the cloud agrees with the median's tempo, 0 to 1. Not upstream's —
    /// it publishes no confidence — but the natural one to gate on (§5.5).
    double tempoAgreement = 0.0;
};

/// BeatNet+'s two-stage particle filter cascade (HANDOFF §5.4), ported.
///
/// A beat stage of 1500 particles walks the (beat position x tempo) state space; when
/// its median sits at the start of a beat, a downbeat stage of 250 particles walks the
/// (beat in bar x meter) space to decide whether that beat is the downbeat. Both are
/// corrected against the network's activations by systematic resampling.
///
/// Only the runtime loop is here. The state space, the transition models and the
/// observation pointers are precomputed with madmom by tools/dump_statespace.py and
/// read from a blob, which is what keeps this a few hundred lines of arithmetic rather
/// than a port of part of madmom.
///
/// **Deterministic.** Given the same seed and the same activations this produces the
/// same frames, on every platform, and identically to tools/pf_reference.py — which is
/// what tests/tracking/particle_filter_test.cpp gates on. See that script's header for
/// where the algorithm deviates from upstream and why.
///
/// Allocation happens when the filter is built or reset, never in process(): §4.2 puts
/// this on the inference thread, which may allocate, but the resampling step is
/// data-dependent in cost and there is no reason to add a heap to that.
class ParticleFilter {
public:
    struct Options {
        std::size_t particles = 1500;        ///< upstream's PARTICLE_SIZE
        std::size_t downbeatParticles = 250; ///< upstream's DOWN_PARTICLE_SIZE
        std::uint64_t seed = 1;
    };

    /// The model has to outlive the filter; nothing is copied out of it.
    ///
    /// Two constructors rather than `Options options = {}`: a default argument is
    /// written inside the enclosing class but outside any member function, where a
    /// nested class's own default member initialisers are not yet available. MSVC
    /// accepts it; Clang rejects it, correctly.
    ParticleFilter(const StateSpaceModel& model, Options options);
    explicit ParticleFilter(const StateSpaceModel& model);

    /// Back to the state a freshly built filter is in, same seed and all.
    void reset() noexcept;

    /// One frame of class probabilities in, one decision out.
    TrackedFrame process(float beatActivation, float downbeatActivation) noexcept;

    const StateSpaceModel& model() const noexcept { return *model_; }
    const Options& options() const noexcept { return options_; }

    /// The particle clouds, for tests and diagnostics. Both keep a fixed size.
    const std::vector<std::uint32_t>& particles() const noexcept { return particles_; }
    const std::vector<std::uint32_t>& downbeatParticles() const noexcept { return downParticles_; }

private:
    /// Injections are bounded, so every buffer can be sized once and left alone.
    std::size_t maxInjection() const noexcept;

    void moveAlong(std::vector<std::uint32_t>& particles, const StateSpace& space,
                   bool downbeat) noexcept;
    void resample(std::vector<std::uint32_t>& particles, const double* weightPerState) noexcept;
    void dropAtRandom(std::vector<std::uint32_t>& particles, std::size_t count) noexcept;
    std::uint32_t median(const std::vector<std::uint32_t>& particles) noexcept;
    std::uint32_t mode(const std::vector<std::uint32_t>& particles, std::size_t numStates) noexcept;

    const StateSpaceModel* model_;
    Options options_;
    Xoshiro256pp rng_;

    std::vector<std::uint32_t> particles_;
    std::vector<std::uint32_t> downParticles_;

    // Scratch, all sized in the constructor.
    std::vector<std::uint32_t> moved_;
    std::vector<std::uint32_t> wrapped_;
    std::vector<std::uint32_t> resampled_;
    std::vector<std::uint32_t> sorted_;
    std::vector<std::uint32_t> counts_;
    std::vector<std::uint64_t> dropped_;
    std::vector<double> cumulative_;
    std::vector<double> beatWeights_;
    std::vector<double> downWeights_;

    std::uint64_t counter_ = 0; ///< frames seen; the first frame is 0
    double lastEmitTime_ = 0.0;
    TrackedFrame::Emitted lastEmitted_ = TrackedFrame::Emitted::None;
    std::uint32_t downMax_ = 0;
    std::size_t gatherWindow_ = 0;
};

} // namespace takt4::tracking
