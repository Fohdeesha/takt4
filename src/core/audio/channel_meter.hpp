#pragma once

#include "core/audio/devices.hpp"
#include "core/audio/input_stream.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace takt4::audio {

class PortAudioSession;

/// Levels of every input channel of a device at once, for choosing the right one:
/// the console meter's --all view now, the setup screen's channel picker later. Opens
/// all channels with a software slice regardless of host API; nothing is resampled.
///
/// ASIO drivers are single-client, so this cannot run beside an InputStream on the
/// same ASIO device.
class ChannelMeter {
public:
    struct Level {
        float rms = 0.0f;
        float peak = 0.0f;
    };

    /// Opens the stream; start() begins metering. Throws PortAudioError.
    ChannelMeter(const PortAudioSession& session, const InputDevice& device, double sampleRate = 0.0);
    ~ChannelMeter();

    ChannelMeter(const ChannelMeter&) = delete;
    ChannelMeter& operator=(const ChannelMeter&) = delete;

    void start();
    void stop();
    bool running() const noexcept;

    const InputDevice& device() const noexcept;
    int channelCount() const noexcept;
    double sampleRate() const noexcept;

    /// Reader side: each channel's level over the frames delivered since the previous
    /// read, then starts a fresh window. `out` must hold channelCount() entries.
    /// Returns the number of frames the window covered.
    std::uint64_t read(std::span<Level> out) noexcept;

    InputStreamCounters counters() const noexcept; // hopsOut is always 0 here

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takt4::audio
