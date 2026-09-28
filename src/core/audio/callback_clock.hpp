#pragma once

#include "core/rt/published.hpp"

#include <chrono>
#include <cstdint>

namespace takt4::audio {

/// An input's account of its own clock, kept by its audio callback — which is the only place the
/// time a buffer arrived can be known to within a buffer.
///
/// **Why the callback keeps it** (the audit of 2026-09-25's last watchdog residue). The watchdog
/// used to measure a device's rate from the frame counter as the window's redraw found it, and
/// looks are 33 ms apart on a quiet machine and 160 ms on a busy one. A pair of looks had to be
/// allowed to be as far apart as the redraw was, so audio a driver dropped inside that span —
/// frames never delivered — read as a slower clock, and with 2048- to 4096-frame buffers a run of
/// such dropouts could land on another standard rate and reopen a stream that had recovered. The
/// callback knows exactly how long after the last one it came, and how many frames it brought.
///
/// So each callback that came **a normal distance after the one before** — at most two of its own
/// buffers' length, for a driver whose callbacks are uneven, plus a few milliseconds — adds its
/// frames and that distance to a running total; one that came after a longer gap adds neither.
/// Audio lost to a dropout is thereby left out with the time it was lost in, however seldom the
/// window looks, and the rate is the totals' ratio over any span a reader likes.
///
/// One writer, the callback; any number of readers. Nothing here allocates or locks.
class CallbackClock {
public:
    /// What a reader sees: whole, as of one callback (`rt::Published`).
    struct Reading {
        std::uint64_t callbacks = 0;
        /// Frames delivered by callbacks that came a normal distance after the one before.
        std::uint64_t normalFrames = 0;
        /// And the time those callbacks took, in nanoseconds.
        std::int64_t normalNanos = 0;
        /// When the last callback came, on `steadyNanos()`'s clock; 0 before any.
        std::int64_t lastNanos = 0;
    };

    /// A driver's callbacks are not quite even, and a thread is woken a little late now and then.
    static constexpr std::int64_t kSlackNanos = 5'000'000;

    /// `openedRate` is what the stream was opened at: a callback's buffer at that rate is how long
    /// it should have taken. Before the stream starts; never while callbacks are coming.
    explicit CallbackClock(double openedRate = 0.0) noexcept : openedRate_(openedRate) {}

    /// The callback's side: `frames` delivered, at `nowNanos` on `steadyNanos()`'s clock.
    void onCallback(std::uint32_t frames, std::int64_t nowNanos) noexcept {
        ++callbacks_;
        if (lastNanos_ != 0 && openedRate_ > 0.0) {
            const std::int64_t gap = nowNanos - lastNanos_;
            // The buffer this callback brought filled up during the gap before it, so its own
            // length is what the gap should have been.
            const double period = static_cast<double>(frames) / openedRate_ * 1e9;
            if (gap > 0 && static_cast<double>(gap) <= 2.0 * period + static_cast<double>(kSlackNanos)) {
                normalFrames_ += frames;
                normalNanos_ += gap;
            }
        }
        lastNanos_ = nowNanos;
        published_.publish(Reading{callbacks_, normalFrames_, normalNanos_, lastNanos_});
    }

    Reading read() const noexcept { return published_.load(); }

    /// The steady clock, in nanoseconds since its epoch. Safe on the audio thread: on Windows it
    /// is QueryPerformanceCounter, which neither allocates nor waits.
    static std::int64_t steadyNanos() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

private:
    double openedRate_;
    std::uint64_t callbacks_ = 0;
    std::uint64_t normalFrames_ = 0;
    std::int64_t normalNanos_ = 0;
    std::int64_t lastNanos_ = 0;
    rt::Published<Reading> published_;
};

} // namespace takt4::audio
