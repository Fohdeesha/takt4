#pragma once

#include "core/tracking/beat_decoder.hpp"
#include "core/tracking/random.hpp"
#include "core/tracking/state_space.hpp"
#include "core/tracking/tracked_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace takt4::tracking {

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
class ParticleFilter final : public BeatDecoder {
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
    void reset() noexcept override;

    /// One frame of class probabilities in, one decision out.
    TrackedFrame process(float beatActivation, float downbeatActivation) noexcept override;

    /// The blob's frame rate: 50 Hz, the network's own.
    double secondsPerFrame() const noexcept override { return model_->secondsPerFrame(); }
    const char* name() const noexcept override { return "particle filter"; }

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
    /// The meter the cloud as a whole is on — the beats-per-bar carrying the most
    /// particles, not the beats-per-bar of the single commonest one. See the definition.
    std::uint32_t meterOf(const std::vector<std::uint32_t>& particles) noexcept;

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
    std::vector<double> meterScores_; ///< decaying evidence, one per meter; see meterOf
    std::vector<std::uint64_t> dropped_;
    std::vector<double> cumulative_;
    std::vector<double> beatWeights_;
    std::vector<double> downWeights_;

    std::uint64_t counter_ = 0; ///< frames seen; the first frame is 0
    double lastEmitTime_ = 0.0;
    TrackedFrame::Emitted lastEmitted_ = TrackedFrame::Emitted::None;
    std::uint32_t downMax_ = 0;
    std::uint32_t meterNow_ = 0; ///< the meter last read off the cloud; see meterOf
    std::size_t gatherWindow_ = 0;
};

} // namespace takt4::tracking
