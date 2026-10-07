#include "core/audio/host_time_fit.hpp"
#include "core/audio/lost_time.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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
    // do not — then 512-frame buffers again.
    LostTime lost;
    constexpr double kRate = 48000.0;
    constexpr double kBuffer = 512.0 / kRate;
    std::int64_t now = 1'000'000'000;
    const auto arrive = [&](double after) {
        now += static_cast<std::int64_t>(std::llround(after * 1e9));
        return lost.onBuffer(512, kRate, now);
    };
    for (int i = 0; i < 20; ++i) {
        CHECK(arrive(kBuffer) == 0.0);
    }
    CHECK_THAT(arrive(kBuffer + 0.040), WithinAbs(0.040, 1e-6));
    for (int i = 0; i < 20; ++i) {
        CHECK_THAT(arrive(kBuffer), WithinAbs(0.040, 1e-6));
    }

    SECTION("a thread woken a little late is not a dropout") {
        CHECK_THAT(arrive(kBuffer + 0.003), WithinAbs(0.040, 1e-6));
        CHECK_THAT(arrive(kBuffer - 0.003), WithinAbs(0.040, 1e-6));
    }
    SECTION("a buffer held up and handed over with the next nets out to nothing") {
        CHECK_THAT(arrive(2.0 * kBuffer + 0.002), WithinAbs(0.040 + kBuffer + 0.002, 1e-6));
        CHECK_THAT(arrive(0.0001), WithinAbs(0.040 + 0.0021, 1e-4));
        // And never below none: a buffer early with nothing owed gives nothing back.
        LostTime fresh;
        (void)fresh.onBuffer(512, kRate, 5'000'000'000);
        CHECK(fresh.onBuffer(512, kRate, 5'000'000'000 + 100'000) == 0.0);
    }
    SECTION("a loopback paused for thirty seconds is thirty seconds lost") {
        CHECK_THAT(arrive(30.0), WithinAbs(0.040 + 30.0 - kBuffer, 1e-6));
    }
}
