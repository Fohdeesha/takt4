#include "core/audio/host_apis.hpp"

#include "core/audio/portaudio_session.hpp"

#include <portaudio.h>

#include <cstddef>
#include <string>

namespace takt4::audio {

std::vector<HostApiInfo> listHostApis() {
    PortAudioSession session;

    const PaHostApiIndex count = Pa_GetHostApiCount();
    if (count < 0) {
        throw PortAudioError(count, std::string("Pa_GetHostApiCount failed: ") + Pa_GetErrorText(count));
    }

    std::vector<HostApiInfo> apis;
    apis.reserve(static_cast<std::size_t>(count));
    for (PaHostApiIndex i = 0; i < count; ++i) {
        const PaHostApiInfo* info = Pa_GetHostApiInfo(i);
        if (info == nullptr) {
            continue;
        }
        apis.push_back({i, info->name, info->deviceCount, info->type == paASIO});
    }
    return apis;
}

} // namespace takt4::audio
