#include "core/audio/hop_meter.hpp"

#include "core/audio/rates.hpp"

#include <cmath>

namespace takt4::audio {

void HopMeter::processHop(const float* hop, std::uint64_t hopIndex) noexcept {
    double sumSquares = 0.0;
    float peak = 0.0f;
    for (std::size_t i = 0; i < kHopSize; ++i) {
        const float x = hop[i];
        sumSquares += static_cast<double>(x) * static_cast<double>(x);
        const float magnitude = std::fabs(x);
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    HopLevel level;
    level.hopIndex = hopIndex;
    level.rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(kHopSize)));
    level.peak = peak;
    if (!levels_.tryPush(level)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

float toDbfs(float linear) noexcept {
    constexpr float kFloorDb = -120.0f;
    if (!(linear > 0.0f)) {
        return kFloorDb;
    }
    const float db = 20.0f * std::log10(linear);
    return db < kFloorDb ? kFloorDb : db;
}

} // namespace takt4::audio
