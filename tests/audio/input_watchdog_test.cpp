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
/// the window really has. Advances in 1 ms steps and reports what each look saw.
class Rig {
public:
    Rig(double rate, std::uint64_t buffer) : rate_(rate), buffer_(buffer) {}

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
    std::uniform_real_distribution<double> jitter_{0.020, 0.050};
};

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

TEST_CASE("an input whose callbacks stop reads silent within half a second", "[audio][watchdog]") {
    // The MOTU unplugged or power-cycled: ASIO has no message for it, the callbacks simply
    // stop, and PortAudio goes on calling the stream active.
    Rig rig(48000.0, 128);
    InputWatchdog watchdog;
    watchdog.reset(48000.0, rig.now());
    REQUIRE(rig.run(watchdog, 5.0).verdict == Verdict::Healthy);

    rig.stopCallbacks();
    CHECK(rig.run(watchdog, 0.3).verdict == Verdict::Healthy); // one buffer late is not dead
    const InputWatchdog::Reading dead = rig.run(watchdog, 0.4);
    CHECK(dead.verdict == Verdict::Silent);
    CHECK(dead.silentForSeconds >= 0.5);

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
