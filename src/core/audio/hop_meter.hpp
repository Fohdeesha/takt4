#pragma once

#include "core/audio/hop_processor.hpp"
#include "core/rt/spsc_ring.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace takt4::audio {

/// Level of one hop of the picked mono signal.
struct HopLevel {
    std::uint64_t hopIndex = 0;
    float rms = 0.0f;  // linear, full scale = 1.0
    float peak = 0.0f; // absolute peak, linear
};

/// The Phase 1 hop consumer: measures every hop and queues the result for another
/// thread to print or draw. If the reader falls behind, hops are dropped and counted
/// rather than blocking the audio thread.
class HopMeter final : public HopProcessor {
public:
    /// About ten seconds of hops.
    static constexpr std::size_t kQueueCapacity = 512;

    void processHop(const float* hop, std::uint64_t hopIndex) noexcept override;

    /// Reader side. Returns false when nothing is queued.
    bool pop(HopLevel& out) noexcept { return levels_.tryPop(out); }

    /// Hops that arrived while the queue was full, since construction.
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    rt::SpscRing<HopLevel, kQueueCapacity> levels_;
    std::atomic<std::uint64_t> dropped_{0};
};

/// Level in dBFS, floored so silence prints as a number rather than -inf.
float toDbfs(float linear) noexcept;

} // namespace takt4::audio
