#pragma once

// A followed source a test sets by hand: a steady beat grid on a clock it is given — Link's own,
// in a test of the whole chain — there or not, its tempo and phase movable while the timing
// worker reads it.

#include "core/timing/grid_source.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace takt4::testing {

class FakeGridSource final : public timing::GridSource {
public:
    /// `clock` is now, in microseconds; the grid is `bpm` with its beat 0 at `beatZeroAt` on it.
    FakeGridSource(std::function<std::int64_t()> clock, double bpm, double beatZeroAt,
                   bool anchored = true)
        : clock_(std::move(clock)), bpm_(bpm), zero_(beatZeroAt), anchored_(anchored) {}

    void begin(std::uint32_t beatsPerBar) noexcept override {
        beatsPerBar_.store(beatsPerBar, std::memory_order_relaxed);
        begun_.fetch_add(1, std::memory_order_relaxed);
    }
    timing::GridReading read() noexcept override {
        timing::GridReading reading;
        reading.at = clock_();
        const std::lock_guard<std::mutex> lock(mutex_);
        reading.present = present_;
        reading.stopped = stopped_;
        if (present_) {
            reading.bpm = bpm_;
            reading.beat = (static_cast<double>(reading.at) - zero_) * bpm_ / 60e6;
            reading.anchored = anchored_;
        }
        return reading;
    }
    void end() noexcept override { ended_.fetch_add(1, std::memory_order_relaxed); }

    void setPresent(bool present) {
        const std::lock_guard<std::mutex> lock(mutex_);
        present_ = present;
    }
    void setStopped(bool stopped) {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = stopped;
    }
    /// Where beat 0 of the grid is, and its tempo — a new session, a DJ's pitch fader.
    void setGrid(double bpm, double beatZeroAt) {
        const std::lock_guard<std::mutex> lock(mutex_);
        bpm_ = bpm;
        zero_ = beatZeroAt;
    }

    int begun() const noexcept { return begun_.load(std::memory_order_relaxed); }
    int ended() const noexcept { return ended_.load(std::memory_order_relaxed); }
    std::uint32_t beatsPerBar() const noexcept {
        return beatsPerBar_.load(std::memory_order_relaxed);
    }

private:
    std::function<std::int64_t()> clock_;
    std::mutex mutex_;
    double bpm_;
    double zero_;
    bool anchored_;
    bool present_ = true;
    bool stopped_ = false;
    std::atomic<int> begun_{0};
    std::atomic<int> ended_{0};
    std::atomic<std::uint32_t> beatsPerBar_{0};
};

} // namespace takt4::testing
