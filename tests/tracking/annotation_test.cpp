#include "core/tracking/annotation.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

using Catch::Approx;
using takt4::tracking::Annotation;
using takt4::tracking::AnnotationOptions;
using takt4::tracking::annotate;
using takt4::tracking::Tap;

namespace {

/// A 50 Hz activation, quiet everywhere but for a three-frame peak of `height` at each of
/// `seconds` — the shape the network's beat class takes on a clean beat.
std::vector<float> activationWithPeaks(double lengthSeconds, const std::vector<double>& seconds,
                                       float height = 0.9f) {
    std::vector<float> act(static_cast<std::size_t>(lengthSeconds * 50.0), 0.02f);
    for (const double s : seconds) {
        const std::size_t f = static_cast<std::size_t>(std::lround(s * 50.0));
        if (f >= 1 && f + 1 < act.size()) {
            act[f - 1] = height * 0.3f;
            act[f] = height;
            act[f + 1] = height * 0.3f;
        }
    }
    return act;
}

std::vector<Tap> tapsAt(const std::vector<double>& seconds, const std::vector<std::size_t>& downbeats = {}) {
    std::vector<Tap> taps;
    for (std::size_t i = 0; i < seconds.size(); ++i) {
        bool down = false;
        for (const std::size_t d : downbeats) {
            down = down || d == i;
        }
        taps.push_back(Tap{seconds[i], down});
    }
    return taps;
}

AnnotationOptions noSnap() {
    AnnotationOptions options;
    options.snapWindowSeconds = 0.0;
    return options;
}

} // namespace

TEST_CASE("Taps are ordered and debounced, and a late downbeat key marks the beat it followed",
          "[annotation]") {
    std::vector<Tap> taps = {{1.0, false}, {1.02, true}, {0.5, false}, {1.5, false}};
    const Annotation a = annotate(taps, {}, noSnap());
    REQUIRE(a.times.size() == 3);
    CHECK(a.times[0] == Approx(0.5));
    CHECK(a.times[1] == Approx(1.0));
    CHECK(a.times[2] == Approx(1.5));
    // The one downbeat is the second beat; with the default bar of four the first counts
    // back from it as beat 4, and the third on from it as beat 2.
    CHECK(a.beatInBar == std::vector<int>{4, 1, 2});
    CHECK(a.stats.taps == 3);
    CHECK(a.stats.downbeats == 1);
    CHECK(a.stats.meter == 4);
    CHECK(a.stats.bpm == Approx(120.0));
}

TEST_CASE("Taps within reach of an activation peak go to it, and the rest inherit the thumb's lateness",
          "[annotation]") {
    const std::vector<float> act = activationWithPeaks(5.0, {1.0, 1.5, 2.0, 2.5, 3.0});
    // Four taps 30 to 50 ms late on real peaks, and one in a passage with no peak at all.
    const Annotation a = annotate(tapsAt({1.04, 1.53, 2.05, 2.54, 4.04}), act, AnnotationOptions{});
    REQUIRE(a.times.size() == 5);
    CHECK(a.times[0] == Approx(1.0).margin(0.006));
    CHECK(a.times[1] == Approx(1.5).margin(0.006));
    CHECK(a.times[2] == Approx(2.0).margin(0.006));
    CHECK(a.times[3] == Approx(2.5).margin(0.006));
    CHECK(a.stats.snapped == 4);
    CHECK(a.stats.medianOffsetSeconds == Approx(0.04).margin(0.006));
    // The fifth found nothing to snap to and was moved back by the median lateness.
    CHECK(a.times[4] == Approx(4.0).margin(0.006));
}

TEST_CASE("A peak too weak to be a beat is not snapped onto", "[annotation]") {
    std::vector<float> act = activationWithPeaks(4.0, {1.0});
    for (std::size_t f = 148; f <= 152; ++f) {
        act[f] = 0.05f; // a bump at 3.0 s below the threshold
    }
    const Annotation a = annotate(tapsAt({1.03, 3.03}), act, AnnotationOptions{});
    REQUIRE(a.times.size() == 2);
    CHECK(a.stats.snapped == 1);
    CHECK(a.times[0] == Approx(1.0).margin(0.006));
    CHECK(a.times[1] == Approx(3.0).margin(0.006)); // moved by the one snapped tap's 30 ms, not onto the bump
}

TEST_CASE("With the snap off the taps stand where they landed", "[annotation]") {
    const std::vector<float> act = activationWithPeaks(3.0, {1.0, 2.0});
    const Annotation a = annotate(tapsAt({1.04, 2.05}), act, noSnap());
    REQUIRE(a.times.size() == 2);
    CHECK(a.times[0] == Approx(1.04));
    CHECK(a.times[1] == Approx(2.05));
    CHECK(a.stats.snapped == 0);
    CHECK(a.stats.medianOffsetSeconds == Approx(0.0));
}

TEST_CASE("Halving keeps the downbeat's parity and doubling fills the midpoints", "[annotation]") {
    AnnotationOptions half = noSnap();
    half.octave = -1;
    const Annotation h = annotate(tapsAt({0.0, 0.5, 1.0, 1.5, 2.0, 2.5}, {1}), {}, half);
    CHECK(h.times == std::vector<double>{0.5, 1.5, 2.5});
    CHECK(h.beatInBar == std::vector<int>{1, 2, 3});
    CHECK(h.stats.bpm == Approx(60.0));
    CHECK(h.stats.taps == 6);

    AnnotationOptions twice = noSnap();
    twice.octave = 1;
    const Annotation d = annotate(tapsAt({0.0, 0.5, 1.0}, {0}), {}, twice);
    CHECK(d.times == std::vector<double>{0.0, 0.25, 0.5, 0.75, 1.0});
    CHECK(d.beatInBar == std::vector<int>{1, 2, 3, 4, 1});
    CHECK(d.stats.bpm == Approx(240.0));
}

TEST_CASE("The bar is counted from the downbeats, and back from the first of them", "[annotation]") {
    std::vector<double> seconds;
    for (int i = 0; i < 12; ++i) {
        seconds.push_back(0.5 * i);
    }
    const Annotation a = annotate(tapsAt(seconds, {2, 6, 10}), {}, noSnap());
    CHECK(a.beatInBar == std::vector<int>{3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 1, 2});
    CHECK(a.stats.meter == 4);
    CHECK(a.stats.downbeats == 3);
    CHECK(a.stats.bpm == Approx(120.0));
}

TEST_CASE("Irregular downbeats reset the count where they fall", "[annotation]") {
    std::vector<double> seconds;
    for (int i = 0; i < 8; ++i) {
        seconds.push_back(0.5 * i);
    }
    // Bars of three and four: the meter is the rounded median gap, the count restarts at
    // every downbeat regardless.
    const Annotation a = annotate(tapsAt(seconds, {0, 3, 7}), {}, noSnap());
    CHECK(a.beatInBar == std::vector<int>{1, 2, 3, 1, 2, 3, 4, 1});
    CHECK(a.stats.meter == 4);
}

TEST_CASE("Without a downbeat every beat's place in the bar is unknown", "[annotation]") {
    const Annotation a = annotate(tapsAt({0.0, 0.5, 1.0, 1.5}), {}, noSnap());
    CHECK(a.beatInBar == std::vector<int>{0, 0, 0, 0});
    CHECK(a.stats.meter == 0);
    CHECK(a.stats.downbeats == 0);
}

TEST_CASE("No taps is an empty annotation", "[annotation]") {
    const Annotation a = annotate({}, activationWithPeaks(2.0, {1.0}), AnnotationOptions{});
    CHECK(a.times.empty());
    CHECK(a.stats.taps == 0);
    CHECK(a.stats.bpm == 0.0);
}

TEST_CASE("Taps and beats round-trip through their files", "[annotation]") {
    const takt4::test::TempDir dir;
    const std::vector<Tap> taps = {{0.5, true}, {1.0, false}, {1.5, false}};
    takt4::tracking::writeTaps(dir.file("a.taps"), taps);
    const std::vector<Tap> back = takt4::tracking::readTaps(dir.file("a.taps"));
    REQUIRE(back.size() == 3);
    CHECK(back[0].seconds == Approx(0.5));
    CHECK(back[0].downbeat);
    CHECK_FALSE(back[1].downbeat);
    CHECK(back[2].seconds == Approx(1.5));

    const Annotation a = annotate(taps, {}, noSnap());
    takt4::tracking::writeBeats(dir.file("a.beats"), a);
    std::ifstream in(dir.file("a.beats"));
    std::string line;
    REQUIRE(std::getline(in, line));
    CHECK(line == "0.5000\t1");
    REQUIRE(std::getline(in, line));
    CHECK(line == "1.0000\t2");
    REQUIRE(std::getline(in, line));
    CHECK(line == "1.5000\t3");
    CHECK_FALSE(std::getline(in, line));

    // A Ballroom-layout file reads as taps: the rows marked 1 are the downbeats, and a
    // comment or a blank line is skipped.
    {
        std::ofstream ref(dir.file("ref.beats"));
        ref << "# a reference\n1.2345\t1\n\n1.7000\t2\n2.1655\t3\t127.5\n";
    }
    const std::vector<Tap> ref = takt4::tracking::readTaps(dir.file("ref.beats"));
    REQUIRE(ref.size() == 3);
    CHECK(ref[0].downbeat);
    CHECK_FALSE(ref[1].downbeat);
    CHECK(ref[2].seconds == Approx(2.1655));
}
