#include "core/audio/hop_accumulator.hpp"

#include "core/audio/rates.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <numeric>
#include <vector>

using takt4::audio::HopAccumulator;
using takt4::audio::kHopSize;

TEST_CASE("HopAccumulator hands out whole hops whatever the block size", "[audio]") {
    // Ramp 0, 1, 2, ... so every hop's contents identify exactly which samples it holds.
    constexpr std::size_t kTotal = 20000;
    std::vector<float> ramp(kTotal);
    std::iota(ramp.begin(), ramp.end(), 0.0f);

    for (const std::size_t block : {std::size_t{1}, std::size_t{7}, std::size_t{441}, std::size_t{442},
                                    std::size_t{1000}, std::size_t{4096}}) {
        INFO("block size " << block);
        HopAccumulator hops;
        REQUIRE(hops.hopSize() == kHopSize);
        std::size_t hopsSeen = 0;
        std::size_t wrongSamples = 0;
        for (std::size_t pos = 0; pos < kTotal; pos += block) {
            const std::size_t n = std::min(block, kTotal - pos);
            hops.push(ramp.data() + pos, n, [&](const float* hop) {
                for (std::size_t i = 0; i < kHopSize; ++i) {
                    if (hop[i] != static_cast<float>(hopsSeen * kHopSize + i)) {
                        ++wrongSamples;
                    }
                }
                ++hopsSeen;
            });
        }
        CHECK(hopsSeen == kTotal / kHopSize);
        CHECK(wrongSamples == 0);
        CHECK(hops.pending() == kTotal % kHopSize);
    }
}

TEST_CASE("HopAccumulator reset() drops a partial hop", "[audio]") {
    HopAccumulator hops(10);
    const std::vector<float> data(7, 1.0f);
    int seen = 0;
    hops.push(data.data(), 7, [&](const float*) { ++seen; });
    CHECK(hops.pending() == 7);
    hops.reset();
    CHECK(hops.pending() == 0);
    hops.push(data.data(), 7, [&](const float*) { ++seen; });
    CHECK(seen == 0);
    hops.push(data.data(), 3, [&](const float*) { ++seen; });
    CHECK(seen == 1);
    CHECK(hops.pending() == 0);
}

TEST_CASE("HopAccumulator::push() does not touch the heap", "[audio][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    HopAccumulator hops;
    const std::vector<float> data(5000, 0.1f);
    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    int seen = 0;
    {
        const takt4::rt::RealtimeScope realtime;
        hops.push(data.data(), data.size(), [&](const float*) { ++seen; });
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(seen == 11);
    CHECK(takt4::rt::violationCount() == before);
}
