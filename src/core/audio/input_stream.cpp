#include "core/audio/input_stream.hpp"

#include "core/audio/asio_driver.hpp"
#include "core/audio/callback_gate.hpp"
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
        : device(dev), picker(pick), sampleRate(rate), pipeline(pick, rate, processor),
          clock(rate) {}

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
    /// See `detach`.
    CallbackGate gate;
    /// The callbacks' own account of when they came — what the watchdog measures the device's
    /// rate over, however seldom the window looks.
    CallbackClock clock;

    static int callback(const void* input, void* /*output*/, unsigned long frameCount,
                        const PaStreamCallbackTimeInfo* /*timeInfo*/,
                        PaStreamCallbackFlags statusFlags, void* userData) noexcept {
        auto* self = static_cast<Impl*>(userData);
        const rt::RealtimeScope realtime;
        // Detached: whatever the driver does now, nothing it sends reaches the processor.
        const CallbackGate::Pass pass(self->gate);
        if (!pass.admitted()) {
            return paContinue;
        }
        self->callbacks.fetch_add(1, std::memory_order_relaxed);
        if ((statusFlags & paInputOverflow) != 0) {
            self->inputOverflows.fetch_add(1, std::memory_order_relaxed);
        }
        if (input != nullptr) {
            self->pipeline.process(static_cast<const float*>(input), frameCount);
        }
        self->clock.onCallback(static_cast<std::uint32_t>(frameCount), CallbackClock::steadyNanos());
        return paContinue;
    }
};

void addClockReading(InputStreamCounters& counters, const CallbackClock::Reading& clock,
                     std::int64_t nowNanos) noexcept {
    counters.normalFrames = clock.normalFrames;
    counters.normalSeconds = static_cast<double>(clock.normalNanos) / 1e9;
    counters.sinceLastCallbackSeconds =
        clock.lastNanos == 0 ? 0.0 : static_cast<double>(nowNanos - clock.lastNanos) / 1e9;
}

namespace {

std::string hertz(double rate) {
    return std::to_string(static_cast<long long>(std::llround(rate))) + " Hz";
}

} // namespace

InputStream::InputStream(const PortAudioSession& /*session*/, const InputDevice& device,
                         const ChannelSelection& selection, HopProcessor& processor,
                         const InputStreamOptions& options) {
    detail::refuseInSandbox(device.name);
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
    // Abort rather than stop: an input stream has nothing queued to play out, so nothing is lost
    // by not waiting for the buffers in flight. **It is no guard against a hung driver** (the
    // audit of 2026-09-25, L23): in PortAudio's ASIO and WASAPI hosts both calls end in the same
    // blocking code — `ASIOStop`, and a wait with no timeout — so a driver wedged in there holds
    // whichever thread called this. That is why `engine::LiveTracker` calls it on its audio
    // thread, having `detach`ed the stream first on its own, and waits only as long as it chooses.
    if (const PaError err = Pa_AbortStream(impl_->stream); err != paNoError) {
        detail::throwPortAudioError("Pa_AbortStream failed", err);
    }
}

bool InputStream::running() const noexcept {
    return Pa_IsStreamActive(impl_->stream) == 1;
}

void InputStream::detach() noexcept {
    (void)impl_->gate.close();
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

StereoSums InputStream::stereo() const noexcept {
    return impl_->pipeline.stereoSums();
}

InputStreamCounters InputStream::counters() const noexcept {
    InputStreamCounters c;
    c.callbacks = impl_->callbacks.load(std::memory_order_relaxed);
    c.framesIn = impl_->pipeline.framesIn();
    c.hopsOut = impl_->pipeline.hopsOut();
    c.inputOverflows = impl_->inputOverflows.load(std::memory_order_relaxed);
    c.samplesRepaired = impl_->pipeline.samplesRepaired();
    addClockReading(c, impl_->clock.read(), CallbackClock::steadyNanos());
    return c;
}

} // namespace takt4::audio
