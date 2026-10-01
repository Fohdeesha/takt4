#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace takt4::audio {

class PortAudioSession;

/// One output-capable device as PortAudio reports it.
struct OutputDevice {
    int index = -1;        // PaDeviceIndex; valid only while the session that listed it lives
    int hostApiIndex = -1; // PaHostApiIndex
    std::string hostApiName;
    std::string name;
    int maxOutputChannels = 0;
    double defaultSampleRate = 0.0;
    double defaultLowOutputLatency = 0.0; // seconds
    bool isDefaultOutput = false;         // the host API's default output device
};

/// Every device with at least one output channel, in PortAudio's device order.
std::vector<OutputDevice> listOutputDevices(const PortAudioSession& session);

/// The device to play through when none is named: WASAPI's default output where that
/// host API is present — it reports the DAC time of each buffer, which is what a tap is
/// timed against — and PortAudio's own default otherwise. Nothing on a machine with no
/// output at all.
std::optional<OutputDevice> defaultOutputDevice(const PortAudioSession& session);

/// `samples` at `inputRate` converted to `outputRate`, time-aligned: output sample k
/// stands for input time k / outputRate, so a position in the output is a position in
/// the source. Offline, and it allocates; it is the whole track, done once.
std::vector<float> resampleForPlayback(const std::vector<float>& samples, double inputRate,
                                       double outputRate);

/// A mono buffer played to an output device, with the track's own clock readable from
/// any thread — what `takt4-cli annotate` times its taps by.
///
/// The callback copies from a buffer resampled to the device's rate at construction and
/// stamps each buffer it hands over with the DAC time PortAudio gives it, so
/// `positionSeconds()` can say what the operator is hearing *now* rather than what has
/// been written so far — a difference of one output latency, 10 to 100 ms depending on
/// the host, and the whole of a tap's accuracy.
class PlaybackStream {
public:
    /// The buffer is resampled here, once. `startSeconds` is where playback begins;
    /// positions stay absolute, so a track can be picked up from its second half.
    PlaybackStream(const PortAudioSession& session, const OutputDevice& device,
                   const std::vector<float>& samples, double sampleRate, double startSeconds = 0.0);
    ~PlaybackStream();

    PlaybackStream(const PlaybackStream&) = delete;
    PlaybackStream& operator=(const PlaybackStream&) = delete;

    void start();
    void stop();
    /// True while the stream is active: from start() until the last sample has gone out
    /// or stop() was called.
    bool running() const noexcept;
    /// True once the callback has handed the last sample over.
    bool finished() const noexcept;

    /// The source time at the speakers now, in seconds: the DAC time the callback
    /// stamped its last buffer with, carried forward on the stream's clock.
    double positionSeconds() const noexcept;
    double durationSeconds() const noexcept;
    double outputLatencySeconds() const noexcept;
    double deviceSampleRate() const noexcept;
    int channels() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takt4::audio
