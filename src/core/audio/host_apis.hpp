#pragma once

#include <string>
#include <vector>

namespace takt4::audio {

struct HostApiInfo {
    int index = -1; // PortAudio host API index, valid while PortAudio is initialised
    std::string name;
    int deviceCount = 0;
    bool isAsio = false; // Steinberg ASIO; Windows only
};

/// The host APIs PortAudio was built with and could initialise on this machine.
/// Initialises and terminates PortAudio for the duration of the call.
/// Throws PortAudioError.
std::vector<HostApiInfo> listHostApis();

} // namespace takt4::audio
