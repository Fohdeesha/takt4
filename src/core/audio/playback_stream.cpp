#include "core/audio/playback_stream.hpp"

#include "core/audio/portaudio_session.hpp"
#include "core/audio/resampler.hpp"
#include "core/audio/stream_setup.hpp"
#include "core/rt/alloc_guard.hpp"
#include "core/rt/published.hpp"

#include <portaudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace takt4::audio {

namespace {

OutputDevice describe(PaDeviceIndex index, const PaDeviceInfo& info, const PaHostApiInfo& api) {
    OutputDevice device;
    device.index = index;
    device.hostApiIndex = info.hostApi;
    device.hostApiName = api.name;
    device.name = info.name;
    device.maxOutputChannels = info.maxOutputChannels;
    device.defaultSampleRate = info.defaultSampleRate;
    device.defaultLowOutputLatency = info.defaultLowOutputLatency;
    device.isDefaultOutput = api.defaultOutputDevice == index;
    return device;
}

} // namespace

std::vector<OutputDevice> listOutputDevices(const PortAudioSession&) {
    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        throw PortAudioError(count, std::string("Pa_GetDeviceCount failed: ") + Pa_GetErrorText(count));
    }
    std::vector<OutputDevice> devices;
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info == nullptr || info->maxOutputChannels <= 0) {
            continue;
        }
        const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
        if (api == nullptr) {
            continue;
        }
        devices.push_back(describe(i, *info, *api));
    }
    return devices;
}

std::optional<OutputDevice> defaultOutputDevice(const PortAudioSession& session) {
    const std::vector<OutputDevice> devices = listOutputDevices(session);
    const PaHostApiIndex wasapi = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (wasapi >= 0) {
        for (const OutputDevice& device : devices) {
            if (device.hostApiIndex == wasapi && device.isDefaultOutput) {
                return device;
            }
        }
    }
    const PaDeviceIndex fallback = Pa_GetDefaultOutputDevice();
    for (const OutputDevice& device : devices) {
        if (device.index == fallback) {
            return device;
        }
    }
    return std::nullopt;
}

std::vector<float> resampleForPlayback(const std::vector<float>& samples, double inputRate,
                                       double outputRate) {
    if (!(inputRate > 0.0) || !(outputRate > 0.0)) {
        throw std::invalid_argument("resampleForPlayback: rates must be positive");
    }
    if (inputRate == outputRate) {
        return samples;
    }
    Resampler resampler(inputRate, outputRate);
    const std::size_t wanted =
        static_cast<std::size_t>(std::ceil(static_cast<double>(samples.size()) * outputRate / inputRate));
    std::vector<float> out;
    out.reserve(wanted + resampler.maxOutputPerChunk());
    const auto sink = [&out](const float* block, std::size_t count) {
        out.insert(out.end(), block, block + count);
    };
    resampler.process(samples.data(), samples.size(), sink);
    // The converter holds inputDelayFrames() back; silence pushes the tail out.
    const std::vector<float> silence(Resampler::kDefaultChunkFrames, 0.0f);
    const std::size_t flushes = resampler.inputDelayFrames() / silence.size() + 4;
    for (std::size_t n = 0; n < flushes && out.size() < wanted; ++n) {
        resampler.process(silence.data(), silence.size(), sink);
    }
    out.resize(wanted, 0.0f);
    return out;
}

/// What the callback publishes: which source frame the buffer it last handed over
/// begins at, and when that buffer reaches the DAC on the stream's clock.
struct PlaybackMarker {
    std::uint64_t cursor = 0;
    double dacTime = 0.0;
    bool valid = false;
};

struct PlaybackStream::Impl {
    OutputDevice device;
    std::vector<float> buffer; // mono, at the device's rate
    double rate = 0.0;
    int channels = 2;
    std::uint64_t cursor = 0; // the audio thread's, after start()
    std::uint64_t startCursor = 0;
    rt::Published<PlaybackMarker> marker;
    std::atomic<bool> finished{false};
    PaStream* stream = nullptr;
    double outputLatency = 0.0;

    static int callback(const void* /*input*/, void* output, unsigned long frameCount,
                        const PaStreamCallbackTimeInfo* timeInfo,
                        PaStreamCallbackFlags /*statusFlags*/, void* userData) noexcept {
        auto* self = static_cast<Impl*>(userData);
        const rt::RealtimeScope realtime;
        PlaybackMarker m;
        m.cursor = self->cursor;
        m.dacTime = timeInfo != nullptr ? timeInfo->outputBufferDacTime : 0.0;
        m.valid = true;
        self->marker.publish(m);

        auto* out = static_cast<float*>(output);
        const std::size_t channels = static_cast<std::size_t>(self->channels);
        const std::size_t available =
            self->buffer.size() > self->cursor ? self->buffer.size() - self->cursor : 0;
        const std::size_t copy = std::min<std::size_t>(available, frameCount);
        for (std::size_t i = 0; i < copy; ++i) {
            const float sample = self->buffer[self->cursor + i];
            for (std::size_t c = 0; c < channels; ++c) {
                out[i * channels + c] = sample;
            }
        }
        for (std::size_t i = copy; i < frameCount; ++i) {
            for (std::size_t c = 0; c < channels; ++c) {
                out[i * channels + c] = 0.0f;
            }
        }
        self->cursor += copy;
        if (copy < frameCount) {
            self->finished.store(true, std::memory_order_release);
            return paComplete;
        }
        return paContinue;
    }
};

PlaybackStream::PlaybackStream(const PortAudioSession& /*session*/, const OutputDevice& device,
                               const std::vector<float>& samples, double sampleRate,
                               double startSeconds)
    : impl_(std::make_unique<Impl>()) {
    if (!(sampleRate > 0.0)) {
        throw std::invalid_argument("PlaybackStream: the sample rate must be positive");
    }
    impl_->device = device;
    impl_->rate = device.defaultSampleRate > 0.0 ? device.defaultSampleRate : sampleRate;
    impl_->channels = std::clamp(device.maxOutputChannels, 1, 2);
    impl_->buffer = resampleForPlayback(samples, sampleRate, impl_->rate);
    const double startFrame = std::max(0.0, startSeconds) * impl_->rate;
    impl_->startCursor = std::min<std::uint64_t>(static_cast<std::uint64_t>(startFrame), impl_->buffer.size());
    impl_->cursor = impl_->startCursor;

    PaStreamParameters params{};
    params.device = device.index;
    params.channelCount = impl_->channels;
    params.sampleFormat = paFloat32;
    params.suggestedLatency = device.defaultLowOutputLatency;
    params.hostApiSpecificStreamInfo = nullptr;
    const PaError err = Pa_OpenStream(&impl_->stream, nullptr, &params, impl_->rate,
                                      paFramesPerBufferUnspecified, paNoFlag, &Impl::callback,
                                      impl_.get());
    if (err != paNoError) {
        impl_->stream = nullptr;
        detail::throwPortAudioError("Pa_OpenStream (output) failed", err);
    }
    if (const PaStreamInfo* info = Pa_GetStreamInfo(impl_->stream); info != nullptr) {
        impl_->outputLatency = info->outputLatency;
        if (info->sampleRate > 0.0) {
            impl_->rate = info->sampleRate;
        }
    }
}

PlaybackStream::~PlaybackStream() {
    if (impl_ && impl_->stream != nullptr) {
        if (Pa_IsStreamActive(impl_->stream) == 1) {
            Pa_AbortStream(impl_->stream);
        }
        Pa_CloseStream(impl_->stream);
    }
}

void PlaybackStream::start() {
    if (const PaError err = Pa_StartStream(impl_->stream); err != paNoError) {
        detail::throwPortAudioError("Pa_StartStream (output) failed", err);
    }
}

void PlaybackStream::stop() {
    if (!running()) {
        return;
    }
    // Abort rather than stop: stop() waits for the buffers already queued to play out,
    // and an operator who pressed q has heard enough.
    if (const PaError err = Pa_AbortStream(impl_->stream); err != paNoError) {
        detail::throwPortAudioError("Pa_AbortStream (output) failed", err);
    }
}

bool PlaybackStream::running() const noexcept {
    return Pa_IsStreamActive(impl_->stream) == 1;
}

bool PlaybackStream::finished() const noexcept {
    return impl_->finished.load(std::memory_order_acquire);
}

double PlaybackStream::positionSeconds() const noexcept {
    const PlaybackMarker m = impl_->marker.load();
    if (!m.valid) {
        return static_cast<double>(impl_->startCursor) / impl_->rate;
    }
    double frames = static_cast<double>(m.cursor);
    const double now = Pa_GetStreamTime(impl_->stream);
    if (m.dacTime > 0.0 && now > 0.0) {
        frames += (now - m.dacTime) * impl_->rate;
    } else {
        frames -= impl_->outputLatency * impl_->rate;
    }
    frames = std::clamp(frames, 0.0, static_cast<double>(impl_->buffer.size()));
    return frames / impl_->rate;
}

double PlaybackStream::durationSeconds() const noexcept {
    return static_cast<double>(impl_->buffer.size()) / impl_->rate;
}

double PlaybackStream::outputLatencySeconds() const noexcept {
    return impl_->outputLatency;
}

double PlaybackStream::deviceSampleRate() const noexcept {
    return impl_->rate;
}

int PlaybackStream::channels() const noexcept {
    return impl_->channels;
}

} // namespace takt4::audio
