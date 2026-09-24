#include "core/audio/input_stream.hpp"

#include "core/audio/asio_driver.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/stream_setup.hpp"
#include "core/rt/alloc_guard.hpp"

#include <portaudio.h>

#include <atomic>
#include <cmath>
#include <string>

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

namespace {

std::string hertz(double rate) {
    return std::to_string(static_cast<long long>(std::llround(rate))) + " Hz";
}

} // namespace

InputStream::InputStream(const PortAudioSession& /*session*/, const InputDevice& device,
                         const ChannelSelection& selection, HopProcessor& processor,
                         const InputStreamOptions& options) {
    const ChannelPicker picker(device, selection, !options.forceSoftwareSlice);
    double rate = detail::resolveSampleRate(device, options.sampleRate);

    // One attempt at `atRate`, into a fresh Impl: the pipeline is built for its rate, so a
    // second attempt at another rate starts again from nothing.
    const auto openAt = [&](double atRate) {
        impl_ = std::make_unique<Impl>(device, picker, atRate, processor);
        PaStreamParameters params;
        detail::setupInputParameters(params, impl_->hostInfo, device,
                                     picker.streamChannelCount(), picker.nativeSelectors(),
                                     options.suggestedLatency);
        const PaError opened = Pa_OpenStream(&impl_->stream, &params, nullptr, atRate,
                                             paFramesPerBufferUnspecified, paNoFlag,
                                             &Impl::callback, impl_.get());
        if (opened != paNoError) {
            impl_->stream = nullptr;
        }
        return opened;
    };

    const bool asio = device.hostApi == HostApiKind::Asio;
    if (asio) {
        forgetAsioClockedRate();
    }
    PaError err = openAt(rate);

    // **An ASIO interface is never re-clocked** (the audit's C5). PortAudio's ASIO host used to
    // call ASIOSetSampleRate whenever the rate asked for differed from the interface's own, so
    // pressing Start with Live or a front-of-house mix on the MOTU at 48 kHz moved everyone to
    // 44.1 kHz — audibly, mid-show. The patched host refuses instead and says what the
    // interface is running at (cmake/pa_asio_patch.cmake), and takt4 opens again at that rate:
    // everything is resampled to 22050 Hz anyway, so the device's own rate costs nothing.
    //
    // Only when the rate was the device's default rather than asked for. `takt4-cli --rate`
    // naming a rate is a question with an answer, and the answer is no.
    if (asio && err == paInvalidSampleRate) {
        const double clocked = asioClockedRate();
        if (clocked > 0.0 && clocked != rate) {
            if (options.sampleRate > 0.0) {
                throw PortAudioError(
                    err, device.name + " is running at " + hertz(clocked) + ", not " +
                             hertz(rate) + ", and takt4 does not change the rate of an interface "
                             "other programs may be using. Open it at " +
                             hertz(clocked) + ", or change the rate in its own control panel.");
            }
            rate = clocked;
            err = openAt(rate);
        }
    }
    if (err != paNoError) {
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
    // **Abort, not stop.** `Pa_StopStream` waits for the driver to hand back every buffer in
    // flight, on the UI thread, with no timeout — so a driver that had hung (the unplugged or
    // wedged interface the watchdog exists for) froze the window on Stop. An input stream has
    // nothing queued to play out, so aborting it costs at most the last buffer's few
    // milliseconds of audio (the audit's ASIO section).
    if (const PaError err = Pa_AbortStream(impl_->stream); err != paNoError) {
        detail::throwPortAudioError("Pa_AbortStream failed", err);
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
    c.samplesRepaired = impl_->pipeline.samplesRepaired();
    return c;
}

} // namespace takt4::audio
