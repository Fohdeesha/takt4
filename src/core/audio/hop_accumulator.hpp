#pragma once

#include "core/audio/rates.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace takt4::audio {

/// Regroups a sample stream into fixed hops (HANDOFF §5.1: hop accumulator to 20 ms
/// frames). Blocks of any length go in; whenever a hop's worth has arrived, the hop is
/// handed out. The host block size is never assumed to divide the hop size.
class HopAccumulator {
public:
    explicit HopAccumulator(std::size_t hopSize = kHopSize) : hop_(hopSize) {}

    std::size_t hopSize() const noexcept { return hop_.size(); }

    /// Samples held back since the last complete hop.
    std::size_t pending() const noexcept { return fill_; }

    /// Real-time. Consumes `frames` samples and calls `onHop(const float* hop)` once
    /// per completed hop; the pointer is valid only during the call.
    template <class OnHop>
    void push(const float* input, std::size_t frames, OnHop&& onHop) noexcept {
        while (frames > 0) {
            const std::size_t take = std::min(frames, hop_.size() - fill_);
            std::copy_n(input, take, hop_.data() + fill_);
            fill_ += take;
            input += take;
            frames -= take;
            if (fill_ == hop_.size()) {
                onHop(static_cast<const float*>(hop_.data()));
                fill_ = 0;
            }
        }
    }

    /// Drops any partial hop.
    void reset() noexcept { fill_ = 0; }

private:
    std::vector<float> hop_;
    std::size_t fill_ = 0;
};

} // namespace takt4::audio
