#include "core/audio/input_stream.hpp"

#include "core/audio/portaudio_session.hpp"
#include "core/audio/stream_setup.hpp"
#include "core/rt/alloc_guard.hpp"

#include <portaudio.h>

#include <atomic>

namespace takt4::audio {

struct InputStream::Impl {
    Impl(const InputDevice& dev, const ChannelPicker& pick, double rate, HopProcessor& processor)
        : device(dev), picker(pick), sampleRate(rate), pipeline(pick, rate, processor) {}

    InputDevice device;
    ChannelPicker picker;
    double sampleRate;
    InputPipeline pipeline;
    detail::HostStreamInfo hostInfo;
    PaStream* stream = nullptr;
    double inputLatency = 0.0;
    double reportedSampleRate = 0.0;

    std::atomic<std::uint64_t> callbacks{0};
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
            self->pipeline.process(static_cast<const float*>(input), frameCount);
        }
        return paContinue;
    }
};

InputStream::InputStream(const PortAudioSession& /*session*/, const InputDevice& device,
                         const ChannelSelection& selection, HopProcessor& processor,
                         const InputStreamOptions& options) {
    const ChannelPicker picker(device, selection, !options.forceSoftwareSlice);
    const double rate = detail::resolveSampleRate(device, options.sampleRate);
    impl_ = std::make_unique<Impl>(device, picker, rate, processor);

    PaStreamParameters params;
    detail::setupInputParameters(params, impl_->hostInfo, device, picker.streamChannelCount(),
                                 picker.nativeSelectors(), options.suggestedLatency);

    const PaError err = Pa_OpenStream(&impl_->stream, &params, nullptr, rate,
                                      paFramesPerBufferUnspecified, paNoFlag, &Impl::callback,
                                      impl_.get());
    if (err != paNoError) {
        impl_->stream = nullptr;
        detail::throwPortAudioError("Pa_OpenStream failed", err);
    }
    impl_->reportedSampleRate = rate;
    if (const PaStreamInfo* info = Pa_GetStreamInfo(impl_->stream); info != nullptr) {
        impl_->inputLatency = info->inputLatency;
        impl_->reportedSampleRate = info->sampleRate;
    }
}

InputStream::~InputStream() {
    if (impl_ && impl_->stream != nullptr) {
        // Errors here have nowhere useful to go; the stream is being torn down anyway.
        if (Pa_IsStreamActive(impl_->stream) == 1) {
            Pa_AbortStream(impl_->stream);
        }
        Pa_CloseStream(impl_->stream);
    }
}

void InputStream::start() {
    if (const PaError err = Pa_StartStream(impl_->stream); err != paNoError) {
        detail::throwPortAudioError("Pa_StartStream failed", err);
    }
}

void InputStream::stop() {
    if (!running()) {
        return;
    }
    if (const PaError err = Pa_StopStream(impl_->stream); err != paNoError) {
        detail::throwPortAudioError("Pa_StopStream failed", err);
    }
}

bool InputStream::running() const noexcept {
    return Pa_IsStreamActive(impl_->stream) == 1;
}

const InputDevice& InputStream::device() const noexcept {
    return impl_->device;
}

const ChannelPicker& InputStream::picker() const noexcept {
    return impl_->picker;
}

double InputStream::sampleRate() const noexcept {
    return impl_->sampleRate;
}

double InputStream::reportedSampleRate() const noexcept {
    return impl_->reportedSampleRate;
}

double InputStream::inputLatencySeconds() const noexcept {
    return impl_->inputLatency;
}

std::size_t InputStream::resamplerDelayFrames() const noexcept {
    return impl_->pipeline.inputDelayFrames();
}

InputStreamCounters InputStream::counters() const noexcept {
    InputStreamCounters c;
    c.callbacks = impl_->callbacks.load(std::memory_order_relaxed);
    c.framesIn = impl_->pipeline.framesIn();
    c.hopsOut = impl_->pipeline.hopsOut();
    c.inputOverflows = impl_->inputOverflows.load(std::memory_order_relaxed);
    return c;
}

} // namespace takt4::audio
