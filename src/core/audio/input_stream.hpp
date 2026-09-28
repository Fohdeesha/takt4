#pragma once

#include "core/audio/callback_clock.hpp"
#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/hop_processor.hpp"
#include "core/audio/input_pipeline.hpp"

#include <cstdint>
#include <memory>

namespace takt4::audio {

class PortAudioSession;

struct InputStreamOptions {
    double sampleRate = 0.0;         // 0: the device's default rate
    double suggestedLatency = -1.0;  // seconds; < 0: the device's default low input latency
    bool forceSoftwareSlice = false; // open every channel and slice even where selectors exist
};

/// Counts kept by the audio callback; read from any thread.
struct InputStreamCounters {
    std::uint64_t callbacks = 0;
    std::uint64_t framesIn = 0;      // device frames delivered (HANDOFF §4.3 sample clock)
    std::uint64_t hopsOut = 0;       // hops handed to the processor
    std::uint32_t inputOverflows = 0; // callbacks flagged paInputOverflow: the device dropped input
    std::uint64_t samplesRepaired = 0; // not a number, so passed on as silence — InputPipeline
    /// The callbacks' own account of their clock (`CallbackClock`): the frames and the seconds
    /// of every callback that came a normal distance after the one before, which is what the
    /// device's rate is measured over — a dropout is left out with the time it lasted.
    std::uint64_t normalFrames = 0;
    double normalSeconds = 0.0;
    /// How long ago the last callback came, as of the moment these were read; 0 before any.
    double sinceLastCallbackSeconds = 0.0;
};

/// Puts `clock` into `counters` as of `nowNanos` on `CallbackClock::steadyNanos()`'s clock — how
/// `InputStream::counters` fills the clock's fields, and how a test that drives a `CallbackClock`
/// of its own on a clock of its own does the same.
void addClockReading(InputStreamCounters& counters, const CallbackClock::Reading& clock,
                     std::int64_t nowNanos) noexcept;

/// One channel (or pair) of one device, open through PortAudio and feeding a
/// HopProcessor: the live end of HANDOFF §4.1's input stage. The callback does nothing
/// but run the InputPipeline.
class InputStream {
public:
    /// Opens the stream; start() begins delivery. The device must come from the given
    /// session's listInputDevices(). Throws PortAudioError or std::invalid_argument.
    /// `processor` must outlive the stream.
    InputStream(const PortAudioSession& session, const InputDevice& device,
                const ChannelSelection& selection, HopProcessor& processor,
                const InputStreamOptions& options = {});
    ~InputStream(); // stops and closes

    InputStream(const InputStream&) = delete;
    InputStream& operator=(const InputStream&) = delete;

    void start(); // throws PortAudioError
    void stop();  // throws PortAudioError; no-op when not running
    bool running() const noexcept;

    /// Shuts the callback off from the `HopProcessor`, and waits until no callback is inside it
    /// — a callback's length. From here on the driver may call back as it likes; nothing reaches
    /// the processor. Makes no PortAudio call at all, so it is what a caller does on its own
    /// thread before handing the stream to another to be stopped and closed, however long the
    /// driver takes over that (the audit of 2026-09-25, L23; see `CallbackGate`).
    void detach() noexcept;

    const InputDevice& device() const noexcept;
    const ChannelPicker& picker() const noexcept;
    /// The rate the stream was opened at and the pipeline resamples from.
    double sampleRate() const noexcept;
    /// The rate PortAudio reports for the open stream. Differs from sampleRate() only
    /// where the host API knows the hardware clock is off from the nominal rate.
    double reportedSampleRate() const noexcept;
    double inputLatencySeconds() const noexcept; // PortAudio's estimate for this stream
    std::size_t resamplerDelayFrames() const noexcept;

    InputStreamCounters counters() const noexcept;
    /// For a pair, what its two sides have brought — see `StereoCheck`. Zero for one channel.
    StereoSums stereo() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takt4::audio
