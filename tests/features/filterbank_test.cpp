#include "core/features/filterbank.hpp"

#include "core/features/dimensions.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::features::applyFilterbank;
using takt4::features::FilterBand;
using takt4::features::filterbankBands;
using takt4::features::filterbankWeights;
using takt4::features::kNumBands;
using takt4::features::kNumBins;

// What tools/dump_filterbank.py checked on the Python side, checked again on the table
// it produced: the structure of madmom's LogarithmicFilterbank(num_bands=24, fmin=30,
// fmax=17000, norm_filters=True, unique_filters=True) over 882 bins of 12.5 Hz.

TEST_CASE("the filterbank table is madmom's, structurally", "[features]") {
    const auto bands = filterbankBands();
    const auto weights = filterbankWeights();
    REQUIRE(bands.size() == kNumBands);
    REQUIRE(weights.size() == 1601); // the nonzeros of the (882 × 144) matrix

    SECTION("the weights are stored band after band, without gaps") {
        std::size_t offset = 0;
        for (const FilterBand& band : bands) {
            CHECK(band.weightOffset == offset);
            CHECK(band.numBins >= 1);
            offset += band.numBins;
        }
        CHECK(offset == weights.size());
    }

    SECTION("the bands climb from 37.5 Hz to 10862.5 Hz and use bins 3 to 880") {
        CHECK(bands.front().firstBin == 3);
        CHECK(bands.front().numBins == 1);
        CHECK(bands.back().firstBin + bands.back().numBins == 881); // bin 881, Nyquist − 12.5 Hz, is unused
        std::size_t widest = 0;
        for (std::size_t b = 1; b < bands.size(); ++b) {
            CHECK(bands[b].firstBin >= bands[b - 1].firstBin);
            CHECK(bands[b].firstBin + bands[b].numBins > bands[b - 1].firstBin + bands[b - 1].numBins);
            widest = std::max<std::size_t>(widest, bands[b].numBins);
        }
        CHECK(widest == 48);
        // 17 kHz is above Nyquist, so the top triangle (centre 10862.5 Hz, bin 869) is
        // cut off where the spectrum ends instead of reaching the next centre.
        CHECK(bands.back().firstBin == 845);
        CHECK(bands.back().numBins == 36);
    }

    SECTION("every band is a triangle whose weights sum to one") {
        for (std::size_t b = 0; b < bands.size(); ++b) {
            const FilterBand& band = bands[b];
            double sum = 0.0;
            std::size_t peak = 0;
            for (std::size_t i = 0; i < band.numBins; ++i) {
                const float w = weights[band.weightOffset + i];
                CHECK(w > 0.0f);
                CHECK(w <= 1.0f);
                sum += static_cast<double>(w);
                if (w > weights[band.weightOffset + peak]) {
                    peak = i;
                }
            }
            INFO("band " << b);
            CHECK_THAT(sum, WithinAbs(1.0, 1e-5)); // float32 weights, normalised in Python
            for (std::size_t i = 1; i < band.numBins; ++i) { // rising to the peak, falling after
                const float previous = weights[band.weightOffset + i - 1];
                const float current = weights[band.weightOffset + i];
                CHECK((i <= peak ? current >= previous : current <= previous));
            }
        }
    }
}

TEST_CASE("applyFilterbank() is the dense matrix product", "[features]") {
    // Rebuild the (882 × 144) matrix from the table and multiply the long way round.
    const auto bands = filterbankBands();
    const auto weights = filterbankWeights();
    std::vector<double> dense(kNumBins * kNumBands, 0.0);
    for (std::size_t b = 0; b < bands.size(); ++b) {
        for (std::size_t i = 0; i < bands[b].numBins; ++i) {
            dense[(bands[b].firstBin + i) * kNumBands + b] = static_cast<double>(weights[bands[b].weightOffset + i]);
        }
    }

    std::array<double, kNumBins> magnitudes{};
    std::mt19937 rng(20260903u);
    std::uniform_real_distribution<double> dist(0.0, 100.0);
    for (double& m : magnitudes) {
        m = dist(rng);
    }

    std::array<double, kNumBands> expected{};
    for (std::size_t k = 0; k < kNumBins; ++k) {
        for (std::size_t b = 0; b < kNumBands; ++b) {
            expected[b] += magnitudes[k] * dense[k * kNumBands + b];
        }
    }

    std::array<double, kNumBands> got{};
    applyFilterbank(magnitudes, got);
    for (std::size_t b = 0; b < kNumBands; ++b) {
        INFO("band " << b);
        CHECK_THAT(got[b], WithinAbs(expected[b], 1e-9));
    }

    SECTION("a flat spectrum comes out as the column sums") {
        magnitudes.fill(2.0);
        applyFilterbank(magnitudes, got);
        for (std::size_t b = 0; b < kNumBands; ++b) {
            CHECK_THAT(got[b], WithinAbs(2.0, 2e-5));
        }
    }
}

TEST_CASE("applyFilterbank() does not touch the heap", "[features][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    std::array<double, kNumBins> magnitudes{};
    magnitudes.fill(1.0);
    std::array<double, kNumBands> bands{};

    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope realtime;
        for (int i = 0; i < 100; ++i) {
            applyFilterbank(magnitudes, bands);
        }
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(takt4::rt::violationCount() == before);
}
