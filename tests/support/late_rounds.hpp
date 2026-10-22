#pragma once

// The rounds of takt4's own threads that came late, kept for a timing test: what it puts a cue
// that went out late down to. A round late because its thread **woke** late is the machine's — a
// busy core, a paused virtual machine — and excuses the cue, which the test says as a warning; a
// round that **ran** long is takt4's, and excuses nothing (see `rt::RoundLateness`).
//
// The tests that use this are tagged [realtime]: they measure milliseconds, and CI reports rather
// than requires them where its machines stall (GitHub's Intel Mac).

#include "core/rt/thread_priority.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace takt4::testing {

/// Late rounds, in seconds since `origin` — the clock the test puts its beats and cues on.
/// **Declared before the threads it watches**, so it outlives every call they make to it.
class LateRounds {
public:
    explicit LateRounds(std::chrono::steady_clock::time_point origin) : origin_(origin) {}

    LateRounds(const LateRounds&) = delete;
    LateRounds& operator=(const LateRounds&) = delete;

    /// Seconds since the origin.
    double since(std::chrono::steady_clock::time_point point) const {
        return std::chrono::duration<double>(point - origin_).count();
    }
    double now() const { return since(std::chrono::steady_clock::now()); }

    /// Keeps the late rounds of the thread `lateness` belongs to, under `thread`. Before that
    /// thread starts.
    void watch(rt::RoundLateness& lateness, std::string thread) {
        lateness.setObserver(
            [this, thread = std::move(thread)](const rt::RoundLateness::Late& round) {
                const std::lock_guard<std::mutex> lock(mutex_);
                rounds_.push_back({thread, since(round.due), since(round.woke), since(round.done)});
            });
    }

    /// How much of `from` to `to` a thread watched slept past when it should have woken. Two
    /// threads asleep at once count once.
    double asleepBetween(double from, double to) const {
        std::vector<std::pair<double, double>> spans;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (const Round& round : rounds_) {
                const double start = std::max(round.due, from);
                const double end = std::min(round.woke, to);
                if (end > start) {
                    spans.emplace_back(start, end);
                }
            }
        }
        std::sort(spans.begin(), spans.end());
        double asleep = 0.0;
        double reached = from;
        for (const auto& [start, end] : spans) {
            if (end > reached) {
                asleep += end - std::max(start, reached);
                reached = end;
            }
        }
        return asleep;
    }

    /// Every late round that touches `from` to `to`, for a message: whose, and how late it woke
    /// and how long it ran, in milliseconds.
    std::string describe(double from, double to) const {
        std::ostringstream out;
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const Round& round : rounds_) {
            if (round.done >= from && round.due <= to) {
                out << round.thread << " at " << round.due << " s woke "
                    << (round.woke - round.due) * 1000.0 << " ms late and ran "
                    << (round.done - round.woke) * 1000.0 << " ms; ";
            }
        }
        const std::string said = out.str();
        return said.empty() ? std::string("no round of takt4's late then") : said;
    }

private:
    struct Round {
        std::string thread;
        double due = 0.0;
        double woke = 0.0;
        double done = 0.0;
    };

    std::chrono::steady_clock::time_point origin_;
    mutable std::mutex mutex_;
    std::vector<Round> rounds_;
};

} // namespace takt4::testing
