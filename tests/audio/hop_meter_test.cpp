#include "core/audio/hop_meter.hpp"

#include "core/audio/rates.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::audio::HopLevel;
using takt4::audio::HopMeter;
using takt4::audio::kHopSize;

TEST_CASE("HopMeter measures RMS and peak per hop", "[audio]") {
    HopMeter meter;
    std::vector<float> hop(kHopSize);

    SECTION("silence") {
        meter.processHop(hop.data(), 0);
        HopLevel level;
        REQUIRE(meter.pop(level));
        CHECK(level.hopIndex == 0);
        CHECK(level.rms == 0.0f);
        CHECK(level.peak == 0.0f);
        CHECK_FALSE(meter.pop(level));
    }

    SECTION("full-scale square wave") {
        for (std::size_t i = 0; i < kHopSize; ++i) {
            hop[i] = (i % 2 == 0) ? 1.0f : -1.0f;
        }
        meter.processHop(hop.data(), 5);
        HopLevel level;
        REQUIRE(meter.pop(level));
        CHECK(level.hopIndex == 5);
        CHECK_THAT(level.rms, WithinAbs(1.0, 1e-6));
        CHECK_THAT(level.peak, WithinAbs(1.0, 1e-6));
    }

    SECTION("sine of amplitude 0.5") {
        // Whole cycles fit in the hop (441 samples, 9 cycles), so the RMS is exactly A/√2.
        for (std::size_t i = 0; i < kHopSize; ++i) {
            hop[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 9.0 * static_cast<double>(i) /
                                                        static_cast<double>(kHopSize)));
        }
        meter.processHop(hop.data(), 1);
        HopLevel level;
        REQUIRE(meter.pop(level));
        CHECK_THAT(level.rms, WithinAbs(0.5 / std::numbers::sqrt2, 1e-4));
        CHECK_THAT(level.peak, WithinAbs(0.5, 1e-3));
    }
}

TEST_CASE("HopMeter counts hops it had to drop", "[audio]") {
    HopMeter meter;
    const std::vector<float> hop(kHopSize, 0.25f);
    for (std::uint64_t i = 0; i < HopMeter::kQueueCapacity + 3; ++i) {
        meter.processHop(hop.data(), i);
    }
    CHECK(meter.dropped() == 3);
    HopLevel level;
    std::uint64_t popped = 0;
    while (meter.pop(level)) {
        CHECK(level.hopIndex == popped); // the oldest survive; the newest were dropped
        ++popped;
    }
    CHECK(popped == HopMeter::kQueueCapacity);
}

TEST_CASE("HopMeter::processHop() does not touch the heap", "[audio][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    HopMeter meter;
    const std::vector<float> hop(kHopSize, 0.25f);
    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope realtime;
        for (std::uint64_t i = 0; i < 1000; ++i) {
            meter.processHop(hop.data(), i);
        }
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(takt4::rt::violationCount() == before);
}

TEST_CASE("toDbfs converts and floors", "[audio]") {
    CHECK_THAT(takt4::audio::toDbfs(1.0f), WithinAbs(0.0, 1e-5));
    CHECK_THAT(takt4::audio::toDbfs(0.5f), WithinAbs(-6.0206, 1e-3));
    CHECK_THAT(takt4::audio::toDbfs(0.001f), WithinAbs(-60.0, 1e-3));
    CHECK(takt4::audio::toDbfs(0.0f) == -120.0f);
    CHECK(takt4::audio::toDbfs(-1.0f) == -120.0f);
    CHECK(takt4::audio::toDbfs(1e-9f) == -120.0f);
}
