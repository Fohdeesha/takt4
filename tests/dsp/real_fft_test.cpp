#include "core/dsp/real_fft.hpp"

#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <random>
#include <stdexcept>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::dsp::RealFft;

namespace {

// The definition, term by term: X[k] = Σ x[n] e^(-2πikn/N).
std::vector<std::complex<double>> naiveDft(const std::vector<double>& x) {
    const std::size_t n = x.size();
    std::vector<std::complex<double>> out(n / 2 + 1);
    for (std::size_t k = 0; k < out.size(); ++k) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(k * i % n) / static_cast<double>(n);
            re += x[i] * std::cos(angle);
            im += x[i] * std::sin(angle);
        }
        out[k] = {re, im};
    }
    return out;
}

std::vector<double> noise(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::vector<double> x(n);
    for (double& v : x) {
        v = dist(rng);
    }
    return x;
}

} // namespace

TEST_CASE("RealFft matches the DFT for every factorisation KissFFT has a path for", "[dsp]") {
    // 2, 3, 4 and 5 have dedicated butterflies; 7 and 11 go through the generic one.
    // 1764 = 2² 3² 7² is the frame size.
    const std::size_t size = GENERATE(Catch::Generators::as<std::size_t>{}, 2, 4, 6, 8, 10, 12, 14, 22, 30, 98, 210, 1764);
    RealFft fft(size);
    CHECK(fft.size() == size);
    CHECK(fft.numBins() == size / 2 + 1);

    const std::vector<double> x = noise(size, 1234u + static_cast<unsigned>(size));
    const std::vector<std::complex<double>> expected = naiveDft(x);
    std::vector<std::complex<double>> got(fft.numBins());
    fft.forward(x, got);

    double maxError = 0.0;
    for (std::size_t k = 0; k < got.size(); ++k) {
        maxError = std::max(maxError, std::abs(got[k] - expected[k]));
    }
    INFO("size " << size << ", max |error| " << maxError);
    CHECK(maxError < 1e-9 * static_cast<double>(size));
    // The half spectrum of a real signal has real DC and Nyquist bins.
    CHECK(got.front().imag() == 0.0);
    CHECK(got.back().imag() == 0.0);
}

TEST_CASE("RealFft transforms the textbook signals exactly", "[dsp]") {
    RealFft fft(16);
    std::vector<double> x(16, 0.0);
    std::vector<std::complex<double>> spectrum(fft.numBins());

    SECTION("an impulse at zero is flat") {
        x[0] = 1.0;
        fft.forward(x, spectrum);
        for (const auto& bin : spectrum) {
            CHECK_THAT(bin.real(), WithinAbs(1.0, 1e-15));
            CHECK_THAT(bin.imag(), WithinAbs(0.0, 1e-15));
        }
    }

    SECTION("a delayed impulse is a pure phase ramp") {
        x[1] = 1.0;
        fft.forward(x, spectrum);
        for (std::size_t k = 0; k < spectrum.size(); ++k) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(k) / 16.0;
            CHECK_THAT(spectrum[k].real(), WithinAbs(std::cos(angle), 1e-14));
            CHECK_THAT(spectrum[k].imag(), WithinAbs(std::sin(angle), 1e-14));
        }
    }

    SECTION("a constant is all DC, unnormalised") {
        std::fill(x.begin(), x.end(), 0.25);
        fft.forward(x, spectrum);
        CHECK_THAT(spectrum[0].real(), WithinAbs(4.0, 1e-14));
        for (std::size_t k = 1; k < spectrum.size(); ++k) {
            CHECK_THAT(std::abs(spectrum[k]), WithinAbs(0.0, 1e-14));
        }
    }

    SECTION("the smallest transform") {
        RealFft two(2);
        std::vector<double> pair = {1.0, -3.0};
        std::vector<std::complex<double>> bins(2);
        two.forward(pair, bins);
        CHECK(bins[0] == std::complex<double>(-2.0, 0.0));
        CHECK(bins[1] == std::complex<double>(4.0, 0.0));
    }
}

TEST_CASE("RealFft rejects sizes it cannot transform", "[dsp]") {
    CHECK_THROWS_AS(RealFft(0), std::invalid_argument);
    CHECK_THROWS_AS(RealFft(1), std::invalid_argument);
    CHECK_THROWS_AS(RealFft(3), std::invalid_argument);
    CHECK_THROWS_AS(RealFft(1763), std::invalid_argument);
}

TEST_CASE("RealFft::forward() does not touch the heap", "[dsp][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    RealFft fft(1764);
    const std::vector<double> x = noise(1764, 99u);
    std::vector<std::complex<double>> spectrum(fft.numBins());

    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope realtime;
        for (int i = 0; i < 100; ++i) {
            fft.forward(x, spectrum);
        }
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(takt4::rt::violationCount() == before);
}
