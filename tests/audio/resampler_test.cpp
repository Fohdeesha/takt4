#include "core/audio/resampler.hpp"

#include "core/audio/rates.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::audio::kInternalSampleRate;
using takt4::audio::Resampler;

namespace {

std::vector<float> sine(double frequency, double rate, std::size_t frames, float amplitude = 0.5f) {
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency *
                                                         static_cast<double>(i) / rate));
    }
    return out;
}

std::vector<float> noise(std::size_t frames, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.9f, 0.9f);
    std::vector<float> out(frames);
    for (float& x : out) {
        x = dist(rng);
    }
    return out;
}

// Runs the whole signal through in blocks of `block` frames and collects the output.
std::vector<float> run(Resampler& resampler, const std::vector<float>& input, std::size_t block) {
    std::vector<float> out;
    out.reserve(input.size());
    for (std::size_t pos = 0; pos < input.size(); pos += block) {
        const std::size_t n = std::min(block, input.size() - pos);
        resampler.process(input.data() + pos, n, [&](const float* samples, std::size_t count) {
            out.insert(out.end(), samples, samples + count);
        });
    }
    return out;
}

} // namespace

TEST_CASE("Resampler reports the r8brain version it was built with", "[audio]") {
    CHECK(std::string(Resampler::libraryVersion()) == "7.5");
}

TEST_CASE("Resampler rejects bad parameters", "[audio]") {
    CHECK_THROWS_AS(Resampler(0.0, 22050.0), std::invalid_argument);
    CHECK_THROWS_AS(Resampler(48000.0, -1.0), std::invalid_argument);
    CHECK_THROWS_AS(Resampler(48000.0, 22050.0, 0), std::invalid_argument);
}

TEST_CASE("Resampler output is time-aligned to its input", "[audio]") {
    // Output sample k must equal the input signal at time k / outputRate: r8brain
    // compensates its filter delay, so hop indices map to device time without an
    // offset (HANDOFF §4.3). The first stretch is skipped because the sine switches on
    // abruptly at t = 0 and that step is not band-limited.
    const double inputRate = GENERATE_COPY(44100.0, 48000.0, 96000.0);
    INFO("input rate " << inputRate);
    constexpr double kFrequency = 1000.0;
    Resampler resampler(inputRate, kInternalSampleRate);
    const std::vector<float> input = sine(kFrequency, inputRate, static_cast<std::size_t>(inputRate) * 2);
    const std::vector<float> output = run(resampler, input, 480);

    REQUIRE(output.size() > 30000);
    const std::size_t skip = 4000;
    double worst = 0.0;
    for (std::size_t k = skip; k < output.size(); ++k) {
        const double expected = 0.5 * std::sin(2.0 * std::numbers::pi * kFrequency *
                                               static_cast<double>(k) / kInternalSampleRate);
        worst = std::max(worst, std::fabs(static_cast<double>(output[k]) - expected));
    }
    INFO("worst deviation from the ideal sine: " << worst);
    CHECK(worst < 1e-3);
}

TEST_CASE("Resampler passes DC at unity gain", "[audio]") {
    Resampler resampler(48000.0, kInternalSampleRate);
    const std::vector<float> input(96000, 1.0f);
    const std::vector<float> output = run(resampler, input, 1024);
    REQUIRE(output.size() > 20000);
    for (std::size_t k = 4000; k < output.size(); ++k) {
        REQUIRE_THAT(output[k], WithinAbs(1.0, 1e-4));
    }
}

TEST_CASE("Resampler passes the band and removes what would alias", "[audio]") {
    // 48 kHz input. An 8 kHz tone sits inside the passband and must come through at
    // level; a 15 kHz tone lies above the 22050 Hz Nyquist and would fold down to
    // 7050 Hz without the anti-aliasing filter, so next to nothing of it may remain.
    constexpr double kInputRate = 48000.0;
    constexpr std::size_t kFrames = 48000;
    const auto rmsAfter = [](const std::vector<float>& samples, std::size_t skip) {
        double sum = 0.0;
        for (std::size_t k = skip; k < samples.size(); ++k) {
            sum += static_cast<double>(samples[k]) * static_cast<double>(samples[k]);
        }
        return std::sqrt(sum / static_cast<double>(samples.size() - skip));
    };

    Resampler inBand(kInputRate, kInternalSampleRate);
    const std::vector<float> passed = run(inBand, sine(8000.0, kInputRate, kFrames), 512);
    REQUIRE(passed.size() > 10000);
    CHECK_THAT(rmsAfter(passed, 4000), WithinAbs(0.5 / std::numbers::sqrt2, 1e-3));

    Resampler aboveNyquist(kInputRate, kInternalSampleRate);
    const std::vector<float> removed = run(aboveNyquist, sine(15000.0, kInputRate, kFrames), 512);
    REQUIRE(removed.size() > 10000);
    CHECK(rmsAfter(removed, 4000) < 1e-6);
}

TEST_CASE("Resampler produces the right number of samples", "[audio]") {
    const double inputRate = GENERATE_COPY(44100.0, 48000.0, 96000.0);
    Resampler resampler(inputRate, kInternalSampleRate);
    const std::size_t frames = static_cast<std::size_t>(inputRate) * 3;
    const std::vector<float> output = run(resampler, noise(frames, 1), 333);

    // Everything but the held-back delay must have come out.
    const double expected = (static_cast<double>(frames) - static_cast<double>(resampler.inputDelayFrames())) *
                            kInternalSampleRate / inputRate;
    INFO("input rate " << inputRate << ": " << output.size() << " out, expected about " << expected);
    CHECK(std::fabs(static_cast<double>(output.size()) - expected) <= 2.0);
}

TEST_CASE("Resampler output does not depend on how the input is blocked", "[audio]") {
    // HANDOFF §5.1: never assume the host block size. The same signal fed in blocks of
    // 1, 7, 441, 1000 and 4096 frames must come out the same.
    const std::vector<float> input = noise(48000 * 2, 2);
    Resampler reference(48000.0, kInternalSampleRate);
    const std::vector<float> expected = run(reference, input, 480);

    for (const std::size_t block : {std::size_t{1}, std::size_t{7}, std::size_t{441}, std::size_t{1000},
                                    std::size_t{4096}}) {
        INFO("block size " << block);
        Resampler resampler(48000.0, kInternalSampleRate);
        const std::vector<float> output = run(resampler, input, block);
        REQUIRE(output.size() == expected.size());
        std::size_t mismatches = 0;
        for (std::size_t k = 0; k < output.size(); ++k) {
            if (output[k] != expected[k]) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }
}

TEST_CASE("Resampler's reported delay is what it actually holds back", "[audio]") {
    const double inputRate = GENERATE_COPY(44100.0, 48000.0, 96000.0);
    Resampler resampler(inputRate, kInternalSampleRate);
    const std::size_t delay = resampler.inputDelayFrames();

    // Feed one sample at a time; the first call that yields output is the one after
    // `delay` silent ones.
    std::size_t fed = 0;
    std::size_t firstOutputAt = 0;
    const float zero = 0.0f;
    while (firstOutputAt == 0 && fed < 100000) {
        std::size_t produced = 0;
        resampler.process(&zero, 1, [&](const float*, std::size_t count) { produced += count; });
        ++fed;
        if (produced > 0) {
            firstOutputAt = fed;
        }
    }
    std::ostringstream note;
    note << "resampler delay " << inputRate << " -> " << kInternalSampleRate << " Hz: " << delay << " frames = "
         << 1000.0 * static_cast<double>(delay) / inputRate << " ms";
    WARN(note.str());
    CHECK(firstOutputAt == delay + 1);
    CHECK(delay > 0);
    // The budget behind the filter settings in resampler.cpp; a "sharper" filter would
    // silently push the whole tracker later.
    CHECK(static_cast<double>(delay) < inputRate * 0.025); // under 25 ms
}

TEST_CASE("Resampler at the internal rate is a pass-through", "[audio]") {
    Resampler resampler(kInternalSampleRate, kInternalSampleRate);
    CHECK(resampler.inputDelayFrames() == 0);
    const std::vector<float> input = noise(5000, 3);
    const std::vector<float> output = run(resampler, input, 300);
    REQUIRE(output.size() == input.size());
    CHECK(output == input);
}

TEST_CASE("Resampler::reset() forgets history", "[audio]") {
    Resampler resampler(48000.0, kInternalSampleRate);
    const std::vector<float> input = noise(48000, 4);
    const std::vector<float> first = run(resampler, input, 512);
    resampler.reset();
    const std::vector<float> second = run(resampler, input, 512);
    CHECK(first == second);
}

TEST_CASE("Resampler::process() does not touch the heap", "[audio][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    const double inputRate = GENERATE_COPY(44100.0, 48000.0, 96000.0, kInternalSampleRate);
    Resampler resampler(inputRate, kInternalSampleRate);
    const std::vector<float> input = noise(20000, 5);
    std::vector<float> sink(resampler.maxOutputPerChunk());

    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    std::size_t produced = 0;
    std::size_t oversized = 0;
    {
        const takt4::rt::RealtimeScope realtime;
        for (const std::size_t block : {std::size_t{1}, std::size_t{7}, std::size_t{441}, std::size_t{4096}}) {
            for (std::size_t pos = 0; pos < input.size(); pos += block) {
                const std::size_t n = std::min(block, input.size() - pos);
                resampler.process(input.data() + pos, n, [&](const float* samples, std::size_t count) {
                    if (count > sink.size()) {
                        ++oversized;
                    } else {
                        std::copy_n(samples, count, sink.data());
                    }
                    produced += count;
                });
            }
        }
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(takt4::rt::violationCount() == before);
    CHECK(oversized == 0);
    CHECK(produced > 0);
}
