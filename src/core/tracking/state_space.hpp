#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace takt4::tracking {

/// One discretised bar-pointer state space, as madmom's `BarStateSpace` builds it.
///
/// A state is a (position in the beat, tempo) pair. The states of one tempo — one
/// *interval*, in frames per beat — are laid out consecutively, intervals back to back,
/// so a state's position within its interval is its distance from that interval's first
/// state. Both of the cascade's stages use the same shape: the beat stage's intervals
/// are beat periods in frames, the downbeat stage's are bar lengths in beats.
///
/// Every array here is a view into the StateSpaceModel that owns it.
class StateSpace {
public:
    /// The six sections one state space occupies in the blob, in the order they appear.
    /// StateSpaceModel fills one of these and hands it over; nothing else should.
    struct Tables {
        std::vector<std::uint32_t> intervals;
        std::vector<std::uint32_t> firstStates;
        std::vector<std::uint32_t> lastStates;
        std::vector<std::uint32_t> stateIntervals;
        std::vector<double> statePositions;
        std::vector<std::uint32_t> pointers;
    };

    StateSpace() = default;

    /// Derives the state-to-interval index and checks the tables describe a state space
    /// the filter can walk: ascending distinct intervals, laid end to end with no gap,
    /// each starting at position 0 on a beat state. `context` names the file and which
    /// of the two spaces it is, for the message. Throws std::runtime_error.
    StateSpace(Tables tables, const std::string& context);

    std::size_t numStates() const noexcept { return stateIntervals_.size(); }
    std::size_t numIntervals() const noexcept { return intervals_.size(); }

    /// Interval lengths, ascending and distinct. Frames per beat, or beats per bar.
    std::span<const std::uint32_t> intervals() const noexcept { return intervals_; }
    /// The state each interval starts and ends at.
    std::span<const std::uint32_t> firstStates() const noexcept { return firstStates_; }
    std::span<const std::uint32_t> lastStates() const noexcept { return lastStates_; }

    /// Per state: the length of the interval it belongs to, and madmom's position in it
    /// (0 at the first state, approaching 1 at the last).
    std::span<const std::uint32_t> stateIntervals() const noexcept { return stateIntervals_; }
    std::span<const double> statePositions() const noexcept { return statePositions_; }

    /// Per state: madmom's observation pointer. 2 selects the (down-)beat density, 0 the
    /// non-beat one. See the header of tools/dump_statespace.py for why 1 never occurs
    /// at the configuration BeatNet+ ships.
    std::span<const std::uint32_t> pointers() const noexcept { return pointers_; }

    /// Which interval a state belongs to, and how far into it that state is. Derived
    /// when the blob is read, so the filter does not search for it every frame.
    std::size_t intervalOf(std::size_t state) const noexcept { return intervalIndex_[state]; }
    std::size_t phaseOf(std::size_t state) const noexcept {
        return state - firstStates_[intervalIndex_[state]];
    }
    bool isBeatState(std::size_t state) const noexcept { return pointers_[state] == 2; }

private:
    std::vector<std::uint32_t> intervals_;
    std::vector<std::uint32_t> firstStates_;
    std::vector<std::uint32_t> lastStates_;
    std::vector<std::uint32_t> stateIntervals_;
    std::vector<double> statePositions_;
    std::vector<std::uint32_t> pointers_;
    std::vector<std::uint32_t> intervalIndex_;
};

/// The two state spaces of the particle filter cascade and their transition models, read
/// from an `assets/statespace/*.bin` blob written by tools/dump_statespace.py.
///
/// HANDOFF §5.4: these depend only on configuration, never on audio, so they are
/// precomputed with madmom and shipped. Reading one opens a file and allocates; nothing
/// here belongs on the audio thread, and nothing here changes once it is read.
class StateSpaceModel {
public:
    /// What the blob was generated with. All of it is checked rather than assumed: a
    /// blob built for a different tempo range is a different tracker.
    struct Config {
        std::uint32_t fps = 0;
        double minBpm = 0.0;
        double maxBpm = 0.0;
        std::uint32_t numTempi = 0;
        double lambdaBeat = 0.0;
        double lambdaDown = 0.0;
        std::uint32_t minBeatsPerBar = 0;
        std::uint32_t maxBeatsPerBar = 0;
        std::uint32_t observationLambdaBeat = 0;
        std::uint32_t observationLambdaDown = 0;
    };

    /// Reads and validates the blob. Throws std::runtime_error on anything wrong with
    /// it: missing, truncated, wrong magic, a format version this build does not know, a
    /// checksum mismatch, or tables that do not describe a usable state space.
    static StateSpaceModel fromFile(const std::filesystem::path& path);

    const Config& config() const noexcept { return config_; }
    const StateSpace& beat() const noexcept { return beat_; }
    const StateSpace& downbeat() const noexcept { return downbeat_; }

    /// One frame of the tracker, in seconds. The reference calls this T.
    double secondsPerFrame() const noexcept { return 1.0 / static_cast<double>(config_.fps); }

    /// The tempo of an interval of the beat state space, in BPM.
    double bpmOfInterval(std::size_t interval) const noexcept {
        return 60.0 * static_cast<double>(config_.fps) /
               static_cast<double>(beat_.intervals()[interval]);
    }

    /// Where a beat particle at the last state of `interval` may go, and with what
    /// probability: the exponential tempo-change distribution, already normalised.
    /// Indices are into the beat state space's intervals. Each row sums to one.
    std::span<const std::uint32_t> tempoDestinations(std::size_t interval) const noexcept;
    std::span<const double> tempoProbabilities(std::size_t interval) const noexcept;

    /// The downbeat stage's meter transitions, row-major over the downbeat intervals:
    /// row i is where a particle at the last state of meter i may go.
    std::span<const double> meterTransitions(std::size_t meter) const noexcept;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
    Config config_;
    StateSpace beat_;
    StateSpace downbeat_;
    std::vector<std::uint32_t> tempoRowOffsets_;
    std::vector<std::uint32_t> tempoDestination_;
    std::vector<double> tempoProbability_;
    std::vector<double> meterTransitions_;
};

} // namespace takt4::tracking
