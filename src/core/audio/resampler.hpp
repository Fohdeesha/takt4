#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>

namespace takt4::audio {

/// Sample-rate converter from the device rate to the internal rate (HANDOFF §4.1:
/// r8brain-free, device rate → 22050 Hz mono). Mono, streaming, real-time safe after
/// construction: process() neither allocates nor locks.
///
/// Output is time-aligned to input: output sample k stands for input time
/// k · inputRate / outputRate, with no offset. The price is inputDelayFrames() input
/// samples the converter holds back before the first output appears.
class Resampler {
public:
    /// Input samples handed to r8brain in one go; longer blocks are split. Every
    /// intermediate buffer is sized from this at construction.
    static constexpr std::size_t kDefaultChunkFrames = 512;

    /// Throws std::invalid_argument on a non-positive rate or chunk size.
    Resampler(double inputRate, double outputRate,
              std::size_t chunkFrames = kDefaultChunkFrames);
    ~Resampler();

    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    double inputRate() const noexcept { return inputRate_; }
    double outputRate() const noexcept { return outputRate_; }
    std::size_t chunkFrames() const noexcept { return chunkFrames_; }

    /// Input samples that must arrive before the first output sample is produced,
    /// i.e. the real-time delay this stage adds, in input frames.
    std::size_t inputDelayFrames() const noexcept { return inputDelayFrames_; }

    /// Real-time. Feeds `frames` input samples (any count) and hands each run of
    /// output samples to `sink(const float* samples, std::size_t count)`; the pointer
    /// is only valid during that call. Output arrives in chunks of no more than
    /// maxOutputPerChunk() samples.
    template <class Sink>
    void process(const float* input, std::size_t frames, Sink&& sink) noexcept {
        while (frames > 0) {
            const std::size_t take = std::min(frames, chunkFrames_);
            const float* output = nullptr;
            const std::size_t produced = processChunk(input, take, output);
            if (produced > 0) {
                sink(output, produced);
            }
            input += take;
            frames -= take;
        }
    }

    /// Upper bound on the samples one sink call receives.
    std::size_t maxOutputPerChunk() const noexcept;

    /// Forgets all state, as if freshly constructed.
    void reset() noexcept;

    /// The r8brain-free version compiled in.
    static const char* libraryVersion() noexcept;

private:
    std::size_t processChunk(const float* input, std::size_t frames, const float*& output) noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    double inputRate_;
    double outputRate_;
    std::size_t chunkFrames_;
    std::size_t inputDelayFrames_ = 0;
};

} // namespace takt4::audio
