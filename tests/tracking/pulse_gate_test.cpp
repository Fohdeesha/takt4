#include "core/rt/alloc_guard.hpp"
#include "core/tracking/pulse_gate.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using Catch::Approx;
using takt4::tracking::PulseGate;

namespace {

/// A beat every `period` frames over a low, uneven floor: the activation a steady kick gives,
/// with the network never quite sure of the frames between. Bit for bit what the reference
/// numbers below were computed from.
float beatAt(std::size_t frame, std::size_t period) {
    double value = 0.05 + 0.1 * std::fmod(static_cast<double>(frame) * 0.6180339887498949, 1.0);
    if (frame % period == 0) {
        value += 0.8;
    } else if (frame % period == 1) {
        value += 0.4;
    }
    return static_cast<float>(value);
}

/// A downbeat every `bar` frames, over a lower floor.
float downbeatAt(std::size_t frame, std::size_t bar) {
    double value = 0.02 + 0.05 * std::fmod(static_cast<double>(frame) * 0.7548776662466927, 1.0);
    if (frame % bar == 0) {
        value += 0.6;
    }
    return static_cast<float>(value);
}

/// Feeds `frames` of a beat every `period` and a downbeat every `bar`, the decoder counting
/// `decoderPeriod`.
void feed(PulseGate& gate, std::size_t frames, std::size_t period, double decoderPeriod,
          std::size_t bar, std::size_t from = 0) {
    for (std::size_t i = from; i < from + frames; ++i) {
        gate.push(beatAt(i, period), downbeatAt(i, bar), decoderPeriod);
    }
}

/// A uniform number in [0, 1) from a fixed seed, the same on every compiler.
struct Noise {
    std::uint32_t state = 2463534242U;
    double next() {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return static_cast<double>(state) / 4294967296.0;
    }
};

} // namespace

TEST_CASE("the gate's numbers are the ones its thresholds were measured with",
          "[tracking][pulse]") {
    // The thresholds were chosen in numpy over traces (numpy's percentile and median, the
    // autocorrelation normalised by its energy). These are numpy's answers for the same
    // activations, so a C++ that drifted from what was measured fails here rather than at a gig.
    SECTION("the window full, the decoder on the beat, on twice it, and on neither") {
        PulseGate gate;
        feed(gate, 300, 26, 26.0, 104);
        CHECK(gate.spread() == Approx(0.470184963383).margin(1e-7));
        CHECK(gate.repeat() == Approx(0.970076772864).margin(1e-7));
        CHECK(gate.floor() == Approx(0.061287306622).margin(1e-7));
        CHECK(gate.twice() == Approx(0.879590865625).margin(1e-7));
        CHECK(gate.bar() == Approx(0.627283319324).margin(1e-7));

        PulseGate twice;
        feed(twice, 300, 26, 52.0, 104);
        CHECK(twice.repeat() == Approx(0.970076772864).margin(1e-7));
        CHECK(twice.twice() == Approx(0.879590865625).margin(1e-7));
        CHECK(twice.bar() == Approx(0.627283319324).margin(1e-7));

        PulseGate neither;
        feed(neither, 300, 26, 39.0, 104);
        CHECK(neither.repeat() == Approx(0.009453297834).margin(1e-7));
        CHECK(neither.twice() == Approx(0.791286060437).margin(1e-7));
        CHECK(neither.bar() == Approx(0.026243156430).margin(1e-7));
    }
    SECTION("a bar of three") {
        PulseGate gate;
        feed(gate, 300, 26, 26.0, 78);
        CHECK(gate.bar() == Approx(0.748677770504).margin(1e-7));
    }
    SECTION("before the window is full, over what there is, and no bar without room for one") {
        PulseGate gate;
        feed(gate, 100, 26, 26.0, 104);
        CHECK(gate.spread() == Approx(0.469915053807).margin(1e-7));
        CHECK(gate.repeat() == Approx(0.777890574969).margin(1e-7));
        CHECK(gate.floor() == Approx(0.061516562849).margin(1e-7));
        CHECK(gate.twice() == Approx(0.526206844923).margin(1e-7));
        CHECK(std::isnan(gate.bar()));
    }
    SECTION("after it has gone round, over the last six seconds only") {
        PulseGate gate;
        feed(gate, 450, 26, 26.0, 104);
        CHECK(gate.spread() == Approx(0.452835782245).margin(1e-7));
        CHECK(gate.repeat() == Approx(0.967253137731).margin(1e-7));
        CHECK(gate.floor() == Approx(0.061789807305).margin(1e-7));
        CHECK(gate.twice() == Approx(0.881674999189).margin(1e-7));
        CHECK(gate.bar() == Approx(0.626211181593).margin(1e-7));
        // A half-frame period rounds up, and its multiples are counted from the frame it rounds
        // to: the beat itself still found a frame either side, its bar not.
        PulseGate half;
        feed(half, 450, 26, 26.5, 104);
        CHECK(half.repeat() == Approx(0.967253137731).margin(1e-7));
        CHECK(half.twice() == Approx(0.334914150452).margin(1e-7));
        CHECK(half.bar() == Approx(0.033444635453).margin(1e-7));
    }
}

TEST_CASE("a steady beat at the decoder's period opens the gate, after two seconds",
          "[tracking][pulse]") {
    PulseGate gate;
    CHECK_FALSE(gate.open());
    // Judged every half second over what there is, and a window too short for the longest lag
    // judges nothing: the first that can open it is at two seconds.
    feed(gate, 75, 25, 25.0, 100);
    CHECK_FALSE(gate.open());
    CHECK(gate.repeat() == -1.0);
    feed(gate, 25, 25, 25.0, 100, 75);
    CHECK(gate.open());
}

TEST_CASE(
    "a beat that falls back to nothing between opens it on its own; a busy one needs two more "
    "signs",
    "[tracking][pulse]") {
    // A clean kick: nothing between the beats, no bar to be seen.
    const auto clean = [](std::size_t i) { return i % 25 == 0 ? 0.9f : i % 25 == 1 ? 0.4f : 0.0f; };
    PulseGate gate;
    for (std::size_t i = 0; i < 300; ++i) {
        gate.push(clean(i), 0.0f, 25.0);
    }
    CHECK(gate.floor() <= 0.03);
    CHECK(std::isnan(gate.bar()));
    CHECK(gate.open());

    // The same beat over a floor that never falls: open only while it also repeats at twice the
    // period, or at the bar, or far from chance — two of the three.
    PulseGate busy;
    feed(busy, 300, 25, 25.0, 100);
    CHECK(busy.floor() > 0.03);
    CHECK(busy.twice() >= 0.35);
    CHECK(busy.open());
}

TEST_CASE("a beat the decoder is not counting does not open it", "[tracking][pulse]") {
    // Only the decoder's period, or an octave of it: a gate that looked for whichever period
    // flattered the activation found one in clicks at random times. Periods whose octaves, and a
    // frame either side, miss 25, 50 and 75.
    for (const double decoder : {35.0, 43.0, 61.0}) {
        INFO("decoder at " << decoder);
        PulseGate gate;
        feed(gate, 3000, 25, decoder, 100);
        CHECK_FALSE(gate.open());
    }
    // A beat every 30 frames, 100 BPM, with the decoder counting it at double or half time.
    for (const double decoder : {15.0, 60.0}) {
        INFO("decoder at " << decoder << ", an octave of the beat's 30 frames");
        PulseGate gate;
        feed(gate, 300, 30, decoder, 120);
        CHECK(gate.open());
    }
    SECTION("nor does a decoder with no period, or one outside 40 to 200 BPM") {
        for (const double decoder : {0.0, -5.0, 14.0, 76.0, 300.0}) {
            INFO("decoder at " << decoder);
            PulseGate gate;
            feed(gate, 3000, 25, decoder, 100);
            CHECK_FALSE(gate.open());
            CHECK(gate.repeat() == -1.0);
        }
    }
}

TEST_CASE("clicks at random times swing the activation as far as music, and do not open it",
          "[tracking][pulse]") {
    // One, two and three a second for a minute each, over the floor the network keeps between
    // sounds it cannot predict, the decoder on each period in turn from 200 BPM to 40. Chance
    // lines a few of them up at some period now and then.
    int firstTwo = 0;
    for (const double perSecond : {1.0, 2.0, 3.0}) {
        std::vector<float> clicks(3000, 0.0f);
        Noise noise;
        for (float& value : clicks) {
            value = static_cast<float>(0.04 + 0.05 * noise.next());
        }
        for (std::size_t i = 0; i < clicks.size(); ++i) {
            if (noise.next() < perSecond / 50.0) {
                for (std::size_t k = 0; k < 4 && i + k < clicks.size(); ++k) {
                    clicks[i + k] =
                        std::max(clicks[i + k], static_cast<float>(0.9 * std::pow(0.5, k)));
                }
            }
        }
        for (int period = 15; period <= 75; ++period) {
            PulseGate gate;
            for (const float value : clicks) {
                gate.push(value, 0.0f, static_cast<double>(period));
                firstTwo += gate.spread() >= 0.40 && gate.repeat() >= 0.35 ? 1 : 0;
            }
            INFO(perSecond << " a second, the decoder at " << period);
            CHECK_FALSE(gate.open());
        }
    }
    // The swing and the repeat at the period, which alone would have opened the gate on these
    // clicks: what holds it shut is the floor that never falls, and no second sign.
    INFO("judgements passing the first two tests: " << firstTwo);
    CHECK(firstTwo > 0);
}

TEST_CASE("a smooth swell repeats but does not swing, and does not open it", "[tracking][pulse]") {
    // A pad breathing in and out every two seconds, the activation following it.
    PulseGate gate;
    for (std::size_t i = 0; i < 3000; ++i) {
        const double swell =
            0.3 + 0.12 * std::sin(2.0 * 3.141592653589793 * static_cast<double>(i) / 100.0);
        gate.push(static_cast<float>(swell), 0.0f, 50.0);
    }
    CHECK(gate.repeat() > 0.35);
    CHECK(gate.spread() < 0.40);
    CHECK_FALSE(gate.open());
}

TEST_CASE("once a beat is heard the gate stays open through a breakdown, until it is reset",
          "[tracking][pulse]") {
    PulseGate gate;
    feed(gate, 300, 25, 25.0, 100);
    REQUIRE(gate.open());
    // Twenty seconds of a pad, then of nothing: what the gate said was about the source.
    for (std::size_t i = 0; i < 1000; ++i) {
        gate.push(0.2f, 0.0f, 25.0);
    }
    for (std::size_t i = 0; i < 1000; ++i) {
        gate.push(0.0f, 0.0f, 0.0);
    }
    CHECK(gate.open());

    gate.reset();
    CHECK_FALSE(gate.open());
    CHECK(gate.repeat() == -1.0);
    // And what follows is judged on its own: the beat before the reset is not in the window.
    for (std::size_t i = 0; i < 3000; ++i) {
        gate.push(0.2f, 0.0f, 25.0);
    }
    CHECK_FALSE(gate.open());
    feed(gate, 300, 25, 25.0, 100);
    CHECK(gate.open());
}

TEST_CASE("a person saying there is a beat opens it at once, and a reset shuts it again",
          "[tracking][pulse]") {
    PulseGate gate;
    for (std::size_t i = 0; i < 500; ++i) {
        gate.push(0.2f, 0.0f, 25.0);
    }
    REQUIRE_FALSE(gate.open());
    gate.markHeard();
    CHECK(gate.open());
    for (std::size_t i = 0; i < 500; ++i) {
        gate.push(0.2f, 0.0f, 25.0);
    }
    CHECK(gate.open());
    gate.reset();
    CHECK_FALSE(gate.open());
}

TEST_CASE("an activation that is not a number counts as none", "[tracking][pulse]") {
    PulseGate gate;
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity()};
    for (std::size_t i = 0; i < 3000; ++i) {
        gate.push(bad[i % 3], bad[(i + 1) % 3], 25.0);
    }
    CHECK_FALSE(gate.open());
    CHECK(std::isfinite(gate.spread()));
    CHECK(std::isfinite(gate.repeat()));
    CHECK(std::isfinite(gate.floor()));
    // And a beat after them still opens it: nothing they left behind poisons the window.
    feed(gate, 300, 25, 25.0, 100);
    CHECK(gate.open());
}

TEST_CASE("listening for a beat touches no heap", "[tracking][pulse][rt]") {
    PulseGate gate;
    const bool abortWas = takt4::rt::abortOnViolation();
    takt4::rt::setAbortOnViolation(false);
    const std::uint64_t before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope scope;
        feed(gate, 1000, 25, 25.0, 100);
        gate.reset();
        feed(gate, 1000, 25, 25.0, 100);
    }
    const std::uint64_t after = takt4::rt::violationCount();
    takt4::rt::setAbortOnViolation(abortWas);
    CHECK(gate.open());
    CHECK(takt4::rt::allocationGuardEnabled());
    CHECK(after == before);
}
