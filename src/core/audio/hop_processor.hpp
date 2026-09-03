#pragma once

#include <cstdint>

namespace takt4::audio {

/// Consumer of the input pipeline's output: mono hops of kHopSize samples at
/// kInternalSampleRate. The feature front end and tracker (HANDOFF §5.2, §5.3) sit
/// behind this in later phases; Phase 1 has a level meter.
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

protected:
    HopProcessor() = default;
    HopProcessor(const HopProcessor&) = default;
    HopProcessor& operator=(const HopProcessor&) = default;
};

} // namespace takt4::audio
