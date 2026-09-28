#include "core/audio/callback_clock.hpp"
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

/// A device delivering `rate` frames a second in buffers of `buffer` frames, each callback at the
/// moment its buffer is full, and timed by the stream's own `CallbackClock` as the real callback
/// times it — watched on a redraw timer that fires every 33 ms give or take a scheduler's worth
/// of jitter, the shape the window really has, or, given `lookFrom` and `lookTo`, as far apart as
/// a busy one. A stopped device loses the audio it would have sent. Advances in 1 ms steps and
/// reports what each look saw.
class Rig {
public:
    Rig(double rate, std::uint64_t buffer, double lookFrom = 0.020, double lookTo = 0.050)
        : rate_(rate), buffer_(buffer), jitter_(lookFrom, lookTo), clock_(rate),
          nextCallback_(now_ + static_cast<double>(buffer) / rate) {}

    void setRate(double rate) { rate_ = rate; }
    void stopCallbacks() { stopped_ = true; }
    void resumeCallbacks() { stopped_ = false; }
    /// The device loses `seconds` of audio before its next callback: that callback comes that
    /// much later and brings one buffer, as a driver that dropped the audio does.
    void loseBeforeNextCallback(double seconds) { nextCallback_ += seconds; }

    /// Runs for `seconds`, looking every ~33 ms, and returns the last reading. `worst`
    /// collects whether any look along the way said something other than Healthy/Starting.
    InputWatchdog::Reading run(InputWatchdog& watchdog, double seconds, bool* anyAlarm = nullptr) {
        InputWatchdog::Reading last;
        const double until = now_ + seconds;
        while (now_ < until) {
            now_ += 0.001;
            if (stopped_) {
                // Nothing arrives, and what the device would have sent is gone: the first buffer
                // after it comes back is a buffer's length after it came back.
                nextCallback_ = now_ + static_cast<double>(buffer_) / rate_;
            } else {
                while (nextCallback_ <= now_) {
                    counters_.framesIn += buffer_;
                    ++counters_.callbacks;
                    clock_.onCallback(static_cast<std::uint32_t>(buffer_), nanos(nextCallback_));
                    nextCallback_ += static_cast<double>(buffer_) / rate_;
                }
            }
            if (now_ >= nextLook_) {
                takt4::audio::addClockReading(counters_, clock_.read(), nanos(now_));
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
    static std::int64_t nanos(double seconds) { return static_cast<std::int64_t>(seconds * 1e9); }

    double rate_;
    std::uint64_t buffer_;
    bool stopped_ = false;
    double now_ = 100.0;
    double nextLook_ = 100.0;
    InputStreamCounters counters_;
    std::mt19937 random_{20260923};
    std::uniform_real_distribution<double> jitter_;
    takt4::audio::CallbackClock clock_;
    double nextCallback_;
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
    // By 0.6 s. Silence is counted from the last callback as the callback timed it, which came up
    // to a buffer (2.7 ms here) before the stop, and looks are up to 50 ms apart — so half a
    // second of it is seen within 0.6 s of the stop and a buffer. This gave it 0.7 s under a name
    // that said half a second (the audit of 2026-09-25, T17).
    const InputWatchdog::Reading dead = rig.run(watchdog, 0.3);
    CHECK(dead.verdict == Verdict::Silent);
    CHECK(dead.silentForSeconds >= 0.5);
    CHECK(dead.silentForSeconds <= 0.6 + 128.0 / 48000.0);

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

TEST_CASE("a redraw 160 ms apart with 2048- to 4096-frame buffers makes no clock of a dropout",
          "[audio][watchdog]") {
    // The last of the 2026-09-25 audit's watchdog residue: measured from the looks, a pair of them
    // had to be allowed as far apart as the redraw, so audio a driver lost inside that span read
    // as a slower clock — two false "rate changed" in the worst simulated case, each reopening a
    // stream that had recovered. Timed by the callbacks themselves, a dropout is left out with
    // the time it lasted, however seldom the window looks. Dropouts shorter than a callback
    // period among them, at every buffer and rate the case named, with looks 20 to 160 ms apart.
    for (const std::uint64_t buffer : {2048U, 4096U}) {
        for (const double rate : {44100.0, 48000.0, 96000.0}) {
            INFO(rate << " Hz in " << buffer << "-frame buffers, looks 20 to 160 ms apart");
            Rig rig(rate, buffer, 0.020, 0.160);
            InputWatchdog watchdog;
            watchdog.reset(rate, rig.now());
            rig.run(watchdog, 5.0);
            bool alarm = false;
            const double period = static_cast<double>(buffer) / rate;
            for (int i = 0; i < 30; ++i) {
                rig.stopCallbacks();
                // A third of a period, two thirds, a period, then the old story's 80 and 350 ms.
                const double lengths[] = {period / 3.0, 2.0 * period / 3.0, period, 0.08, 0.35};
                rig.run(watchdog, lengths[i % 5], &alarm);
                rig.resumeCallbacks();
                rig.run(watchdog, 1.9, &alarm);
            }
            INFO("first alarm: verdict " << static_cast<int>(rig.firstAlarm.verdict)
                                         << ", measured " << rig.firstAlarm.measuredRate
                                         << " Hz at t=" << rig.firstAlarmAt);
            CHECK_FALSE(alarm);
            // And a clock that really moves is still noticed, looks that far apart or not.
            const double moved = secondsToNotice(rate, rate == 48000.0 ? 44100.0 : 48000.0,
                                                 buffer, 0.020, 0.160);
            CHECK(moved > 0.0);
            CHECK(moved < 4.0);
        }
    }
}

TEST_CASE("two dropouts of just under a buffer in three seconds are not taken for another clock",
          "[audio][watchdog]") {
    // The worst the callback clock lets through: a gap of up to two buffers is normal, so a
    // dropout losing just under a buffer is counted as time with no audio in it. Two of those in
    // one window, at 48 kHz in 4096-frame buffers, lose 6 % of it: measured, the lowest reading
    // here is 45.19 kHz, 2.47 % off 44.1 kHz — inside the 2.5 % a clock used to be matched
    // within, and outside today's 1.5 %. (A dropout every 1.5 s puts three in a window and
    // reads 43.9 kHz, which either match takes for a clock: that is a stream losing a tenth of
    // its audio, and reopening it is no wrong answer.)
    const double rate = 48000.0;
    const std::uint64_t buffer = 4096;
    const double period = static_cast<double>(buffer) / rate;
    Rig rig(rate, buffer);
    InputWatchdog watchdog;
    watchdog.reset(rate, rig.now());
    rig.run(watchdog, 5.0);
    bool alarm = false;
    for (int i = 0; i < 20; ++i) {
        rig.loseBeforeNextCallback(period + 0.0048);
        rig.run(watchdog, 1.6, &alarm);
    }
    INFO("first alarm: measured " << rig.firstAlarm.measuredRate << " Hz at t=" << rig.firstAlarmAt);
    CHECK_FALSE(alarm);
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
