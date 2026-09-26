#include "core/audio/input_watchdog.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>

using Catch::Approx;
using takt4::audio::InputStreamCounters;
using takt4::audio::InputWatchdog;
using Verdict = takt4::audio::InputWatchdog::Verdict;

namespace {

/// A device delivering `rate` frames a second in buffers of `buffer` frames, watched on a
/// redraw timer that fires every 33 ms give or take a scheduler's worth of jitter — the shape
/// the window really has — or, given `lookFrom` and `lookTo`, as far apart as a busy one.
/// Advances in 1 ms steps and reports what each look saw.
class Rig {
public:
    Rig(double rate, std::uint64_t buffer, double lookFrom = 0.020, double lookTo = 0.050)
        : rate_(rate), buffer_(buffer), jitter_(lookFrom, lookTo) {}

    void setRate(double rate) { rate_ = rate; }
    void stopCallbacks() { stopped_ = true; }
    void resumeCallbacks() { stopped_ = false; }

    /// Runs for `seconds`, looking every ~33 ms, and returns the last reading. `worst`
    /// collects whether any look along the way said something other than Healthy/Starting.
    InputWatchdog::Reading run(InputWatchdog& watchdog, double seconds, bool* anyAlarm = nullptr) {
        InputWatchdog::Reading last;
        const double until = now_ + seconds;
        while (now_ < until) {
            now_ += 0.001;
            if (!stopped_) {
                owed_ += rate_ * 0.001;
                while (owed_ >= static_cast<double>(buffer_)) {
                    owed_ -= static_cast<double>(buffer_);
                    counters_.framesIn += buffer_;
                    ++counters_.callbacks;
                }
            }
            if (now_ >= nextLook_) {
                last = watchdog.observe(counters_, now_);
                if (anyAlarm != nullptr && last.verdict != Verdict::Healthy &&
                    last.verdict != Verdict::Starting) {
                    if (!*anyAlarm) {
                        firstAlarm = last;
                        firstAlarmAt = now_;
                    }
                    *anyAlarm = true;
                }
                nextLook_ = now_ + jitter_(random_);
            }
        }
        return last;
    }

    double now() const { return now_; }

    /// The first look that raised an alarm, for a failure message that says which.
    InputWatchdog::Reading firstAlarm;
    double firstAlarmAt = 0.0;

private:
    double rate_;
    std::uint64_t buffer_;
    bool stopped_ = false;
    double owed_ = 0.0;
    double now_ = 100.0;
    double nextLook_ = 100.0;
    InputStreamCounters counters_;
    std::mt19937 random_{20260923};
    std::uniform_real_distribution<double> jitter_;
};

/// Seconds until the watchdog says the clock moved, after the device underneath it goes from
/// `from` to `to` — or a negative number if it never says so in half a minute.
double secondsToNotice(double from, double to, std::uint64_t buffer, double lookFrom = 0.020,
                       double lookTo = 0.050) {
    Rig rig(from, buffer, lookFrom, lookTo);
    InputWatchdog watchdog;
    watchdog.reset(from, rig.now());
    rig.run(watchdog, 5.0);
    rig.setRate(to);
    const double changed = rig.now();
    while (rig.now() - changed < 30.0) {
        if (rig.run(watchdog, 0.05).verdict == Verdict::RateChanged) {
            return rig.now() - changed;
        }
    }
    return -1.0;
}

/// Whether the dropout story of "dropouts the driver recovers from are not a new clock" raised
/// any alarm at this buffer and redraw spacing.
bool dropoutsAlarm(double rate, std::uint64_t buffer, double lookFrom = 0.020,
                   double lookTo = 0.050) {
    Rig rig(rate, buffer, lookFrom, lookTo);
    InputWatchdog watchdog;
    watchdog.reset(rate, rig.now());
    rig.run(watchdog, 5.0);
    bool alarm = false;
    for (int i = 0; i < 20; ++i) {
        rig.stopCallbacks();
        rig.run(watchdog, i % 2 == 0 ? 0.08 : 0.35, &alarm);
        rig.resumeCallbacks();
        rig.run(watchdog, 2.7, &alarm);
    }
    return alarm;
}

} // namespace

TEST_CASE("a healthy input reads healthy, at the rate it was opened at", "[audio][watchdog]") {
    Rig rig(44100.0, 256);
    InputWatchdog watchdog;
    watchdog.reset(44100.0, rig.now());
    bool alarm = false;
    // Ten minutes of an ordinary set: never a false alarm from timer jitter or the buffer
    // steps the frame counter moves in.
    const InputWatchdog::Reading reading = rig.run(watchdog, 600.0, &alarm);
    CHECK_FALSE(alarm);
    CHECK(reading.verdict == Verdict::Healthy);
    CHECK(reading.measuredRate == Approx(44100.0).epsilon(0.002));
}

TEST_CASE("an input whose callbacks stop reads silent after half a second of none",
          "[audio][watchdog]") {
    // The MOTU unplugged or power-cycled: ASIO has no message for it, the callbacks simply
    // stop, and PortAudio goes on calling the stream active.
    Rig rig(48000.0, 128);
    InputWatchdog watchdog;
    watchdog.reset(48000.0, rig.now());
    REQUIRE(rig.run(watchdog, 5.0).verdict == Verdict::Healthy);

    rig.stopCallbacks();
    CHECK(rig.run(watchdog, 0.3).verdict == Verdict::Healthy); // one buffer late is not dead
    // By 0.6 s. Silence is counted from the first look that found the last callbacks, and
    // looks are up to 50 ms apart, so half a second of it is seen at most two looks late. This
    // gave it 0.7 s under a name that said half a second (the audit of 2026-09-25, T17).
    const InputWatchdog::Reading dead = rig.run(watchdog, 0.3);
    CHECK(dead.verdict == Verdict::Silent);
    CHECK(dead.silentForSeconds >= 0.5);
    CHECK(dead.silentForSeconds <= 0.6);

    // And it comes back as healthy — without a false "rate changed" from measuring across
    // the gap, which would have made the window reopen a stream that had just recovered.
    rig.resumeCallbacks();
    bool alarm = false;
    rig.run(watchdog, 0.2);
    CHECK(rig.run(watchdog, 10.0, &alarm).verdict == Verdict::Healthy);
    CHECK_FALSE(alarm);
}

TEST_CASE("dropouts the driver recovers from are not a new clock", "[audio][watchdog]") {
    // The failure this watchdog's first version had: frames missing from the rate window read
    // as a slower clock, so a hiccup of a few hundred milliseconds — one that never became
    // silence — came out as "rate changed", which the window answers by reopening a stream
    // that had already recovered. A minute of them, short and long, at odd moments.
    Rig rig(44100.0, 256);
    InputWatchdog watchdog;
    watchdog.reset(44100.0, rig.now());
    REQUIRE(rig.run(watchdog, 5.0).verdict == Verdict::Healthy);
    bool alarm = false;
    for (int i = 0; i < 20; ++i) {
        rig.stopCallbacks();
        rig.run(watchdog, i % 2 == 0 ? 0.08 : 0.35, &alarm);
        rig.resumeCallbacks();
        rig.run(watchdog, 2.7, &alarm);
    }
    INFO("first alarm: verdict " << static_cast<int>(rig.firstAlarm.verdict) << ", measured "
                                 << rig.firstAlarm.measuredRate << " Hz, silent for "
                                 << rig.firstAlarm.silentForSeconds << " s, at t="
                                 << rig.firstAlarmAt);
    CHECK_FALSE(alarm);
}

TEST_CASE("a stream that never starts calling back is silent after its grace period",
          "[audio][watchdog]") {
    Rig rig(44100.0, 256);
    rig.stopCallbacks();
    InputWatchdog watchdog;
    watchdog.reset(44100.0, rig.now());
    CHECK(rig.run(watchdog, 1.5).verdict == Verdict::Starting);
    CHECK(rig.run(watchdog, 1.0).verdict == Verdict::Silent);
}

TEST_CASE("an interface moved to another rate underneath the stream is noticed",
          "[audio][watchdog]") {
    // Another client — Live, a control panel — moves the MOTU from 44.1 to 48 kHz. The
    // callbacks keep coming, so nothing looks wrong; the resampler keeps converting at the
    // old ratio and every tempo is out by 8.8 %.
    Rig rig(44100.0, 256);
    InputWatchdog watchdog;
    watchdog.reset(44100.0, rig.now());
    REQUIRE(rig.run(watchdog, 5.0).verdict == Verdict::Healthy);

    rig.setRate(48000.0);
    const InputWatchdog::Reading changed = rig.run(watchdog, 5.0);
    CHECK(changed.verdict == Verdict::RateChanged);
    CHECK(changed.measuredRate == Approx(48000.0).epsilon(0.01));

    // Reopened at the new rate, it is healthy again.
    watchdog.reset(48000.0, rig.now());
    bool alarm = false;
    CHECK(rig.run(watchdog, 10.0, &alarm).verdict == Verdict::Healthy);
    CHECK_FALSE(alarm);
}

TEST_CASE("a moved clock is noticed whatever the buffer size, 64 to 8192 frames",
          "[audio][watchdog]") {
    // The audit of 2026-09-25, L21: limits set for 256-frame buffers left out nearly every pair
    // of looks at 2048, where a callback comes every 46 ms, and a clock moved underneath the
    // stream was never noticed — 0 detections in 30 s, and every tempo 8.8 % off. Every buffer
    // an ASIO panel offers, at the rates a set runs at, moving each way and by an octave. At
    // 8192 a callback is 186 ms apart, past the fixed stall limit itself.
    struct Move {
        double from;
        double to;
    };
    for (const Move move : {Move{44100.0, 48000.0}, Move{48000.0, 44100.0},
                            Move{96000.0, 48000.0}, Move{44100.0, 88200.0},
                            Move{96000.0, 88200.0}}) {
        for (const std::uint64_t buffer : {64U, 256U, 1024U, 2048U, 4096U, 8192U}) {
            INFO(move.from << " Hz to " << move.to << " Hz in " << buffer << "-frame buffers");
            const double took = secondsToNotice(move.from, move.to, buffer);
            CHECK(took > 0.0);
            CHECK(took < 3.0);
        }
    }
}

TEST_CASE("a healthy input with large buffers reads healthy, and its dropouts are not a new clock",
          "[audio][watchdog]") {
    // What the limits that follow the buffer must not do instead: take the buffer steps of a
    // healthy 4096-frame stream, or its hiccups, for a clock.
    for (const std::uint64_t buffer : {1024U, 2048U, 4096U}) {
        for (const double rate : {44100.0, 48000.0, 96000.0}) {
            INFO(rate << " Hz in " << buffer << "-frame buffers");
            Rig rig(rate, buffer);
            InputWatchdog watchdog;
            watchdog.reset(rate, rig.now());
            bool alarm = false;
            const InputWatchdog::Reading reading = rig.run(watchdog, 120.0, &alarm);
            CHECK_FALSE(alarm);
            CHECK(reading.measuredRate == Approx(rate).epsilon(0.02));
            CHECK_FALSE(dropoutsAlarm(rate, buffer));
        }
    }
}

TEST_CASE("a busy redraw neither hides a moved clock nor makes one of a dropout",
          "[audio][watchdog]") {
    // The window looks on its redraw timer, and a busy machine spaces the looks out. With looks
    // up to 80 ms apart the fixed limits never measured at all, and limits that only let the
    // pairs of looks be further apart took the audio a dropout lost inside a pair for a slower
    // clock — which the window answers by reopening the stream (L21, found by simulation).
    for (const std::uint64_t buffer : {256U, 2048U}) {
        INFO(buffer << "-frame buffers, looks 20 to 80 ms apart");
        const double took = secondsToNotice(44100.0, 48000.0, buffer, 0.020, 0.080);
        CHECK(took > 0.0);
        CHECK(took < 3.0);
        CHECK_FALSE(dropoutsAlarm(44100.0, buffer, 0.020, 0.080));
        CHECK_FALSE(dropoutsAlarm(48000.0, buffer, 0.020, 0.080));
    }
}
