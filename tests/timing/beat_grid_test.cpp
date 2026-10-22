#include "core/timing/beat_grid.hpp"
#include "core/timing/grid_source.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

using Catch::Approx;
using takt4::engine::EngineBeat;
using takt4::timing::BeatGrid;
using takt4::timing::GridReading;

namespace {

/// Somewhere a Link clock might be: ten seconds in, in microseconds.
constexpr std::int64_t kStart = 10'000'000;
constexpr std::int64_t kSecond = 1'000'000;

double periodOf(double bpm) {
    return 60e6 / bpm;
}

/// A source on a steady grid: `bpm`, its beat 0 at `beatZeroAt`.
GridReading onGrid(std::int64_t at, double bpm, double beatZeroAt, bool anchored = true) {
    GridReading reading;
    reading.at = at;
    reading.present = true;
    reading.bpm = bpm;
    reading.beat = (static_cast<double>(at) - beatZeroAt) * bpm / 60e6;
    reading.anchored = anchored;
    return reading;
}

GridReading absent(std::int64_t at) {
    GridReading reading;
    reading.at = at;
    return reading;
}

/// One beat sent, and the reading that sent it.
struct Sent {
    EngineBeat beat;
    std::int64_t readAt = 0;
};

using Source = std::function<GridReading(std::int64_t)>;

/// A reading every millisecond from `at` until `to`, as `ExternalBeats` makes them. `at` is left
/// at the reading after the last, so a test carries on from there and time never goes back.
std::vector<Sent> drive(BeatGrid& grid, const Source& source, std::int64_t& at, std::int64_t to) {
    std::vector<Sent> sent;
    for (; at < to; at += 1000) {
        if (const std::optional<EngineBeat> beat = grid.advance(source(at))) {
            sent.push_back({*beat, at});
        }
    }
    return sent;
}

/// The same from `from`, for a test that reads once.
std::vector<Sent> driveFrom(BeatGrid& grid, const Source& source, std::int64_t from,
                            std::int64_t to) {
    return drive(grid, source, from, to);
}

/// Where on a grid with its beat 0 at `beatZeroAt` a beat stamped `hostMicros` falls.
double positionOf(const EngineBeat& beat, double bpm, double beatZeroAt) {
    return (static_cast<double>(beat.hostMicros) - beatZeroAt) / periodOf(bpm);
}

} // namespace

TEST_CASE("a followed grid gives every beat once, stamped with its own moment",
          "[timing][follow]") {
    // The whole of what an external source is for: no beat missed, none twice, each stamped with
    // the moment it is on the source's grid — not the reading that noticed it — and sent by the
    // first reading after that moment, so within a millisecond of it.
    const double bpm = GENERATE(90.0, 128.0, 174.0);
    INFO(bpm << " BPM");
    const double period = periodOf(bpm);
    // Beat 0 off the millisecond grid, so no beat falls on a reading.
    const double zero = static_cast<double>(kStart) + 377.0;
    BeatGrid grid;
    const std::vector<Sent> sent = driveFrom(
        grid, [&](std::int64_t at) { return onGrid(at, bpm, zero); }, kStart,
        kStart + 20 * kSecond);

    const auto expected = static_cast<std::size_t>(
        std::floor((static_cast<double>(kStart + 20 * kSecond - 1000) - zero) / period) + 1.0);
    REQUIRE(sent.size() == expected);
    for (std::size_t k = 0; k < sent.size(); ++k) {
        INFO("beat " << k);
        const EngineBeat& beat = sent[k].beat;
        const double moment = zero + static_cast<double>(k) * period;
        CHECK(std::abs(static_cast<double>(beat.hostMicros) - moment) <= 1.0);
        CHECK(sent[k].readAt >= beat.hostMicros);
        CHECK(sent[k].readAt - beat.hostMicros < 1000);
        CHECK(beat.state.beats == k + 1);
        CHECK(beat.event.frameIndex == k + 1);
        CHECK(beat.event.beatInBar == k % 4 + 1);
        CHECK(beat.event.downbeat == (k % 4 == 0));
        CHECK(beat.state.bars == k / 4 + 1);
        CHECK(beat.event.time == Approx((moment - static_cast<double>(kStart)) / 1e6).margin(1e-6));
        CHECK(beat.event.locked);
        CHECK(beat.event.acquired);
        CHECK(beat.event.confidence == 1.0);
        CHECK(beat.event.bpm == bpm);
        CHECK(beat.event.gridBpm == bpm);
        CHECK(beat.event.clockOctaves == 0);
        CHECK_FALSE(beat.event.snapped);
        CHECK(beat.hostMicros != 0);
    }
    const auto& state = grid.state();
    CHECK(state.locked);
    CHECK(state.acquired);
    CHECK_FALSE(state.noSignal);
    CHECK_FALSE(state.holding);
    CHECK_FALSE(state.pinned);
    CHECK(state.refined);
    CHECK(state.beatDivisor == 1);
    CHECK(state.rawBpm == bpm);
    CHECK(state.calledBpm == bpm);
    CHECK(state.beatsBpm == Approx(bpm).epsilon(1e-6));
}

TEST_CASE("a followed grid counts its bars in whatever beats to the bar it is set to",
          "[timing][follow]") {
    // Neither Link nor a MIDI clock carries a meter: it is the operator's, and the bar line is
    // every whole multiple of it on the source's grid — Link's own quantum.
    const std::uint32_t meter = GENERATE(3u, 4u, 7u);
    INFO(meter << " beats to the bar");
    const double bpm = 128.0;
    const double zero = static_cast<double>(kStart) + 600.0;
    BeatGrid grid(BeatGrid::Options{meter, 4.0});
    const std::vector<Sent> sent = driveFrom(
        grid, [&](std::int64_t at) { return onGrid(at, bpm, zero); }, kStart,
        kStart + 15 * kSecond);
    REQUIRE(sent.size() > 3 * meter);
    std::uint64_t bars = 0;
    for (std::size_t k = 0; k < sent.size(); ++k) {
        INFO("beat " << k);
        const EngineBeat& beat = sent[k].beat;
        bars += k % meter == 0 ? 1 : 0;
        CHECK(beat.event.beatsPerBar == meter);
        CHECK(beat.event.beatInBar == k % meter + 1);
        CHECK(beat.event.downbeat == (k % meter == 0));
        CHECK(beat.state.bars == bars);
    }
}

TEST_CASE("DOWNBEAT on a followed grid starts the bar on the beat nearest the press",
          "[timing][follow]") {
    const double bpm = 120.0;
    const double period = periodOf(bpm);
    const double zero = static_cast<double>(kStart) + 250.0;
    const Source source = [&](std::int64_t at) { return onGrid(at, bpm, zero); };
    BeatGrid grid;
    // Up to just after beat 6, which is the third of its bar.
    const auto beat6 = static_cast<std::int64_t>(zero + 6.0 * period);
    std::int64_t at = kStart;
    const std::vector<Sent> before = drive(grid, source, at, beat6 + 1000);
    REQUIRE(before.size() == 7);
    REQUIRE(before.back().beat.event.beatInBar == 3);
    const std::uint64_t barsBefore = grid.state().bars;

    SECTION("pressed just after a beat, that beat is bar 1 at once and the bar is declared") {
        (void)drive(grid, source, at, static_cast<std::int64_t>(zero + 6.1 * period));
        grid.snapDownbeat();
        REQUIRE_FALSE(grid.advance(source(at)).has_value());
        at += 1000;
        CHECK(grid.state().beatInBar == 1);
        CHECK(grid.state().bars == barsBefore + 1);
        CHECK(grid.state().barsDeclared == 1);
        CHECK(grid.state().declaredBar == grid.state().bars);
        const std::vector<Sent> after = drive(grid, source, at, at + 5 * kSecond);
        REQUIRE(after.size() >= 5);
        // The beat after it is beat 2, carries the snap to the outputs, and the grid is unmoved.
        CHECK(after[0].beat.event.beatInBar == 2);
        CHECK(after[0].beat.event.snapped);
        CHECK(positionOf(after[0].beat, bpm, zero) == Approx(7.0).margin(1e-5));
        CHECK(after[1].beat.event.beatInBar == 3);
        CHECK_FALSE(after[1].beat.event.snapped);
        CHECK(after[3].beat.event.beatInBar == 1);
        CHECK(after[3].beat.state.bars == barsBefore + 2);
    }

    SECTION("pressed nearer the beat to come, that beat is bar 1 and nothing is declared") {
        (void)drive(grid, source, at, static_cast<std::int64_t>(zero + 6.8 * period));
        grid.snapDownbeat();
        REQUIRE_FALSE(grid.advance(source(at)).has_value());
        at += 1000;
        CHECK(grid.state().beatInBar == 3); // nothing has moved yet
        CHECK(grid.state().barsDeclared == 0);
        const std::vector<Sent> after = drive(grid, source, at, at + 5 * kSecond);
        REQUIRE(after.size() >= 5);
        CHECK(after[0].beat.event.beatInBar == 1);
        CHECK(after[0].beat.event.downbeat);
        CHECK(after[0].beat.event.snapped);
        CHECK(after[0].beat.state.bars == barsBefore + 1);
        CHECK(positionOf(after[0].beat, bpm, zero) == Approx(7.0).margin(1e-5));
        CHECK(after[1].beat.event.beatInBar == 2);
        CHECK_FALSE(after[1].beat.event.snapped);
        CHECK(after[4].beat.event.beatInBar == 1);
        CHECK(grid.state().barsDeclared == 0);
    }
}

TEST_CASE("DOWNBEAT on a source with no phase puts the beats and the bar under the press",
          "[timing][follow]") {
    // A MIDI clock nobody has pressed Start on: its ticks give the tempo and nothing else, so its
    // whole numbers are nobody's beats. The beats go out at the tempo, at a phase that is
    // anybody's, with no bar — until the operator presses DOWNBEAT on a beat they can hear.
    const double bpm = 120.0;
    const double period = periodOf(bpm);
    const double zero = static_cast<double>(kStart) + 100.0; // the clock's own beat 0
    const Source source = [&](std::int64_t at) { return onGrid(at, bpm, zero, false); };
    BeatGrid grid;
    std::int64_t at = kStart;
    const std::vector<Sent> before =
        drive(grid, source, at, static_cast<std::int64_t>(zero + 8.05 * period));
    REQUIRE(before.size() == 9);
    for (const Sent& one : before) {
        CHECK(one.beat.event.beatInBar == 0); // no bar is known, and none is guessed
        CHECK_FALSE(one.beat.event.downbeat);
    }
    CHECK(grid.state().bars == 0);

    // The music's beats are this far after the clock's. Between 0.4 and 0.5 of a beat after, the
    // press's own crossing was let out as a downbeat as well as the bar declared: the bar counted
    // twice, its rules fired twice.
    const double offset = GENERATE(0.6, 0.3, 0.42, 0.45, 0.49);
    INFO("the music " << offset << " of a beat after the clock");
    (void)drive(grid, source, at, static_cast<std::int64_t>(zero + (8.0 + offset) * period));
    // Pressed on the reading that follows the music's beat — within a millisecond of it.
    const std::int64_t press = at;
    grid.snapDownbeat();
    const std::optional<EngineBeat> atPress = grid.advance(source(at));
    at += 1000;
    const std::vector<Sent> after = drive(grid, source, at, at + 5 * kSecond);
    REQUIRE(after.size() >= 8);
    if (offset > 0.5) {
        // Further from the last beat than half a beat: the press is a beat of its own, sent there
        // and then as bar 1.
        REQUIRE(atPress.has_value());
        CHECK(atPress->hostMicros == press);
        CHECK(atPress->event.beatInBar == 1);
        CHECK(atPress->event.downbeat);
        CHECK(atPress->event.snapped);
        CHECK(grid.state().barsDeclared == 0);
        CHECK(after[0].beat.event.beatInBar == 2);
    } else {
        // Within half a beat of the last: that was the beat, sent a little early, and its bar is
        // declared — once. The next is a beat after the press, as beat 2.
        CHECK_FALSE(atPress.has_value());
        CHECK(grid.state().barsDeclared == 1);
        CHECK(after[0].beat.state.bars == 1);
        CHECK(after[0].beat.event.beatInBar == 2);
        CHECK(after[0].beat.event.snapped);
    }
    // And from there every beat is on the press's grid, counted in bars.
    for (std::size_t i = 0; i < after.size(); ++i) {
        INFO("beat " << i << " after the press");
        const double beatsAfter = static_cast<double>(after[i].beat.hostMicros - press) / period;
        CHECK(beatsAfter == Approx(static_cast<double>(i + 1)).margin(1e-5));
        CHECK(after[i].beat.event.beatInBar == (i + 1) % 4 + 1);
    }
}

TEST_CASE("halving a followed grid keeps its downbeats and doubling adds a beat between",
          "[timing][follow]") {
    const double bpm = 128.0;
    const double period = periodOf(bpm);
    const double zero = static_cast<double>(kStart) + 333.0;
    const Source source = [&](std::int64_t at) { return onGrid(at, bpm, zero); };
    BeatGrid grid;
    // Half-way from beat 8 to beat 9.
    std::int64_t at = kStart;
    const std::vector<Sent> sent =
        drive(grid, source, at, static_cast<std::int64_t>(zero + 8.5 * period));
    REQUIRE(sent.size() == 9);

    SECTION("÷2 publishes every second beat, the bar's downbeat among them, at half the tempo") {
        grid.halve();
        const std::vector<Sent> halved = drive(grid, source, at, at + 15 * kSecond);
        REQUIRE(halved.size() > 8);
        CHECK(grid.state().bpm == Approx(64.0));
        CHECK(grid.state().gridBpm == Approx(64.0));
        CHECK(grid.state().rawBpm == bpm);
        CHECK(grid.state().octaveShift == -1);
        CHECK(grid.state().clockOctaves == 0);
        // The first kept is beat 10, the next even one.
        CHECK(positionOf(halved[0].beat, bpm, zero) == Approx(10.0).margin(1e-5));
        for (std::size_t i = 0; i < halved.size(); ++i) {
            INFO("beat " << i << " after the ÷2");
            const double position = positionOf(halved[i].beat, bpm, zero);
            const auto whole = static_cast<std::int64_t>(std::llround(position));
            CHECK(position == Approx(static_cast<double>(whole)).margin(1e-5));
            CHECK(whole % 2 == 0);
            // A bar of four published beats is two of the source's bars, starting on one of its
            // downbeats — the tracker's rule for a divided grid.
            CHECK(halved[i].beat.event.downbeat == (whole % 8 == 0));
            CHECK(halved[i].beat.event.bpm == Approx(64.0));
            if (i > 0) {
                CHECK(halved[i].beat.hostMicros - halved[i - 1].beat.hostMicros ==
                      Approx(2.0 * period).margin(1.0));
            }
        }
        CHECK(halved.back().beat.state.beatsBpm == Approx(64.0).epsilon(1e-4));
    }

    SECTION("÷2 just after a beat it does not keep sends the next it does, half a beat on") {
        // Beat 9 just went; ÷2 keeps the even ones, and beat 10 is half a new beat later.
        const std::vector<Sent> nine =
            drive(grid, source, at, static_cast<std::int64_t>(zero + 9.0 * period) + 1000);
        REQUIRE(nine.size() == 1);
        REQUIRE(positionOf(nine[0].beat, bpm, zero) == Approx(9.0).margin(1e-5));
        grid.halve();
        const std::vector<Sent> halved = drive(grid, source, at, at + 3 * kSecond);
        REQUIRE_FALSE(halved.empty());
        CHECK(positionOf(halved[0].beat, bpm, zero) == Approx(10.0).margin(1e-5));
        CHECK(static_cast<double>(halved[0].beat.hostMicros - nine[0].beat.hostMicros) ==
              Approx(period).margin(1.0));
    }

    SECTION("÷2 pressed in the millisecond before a beat it keeps still sends that beat") {
        // The reading before beat 10, the ÷2, and the reading after it: the grid's position is
        // halved with it, so the beat is crossed rather than jumped past.
        (void)drive(grid, source, at, static_cast<std::int64_t>(zero + 10.0 * period));
        grid.halve();
        const std::vector<Sent> halved = drive(grid, source, at, at + 3 * kSecond);
        REQUIRE_FALSE(halved.empty());
        CHECK(positionOf(halved[0].beat, bpm, zero) == Approx(10.0).margin(1e-5));
        CHECK(halved[0].readAt - halved[0].beat.hostMicros < 1000);
    }

    SECTION("×2 adds a beat half-way, at twice the tempo, and the clocks follow it") {
        grid.redouble();
        const std::vector<Sent> doubled = drive(grid, source, at, at + 5 * kSecond);
        REQUIRE(doubled.size() > 16);
        CHECK(grid.state().bpm == Approx(256.0));
        CHECK(grid.state().gridBpm == Approx(256.0));
        CHECK(grid.state().octaveShift == 1);
        for (std::size_t i = 0; i < doubled.size(); ++i) {
            INFO("beat " << i << " after the ×2");
            const double position = positionOf(doubled[i].beat, bpm, zero);
            CHECK(2.0 * position == Approx(std::round(2.0 * position)).margin(1e-5));
            // A bar of four published beats is half of one of the source's, starting on one of
            // its beats that is an even number from its downbeat.
            CHECK(doubled[i].beat.event.downbeat == (std::llround(2.0 * position) % 4 == 0));
            CHECK(doubled[i].beat.event.clockOctaves == 0);
            if (i > 0) {
                CHECK(doubled[i].beat.hostMicros - doubled[i - 1].beat.hostMicros ==
                      Approx(0.5 * period).margin(1.0));
                CHECK(doubled[i].beat.state.beats == doubled[i - 1].beat.state.beats + 1);
            }
        }
    }

    SECTION("×2 pressed in the millisecond before a beat still sends that beat") {
        (void)drive(grid, source, at, static_cast<std::int64_t>(zero + 9.0 * period));
        grid.redouble();
        const std::vector<Sent> doubled = drive(grid, source, at, at + 3 * kSecond);
        REQUIRE_FALSE(doubled.empty());
        CHECK(positionOf(doubled[0].beat, bpm, zero) == Approx(9.0).margin(1e-5));
        CHECK(doubled[0].readAt - doubled[0].beat.hostMicros < 1000);
    }

    SECTION("the shift stops two octaves either way") {
        for (int i = 0; i < 5; ++i) {
            grid.halve();
        }
        (void)grid.advance(source(at));
        at += 1000;
        CHECK(grid.state().octaveShift == -BeatGrid::kMaxOctaveShift);
        for (int i = 0; i < 9; ++i) {
            grid.redouble();
        }
        (void)grid.advance(source(at));
        CHECK(grid.state().octaveShift == BeatGrid::kMaxOctaveShift);
        CHECK(grid.state().bpm == Approx(bpm * 4.0));
    }
}

TEST_CASE("a followed source that goes is coasted, then let go of, and acquired again",
          "[timing][follow]") {
    // The operator's call of 2026-10-15: run on at the last tempo and phase for a while, as the
    // tracker's flywheel does through a breakdown, and then stop exactly as silence stops it.
    const double bpm = 128.0;
    const double period = periodOf(bpm);
    const double zero = static_cast<double>(kStart) + 900.0;
    const std::int64_t lost = kStart + 5 * kSecond + 123;
    const std::int64_t back = lost + 8 * kSecond;
    // Back half a beat away from where it was, as a session another app re-founded might be.
    const double zeroAfter = zero + 0.5 * period;
    BeatGrid grid;
    const Source source = [&](std::int64_t at) {
        if (at < lost) {
            return onGrid(at, bpm, zero);
        }
        if (at < back) {
            return absent(at);
        }
        return onGrid(at, bpm, zeroAfter);
    };
    std::int64_t at = kStart;
    const std::vector<Sent> sent = drive(grid, source, at, lost);
    REQUIRE_FALSE(sent.empty());
    const std::uint64_t beatsBefore = grid.state().beats;
    // The last reading the source was there for.
    const std::int64_t lastPresent = at - 1000;

    // Coasting: the beats go on, on the grid the source was on, and the lock stays.
    std::vector<Sent> coasted;
    for (; at < back; at += 1000) {
        const std::optional<EngineBeat> beat = grid.advance(source(at));
        if (grid.state().noSignal) {
            CHECK_FALSE(beat.has_value());
            break;
        }
        CHECK(grid.coasting());
        CHECK(grid.state().locked);
        CHECK(grid.state().acquired);
        if (beat) {
            coasted.push_back({*beat, at});
        }
    }
    // Let go of on the first reading four seconds after the last one the source was there for.
    REQUIRE(grid.state().noSignal);
    CHECK(at - lastPresent == 4 * kSecond);
    at += 1000;
    CHECK_FALSE(grid.state().locked);
    CHECK_FALSE(grid.state().acquired);
    CHECK_FALSE(grid.coasting());
    REQUIRE(coasted.size() >= 8);
    for (const Sent& one : coasted) {
        const double position = positionOf(one.beat, bpm, zero);
        CHECK(position == Approx(std::round(position)).margin(1e-4));
        CHECK(one.beat.event.locked);
        CHECK(one.beat.event.acquired);
    }
    // Nothing more until it is back — and nothing lost from the count.
    const std::vector<Sent> silent = drive(grid, source, at, back);
    CHECK(silent.empty());
    CHECK(grid.state().noSignal);

    const std::vector<Sent> resumed = drive(grid, source, at, back + 3 * kSecond);
    REQUIRE(resumed.size() >= 4);
    CHECK(grid.state().locked);
    CHECK(grid.state().acquired);
    CHECK_FALSE(grid.state().noSignal);
    CHECK(resumed.front().beat.event.acquired);
    CHECK(resumed.front().beat.state.beats > beatsBefore + coasted.size());
    for (const Sent& one : resumed) {
        const double position = positionOf(one.beat, bpm, zeroAfter);
        CHECK(position == Approx(std::round(position)).margin(1e-4));
    }
}

TEST_CASE("an obeyed Stop lets go of the rig at once, with no coast", "[timing][follow]") {
    const double bpm = 120.0;
    const double zero = static_cast<double>(kStart) + 50.0;
    BeatGrid grid;
    std::int64_t at = kStart;
    (void)drive(
        grid, [&](std::int64_t when) { return onGrid(when, bpm, zero); }, at, kStart + 3 * kSecond);
    REQUIRE(grid.state().locked);
    GridReading stopped;
    stopped.at = at;
    stopped.stopped = true;
    CHECK_FALSE(grid.advance(stopped).has_value());
    CHECK(grid.state().noSignal);
    CHECK_FALSE(grid.state().locked);
    CHECK_FALSE(grid.state().acquired);
    // And it stays let go of while the source says so, however long.
    for (int i = 1; i < 3000; ++i) {
        stopped.at += 1000;
        CHECK_FALSE(grid.advance(stopped).has_value());
    }
}

TEST_CASE("a followed grid that jumps is followed from where it lands, never back",
          "[timing][follow]") {
    // A peer forcing a beat, a phase moved: the grid does not run there, it is there. No beat is
    // sent across the jump and none twice, and the count never goes back — the output thread's
    // scheduler starts afresh when it does.
    const double bpm = 120.0;
    const double period = periodOf(bpm);
    const double zero = static_cast<double>(kStart) + 700.0;
    const double jumpBy = GENERATE(-4.0, -1.5, -0.6, -0.2, -0.1, 0.3, 3.0, 17.25);
    INFO("jumped by " << jumpBy << " beats just after beat 10");
    const auto jumpAt = static_cast<std::int64_t>(zero + 10.05 * period);
    BeatGrid grid;
    const Source source = [&](std::int64_t at) {
        return onGrid(at, bpm, at < jumpAt ? zero : zero - jumpBy * period);
    };
    const std::vector<Sent> sent = driveFrom(grid, source, kStart, jumpAt + 6 * kSecond);
    REQUIRE(sent.size() > 20);
    for (std::size_t i = 1; i < sent.size(); ++i) {
        INFO("beat " << i);
        CHECK(sent[i].beat.state.beats > sent[i - 1].beat.state.beats);
        CHECK(static_cast<double>(sent[i].beat.hostMicros - sent[i - 1].beat.hostMicros) >=
              BeatGrid::kSameBeat * period);
    }
    // After the jump, on the new grid.
    for (const Sent& one : sent) {
        if (one.readAt > jumpAt + kSecond) {
            const double position = positionOf(one.beat, bpm, zero - jumpBy * period);
            CHECK(position == Approx(std::round(position)).margin(1e-4));
        }
    }
}

TEST_CASE("a source that comes to say where its bars are takes its own bar", "[timing][follow]") {
    // A MIDI clock ticking with no Start, then Start: from it, the clock's own count is the bar.
    const double bpm = 120.0;
    const double period = periodOf(bpm);
    const double startAt = static_cast<double>(kStart) + 3.3 * kSecond;
    BeatGrid grid;
    const Source source = [&](std::int64_t at) {
        // Before Start, beats counted from somewhere else entirely; from it, from Start.
        const bool started = static_cast<double>(at) >= startAt;
        return onGrid(at, bpm, started ? startAt : startAt - 0.37 * period, started);
    };
    const std::vector<Sent> sent = driveFrom(grid, source, kStart, kStart + 8 * kSecond);
    std::size_t afterStart = 0;
    for (const Sent& one : sent) {
        if (static_cast<double>(one.readAt) < startAt) {
            CHECK(one.beat.event.beatInBar == 0);
            continue;
        }
        const double position = positionOf(one.beat, bpm, startAt);
        const auto whole = std::llround(position);
        CHECK(position == Approx(static_cast<double>(whole)).margin(1e-5));
        CHECK(one.beat.event.beatInBar == static_cast<std::uint32_t>(whole % 4 + 1));
        ++afterStart;
    }
    CHECK(afterStart >= 8);
}

TEST_CASE("a source that places its bar again takes it back from a DOWNBEAT", "[timing][follow]") {
    // A MIDI clock running from a Start, a DOWNBEAT that moves the bar, then the DJ's Start again:
    // Start means bar 1, whatever was pressed before it.
    const double bpm = 120.0;
    const double period = periodOf(bpm);
    const double zero = static_cast<double>(kStart) + 250.0;
    const double againAt = zero + 12.6 * period;
    const Source source = [&](std::int64_t at) {
        const bool again = static_cast<double>(at) >= againAt;
        GridReading reading = onGrid(at, bpm, again ? againAt : zero);
        reading.placements = again ? 2U : 1U;
        return reading;
    };
    BeatGrid grid;
    std::int64_t at = kStart;
    (void)drive(grid, source, at, static_cast<std::int64_t>(zero + 6.8 * period));
    // Beat 7, the fourth of its bar, made bar 1.
    grid.snapDownbeat();
    const std::vector<Sent> moved = drive(grid, source, at, static_cast<std::int64_t>(againAt));
    REQUIRE(moved.size() >= 5);
    CHECK(moved[0].beat.event.beatInBar == 1);
    CHECK(positionOf(moved[0].beat, bpm, zero) == Approx(7.0).margin(1e-5));
    CHECK(moved[4].beat.event.beatInBar == 1); // beat 11

    const std::vector<Sent> after = drive(grid, source, at, at + 6 * kSecond);
    REQUIRE(after.size() >= 8);
    for (const Sent& one : after) {
        const double position = positionOf(one.beat, bpm, againAt);
        const auto whole = std::llround(position);
        CHECK(position == Approx(static_cast<double>(whole)).margin(1e-5));
        CHECK(one.beat.event.beatInBar == static_cast<std::uint32_t>(whole % 4 + 1));
    }
}

TEST_CASE("the rate the followed beats go out at is measured as the tracker measures its own",
          "[timing][follow]") {
    const double bpm = 140.0;
    const double zero = static_cast<double>(kStart) + 10.0;
    BeatGrid grid;
    std::int64_t at = kStart;
    const std::vector<Sent> sent = drive(
        grid, [&](std::int64_t when) { return onGrid(when, bpm, zero); }, at, kStart + 6 * kSecond);
    REQUIRE(sent.size() > BeatGrid::kBeatRateGaps + 1);
    // Nothing until there are gaps enough.
    CHECK(sent[BeatGrid::kBeatRateGaps - 1].beat.state.beatsBpm == 0.0);
    CHECK(sent[BeatGrid::kBeatRateGaps].beat.state.beatsBpm == Approx(bpm).epsilon(1e-5));
    // And nothing once the next is two beats late — the source gone and let go of.
    GridReading stopped;
    stopped.at = sent.back().beat.hostMicros + 3 * static_cast<std::int64_t>(periodOf(bpm));
    REQUIRE(stopped.at >= at);
    stopped.stopped = true;
    (void)grid.advance(stopped);
    CHECK(grid.state().beatsBpm == 0.0);
}

TEST_CASE("a DOWNBEAT pressed with nothing to follow is dropped, not kept for later",
          "[timing][follow]") {
    // Pressed before a source is there, a DOWNBEAT points at no beat: kept, it would put bar 1
    // wherever the source happened to come in, seconds after the press.
    const double bpm = 120.0;
    const double period = periodOf(bpm);
    const std::int64_t arrives = kStart + 2 * kSecond;
    // Arriving half a beat before the third beat of its bar: a press kept until then would make
    // that beat bar 1.
    const double zero = static_cast<double>(arrives) - 1.5 * period;
    BeatGrid grid;
    const Source source = [&](std::int64_t at) {
        return at < arrives ? absent(at) : onGrid(at, bpm, zero);
    };
    std::int64_t at = kStart;
    (void)drive(grid, source, at, kStart + kSecond);
    grid.snapDownbeat();
    const std::vector<Sent> sent = drive(grid, source, at, arrives + 3 * kSecond);
    REQUIRE(sent.size() >= 4);
    // The source's own bar, untouched.
    for (const Sent& one : sent) {
        const auto whole = std::llround(positionOf(one.beat, bpm, zero));
        CHECK(one.beat.event.beatInBar == static_cast<std::uint32_t>(whole % 4 + 1));
        CHECK_FALSE(one.beat.event.snapped);
    }
    CHECK(grid.state().barsDeclared == 0);
}

TEST_CASE("a bar placed a little behind the reading that first sees it still has its downbeat sent",
          "[timing][follow]") {
    // A MIDI clock ticking with no Start, then a placement read only once the grid is already past
    // where it put the song: a Start and its first tick, which a DAW sends together.
    const double bpm = 120.0;
    const double period = periodOf(bpm);
    // Between two readings, as a tick lands: the first reading of the placed grid is 0.6 ms on.
    const double placedMoment = static_cast<double>(kStart) + 3.3 * kSecond + 400.0;
    struct Placement {
        double placedAt;
        double readLate; // how far past the placing tick the first reading is, in beats
        bool sent;
    };
    const Placement placement = GENERATE(Placement{0.0, 0.0, true}, // Start
                                         Placement{2.0, 0.0, true}, // Continue on a beat
                                         // A Song Position a quarter into beat 1: beat 1 was
                                         // before it, and is nobody's to send.
                                         Placement{1.25, 0.0, false},
                                         // Read a third of a beat late: long gone.
                                         Placement{0.0, 0.33, false});
    INFO("placed at " << placement.placedAt << ", read " << placement.readLate << " beat late");
    const double readFrom = placedMoment + placement.readLate * period;
    // The placed grid: song beat `placedAt` at `placedMoment`.
    const double zero = placedMoment - placement.placedAt * period;
    const Source source = [&](std::int64_t at) {
        if (static_cast<double>(at) < readFrom) {
            // Its last beat over half a beat before the placement: the placed beat is one of its
            // own, not one declared just after it.
            return onGrid(at, bpm, placedMoment - 0.55 * period, false);
        }
        GridReading reading = onGrid(at, bpm, zero);
        reading.placements = 1;
        reading.placedAt = placement.placedAt;
        return reading;
    };
    BeatGrid grid;
    std::int64_t at = kStart;
    (void)drive(grid, source, at, static_cast<std::int64_t>(placedMoment) - 1000);
    const std::vector<Sent> sent = drive(grid, source, at, at + 3 * kSecond);
    REQUIRE_FALSE(sent.empty());
    const double first = positionOf(sent[0].beat, bpm, zero);
    if (placement.sent) {
        // On its own moment, worked back from the reading, and the bar's first.
        CHECK(first == Approx(placement.placedAt).margin(1e-5));
        CHECK(sent[0].beat.event.beatInBar ==
              static_cast<std::uint32_t>(std::llround(placement.placedAt)) % 4 + 1);
    } else {
        CHECK(first == Approx(std::floor(placement.placedAt) + 1.0).margin(1e-5));
    }
    // Every beat after it once, on the placed grid.
    for (std::size_t i = 1; i < sent.size(); ++i) {
        CHECK(positionOf(sent[i].beat, bpm, zero) ==
              Approx(first + static_cast<double>(i)).margin(1e-5));
    }
}

TEST_CASE("a reading at a tempo no source has is no reading", "[timing][follow]") {
    // A MIDI clock's delivery stall came out as hundreds of thousands
    // of BPM, and a beat went out every millisecond. Whatever a source says, a tempo outside
    // Link's own range, or a position that is no number, is followed as a source gone: coasted,
    // and then let go of.
    const double bpm = 120.0;
    const double zero = static_cast<double>(kStart) + 400.0;
    const auto wildAt = static_cast<std::int64_t>(zero + 8.3 * periodOf(bpm));
    const double wild = GENERATE(500000.0, 1000.0, 19.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN());
    INFO("a reading at " << wild << " BPM");
    const Source source = [&](std::int64_t at) {
        if (at < wildAt) {
            return onGrid(at, bpm, zero);
        }
        GridReading reading = onGrid(at, bpm, zero);
        reading.bpm = wild;
        reading.beat = std::isnan(wild) ? wild : reading.beat * wild / bpm;
        return reading;
    };
    BeatGrid grid;
    std::int64_t at = kStart;
    (void)drive(grid, source, at, wildAt);
    REQUIRE(grid.state().locked);
    // Coasted at the tempo it had: a beat a period apart, no faster.
    const std::vector<Sent> coasted = drive(grid, source, at, wildAt + 3 * kSecond);
    CHECK(grid.coasting());
    REQUIRE(coasted.size() >= 5);
    for (std::size_t i = 1; i < coasted.size(); ++i) {
        CHECK(static_cast<double>(coasted[i].beat.hostMicros - coasted[i - 1].beat.hostMicros) ==
              Approx(periodOf(bpm)).margin(2.0));
        CHECK(coasted[i].beat.event.bpm == bpm);
    }
    // And let go of once the coast is over.
    (void)drive(grid, source, at, wildAt + 5 * kSecond);
    CHECK(grid.state().noSignal);
    CHECK(drive(grid, source, at, wildAt + 7 * kSecond).empty());
}

TEST_CASE("no two followed beats go out closer than 60 ms", "[timing][follow]") {
    // ×2 twice over a 300 BPM source is a beat every 50 ms. The operator's call: never closer than
    // 60 ms, past any grid a source and the octave buttons make that is still a beat.
    const double bpm = 300.0;
    const double zero = static_cast<double>(kStart) + 900.0;
    BeatGrid grid;
    grid.redouble();
    grid.redouble();
    const std::vector<Sent> sent = driveFrom(
        grid, [&](std::int64_t at) { return onGrid(at, bpm, zero); }, kStart, kStart + 3 * kSecond);
    REQUIRE(sent.size() >= 20);
    for (std::size_t i = 1; i < sent.size(); ++i) {
        CHECK(sent[i].beat.hostMicros - sent[i - 1].beat.hostMicros >= BeatGrid::kLeastBeatMicros);
    }
}

TEST_CASE("a followed grid moved back across the beat just sent does not send it again",
          "[timing][follow]") {
    // an earlier review: a Link peer that moves the phase back by 0.4 to 0.5 of a beat — another
    // computer's old takt4 snapping at its START — made the beat just sent go out again, past the 0.4 of
    // a beat that keeps a beat moved a little from going twice. Its number on the grid says it is
    // the same beat, however long ago it went.
    const double bpm = 120.0;
    const double period = periodOf(bpm);
    const double zero = static_cast<double>(kStart) + 600.0;
    const double back = GENERATE(0.42, 0.45, 0.49, 0.8);
    INFO("moved back " << back << " of a beat just after beat 10");
    const auto movedAt = static_cast<std::int64_t>(zero + 10.05 * period);
    const Source source = [&](std::int64_t at) {
        return onGrid(at, bpm, at < movedAt ? zero : zero + back * period);
    };
    BeatGrid grid;
    const std::vector<Sent> sent = driveFrom(grid, source, kStart, movedAt + 4 * kSecond);
    REQUIRE(sent.size() >= 18);
    std::size_t beatTen = 0;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        const double position =
            positionOf(sent[i].beat, bpm, sent[i].readAt < movedAt ? zero : zero + back * period);
        INFO("beat " << i << " at " << position);
        CHECK(position == Approx(std::round(position)).margin(1e-4));
        beatTen += std::llround(position) == 10 ? 1 : 0;
        if (i > 0) {
            // Every beat the next of its bar: none repeated.
            CHECK(sent[i].beat.event.beatInBar == sent[i - 1].beat.event.beatInBar % 4 + 1);
        }
    }
    CHECK(beatTen == 1);
}

TEST_CASE("halving or doubling a followed grid forgets the rate the beats went out at",
          "[timing][follow]") {
    // an earlier review: the beats' own rate is the median of the last eight gaps, and ÷2 kept the
    // old grid's — so the window read 128 against a published 64 and told the operator to press
    // ×2, for up to five seconds after pressing ÷2. Forgotten at the press, as the tracker forgets
    // its own, and measured afresh on the new grid.
    const double bpm = 128.0;
    const double zero = static_cast<double>(kStart) + 300.0;
    const Source source = [&](std::int64_t at) { return onGrid(at, bpm, zero); };
    BeatGrid grid;
    std::int64_t at = kStart;
    (void)drive(grid, source, at, kStart + 6 * kSecond);
    REQUIRE(grid.state().beatsBpm == Approx(bpm).epsilon(1e-4));
    const bool halving = GENERATE(true, false);
    INFO((halving ? "halved" : "doubled"));
    if (halving) {
        grid.halve();
    } else {
        grid.redouble();
    }
    const double published = halving ? bpm / 2.0 : bpm * 2.0;
    // Never the old rate against the new tempo: unknown until there are gaps enough, then the new.
    const std::vector<Sent> after = drive(grid, source, at, at + 10 * kSecond);
    REQUIRE(after.size() > BeatGrid::kBeatRateGaps + 1);
    for (const Sent& one : after) {
        CHECK((one.beat.state.beatsBpm == 0.0 ||
               one.beat.state.beatsBpm == Approx(published).epsilon(1e-4)));
    }
    CHECK(grid.state().beatsBpm == Approx(published).epsilon(1e-4));
}

TEST_CASE("a followed tempo that jumps forgets the rate the beats went out at",
          "[timing][follow]") {
    // A DJ halving the Link session: the gaps before it are the old tempo's.
    const double zero = static_cast<double>(kStart) + 300.0;
    const auto jumpAt = static_cast<std::int64_t>(zero + 12.02 * periodOf(128.0));
    const double newZero = static_cast<double>(jumpAt) - 0.02 * periodOf(64.0);
    const Source source = [&](std::int64_t at) {
        return at < jumpAt ? onGrid(at, 128.0, zero) : onGrid(at, 64.0, newZero);
    };
    BeatGrid grid;
    std::int64_t at = kStart;
    (void)drive(grid, source, at, jumpAt);
    REQUIRE(grid.state().beatsBpm == Approx(128.0).epsilon(1e-4));
    const std::vector<Sent> after = drive(grid, source, at, at + 12 * kSecond);
    REQUIRE(after.size() > BeatGrid::kBeatRateGaps + 1);
    for (const Sent& one : after) {
        CHECK((one.beat.state.beatsBpm == 0.0 ||
               one.beat.state.beatsBpm == Approx(64.0).epsilon(1e-4)));
    }
}
