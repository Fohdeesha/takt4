#pragma once

#include <cstdint>

namespace takt4::audio {

/// Consumer of the input pipeline's output: mono hops of kHopSize samples at
/// kInternalSampleRate. `engine::BeatEngine` is the one that tracks — the feature front end
/// and the model behind it (HANDOFF §5.2, §5.3) — and `audio::HopMeter` is the level meter
/// the console prints from.
///
/// processHop runs on the audio thread and is bound by HANDOFF §4.2: no allocation,
/// no locks, no I/O, fixed buffers, results out through a lock-free ring only.
class HopProcessor {
public:
    virtual ~HopProcessor() = default;

    /// `hop` holds kHopSize samples and is valid only during the call. `hopIndex`
    /// counts hops from 0 at stream start; hop k covers internal-rate samples
    /// [k · kHopSize, (k + 1) · kHopSize), so it doubles as the monotonic sample clock
    /// of HANDOFF §4.3.
    virtual void processHop(const float* hop, std::uint64_t hopIndex) noexcept = 0;

    /// A buffer of input has arrived, before any hop it completes is handed over: its first
    /// sample is internal-rate sample `firstSample` (the hop clock above, counting only what was
    /// delivered), it was at the input at `steadyMicros` on `std::chrono::steady_clock`, and
    /// `lostSamples` of audio, as internal-rate samples, have been lost before it since the stream
    /// started (`LostTime`). What stamps a hop with the moment its audio was heard — see
    /// `HostTimeSource`. The default ignores it; only a processor that stamps needs it.
    virtual void beginBuffer(double firstSample, std::int64_t steadyMicros,
                             double lostSamples) noexcept {
        (void)firstSample;
        (void)steadyMicros;
        (void)lostSamples;
    }

protected:
    HopProcessor() = default;
    HopProcessor(const HopProcessor&) = default;
    HopProcessor& operator=(const HopProcessor&) = default;
};

} // namespace takt4::audio
