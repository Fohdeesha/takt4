#pragma once

#include "core/model/dimensions.hpp"
#include "core/model/weights.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace takt4::model {

/// BeatNet+'s CRNN, one feature frame at a time (HANDOFF §5.3): the convolution block,
/// Linear(278, 150), four stacked LSTMs and Linear(150, 3), then a softmax. The LSTM
/// state is carried from frame to frame, which is what makes the network causal, so
/// frames must be fed in order and reset() called at the start of a stream and after
/// silence.
///
/// Construction allocates; process() and reset() do not, and are safe to call from a
/// real-time thread — though §6's decision is that they run on a worker thread and the
/// audio callback only hands hops across a ring.
class BeatModel {
public:
    /// One frame's answer. The three classes are, in order, beat, downbeat and
    /// non-beat; the probabilities are the softmax of the logits and sum to 1.
    struct Activation {
        std::array<float, kNumClasses> logits{};
        std::array<float, kNumClasses> probabilities{};

        float beat() const noexcept { return probabilities[0]; }
        float downbeat() const noexcept { return probabilities[1]; }
        float nonBeat() const noexcept { return probabilities[2]; }
    };

    /// Copies `weights` into the network. The ModelWeights need not outlive this.
    explicit BeatModel(const ModelWeights& weights);
    ~BeatModel();

    BeatModel(BeatModel&&) noexcept;
    BeatModel& operator=(BeatModel&&) noexcept;
    BeatModel(const BeatModel&) = delete;
    BeatModel& operator=(const BeatModel&) = delete;

    /// Zeroes the LSTM hidden and cell state — PyTorch's reset_hidden().
    void reset() noexcept;

    /// Runs one feature frame through. Frames are consecutive by assumption.
    Activation process(std::span<const float, kFeatureDim> frame) noexcept;

    /// Frames fed since the last reset().
    std::uint64_t framesProcessed() const noexcept { return framesProcessed_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint64_t framesProcessed_ = 0;
};

} // namespace takt4::model
