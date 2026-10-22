#include "core/audio/host_time_fit.hpp"
#include "core/audio/lost_time.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

using Catch::Matchers::WithinAbs;
using takt4::audio::HostTimeFit;
using takt4::audio::LostTime;

namespace {

constexpr double kMicrosPerSample = 1e6 / 22050.0;

} // namespace

TEST_CASE("the line through a device's buffers gives every sample its moment", "[audio][timing]") {
    // A device whose crystal runs 80 parts in a million fast against the host clock, an uptime
    // of a month — 2.6e12 microseconds — and a millisecond of jitter on when each buffer is seen.
    // Every sample's moment comes back off the line to well inside a millisecond.
    HostTimeFit fit(kMicrosPerSample);
    constexpr double kOrigin = 2.6e12;
    constexpr double kDrift = 1.0 + 80e-6;
    std::mt19937 random(11);
    std::uniform_real_distribution<double> jitter(-1000.0, 1000.0);
    const auto truth = [&](double sample) { return kOrigin + sample * kMicrosPerSample / kDrift; };
    for (std::size_t buffer = 0; buffer < 2000; ++buffer) {
        const double sample = static_cast<double>(buffer) * 235.2; // 512 frames at 48 kHz
        fit.observe(sample, truth(sample) + jitter(random));
    }
    const double last = 1999.0 * 235.2;
    for (const double sample : {last - 50000.0, last - 1000.0, last, last + 441.0}) {
        INFO("sample " << sample);
        CHECK_THAT(fit.at(sample), WithinAbs(truth(sample), 150.0));
    }
    CHECK_THAT(fit.slope(), WithinAbs(kMicrosPerSample / kDrift, 1e-3));
}

TEST_CASE("before the line has points enough its slope is the nominal one", "[audio][timing]") {
    // One buffer is a point, and a regression through one point has a slope of nothing — every
    // hop of the first buffers would be stamped at the same instant. The nominal rate stands until
    // enough buffers are in to fit one.
    HostTimeFit fit(kMicrosPerSample);
    CHECK(fit.at(1000.0) == 0.0); // nothing observed: no host time, rather than a guess
    fit.observe(0.0, 5e9);
    CHECK_THAT(fit.at(441.0), WithinAbs(5e9 + 441.0 * kMicrosPerSample, 1e-3));
    fit.observe(1000.0, 5e9 + 1000.0 * kMicrosPerSample + 300.0);
    CHECK_THAT(fit.slope(), WithinAbs(kMicrosPerSample, 1e-12));
    fit.reset();
    CHECK(fit.points() == 0);
    CHECK(fit.at(0.0) == 0.0);
}

TEST_CASE("audio a device never delivered is counted as time lost", "[audio][timing]") {
    // 512 frames at 48 kHz, 10.67 ms apart, then a 40 ms dropout — the arrivals go on, the frames
    // do not — then 512-frame buffers again. The late buffer cannot tell a dropout from audio held
    // up in the driver; the one after it, on time, can.
    LostTime lost;
    constexpr double kRate = 48000.0;
    constexpr double kBuffer = 512.0 / kRate;
    std::int64_t now = 1'000'000'000;
    const auto arrive = [&](double after) {
        now += static_cast<std::int64_t>(std::llround(after * 1e9));
        return lost.onBuffer(512, kRate, now);
    };
    for (int i = 0; i < 20; ++i) {
        const LostTime::Reading reading = arrive(kBuffer);
        CHECK(reading.seconds == 0.0);
        CHECK_FALSE(reading.delayed);
    }
    const LostTime::Reading late = arrive(kBuffer + 0.040);
    CHECK(late.seconds == 0.0);
    CHECK(late.delayed);
    const LostTime::Reading confirmed = arrive(kBuffer);
    CHECK_THAT(confirmed.seconds, WithinAbs(0.040, 1e-6));
    CHECK_FALSE(confirmed.delayed);
    // Lost before the late buffer's first frame: twenty buffers had come before it.
    CHECK(confirmed.sinceFrame == 20u * 512u);
    for (int i = 0; i < 20; ++i) {
        CHECK_THAT(arrive(kBuffer).seconds, WithinAbs(0.040, 1e-6));
    }

    SECTION("a thread woken a little late is not a dropout") {
        CHECK_FALSE(arrive(kBuffer + 0.003).delayed);
        CHECK_THAT(arrive(kBuffer - 0.003).seconds, WithinAbs(0.040, 1e-6));
    }
    SECTION("a buffer held up and handed over with the next nets out to nothing") {
        CHECK(arrive(2.0 * kBuffer + 0.002).delayed);
        CHECK(arrive(0.0001).delayed); // 2 ms of it not made up yet
        const LostTime::Reading after = arrive(kBuffer);
        CHECK_THAT(after.seconds, WithinAbs(0.040, 1e-6)); // 2 ms is a late wake, not a gap
        CHECK_FALSE(after.delayed);
        CHECK(after.sinceFrame == 20u * 512u); // still the first gap's
    }
    SECTION("a buffer early with nothing owed gives nothing back") {
        CHECK_THAT(arrive(kBuffer - 0.009).seconds, WithinAbs(0.040, 1e-6));
        LostTime fresh;
        (void)fresh.onBuffer(512, kRate, 5'000'000'000);
        CHECK(fresh.onBuffer(512, kRate, 5'000'000'000 + 100'000).seconds == 0.0);
    }
    SECTION("a loopback paused for thirty seconds is thirty seconds lost") {
        CHECK(arrive(30.0).delayed);
        CHECK_THAT(arrive(kBuffer).seconds, WithinAbs(0.040 + 30.0 - kBuffer, 1e-6));
    }
    SECTION("a buffer of no frames says nothing") {
        const std::int64_t before = now;
        CHECK(lost.onBuffer(0, kRate, before + 50'000'000).seconds == lost.seconds());
        CHECK_FALSE(arrive(kBuffer).delayed); // measured from the buffer before it
    }
}

namespace {

/// A buffer of 480 frames at 48 kHz — WASAPI's 10 ms — arriving `ms` into the run.
LostTime::Reading at(LostTime& lost, double ms) {
    return lost.onBuffer(480, 48000.0,
                         1'000'000'000 + static_cast<std::int64_t>(std::llround(ms * 1e6)));
}

} // namespace

TEST_CASE("a thread held up and handed the buffers that waited for it has lost nothing",
          "[audio][timing]") {
    // The audio thread held up 25 ms: the buffer due at 30 ms comes at 55, the ones due at 40 and
    // 50 straight after it, and the one due at 60 on time. No frame was lost. Counted as lost at
    // once, and given back only as far as each early buffer made up, the stall stamped those
    // buffers 25, 15 and 5 ms late, and the last 5 ms stayed counted.
    LostTime lost;
    for (const double ms : {0.0, 10.0, 20.0}) {
        CHECK_FALSE(at(lost, ms).delayed);
    }
    for (const double ms : {55.0, 55.05, 55.1}) {
        INFO(ms << " ms");
        const LostTime::Reading reading = at(lost, ms);
        CHECK(reading.seconds == 0.0);
        CHECK(reading.delayed); // when it came says nothing of when it was heard
    }
    const LostTime::Reading caughtUp = at(lost, 60.0);
    CHECK(caughtUp.seconds == 0.0);
    CHECK_FALSE(caughtUp.delayed);
    for (int i = 1; i <= 50; ++i) {
        const LostTime::Reading reading = at(lost, 60.0 + 10.0 * i);
        CHECK(reading.seconds == 0.0);
        CHECK_FALSE(reading.delayed);
    }
}

TEST_CASE("a stall longer than the driver could hold loses what it could not", "[audio][timing]") {
    // The same stall, and a driver that holds two buffers: the one due at 30 ms was written over,
    // and the two held come at 55 and straight after, then the one due at 60 on time. 10 ms was
    // lost, before the first frame of the late buffer — 1440, three buffers in.
    LostTime lost;
    for (const double ms : {0.0, 10.0, 20.0}) {
        CHECK_FALSE(at(lost, ms).delayed);
    }
    CHECK(at(lost, 55.0).delayed);
    CHECK(at(lost, 55.05).delayed);
    CHECK(at(lost, 60.0).delayed); // 10 ms not made up yet: is it to come?
    const LostTime::Reading onTime = at(lost, 70.0);
    CHECK_THAT(onTime.seconds, WithinAbs(0.010, 1e-6));
    CHECK(onTime.sinceFrame == 1440u);
    CHECK_FALSE(onTime.delayed);
}

TEST_CASE("buffers of a millisecond caught up slowly after a stall have lost nothing",
          "[audio][timing]") {
    // 64 frames at 48 kHz, 1.33 ms each, the thread held up 25 ms, and then half a millisecond a
    // buffer to work through what waited — while more keep coming. Each is less than a
    // millisecond early, but most of a buffer, and the catch-up is fifty buffers long.
    LostTime lost;
    constexpr double kLength = 64.0 / 48000.0;
    double previous = -1.0;
    int delayed = 0;
    LostTime::Reading reading;
    for (int k = 0; k < 300; ++k) {
        const double ready = k * kLength;
        double handed = ready >= 0.020 && ready < 0.045 ? 0.045 : ready;
        if (previous >= 0.0) {
            handed = std::max(handed, previous + 0.0005);
        }
        reading = lost.onBuffer(
            64, 48000.0, 1'000'000'000 + static_cast<std::int64_t>(std::llround(handed * 1e9)));
        INFO("buffer " << k << " ready at " << ready * 1e3 << " ms, handed over at "
                       << handed * 1e3);
        CHECK(reading.seconds == 0.0);
        delayed += reading.delayed ? 1 : 0;
        previous = handed;
    }
    CHECK(delayed > 20);
    CHECK_FALSE(reading.delayed);
}
