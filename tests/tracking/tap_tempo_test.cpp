#include "core/tracking/tap_tempo.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>

using Catch::Approx;
using takt4::tracking::TapTempo;

namespace {

/// The instant of tap `n` at `bpm`, on a clock that starts wherever it likes.
constexpr double tapAt(int n, double bpm, double start = 1000.0) {
    return start + static_cast<double>(n) * 60.0 / bpm;
}

} // namespace

TEST_CASE("a tempo appears once there are enough taps", "[tracking][tempo]") {
    TapTempo tap;
    REQUIRE(tap.options().needTaps == 3);

    CHECK_FALSE(tap.tap(tapAt(0, 128.0)).has_value());
    CHECK(tap.taps() == 1);
    CHECK_FALSE(tap.tap(tapAt(1, 128.0)).has_value());
    CHECK(tap.bpm() == 0.0);

    const std::optional<double> bpm = tap.tap(tapAt(2, 128.0));
    REQUIRE(bpm.has_value());
    CHECK(*bpm == Approx(128.0));
    CHECK(tap.bpm() == Approx(128.0));
    CHECK(tap.taps() == 3);

    // It keeps answering as the taps continue, and stays on the tempo.
    for (int n = 3; n < 12; ++n) {
        const std::optional<double> next = tap.tap(tapAt(n, 128.0));
        REQUIRE(next.has_value());
        CHECK(*next == Approx(128.0));
    }
    // ...without the set growing past what it keeps.
    CHECK(tap.taps() == tap.options().keepTaps);
}

TEST_CASE("one fumbled tap does not move the tempo", "[tracking][tempo]") {
    // The reason the gaps are reduced with a median and not a mean. An operator tapping
    // 120 BPM who fumbles one tap splits a beat into two short gaps, and both stay in the
    // window: a mean over these same taps reads 140 BPM, and the tracker would then be
    // asked to seed itself with a tempo nobody played.
    TapTempo tap;
    for (int n = 0; n < 5; ++n) {
        (void)tap.tap(tapAt(n, 120.0));
    }
    REQUIRE(tap.bpm() == Approx(120.0));

    // A stray tap a quarter of a beat after the last one, then back on the grid.
    (void)tap.tap(tapAt(5, 120.0) - 0.375);
    for (int n = 5; n < 9; ++n) {
        (void)tap.tap(tapAt(n, 120.0));
    }
    CHECK(tap.bpm() == Approx(120.0).margin(0.5));
}

TEST_CASE("a long pause starts the taps again", "[tracking][tempo]") {
    TapTempo tap;
    for (int n = 0; n < 4; ++n) {
        (void)tap.tap(tapAt(n, 100.0));
    }
    REQUIRE(tap.bpm() == Approx(100.0));

    // Longer than the timeout: a fresh attempt, not a 20 BPM tempo.
    const double resume = tapAt(3, 100.0) + tap.options().timeoutSeconds + 0.5;
    CHECK_FALSE(tap.tap(resume).has_value());
    CHECK(tap.taps() == 1);
    CHECK(tap.bpm() == 0.0);

    CHECK_FALSE(tap.tap(tapAt(1, 90.0, resume)).has_value());
    const std::optional<double> bpm = tap.tap(tapAt(2, 90.0, resume));
    REQUIRE(bpm.has_value());
    CHECK(*bpm == Approx(90.0));
}

TEST_CASE("a clock that does not move forward is not a tempo", "[tracking][tempo]") {
    TapTempo tap;
    for (int n = 0; n < 3; ++n) {
        (void)tap.tap(tapAt(n, 140.0));
    }
    REQUIRE(tap.bpm() == Approx(140.0));

    // Two taps in the same instant would be an infinite tempo. It is the same press seen
    // twice — a bounce — and is ignored outright, so the set being counted is kept.
    const double at = tapAt(2, 140.0);
    CHECK_FALSE(tap.tap(at).has_value());
    CHECK(tap.taps() == 3);
    CHECK(tap.bpm() == Approx(140.0));

    // Going backwards would be a negative one, and starts again rather than publishing it.
    CHECK_FALSE(tap.tap(at - 1.0).has_value());
    CHECK(tap.taps() == 1);
    CHECK(tap.bpm() == 0.0);
}

TEST_CASE("a switch that bounces taps once, not twice", "[tracking][tempo]") {
    // The audit's H1: two taps 20 ms apart used to be a tempo of 3000 BPM, and with the fold
    // off that went to Link and the MIDI clock as an octave shift. A tap inside the bounce
    // window is not a tap at all — not counted, and not a reason to start the set again.
    TapTempo tap;
    for (int n = 0; n < 4; ++n) {
        (void)tap.tap(tapAt(n, 120.0));
        // Every press chatters: a second edge 8 ms after the first.
        CHECK_FALSE(tap.tap(tapAt(n, 120.0) + 0.008).has_value());
    }
    CHECK(tap.taps() == 4);
    CHECK(tap.bpm() == Approx(120.0));
}

TEST_CASE("a burst of taps too fast to be a tempo offers none", "[tracking][tempo]") {
    // 150 ms apart is 400 BPM: past the bounce window, and past anything a person means. The
    // set is counted but no tempo leaves it, so nothing absurd reaches the tracker.
    TapTempo tap;
    for (int n = 0; n < 6; ++n) {
        CHECK_FALSE(tap.tap(1000.0 + 0.150 * n).has_value());
    }
    CHECK(tap.bpm() == 0.0);

    SECTION("and slower than 30 BPM is a new attempt each time") {
        TapTempo slow;
        for (int n = 0; n < 4; ++n) {
            CHECK_FALSE(slow.tap(2000.0 + 2.5 * n).has_value());
        }
        CHECK(slow.taps() == 1);
    }
}

TEST_CASE("a reset forgets the taps", "[tracking][tempo]") {
    TapTempo tap;
    for (int n = 0; n < 4; ++n) {
        (void)tap.tap(tapAt(n, 110.0));
    }
    REQUIRE(tap.bpm() == Approx(110.0));
    tap.reset();
    CHECK(tap.taps() == 0);
    CHECK(tap.bpm() == 0.0);
    CHECK_FALSE(tap.tap(tapAt(4, 110.0)).has_value());
}

TEST_CASE("nonsensical tap options are refused", "[tracking][tempo]") {
    TapTempo::Options options;
    options.timeoutSeconds = 0.0;
    CHECK_THROWS_AS(TapTempo(options), std::invalid_argument);

    options = TapTempo::Options{};
    options.needTaps = 1;
    CHECK_THROWS_AS(TapTempo(options), std::invalid_argument);

    options = TapTempo::Options{};
    options.needTaps = 6;
    options.keepTaps = 4;
    CHECK_THROWS_AS(TapTempo(options), std::invalid_argument);
}

TEST_CASE("two taps are enough when the operator asks for two", "[tracking][tempo]") {
    TapTempo::Options options;
    options.needTaps = 2;
    TapTempo tap(options);
    CHECK_FALSE(tap.tap(tapAt(0, 174.0)).has_value());
    const std::optional<double> bpm = tap.tap(tapAt(1, 174.0));
    REQUIRE(bpm.has_value());
    CHECK(*bpm == Approx(174.0));
}
