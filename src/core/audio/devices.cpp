#include "core/audio/devices.hpp"

#include "core/audio/portaudio_session.hpp"

#include <portaudio.h>
#if defined(_WIN32)
#include <pa_asio.h>
#include <pa_win_wasapi.h>
#elif defined(__APPLE__)
#include <pa_mac_core.h>
#endif

#include <cstddef>
#include <string>

namespace takt4::audio {

namespace {

HostApiKind kindOf(PaHostApiTypeId type) noexcept {
    switch (type) {
    case paASIO:
        return HostApiKind::Asio;
    case paWASAPI:
        return HostApiKind::Wasapi;
    case paCoreAudio:
        return HostApiKind::CoreAudio;
    case paALSA:
        return HostApiKind::Alsa;
    case paJACK:
        return HostApiKind::Jack;
    default:
        return HostApiKind::Other;
    }
}

#if defined(_WIN32)
std::vector<std::string> asioInputChannelNames(PaDeviceIndex device, int channelCount) {
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(channelCount));
    for (int ch = 0; ch < channelCount; ++ch) {
        const char* name = nullptr;
        // Read from the layout PortAudio cached when it initialised; nothing is loaded here.
        if (PaAsio_GetInputChannelName(device, ch, &name) != paNoError || name == nullptr) {
            names.clear();
            break;
        }
        names.emplace_back(name);
    }
    return names;
}
#elif defined(__APPLE__)
// PaMacCore_GetChannelName wants the index of the device within the Core Audio host
// API, not the global PaDeviceIndex it is documented with (it indexes the host API's
// own device table directly).
int coreAudioDeviceIndex(PaHostApiIndex hostApi, int deviceCount, PaDeviceIndex device) {
    for (int i = 0; i < deviceCount; ++i) {
        if (Pa_HostApiDeviceIndexToDeviceIndex(hostApi, i) == device) {
            return i;
        }
    }
    return -1;
}

std::vector<std::string> coreAudioInputChannelNames(int hostApiDevice, int channelCount) {
    std::vector<std::string> names;
    if (hostApiDevice < 0) {
        return names;
    }
    names.reserve(static_cast<std::size_t>(channelCount));
    for (int ch = 0; ch < channelCount; ++ch) {
        // Returns a pointer into one static buffer that the next call overwrites.
        const char* name = PaMacCore_GetChannelName(hostApiDevice, ch, true);
        if (name == nullptr) {
            names.clear();
            break;
        }
        names.emplace_back(name);
    }
    return names;
}
#endif

} // namespace

const char* toString(HostApiKind kind) noexcept {
    switch (kind) {
    case HostApiKind::Asio:
        return "ASIO";
    case HostApiKind::Wasapi:
        return "WASAPI";
    case HostApiKind::CoreAudio:
        return "Core Audio";
    case HostApiKind::Alsa:
        return "ALSA";
    case HostApiKind::Jack:
        return "JACK";
    case HostApiKind::Other:
        break;
    }
    return "other";
}

bool hasNativeChannelSelection(HostApiKind kind) noexcept {
    return kind == HostApiKind::Asio || kind == HostApiKind::CoreAudio;
}

std::vector<InputDevice> listInputDevices(const PortAudioSession&) {
    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        throw PortAudioError(count, std::string("Pa_GetDeviceCount failed: ") + Pa_GetErrorText(count));
    }

    std::vector<InputDevice> devices;
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info == nullptr || info->maxInputChannels <= 0) {
            continue;
        }
        const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
        if (api == nullptr) {
            continue;
        }

        InputDevice device;
        device.index = i;
        device.hostApiIndex = info->hostApi;
        device.hostApi = kindOf(api->type);
        device.hostApiName = api->name;
        device.name = info->name;
        device.maxInputChannels = info->maxInputChannels;
        device.defaultSampleRate = info->defaultSampleRate;
        device.defaultLowInputLatency = info->defaultLowInputLatency;
        device.defaultHighInputLatency = info->defaultHighInputLatency;
        device.isDefaultInput = api->defaultInputDevice == i;

#if defined(_WIN32)
        if (device.hostApi == HostApiKind::Wasapi) {
            device.isLoopback = PaWasapi_IsLoopback(i) == 1;
        } else if (device.hostApi == HostApiKind::Asio) {
            device.channelNames = asioInputChannelNames(i, info->maxInputChannels);
        }
#elif defined(__APPLE__)
        if (device.hostApi == HostApiKind::CoreAudio) {
            device.channelNames = coreAudioInputChannelNames(
                coreAudioDeviceIndex(info->hostApi, api->deviceCount, i), info->maxInputChannels);
        }
#endif

        devices.push_back(std::move(device));
    }
    return devices;
}

} // namespace takt4::audio
