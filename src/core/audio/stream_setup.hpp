#pragma once

// Shared plumbing for the classes that open PortAudio input streams (InputStream,
// ChannelMeter). Internal to core/audio.

#include "core/audio/devices.hpp"

#include <portaudio.h>

#if defined(_WIN32)
#include <pa_asio.h>
#elif defined(__APPLE__)
#include <pa_mac_core.h>
#endif

#include <array>
#include <span>

namespace takt4::audio::detail {

/// Storage for the host-specific stream info that PaStreamParameters points into.
/// Must outlive the Pa_OpenStream call it is used for (CoreAudio reads the map array
/// from here while opening).
struct HostStreamInfo {
#if defined(_WIN32)
    PaAsioStreamInfo asio{};
    std::array<int, 2> selectors{};
#elif defined(__APPLE__)
    PaMacCoreStreamInfo mac{};
    std::array<SInt32, 2> map{};
#endif
    void* pointer = nullptr; // what PaStreamParameters::hostApiSpecificStreamInfo gets
};

/// Fills `params` to open `channelCount` float32 input channels of `device`.
/// With a non-empty `nativeSelectors` (size == channelCount) the host API is asked for
/// exactly those device channels; that is only valid where
/// hasNativeChannelSelection(device.hostApi) holds, and throws std::logic_error
/// otherwise. `suggestedLatency` < 0 means the device's default low input latency.
void setupInputParameters(PaStreamParameters& params, HostStreamInfo& info,
                          const InputDevice& device, int channelCount,
                          std::span<const int> nativeSelectors, double suggestedLatency);

/// The rate to open with: `requested` if > 0, else the device's default.
double resolveSampleRate(const InputDevice& device, double requested) noexcept;

/// Throws PortAudioError for `err`, with the host's own message appended when
/// PortAudio has one.
[[noreturn]] void throwPortAudioError(const char* what, PaError err);

} // namespace takt4::audio::detail
