#pragma once

#include "core/tracking/beat_decoder.hpp"
#include "core/tracking/tracked_frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace takt4::tracking {

/// An exact forward filter over madmom's joint bar-pointer state space: the decoder
/// TRACKING-PROPOSAL.md §2.4 measures against the particle filter, in C++.
///
/// The state is (position in the bar, tempo, meter), discretised the way madmom's
/// `BarStateSpace` does it: for each meter a bar of that many identical beats, each beat
/// laid out as one state per frame for every whole-frame tempo interval between
/// `minBpm` and `maxBpm`. Within a beat the position advances one state per frame; at
/// a beat boundary the tempo may change, with probability falling off as
/// exp(−λ · |ratio − 1|) (`BarTransitionModel`); the meters are independent chains whose
/// relative posterior mass is the model comparison between them. The observation model is
/// madmom's `RNNDownBeatTrackingObservationModel`: the first 1/λ_obs of every beat reads
/// the network's beat probability, the first 1/λ_obs of the bar's first beat its downbeat
/// probability, and everything else what is left over, spread over λ_obs − 1 parts.
///
/// `process` is one step of the forward algorithm — a sparse transition, a multiply by the
/// observation densities, a normalisation — over about 39 k states at the default 100 fps
/// with meters 3 and 4. That is deterministic, needs no seed, no injection, no information
/// gate and no gather window, and its confidence is the posterior mass on the tempo it
/// reports. Everything the state space needs is built here from the options; nothing is
/// read from a blob.
///
/// What it does that the particle filter cannot, and why each is here rather than in
/// `TempoTracker` (§2.6, §3.4):
///
///   * **The operator's window is evidence.** `setTempoWindow` puts a per-frame weight on
///     every tempo state — 1 inside the window, `Options::windowPenalty` outside — so an
///     octave outside the window has to keep out-arguing it, and when the octave inside
///     wins the beats come out of the posterior on that grid with the posterior's own phase.
///     A fold applied afterwards can move the number and, with evidence, halve the grid; it
///     cannot choose *which* half, and this can.
///   * **A tempo can be held.** `holdTempo` weights every tempo but the held one and its
///     neighbours down by `Options::holdPenalty` a frame, so the filter tracks phase and
///     nothing else — a DJ's beat grid, and the only thing that holds a track whose
///     periodicities are in non-octave ratios.
///   * **It coasts through a break.** A chain whose position advances every frame carries
///     phase and tempo through silence by construction and re-anchors on the first
///     confident beats; the beats keep coming at the held tempo, and `TempoTracker`'s
///     confidence gate flags them `holding`. The one exception is before the first beat
///     of a run: nothing is emitted until the network has once read a beat at
///     `Options::armThreshold` or above, so a silent start does not fire a rig.
///
/// Where the beats are read off the posterior is `Options::emission`, and it is the part
/// §2.4 measured as unfinished: the MAP state crossing a beat boundary is a frame early as
/// the posterior sharpens. Three rules are here so that the choice was measured rather
/// than assumed (`tools/refeval/jitter.py`), and the one that won — the activation's peak
/// inside the beat range, where the offline references put their beats — is the default;
/// the table under `Options::emission` has the numbers.
///
/// Allocation happens in the constructor; `process` and `reset` touch no heap.
class ForwardFilter final : public BeatDecoder {
public:
    enum class Emission : std::uint8_t {
        /// The MAP state entering a new beat of the bar, at most once per half period:
        /// the prototype's rule, `tools/refeval/decoders.py` `fwd`. Beats land on the
        /// frame, `beatOffsetFrames` is zero. Measured a frame or three early: the network's
        /// rising edge pulls the posterior onto the beat states before the beat itself.
        MapCrossing,
        /// The posterior's circular mean phase within the beat, advanced by the MAP tempo,
        /// **predicted**: the beat is emitted `Options::predictFrames` before the mean is
        /// due to reach the boundary, with a positive `beatOffsetFrames` saying how far
        /// ahead it is. Committed that far out because the prediction has to be made
        /// before the network's rising edge reaches the posterior — after it the mean has
        /// already jumped past the boundary and there is nothing left to predict — so it
        /// rests on the beats before this one, which is what a flywheel is. The output
        /// queue can schedule it ahead, the negative-latency case the per-output offsets
        /// already describe.
        MeanPhase,
        /// The activation's peak while the MAP state is in a beat range, which is where
        /// madmom's offline decoder puts a beat (`correct=True`) and so where the reference
        /// systems' beats are. Read causally one frame late — a peak is known when the next
        /// frame is lower — so `beatOffsetFrames` is −1 for a peak, or further back if the
        /// range ended without one and its loudest frame is taken instead.
        Peak,
    };

    struct Options {
        /// The decoder's own frame rate. The network runs at 50; the engine interpolates
        /// its activations up to this, and §2.5 measures the doubling as worth more than
        /// any change of decoder.
        std::uint32_t fps = 100;
        /// The tempo range the state space holds, as BeatNet+'s particle filter has it.
        double minBpm = 55.0;
        double maxBpm = 215.0;
        /// The bar lengths modelled, zero-terminated. madmom's default is 3 and 4; the
        /// two-beat bar the particle filter's blob also carries reads as 2/4 on 10 to 39 %
        /// of frames of four-to-the-floor tracks (§2.10), so it is left out here and can
        /// be put back by an operator whose material has it.
        ///
        /// **Four alone is the default since 2026-09-09.** With bars of three and four the
        /// meter chains explain electronic material about equally and the three-beat chain
        /// wins whole passages at three quarters of the tempo (daOooooh at 78 for a 103
        /// track, §7.7); four alone was measured +0.02 beat F over the 23 tracks and the
        /// operator, whose material is 4/4, chose it for the rig. Ballroom's waltzes lose
        /// under it, which is the trade the operator made; `settings::Preset::meters` puts
        /// three back for anyone whose set has them.
        std::array<std::uint8_t, 4> meters{4, 0, 0, 0};
        /// λ of the tempo-change distribution at beat boundaries; madmom's default.
        double transitionLambda = 100.0;
        /// How many parts a beat is split into, the first of which is the beat state;
        /// madmom's default.
        std::uint32_t observationLambda = 16;
        /// Per-frame weight on a tempo outside the operator's window. 0.97 is what the
        /// prototype used: over a hundred frames an octave outside is down twenty times.
        double windowPenalty = 0.97;
        /// Per-frame weight on a tempo outside the held one and its two neighbours.
        double holdPenalty = 0.1;
        /// Probability of the bar changing meter at a bar boundary. The prototype had
        /// none, which is madmom's `MultiPatternTransitionModel` default; with none, a
        /// meter that has lost the argument for a minute has lost it to the last decimal
        /// and cannot come back. Off by default so the prototype's numbers reproduce.
        double meterChangeProbability = 0.0;
        /// A share of the posterior spread uniformly every frame, so no state can reach
        /// exactly zero. **Off, and measured as harmful at any size**: a uniform share
        /// lands on an interval in proportion to its length, and the slowest tempo crosses
        /// beat states least often, so in a weak passage it is penalised least and grows
        /// out of the floor — at 1e-9, five of the 23 electronic tracks were published at
        /// 56 BPM, the bottom of the state space, and the agreed fifteen lost 0.03 of
        /// beat F. Kept as an option so the measurement can be repeated, not as a setting.
        double floor = 0.0;
        /// The least share of the posterior a meter's chain is allowed to hold. Below it
        /// the chain is reseeded from the leading chain's own distribution over tempo and
        /// beat phase, bar by bar, and held there — the same beat, a different bar, which
        /// is what a meter change is. Without this a chain that lost the argument for a few
        /// minutes has lost it to the last decimal and underflows to nothing, and a set
        /// that changed meter an hour in could never follow it. Zero disables it.
        ///
        /// **Far below anything the evidence reaches on ordinary material, on purpose.**
        /// Measured over the 23 electronic tracks with the margin at 10: at 1e-6 the kept
        /// chain climbed back over the incumbent whenever the evidence wobbled — 491 meter
        /// changes in 91 minutes against 143 with no floor, and downbeat F 0.674 against
        /// 0.710; at 1e-15, 247 changes; at 1e-30, 145 and every score to the third decimal
        /// the same as none. A chain a factor of 1e30 behind needs about seventy nats of
        /// evidence to win, which a genuine change of meter supplies in well under a minute
        /// and a wobble never does.
        double meterFloor = 1e-30;
        /// Where the beats are read off the posterior. `Peak` — the activation's peak while
        /// the MAP state is in a beat range — is the rule that scored best on every measure,
        /// on the 23 electronic tracks and on Ballroom alike (TRACKING-PROPOSAL.md §5's exit
        /// criteria, `tests/data/tracking/refeval/README.md`):
        ///
        /// | agreed 15, fold off | beat F | CMLt | downbeat F | timing: spread, beyond 40 ms |
        /// |---|---|---|---|---|
        /// | particle filter | 0.864 | 0.752 | 0.602 | 60 ms, 14.0 % |
        /// | MapCrossing | 0.872 | 0.823 | 0.681 | 40 ms, 33.5 % (median −30 ms) |
        /// | MeanPhase | 0.885 | 0.813 | 0.671 | 46 ms, 5.8 % (median −13 ms) |
        /// | **Peak** | **0.895** | **0.827** | **0.710** | 60 ms, 11.5 % (median 0 ms) |
        Emission emission = Emission::Peak;
        /// `Emission::MeanPhase` only: how many frames ahead of the predicted boundary the
        /// beat is committed. More than the network's rising edge, which at 100 fps is two
        /// to four frames, or the commitment is made from a posterior the beat has already
        /// moved. Six is 60 ms.
        double predictFrames = 6.0;
        /// No beat within this fraction of a period of the last one: the prototype's 0.5.
        double minimumBeatFraction = 0.5;
        /// Nothing is emitted until an activation has once reached this — BeatNet+'s own
        /// emission threshold — so a silent start fires nothing.
        double armThreshold = 0.4;
        /// The meter in force keeps it until another carries this much more posterior
        /// mass, as `ParticleFilter::meterOf` does and for the same reason — and the beat,
        /// the tempo and the phase are read off the chain in force, so the bar that calls
        /// the downbeats is always the bar that is published. A hundred, not the particle
        /// filter's 1.5, because a posterior's mass ratio moves exponentially where a
        /// particle count moves linearly: measured over the 23 electronic tracks with the
        /// peak rule, 239 meter changes in 91 minutes at 1.5, 181 at 3, 143 at 10 and 81
        /// at 100 — the particle filter's 66 — with beat and downbeat F unchanged to the
        /// second decimal throughout. A genuine change of meter is still followed within
        /// a few bars, because a factor of a hundred is five nats.
        double meterMargin = 100.0;
    };

    explicit ForwardFilter(Options options);
    ForwardFilter();

    void reset() noexcept override;
    TrackedFrame process(float beatActivation, float downbeatActivation) noexcept override;
    double secondsPerFrame() const noexcept override {
        return 1.0 / static_cast<double>(options_.fps);
    }
    const char* name() const noexcept override { return "forward filter"; }

    bool honoursTempoWindow() const noexcept override { return true; }
    void setTempoWindow(double minBpm, double maxBpm, bool enabled) noexcept override;
    bool canHoldTempo() const noexcept override { return true; }
    void holdTempo(double bpm) noexcept override;

    const Options& options() const noexcept { return options_; }

    // The state space, for tests and diagnostics.
    std::size_t numStates() const noexcept { return numStates_; }
    std::size_t numIntervals() const noexcept { return intervals_.size(); }
    std::size_t numMeters() const noexcept { return patterns_.size(); }
    std::span<const std::uint32_t> intervals() const noexcept { return intervals_; }
    double bpmOfInterval(std::size_t interval) const noexcept;
    /// The tempo-change distribution out of `from`, one probability per destination
    /// interval. Each row sums to one.
    std::span<const double> tempoTransitions(std::size_t from) const noexcept;
    /// The posterior after the last frame, one probability per state.
    std::span<const double> posterior() const noexcept { return posterior_; }
    /// The posterior mass on each meter after the last frame, in `meters` order. The beat,
    /// the tempo and the phase a frame reports are read off the chain of the meter in
    /// force, which the margin may hold at less than the most mass; see `process`.
    std::span<const double> meterMass() const noexcept { return patternMass_; }
    /// Whether a tempo is being held, and at what.
    double heldBpm() const noexcept { return heldBpm_; }

private:
    struct Pattern {
        std::uint32_t meter = 0;
        std::size_t offset = 0; ///< the first state of the pattern
    };

    void buildStateSpace();
    void buildTransitions();
    void combineWeights() noexcept;

    Options options_;

    // The state space: intervals ascending, and where each begins inside one beat.
    std::vector<std::uint32_t> intervals_;
    std::vector<std::size_t> firstOfInterval_;
    std::size_t statesPerBeat_ = 0;
    std::vector<Pattern> patterns_;
    std::size_t numStates_ = 0;
    // Per state.
    std::vector<std::uint16_t> stateInterval_; ///< index into intervals_
    std::vector<std::uint8_t> statePattern_;
    std::vector<std::uint8_t> stateBeat_;    ///< beat of the bar, from 0
    std::vector<std::uint8_t> statePointer_; ///< 0 non-beat, 1 beat, 2 downbeat
    std::vector<float> statePhase_;          ///< position within the beat, 0 to 1
    std::vector<float> phaseCos_;
    std::vector<float> phaseSin_;
    /// Dense, row-major, `from * numIntervals + to`.
    std::vector<double> tempoTransition_;

    // Evidence the operator adds: per interval, a window weight and a hold weight, and
    // their product, which is what a frame multiplies by.
    std::vector<double> windowWeight_;
    std::vector<double> holdWeight_;
    std::vector<double> intervalWeight_;
    bool anyWeight_ = false;
    double heldBpm_ = 0.0;

    // Working storage, sized once.
    std::vector<double> posterior_;
    std::vector<double> next_;
    std::vector<double> lastMass_; ///< mass on the last state of each interval, one beat
    std::vector<double> incoming_; ///< that, pushed through the tempo transitions
    std::vector<double> barIncoming_; ///< per pattern, the mass arriving at its beat 0
    std::vector<double> intervalMass_;
    std::vector<double> patternMass_;
    std::vector<double> beatMarginal_; ///< one beat's worth of the leading chain, for reseeding

    // Between frames.
    std::uint64_t counter_ = 0;
    bool armed_ = false;
    double lastEmitFrame_ = 0.0;
    bool everEmitted_ = false;
    std::int64_t lastBeatIndex_ = -1; ///< the MAP state's beat of the bar, last frame
    double lastPosition_ = 0.0;       ///< the MAP state's position in the bar, last frame
    double lastMeanPhase_ = 0.0;
    bool haveMeanPhase_ = false;
    std::uint32_t meterNow_ = 0;
    // Emission::Peak: the beat range the MAP is in, and what the activation did in it.
    bool inRange_ = false;             ///< the MAP state was in a beat range last frame
    bool rangeEmitted_ = false;        ///< this range's beat has gone out
    std::int64_t rangeBeatIndex_ = 0;  ///< the beat of the bar this range belongs to
    double rangeBestActivation_ = -1.0;
    std::uint64_t rangeBestFrame_ = 0;
    double lastActivation_ = 0.0;      ///< P(beat of any kind), last frame
    double lastActivation2_ = 0.0;     ///< and the frame before
};

} // namespace takt4::tracking
