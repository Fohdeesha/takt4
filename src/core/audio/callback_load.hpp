#pragma once

#include <atomic>
#include <cstdint>

namespace takt4::audio {

/// **How much of its buffer's time an audio callback took** — the worst since a reader last took
/// it. A callback as long as its buffer leaves the driver no time before the next one is due,
/// which is where dropouts begin; a show controller watching takt4 is sent the worst of each
/// second (`remote::Status::load`).
///
/// One writer, the callback; one reader, who takes the worst and so starts the next span. Nothing
/// here allocates or locks.
class CallbackLoad {
public:
    static_assert(std::atomic<double>::is_always_lock_free);

    /// `openedRate` is what the stream was opened at: a callback's buffer at that rate is how long
    /// it had. Before the stream starts.
    explicit CallbackLoad(double openedRate = 0.0) noexcept : openedRate_(openedRate) {}

    /// The callback's side: a buffer of `frames`, handled from `startNanos` to `endNanos` on
    /// `CallbackClock::steadyNanos()`'s clock.
    void onCallback(std::uint32_t frames, std::int64_t startNanos, std::int64_t endNanos) noexcept {
        if (frames == 0 || openedRate_ <= 0.0 || endNanos < startNanos) {
            return;
        }
        const double share = static_cast<double>(endNanos - startNanos) /
                             (static_cast<double>(frames) / openedRate_ * 1e9);
        double worst = worst_.load(std::memory_order_relaxed);
        while (share > worst &&
               !worst_.compare_exchange_weak(worst, share, std::memory_order_relaxed)) {
        }
    }

    /// The worst share since the last call — 1 is a callback as long as its buffer — and, from
    /// here on, the next span's.
    double take() noexcept { return worst_.exchange(0.0, std::memory_order_relaxed); }

private:
    double openedRate_;
    std::atomic<double> worst_{0.0};
};

} // namespace takt4::audio
