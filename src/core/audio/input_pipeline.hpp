#pragma once

#include "core/audio/channel_picker.hpp"
#include "core/audio/hop_accumulator.hpp"
#include "core/audio/hop_processor.hpp"
#include "core/audio/resampler.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace takt4::audio {

/// The audio-thread half of HANDOFF §4.1's input stage, with no PortAudio in it so it
/// can be driven from a test: interleaved device block → ChannelPicker → mono →
/// Resampler → 22050 Hz → HopAccumulator → HopProcessor, one hop at a time.
///
/// Everything is sized at construction; process() allocates nothing and takes no
/// locks, whatever the block size.
class InputPipeline {
public:
    /// `inputRate` is the stream's rate; the picker fixes the stream's channel count.
    /// Blocks are worked in slices of `chunkFrames` frames, which also bounds the
    /// scratch buffers.
    InputPipeline(const ChannelPicker& picker, double inputRate, HopProcessor& processor,
                  std::size_t chunkFrames = Resampler::kDefaultChunkFrames);

    InputPipeline(const InputPipeline&) = delete;
    InputPipeline& operator=(const InputPipeline&) = delete;

    /// Real-time. `interleaved` holds `frames` frames of picker().streamChannelCount()
    /// channels at the input rate.
    void process(const float* interleaved, std::size_t frames) noexcept;

    const ChannelPicker& picker() const noexcept { return picker_; }
    double inputRate() const noexcept { return resampler_.inputRate(); }

    /// Delay from a device sample arriving to it being part of a hop, in input frames,
    /// not counting the hop being filled: the resampler's hold-back.
    std::size_t inputDelayFrames() const noexcept { return resampler_.inputDelayFrames(); }

    // Progress counters, readable from any thread. framesIn is the monotonic sample
    // clock of HANDOFF §4.3: device frames consumed since construction.
    std::uint64_t framesIn() const noexcept { return framesIn_.load(std::memory_order_relaxed); }
    std::uint64_t samplesOut() const noexcept { return samplesOut_.load(std::memory_order_relaxed); }
    std::uint64_t hopsOut() const noexcept { return hopsOut_.load(std::memory_order_relaxed); }

private:
    ChannelPicker picker_;
    HopProcessor& processor_;
    Resampler resampler_;
    HopAccumulator hops_;
    std::vector<float> mono_; // one chunk of picked mono at the input rate

    std::atomic<std::uint64_t> framesIn_{0};
    std::atomic<std::uint64_t> samplesOut_{0};
    std::atomic<std::uint64_t> hopsOut_{0};
};

} // namespace takt4::audio
