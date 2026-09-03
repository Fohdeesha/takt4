#pragma once

#include <string>
#include <vector>

namespace takt4::audio {

class PortAudioSession;

/// The host APIs takt4 knows how to pick channels on (HANDOFF §5.1). PortAudio is built
/// with these and nothing else (cmake/deps.cmake); Other exists so an unexpected one
/// still gets the software slice rather than being dropped.
enum class HostApiKind {
    Other,
    Asio,      // Windows; native channel selectors
    Wasapi,    // Windows; software slice, also offers loopback capture of outputs
    CoreAudio, // macOS; native channel map
    Alsa,      // Linux; software slice
    Jack,      // Linux; software slice, ports are wired outside the app
};

const char* toString(HostApiKind kind) noexcept;

/// Whether streams on this host API can be opened on a chosen subset of the device's
/// channels, or have to open every channel and slice in software.
bool hasNativeChannelSelection(HostApiKind kind) noexcept;

/// One input-capable device as PortAudio reports it.
struct InputDevice {
    int index = -1;         // PaDeviceIndex; valid only while the session that listed it lives
    int hostApiIndex = -1;  // PaHostApiIndex
    HostApiKind hostApi = HostApiKind::Other;
    std::string hostApiName; // PortAudio's name for the host API, e.g. "Windows WASAPI"
    std::string name;        // UTF-8, as PortAudio reports it
    int maxInputChannels = 0;
    double defaultSampleRate = 0.0;
    double defaultLowInputLatency = 0.0;  // seconds
    double defaultHighInputLatency = 0.0; // seconds
    bool isDefaultInput = false;          // the host API's default input device
    bool isLoopback = false;              // WASAPI: an output endpoint captured as input
    /// Driver-reported channel names where the host API has them (ASIO, CoreAudio);
    /// otherwise empty. When present there is one per input channel.
    std::vector<std::string> channelNames;
};

/// Every device with at least one input channel, across all initialised host APIs, in
/// PortAudio's device order. Throws PortAudioError.
///
/// PortAudio loads every ASIO driver once at initialisation to read its channel layout
/// and names. A single-client driver that another application holds open may refuse
/// that, and then the interface is missing from this list rather than failing later at
/// open (HANDOFF R2). Whether it refuses at load or at open is up to the driver.
std::vector<InputDevice> listInputDevices(const PortAudioSession& session);

} // namespace takt4::audio
