#include "core/features/dimensions.hpp"
#include "core/features/intensity.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <vector>

using Catch::Approx;
using takt4::features::Intensity;
using takt4::features::IntensityClassifier;
using takt4::features::kFeatureDim;
using takt4::features::kNumBands;

namespace {

/// A feature frame whose difference half sums to `flux`, spread evenly across the bands.
/// The first half is the log bands themselves, which the classifier never reads — filled
/// with something non-zero so a test would notice if it started.
std::array<float, kFeatureDim> frameWithFlux(double flux) {
    std::array<float, kFeatureDim> frame{};
    const auto share = static_cast<float>(flux / static_cast<double>(kNumBands));
    for (std::size_t band = 0; band < kNumBands; ++band) {
        frame[band] = 3.0f;
        frame[kNumBands + band] = share;
    }
    return frame;
}

/// `count` frames at one flux. Returns how many of them were called onsets.
int hold(IntensityClassifier& classifier, double flux, int count) {
    const std::array<float, kFeatureDim> frame = frameWithFlux(flux);
    int onsets = 0;
    for (int i = 0; i < count; ++i) {
        onsets += classifier.push(frame) ? 1 : 0;
    }
    return onsets;
}

} // namespace

TEST_CASE("the flux is the difference half of the frame, summed", "[features][intensity]") {
    // The whole reason this is cheap: FeatureExtractor already computes
    // max(log10(1 + band) - previous, 0) for all 144 bands and puts it in the second half of
    // every frame. That *is* the positive spectral difference across the full band, so the
    // classifier is 144 additions rather than a second FFT.
    IntensityClassifier classifier;
    (void)classifier.push(frameWithFlux(12.0));
    CHECK(classifier.flux() == Approx(12.0));
    CHECK(classifier.frames() == 1);

    // And the log bands themselves are not part of it, however large they are.
    std::array<float, kFeatureDim> loud = frameWithFlux(3.0);
    for (std::size_t band = 0; band < kNumBands; ++band) {
        loud[band] = 100.0f;
    }
    (void)classifier.push(loud);
    CHECK(classifier.flux() == Approx(3.0));
}

TEST_CASE("nothing but normal is published until there is something to compare against",
          "[features][intensity]") {
    // §5.5's rule throughout: what is not known is published as nothing rather than as a
    // guess. Calling a track calm because it has only just started is a guess.
    IntensityClassifier classifier;
    CHECK(classifier.intensity() == Intensity::Normal);

    const IntensityClassifier::Options options = classifier.options();
    (void)hold(classifier, 10.0, static_cast<int>(options.settleFrames) - 2);
    (void)hold(classifier, 100.0, 1); // wildly over the intense threshold
    CHECK(classifier.intensity() == Intensity::Normal);

    SECTION("silence is normal too, not calm") {
        // A ratio of two near-zeroes is noise amplified into a classification.
        IntensityClassifier quiet;
        (void)hold(quiet, 0.0, 2000);
        CHECK(quiet.intensity() == Intensity::Normal);
        CHECK(quiet.ratio() == Approx(1.0));
    }
}

TEST_CASE("intensity is relative to the track, not to the gain", "[features][intensity]") {
    // Absolute flux is a fact about whoever mastered the track. Calm and intense are meant
    // to separate a breakdown from a drop *within* what is playing.
    IntensityClassifier::Options options;
    options.settleFrames = 20;
    options.slowFrames = 200;
    options.fastFrames = 10;
    options.holdFrames = 5;

    IntensityClassifier quiet(options);
    IntensityClassifier loud(options);
    // The same shape at a hundred times the level: a long steady passage, then a drop to
    // twice it, then away to a third of it.
    for (const double scale : {1.0, 100.0}) {
        IntensityClassifier& classifier = scale == 1.0 ? quiet : loud;
        (void)hold(classifier, 10.0 * scale, 600);
        CHECK(classifier.intensity() == Intensity::Normal);
        (void)hold(classifier, 25.0 * scale, 60);
        CHECK(classifier.intensity() == Intensity::Intense);
        (void)hold(classifier, 3.0 * scale, 120);
        CHECK(classifier.intensity() == Intensity::Calm);
    }

    SECTION("a uniformly busy track is normal throughout, which is the right answer") {
        // Nothing in it is more intense than the rest of it.
        IntensityClassifier steady(options);
        (void)hold(steady, 40.0, 4000);
        CHECK(steady.intensity() == Intensity::Normal);
    }
}

TEST_CASE("the hysteresis is what stops a passage on the threshold from flickering",
          "[features][intensity]") {
    // §5.8 asks for it by name: "classified calm / normal / intense with hysteresis to stop
    // it flickering between states".
    IntensityClassifier::Options options;
    options.settleFrames = 20;
    options.slowFrames = 400;
    options.fastFrames = 4;
    options.holdFrames = 1; // out of the way; this section is about the thresholds
    IntensityClassifier classifier(options);

    (void)hold(classifier, 10.0, 800);
    REQUIRE(classifier.intensity() == Intensity::Normal);

    // Over the entry threshold, so it goes intense.
    (void)hold(classifier, 10.0 * 1.4, 40);
    REQUIRE(classifier.intensity() == Intensity::Intense);

    // Back to between the two thresholds — below the 1.35 that let it in, above the 1.10
    // that would let it out. It stays.
    (void)hold(classifier, 10.0 * 1.2, 40);
    CHECK(classifier.intensity() == Intensity::Intense);

    // And below the inner one, so it leaves.
    (void)hold(classifier, 10.0 * 1.02, 40);
    CHECK(classifier.intensity() == Intensity::Normal);

    SECTION("a hold is a floor on how often a state changes, not a delay on every change") {
        // Threshold hysteresis alone still flickers when the follower crosses back and
        // forth; over references/audio the hold is worth 8.3 s to 13.0 s between changes on
        // the busiest track. See Options::fastFrames for the table.
        options.holdFrames = 500;
        IntensityClassifier sticky(options);
        (void)hold(sticky, 10.0, 800);
        REQUIRE(sticky.intensity() == Intensity::Normal);

        // A state that has stood for a long time changes the moment the threshold says so.
        // Making every change wait would be latency for its own sake.
        (void)hold(sticky, 10.0 * 1.6, 40);
        REQUIRE(sticky.intensity() == Intensity::Intense);

        // But now it owes the hold before it may change again, however far the flux moves.
        (void)hold(sticky, 1.0, 300);
        CHECK(sticky.intensity() == Intensity::Intense);
        (void)hold(sticky, 1.0, 300);
        CHECK(sticky.intensity() == Intensity::Calm);
    }

    SECTION("a hysteresis given backwards is made into one") {
        // These travel in a preset, and settings::load never fails, so an inverted gap has
        // to come out as a gap rather than as something worse than none.
        options.intenseAt = 1.2;
        options.intenseUntil = 1.9; // outside the entry threshold: nonsense
        options.calmAt = 0.8;
        options.calmUntil = 0.4; // likewise
        const IntensityClassifier fixed(options);
        CHECK(fixed.options().intenseUntil <= fixed.options().intenseAt);
        CHECK(fixed.options().calmUntil >= fixed.options().calmAt);
    }
}

TEST_CASE("an onset is a flux peak, and not two of them in a row", "[features][intensity]") {
    // §5.8's "on onset". A peak means: above the short average by the ratio, higher than
    // the frame before it, and not within the minimum gap of the last one.
    IntensityClassifier::Options options;
    options.fastFrames = 10;
    options.onsetGapFrames = 3;
    IntensityClassifier classifier(options);

    (void)hold(classifier, 5.0, 200);
    CHECK(classifier.onsets() == 0); // a steady level has no peaks in it

    CHECK(hold(classifier, 40.0, 1) == 1);
    // The frame straight after is not another onset, however loud: it is not higher than
    // the one before it, and the gap has not passed.
    CHECK(hold(classifier, 40.0, 1) == 0);
    CHECK(hold(classifier, 60.0, 1) == 0); // higher, but inside the gap
    CHECK(classifier.onsets() == 1);

    SECTION("hits far enough apart are counted separately") {
        IntensityClassifier drums(options);
        (void)hold(drums, 5.0, 200);
        int onsets = 0;
        for (int hit = 0; hit < 20; ++hit) {
            onsets += hold(drums, 5.0, 9);
            onsets += hold(drums, 40.0, 1);
        }
        CHECK(onsets == 20);
        CHECK(drums.onsets() == 20);
    }

    SECTION("silence has none") {
        IntensityClassifier quiet(options);
        (void)hold(quiet, 0.0, 500);
        CHECK(quiet.onsets() == 0);
    }
}

TEST_CASE("reset puts the classifier back where it started", "[features][intensity]") {
    IntensityClassifier classifier;
    (void)hold(classifier, 10.0, 400);
    (void)hold(classifier, 60.0, 200);
    REQUIRE(classifier.frames() == 600);

    classifier.reset();
    CHECK(classifier.frames() == 0);
    CHECK(classifier.onsets() == 0);
    CHECK(classifier.intensity() == Intensity::Normal);
    CHECK(classifier.flux() == Approx(0.0));
    CHECK(classifier.ratio() == Approx(1.0));
}

TEST_CASE("the wire numbers for intensity are fixed", "[features][intensity]") {
    // 5.6 of the handoff gives "/<app>/intensity  int  0 | 1 | 2", so these are the format
    // rather than an implementation detail — and trigger::Conditions indexes an array by
    // them, so the two cannot be allowed to drift apart.
    CHECK(static_cast<int>(Intensity::Calm) == 0);
    CHECK(static_cast<int>(Intensity::Normal) == 1);
    CHECK(static_cast<int>(Intensity::Intense) == 2);
    CHECK(takt4::features::labelOf(Intensity::Calm) == "calm");
    CHECK(takt4::features::labelOf(Intensity::Normal) == "normal");
    CHECK(takt4::features::labelOf(Intensity::Intense) == "intense");
}
