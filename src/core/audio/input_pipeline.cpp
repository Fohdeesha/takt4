#include "core/audio/input_pipeline.hpp"

#include "core/audio/rates.hpp"

#include <algorithm>
#include <cmath>

namespace takt4::audio {

InputPipeline::InputPipeline(const ChannelPicker& picker, double inputRate,
                             HopProcessor& processor, std::size_t chunkFrames)
    : picker_(picker),
      processor_(processor),
      resampler_(inputRate, kInternalSampleRate, chunkFrames),
      hops_(kHopSize),
      mono_(chunkFrames) {}

void InputPipeline::process(const float* interleaved, std::size_t frames, Arrival arrival) noexcept {
    const auto stride = static_cast<std::size_t>(picker_.streamChannelCount());
    const std::size_t total = frames;

    // Before any hop this buffer completes: where its first sample is on the hop clock, when it
    // was heard, and what the device has lost so far — so each hop can be stamped with when its
    // own audio was in the room (`HopProcessor::beginBuffer`).
    if (arrival.steadyNanos != 0) {
        const double outputRate = resampler_.outputRate();
        const double lost = lost_.onBuffer(static_cast<std::uint32_t>(frames),
                                           resampler_.inputRate(), arrival.steadyNanos);
        lostSeconds_.store(lost, std::memory_order_relaxed);
        // The resampler is time-aligned — output sample k stands for input time k / outputRate —
        // so the first sample of this buffer is, on the hop clock, simply where the frames
        // delivered so far put it.
        const double first =
            static_cast<double>(framesIn_.load(std::memory_order_relaxed)) * outputRate /
            resampler_.inputRate();
        const std::int64_t heard =
            arrival.steadyNanos / 1000 -
            static_cast<std::int64_t>(std::llround(arrival.inputLatencySeconds * 1e6));
        processor_.beginBuffer(first, heard, lost * outputRate);
    }

    while (frames > 0) {
        const std::size_t take = std::min(frames, mono_.size());
        picker_.pickMono(interleaved, take, mono_.data());
        picker_.addPairSums(interleaved, take, pairSums_);
        std::uint64_t repaired = 0;
        for (std::size_t i = 0; i < take; ++i) {
            if (!std::isfinite(mono_[i])) {
                mono_[i] = 0.0f;
                ++repaired;
            }
        }
        if (repaired != 0) {
            samplesRepaired_.store(samplesRepaired_.load(std::memory_order_relaxed) + repaired,
                                   std::memory_order_relaxed);
        }
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
    if (picker_.selection().count == 2) {
        publishedSums_.publish(pairSums_);
    }
}

} // namespace takt4::audio
