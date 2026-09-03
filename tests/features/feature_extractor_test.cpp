#include "core/features/feature_extractor.hpp"

#include "core/audio/rates.hpp"
#include "core/features/dimensions.hpp"
#include "core/io/npy_file.hpp"
#include "core/io/wav_file.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::audio::kHopSize;
using takt4::features::FeatureExtractor;
using takt4::features::kFeatureDim;
using takt4::features::kFrameSize;
using takt4::features::kNumBands;

namespace {

using Frame = FeatureExtractor::Frame;

// The whole signal through the extractor the way the CLI and the tests use it: zero-pad
// to whole hops, push them, flush. Gives madmom's ⌈N / 441⌉ frames.
std::vector<Frame> extract(FeatureExtractor& extractor, std::span<const float> signal) {
    const std::size_t hops = (signal.size() + kHopSize - 1) / kHopSize;
    std::vector<float> padded(hops * kHopSize, 0.0f);
    std::copy(signal.begin(), signal.end(), padded.begin());
    std::vector<Frame> frames;
    for (std::size_t h = 0; h < hops; ++h) {
        const std::span<const float, kHopSize> hop(padded.data() + h * kHopSize, kHopSize);
        if (extractor.pushHop(hop)) {
            frames.push_back(extractor.frame());
        }
    }
    if (extractor.flush()) {
        frames.push_back(extractor.frame());
    }
    return frames;
}

std::vector<Frame> extract(std::span<const float> signal) {
    FeatureExtractor extractor;
    return extract(extractor, signal);
}

// numpy.hanning(kFrameSize)[n], the same way feature_extractor.cpp computes it.
double hann(std::size_t n) {
    const double m = static_cast<double>(kFrameSize);
    return 0.5 + 0.5 * std::cos(std::numbers::pi * (static_cast<double>(2 * n + 1) - m) / (m - 1.0));
}

std::vector<float> noise(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> x(n);
    for (float& v : x) {
        v = dist(rng);
    }
    return x;
}

bool allZero(const Frame& frame) {
    return std::all_of(frame.begin(), frame.end(), [](float v) { return v == 0.0f; });
}

std::vector<std::filesystem::path> goldenExcerpts() {
    const std::filesystem::path dir = std::filesystem::path(TAKT4_TEST_DATA_DIR) / "features";
    std::vector<std::filesystem::path> wavs;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".wav") {
            wavs.push_back(entry.path());
        }
    }
    std::sort(wavs.begin(), wavs.end());
    return wavs;
}

} // namespace

// HANDOFF §8 Phase 2, the gate: the same audio through madmom (tools/make_golden.py,
// the .npy next to each .wav) and through this code, within floating-point tolerance,
// on at least ten varied excerpts.
TEST_CASE("the C++ features match madmom's on every golden excerpt", "[features][golden]") {
    constexpr double kTolerance = 1e-5; // madmom's pipeline is float32 from the FFT on; this one is double
    constexpr std::size_t kRequiredExcerpts = 10;

    const std::vector<std::filesystem::path> wavs = goldenExcerpts();
    REQUIRE_FALSE(wavs.empty());
    if (wavs.size() < kRequiredExcerpts) {
        WARN("only " << wavs.size() << " golden excerpt(s) in tests/data/features; the Phase 2 exit "
                     << "criterion needs at least " << kRequiredExcerpts << " varied ones (tools/make_golden.py)");
    }

    for (const std::filesystem::path& wav : wavs) {
        INFO("excerpt " << wav.filename().string());
        const takt4::io::WavData audio = takt4::io::readWavFile(wav);
        REQUIRE(audio.channels == 1);
        REQUIRE(audio.sampleRate == 22050);
        REQUIRE(audio.frames() > 0);

        std::filesystem::path npy = wav;
        npy.replace_extension(".npy");
        const takt4::io::NpyMatrix golden = takt4::io::readNpyFloat32(npy);
        REQUIRE(golden.cols == kFeatureDim);
        REQUIRE(golden.rows == (audio.frames() + kHopSize - 1) / kHopSize);

        const std::vector<Frame> frames = extract(audio.samples);
        REQUIRE(frames.size() == golden.rows);

        double worst = 0.0;
        std::size_t worstFrame = 0;
        std::size_t worstColumn = 0;
        for (std::size_t f = 0; f < frames.size(); ++f) {
            for (std::size_t c = 0; c < kFeatureDim; ++c) {
                const double diff = std::abs(static_cast<double>(frames[f][c]) - static_cast<double>(golden.at(f, c)));
                if (diff > worst) {
                    worst = diff;
                    worstFrame = f;
                    worstColumn = c;
                }
            }
        }
        INFO("largest difference " << worst << " at frame " << worstFrame << ", column " << worstColumn << " (C++ "
                                   << frames[worstFrame][worstColumn] << ", madmom "
                                   << golden.at(worstFrame, worstColumn) << ")");
        CHECK(worst <= kTolerance);
    }
}

TEST_CASE("FeatureExtractor delivers frame j - 1 on hop j and the last frame on flush", "[features]") {
    STATIC_CHECK(FeatureExtractor::kLatencyHops == 1);
    FeatureExtractor extractor;
    const std::vector<float> hop(kHopSize, 0.1f);
    const std::span<const float, kHopSize> span(hop.data(), kHopSize);

    CHECK(extractor.hopsPushed() == 0);
    CHECK_FALSE(extractor.pushHop(span)); // frame 0 needs hop 1 as well
    CHECK(extractor.hopsPushed() == 1);
    CHECK(extractor.pushHop(span));
    CHECK(extractor.frameIndex() == 0);
    CHECK(extractor.pushHop(span));
    CHECK(extractor.frameIndex() == 1);
    CHECK(extractor.flush());
    CHECK(extractor.frameIndex() == 2);
    CHECK(extractor.hopsPushed() == 4);

    SECTION("a signal of N hops gives N frames, however many that is") {
        for (std::size_t hops : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{50}}) {
            const std::vector<float> signal(hops * kHopSize, 0.2f);
            CHECK(extract(signal).size() == hops);
        }
        // Partial hops are padded up: 441 · 3 + 1 samples is 4 of madmom's frames.
        CHECK(extract(std::vector<float>(3 * kHopSize + 1, 0.2f)).size() == 4);
    }
}

TEST_CASE("silence gives exactly zero features", "[features]") {
    const std::vector<float> silence(20 * kHopSize, 0.0f);
    const std::vector<Frame> frames = extract(silence);
    REQUIRE(frames.size() == 20);
    for (const Frame& frame : frames) {
        CHECK(allZero(frame));
    }
}

TEST_CASE("an impulse lands in the frames madmom's centred framing puts it in", "[features]") {
    // Frame k is centred on sample 441 k, so an impulse there is at the window's peak in
    // frame k, a quarter in from either edge in frames k ± 1, and exactly on the zero at
    // the window's first sample in frame k + 2. Frames k − 2 and earlier never see it.
    constexpr std::size_t k = 5;
    constexpr float amplitude = 0.5f;
    std::vector<float> signal(12 * kHopSize, 0.0f);
    signal[k * kHopSize] = amplitude;
    const std::vector<Frame> frames = extract(signal);
    REQUIRE(frames.size() == 12);

    for (std::size_t f = 0; f < frames.size(); ++f) {
        INFO("frame " << f);
        if (f >= k - 1 && f <= k + 1) {
            CHECK_FALSE(allZero(frames[f]));
        } else {
            CHECK(allZero(frames[f]));
        }
    }

    // A lone impulse has a flat magnitude spectrum, so every band (weights summing to
    // one) sees the windowed amplitude itself.
    const auto expectedLog = [&](std::size_t offsetInFrame) {
        return std::log10(1.0 + static_cast<double>(amplitude) * hann(offsetInFrame));
    };
    for (std::size_t b = 0; b < kNumBands; ++b) {
        INFO("band " << b);
        CHECK_THAT(frames[k][b], WithinAbs(expectedLog(kFrameSize / 2), 1e-5));
        CHECK_THAT(frames[k - 1][b], WithinAbs(expectedLog(3 * kFrameSize / 4), 1e-5));
        CHECK_THAT(frames[k + 1][b], WithinAbs(expectedLog(kFrameSize / 4), 1e-5));
        // The differences: up from silence, up to the peak, then clipped at zero.
        CHECK(frames[k - 1][kNumBands + b] == frames[k - 1][b]);
        CHECK_THAT(frames[k][kNumBands + b], WithinAbs(static_cast<double>(frames[k][b] - frames[k - 1][b]), 1e-6));
        CHECK(frames[k + 1][kNumBands + b] == 0.0f);
    }
}

TEST_CASE("the first frame's difference is zero, as madmom's is", "[features]") {
    const std::vector<float> signal = noise(6 * kHopSize, 7u);
    const std::vector<Frame> frames = extract(signal);
    REQUIRE(frames.size() == 6);
    for (std::size_t b = 0; b < kNumBands; ++b) {
        CHECK(frames[0][b] > 0.0f);
        CHECK(frames[0][kNumBands + b] == 0.0f);
    }
    // Later differences are the clipped first differences of the log bands.
    for (std::size_t f = 1; f < frames.size(); ++f) {
        for (std::size_t b = 0; b < kNumBands; ++b) {
            const float expected = std::max(frames[f][b] - frames[f - 1][b], 0.0f);
            CHECK_THAT(frames[f][kNumBands + b], WithinAbs(static_cast<double>(expected), 1e-6));
        }
    }
}

TEST_CASE("reset() forgets the past completely", "[features]") {
    const std::vector<float> first = noise(9 * kHopSize, 1u);
    const std::vector<float> second = noise(9 * kHopSize, 2u);

    FeatureExtractor extractor;
    (void)extract(extractor, first);
    extractor.reset();
    CHECK(extractor.hopsPushed() == 0);
    CHECK(extractor.frameIndex() == 0);
    CHECK(allZero(extractor.frame()));

    const std::vector<Frame> reused = extract(extractor, second);
    const std::vector<Frame> fresh = extract(second);
    REQUIRE(reused.size() == fresh.size());
    for (std::size_t f = 0; f < fresh.size(); ++f) {
        CHECK(reused[f] == fresh[f]);
    }
}

TEST_CASE("FeatureExtractor::pushHop() and flush() do not touch the heap", "[features][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    FeatureExtractor extractor;
    const std::vector<float> signal = noise(50 * kHopSize, 3u);
    std::array<bool, 51> delivered{};

    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope realtime;
        for (std::size_t h = 0; h < 50; ++h) {
            const std::span<const float, kHopSize> hop(signal.data() + h * kHopSize, kHopSize);
            delivered[h] = extractor.pushHop(hop);
        }
        delivered[50] = extractor.flush();
        extractor.reset();
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(takt4::rt::violationCount() == before);
    CHECK_FALSE(delivered[0]);
    CHECK(std::all_of(delivered.begin() + 1, delivered.end(), [](bool b) { return b; }));
}
