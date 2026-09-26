#include "core/audio/stream_setup.hpp"

#include "core/audio/portaudio_session.hpp"
#include "core/sandbox.hpp"

#include <stdexcept>
#include <string>

namespace takt4::audio::detail {

void setupInputParameters(PaStreamParameters& params, HostStreamInfo& info,
                          const InputDevice& device, int channelCount,
                          std::span<const int> nativeSelectors, double suggestedLatency) {
    params = PaStreamParameters{};
    params.device = device.index;
    params.channelCount = channelCount;
    params.sampleFormat = paFloat32;
    params.suggestedLatency = suggestedLatency < 0.0 ? device.defaultLowInputLatency : suggestedLatency;
    params.hostApiSpecificStreamInfo = nullptr;
    info.pointer = nullptr;

    if (nativeSelectors.empty()) {
        return;
    }
    if (!hasNativeChannelSelection(device.hostApi)) {
        throw std::logic_error(std::string("native channel selection requested on ") +
                               toString(device.hostApi));
    }
    if (nativeSelectors.size() != static_cast<std::size_t>(channelCount) ||
        nativeSelectors.size() > 2) {
        throw std::logic_error("native channel selectors must match the stream's channel count");
    }

#if defined(_WIN32)
    // ASIO: open only these device channels, in this order (pa_asio.h).
    for (std::size_t i = 0; i < nativeSelectors.size(); ++i) {
        info.selectors[i] = nativeSelectors[i];
    }
    info.asio = PaAsioStreamInfo{};
    info.asio.size = sizeof(PaAsioStreamInfo);
    info.asio.hostApiType = paASIO;
    info.asio.version = 1;
    info.asio.flags = paAsioUseChannelSelectors;
    info.asio.channelSelectors = info.selectors.data();
    info.pointer = &info.asio;
#elif defined(__APPLE__)
    // CoreAudio: for input, map[appChannel] = deviceChannel (coreaudio/notes.txt).
    // paMacCorePlayNice leaves the device's own rate and buffer settings alone; the
    // engine resamples whatever it gets.
    for (std::size_t i = 0; i < nativeSelectors.size(); ++i) {
        info.map[i] = static_cast<SInt32>(nativeSelectors[i]);
    }
    PaMacCore_SetupStreamInfo(&info.mac, paMacCorePlayNice);
    PaMacCore_SetupChannelMap(&info.mac, info.map.data(),
                              static_cast<unsigned long>(nativeSelectors.size()));
    info.pointer = &info.mac;
#else
    throw std::logic_error("no native channel selection on this platform");
#endif
    params.hostApiSpecificStreamInfo = info.pointer;
}

double resolveSampleRate(const InputDevice& device, double requested) noexcept {
    return requested > 0.0 ? requested : device.defaultSampleRate;
}

void throwPortAudioError(const char* what, PaError err) {
    std::string message = std::string(what) + ": " + Pa_GetErrorText(err);
    if (err == paUnanticipatedHostError) {
        if (const PaHostErrorInfo* host = Pa_GetLastHostErrorInfo();
            host != nullptr && host->errorText != nullptr && host->errorText[0] != '\0') {
            message += " (";
            message += host->errorText;
            message += ')';
        }
    }
    throw PortAudioError(err, message);
}

void refuseInSandbox(const std::string& device) {
    if (sandbox::active()) {
        sandbox::refuse(sandbox::Refused::Audio);
        throw PortAudioError(paDeviceUnavailable,
                             "\"" + device + "\" is not opened in the test sandbox");
    }
}

} // namespace takt4::audio::detail
