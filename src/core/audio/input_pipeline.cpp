#include "core/audio/input_pipeline.hpp"

#include "core/audio/rates.hpp"

#include <algorithm>

namespace takt4::audio {

InputPipeline::InputPipeline(const ChannelPicker& picker, double inputRate,
                             HopProcessor& processor, std::size_t chunkFrames)
    : picker_(picker),
      processor_(processor),
      resampler_(inputRate, kInternalSampleRate, chunkFrames),
      hops_(kHopSize),
      mono_(chunkFrames) {}

void InputPipeline::process(const float* interleaved, std::size_t frames) noexcept {
    const auto stride = static_cast<std::size_t>(picker_.streamChannelCount());
    const std::size_t total = frames;

    while (frames > 0) {
        const std::size_t take = std::min(frames, mono_.size());
        picker_.pickMono(interleaved, take, mono_.data());
        resampler_.process(mono_.data(), take, [this](const float* samples, std::size_t count) {
            samplesOut_.store(samplesOut_.load(std::memory_order_relaxed) + count,
                              std::memory_order_relaxed);
            hops_.push(samples, count, [this](const float* hop) {
                const std::uint64_t index = hopsOut_.load(std::memory_order_relaxed);
                processor_.processHop(hop, index);
                hopsOut_.store(index + 1, std::memory_order_relaxed);
            });
        });
        interleaved += take * stride;
        frames -= take;
    }

    framesIn_.store(framesIn_.load(std::memory_order_relaxed) + total, std::memory_order_relaxed);
}

} // namespace takt4::audio
