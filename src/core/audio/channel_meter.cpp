#include "core/audio/channel_meter.hpp"

#include "core/audio/portaudio_session.hpp"
#include "core/audio/stream_setup.hpp"
#include "core/rt/alloc_guard.hpp"

#include <portaudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace takt4::audio {

namespace {

// Lock-free accumulators shared between the callback and the reader. Compare-exchange
// loops rather than fetch_add so nothing depends on atomic<double>::fetch_add, which
// not every libc++ takt4 builds against has.
void atomicAdd(std::atomic<double>& target, double value) noexcept {
    double current = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(current, current + value, std::memory_order_relaxed)) {
    }
}

void atomicMax(std::atomic<float>& target, float value) noexcept {
    float current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

struct ChannelMeter::Impl {
    Impl(const InputDevice& dev, double rate)
        : device(dev),
          channels(dev.maxInputChannels),
          sampleRate(rate),
          sumSquares(static_cast<std::size_t>(dev.maxInputChannels)),
          peaks(static_cast<std::size_t>(dev.maxInputChannels)),
          blockSum(static_cast<std::size_t>(dev.maxInputChannels)),
          blockPeak(static_cast<std::size_t>(dev.maxInputChannels)) {}

    InputDevice device;
    int channels;
    double sampleRate;
    PaStream* stream = nullptr;

    // Shared with the reader: sum of squares and peak per channel since the last read.
    std::vector<std::atomic<double>> sumSquares;
    std::vector<std::atomic<float>> peaks;
    std::atomic<std::uint64_t> windowFrames{0};

    // Callback-only scratch, so the atomics are touched once per channel per block.
    std::vector<double> blockSum;
    std::vector<float> blockPeak;

    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> framesIn{0};
    std::atomic<std::uint32_t> inputOverflows{0};

    static int callback(const void* input, void* /*output*/, unsigned long frameCount,
                        const PaStreamCallbackTimeInfo* /*timeInfo*/,
                        PaStreamCallbackFlags statusFlags, void* userData) noexcept {
        auto* self = static_cast<Impl*>(userData);
        const rt::RealtimeScope realtime;
        self->callbacks.fetch_add(1, std::memory_order_relaxed);
        if ((statusFlags & paInputOverflow) != 0) {
            self->inputOverflows.fetch_add(1, std::memory_order_relaxed);
        }
        if (input != nullptr) {
            self->accumulate(static_cast<const float*>(input), frameCount);
        }
        return paContinue;
    }

    void accumulate(const float* interleaved, std::size_t frames) noexcept {
        const auto stride = static_cast<std::size_t>(channels);
        for (std::size_t c = 0; c < stride; ++c) {
            blockSum[c] = 0.0;
            blockPeak[c] = 0.0f;
        }
        const float* frame = interleaved;
        for (std::size_t i = 0; i < frames; ++i, frame += stride) {
            for (std::size_t c = 0; c < stride; ++c) {
                const float x = frame[c];
                blockSum[c] += static_cast<double>(x) * static_cast<double>(x);
                const float magnitude = std::fabs(x);
                if (magnitude > blockPeak[c]) {
                    blockPeak[c] = magnitude;
                }
            }
        }
        for (std::size_t c = 0; c < stride; ++c) {
            atomicAdd(sumSquares[c], blockSum[c]);
            atomicMax(peaks[c], blockPeak[c]);
        }
        windowFrames.fetch_add(frames, std::memory_order_relaxed);
        framesIn.fetch_add(frames, std::memory_order_relaxed);
    }
};

ChannelMeter::ChannelMeter(const PortAudioSession& /*session*/, const InputDevice& device,
                           double sampleRate) {
    detail::refuseInSandbox(device.name);
    const double rate = detail::resolveSampleRate(device, sampleRate);
    impl_ = std::make_unique<Impl>(device, rate);

    PaStreamParameters params;
    detail::HostStreamInfo hostInfo;
    detail::setupInputParameters(params, hostInfo, device, device.maxInputChannels, {}, -1.0);

    const PaError err = Pa_OpenStream(&impl_->stream, &params, nullptr, rate,
                                      paFramesPerBufferUnspecified, paNoFlag, &Impl::callback,
                                      impl_.get());
    if (err != paNoError) {
        impl_->stream = nullptr;
        detail::throwPortAudioError("Pa_OpenStream failed", err);
    }
}

ChannelMeter::~ChannelMeter() {
    if (impl_ && impl_->stream != nullptr) {
        if (Pa_IsStreamActive(impl_->stream) == 1) {
            Pa_AbortStream(impl_->stream);
        }
        Pa_CloseStream(impl_->stream);
    }
}

void ChannelMeter::start() {
    if (const PaError err = Pa_StartStream(impl_->stream); err != paNoError) {
        detail::throwPortAudioError("Pa_StartStream failed", err);
    }
}

void ChannelMeter::stop() {
    if (!running()) {
        return;
    }
    if (const PaError err = Pa_StopStream(impl_->stream); err != paNoError) {
        detail::throwPortAudioError("Pa_StopStream failed", err);
    }
}

bool ChannelMeter::running() const noexcept {
    return Pa_IsStreamActive(impl_->stream) == 1;
}

const InputDevice& ChannelMeter::device() const noexcept {
    return impl_->device;
}

int ChannelMeter::channelCount() const noexcept {
    return impl_->channels;
}

double ChannelMeter::sampleRate() const noexcept {
    return impl_->sampleRate;
}

std::uint64_t ChannelMeter::read(std::span<Level> out) noexcept {
    Impl& impl = *impl_;
    // The frame count and the per-channel sums are reset separately, so a callback
    // landing in between skews one reading by a block; good enough for a meter.
    const std::uint64_t frames = impl.windowFrames.exchange(0, std::memory_order_relaxed);
    const auto count = std::min(out.size(), static_cast<std::size_t>(impl.channels));
    for (std::size_t c = 0; c < count; ++c) {
        const double sum = impl.sumSquares[c].exchange(0.0, std::memory_order_relaxed);
        out[c].peak = impl.peaks[c].exchange(0.0f, std::memory_order_relaxed);
        out[c].rms = frames > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(frames))) : 0.0f;
    }
    return frames;
}

InputStreamCounters ChannelMeter::counters() const noexcept {
    InputStreamCounters c;
    c.callbacks = impl_->callbacks.load(std::memory_order_relaxed);
    c.framesIn = impl_->framesIn.load(std::memory_order_relaxed);
    c.hopsOut = 0;
    c.inputOverflows = impl_->inputOverflows.load(std::memory_order_relaxed);
    return c;
}

} // namespace takt4::audio
