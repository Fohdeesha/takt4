#include "core/engine/beat_engine.hpp"
#include "core/output/beat_scheduler.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

using Catch::Approx;
using takt4::engine::EngineBeat;
using takt4::output::BeatScheduler;
using takt4::output::ScheduledBeat;

namespace {

/// A beat the tracker called: the `n`th, in a bar of four, at `bpm`.
EngineBeat beat(std::uint64_t n, double bpm, bool locked = true) {
    EngineBeat out;
    out.event.bpm = bpm;
    out.event.locked = locked;
    out.event.confidence = 0.9;
    out.event.beatsPerBar = 4;
    out.event.beatInBar = static_cast<std::uint32_t>((n - 1) % 4 + 1);
    out.event.downbeat = out.event.beatInBar == 1;
    out.state.bpm = bpm;
    out.state.calledBpm = bpm;
    out.state.beatDivisor = 1;
    out.state.locked = locked;
    out.state.beats = n;
    out.state.bars = (n - 1) / 4 + 1;
    out.state.beatInBar = out.event.beatInBar;
    out.state.beatsPerBar = 4;
    out.hostMicros = 1; // stamped; the scheduler is handed the moment itself
    return out;
}

/// One fire, as the output thread would make it: which beat, when in the music, and when sent.
struct Fire {
    ScheduledBeat beat;
    double sentAt = 0.0;
};

/// Runs the scheduler the way the output thread does — 1 ms rounds, beats heard a pipeline
/// after their moment — over a steady grid, and returns every fire.
std::vector<Fire> run(double bpm, double lead, std::size_t count, double pipeline,
                      double jitter = 0.0, const std::vector<std::size_t>& missed = {}) {
    BeatScheduler scheduler;
    std::vector<Fire> fires;
    const double period = 60.0 / bpm;
    std::mt19937 random(7);
    std::vector<double> moments;
    for (std::size_t k = 0; k < count; ++k) {
        const double noise = (static_cast<double>(random()) / 4294967296.0 * 2.0 - 1.0) * jitter;
        moments.push_back(1.0 + static_cast<double>(k) * period + noise);
    }
    std::size_t next = 0;
    std::uint64_t called = 0;
    // Long enough after the last beat for the one prediction past it, and not the second.
    const double end = moments.back() + 0.5;
    for (std::size_t round = 0;; ++round) {
        const double now = static_cast<double>(round) * 0.001;
        if (now > end) {
            break;
        }
        while (next < count && moments[next] + pipeline <= now) {
            bool skip = false;
            for (const std::size_t m : missed) {
                skip = skip || m == next;
            }
            if (!skip) {
                ++called;
                if (auto heard = scheduler.heard(beat(called, bpm), moments[next], now, 0.35)) {
                    fires.push_back({*heard, now});
                }
            }
            ++next;
        }
        while (auto due = scheduler.due(now, lead, 0.35)) {
            fires.push_back({*due, now});
        }
    }
    return fires;
}

} // namespace

TEST_CASE("a locked beat is fired ahead of its moment by the lead, and only once",
          "[output][scheduler]") {
    // The audit's H4: a negative offset has to put a beat's cue *before* that beat, which a
    // beat heard 60 ms after its moment cannot do. So while locked the next beat is fired on a
    // prediction, 200 ms ahead here, and the beat heard afterwards is not fired again.
    const double bpm = 128.0;
    const std::vector<Fire> fires = run(bpm, -0.200, 64, 0.060, 0.004);
    // One fire for every beat, the first heard and the rest predicted. The last is predicted
    // and never heard, as it would be at the end of a record.
    REQUIRE(fires.size() == 65);
    CHECK_FALSE(fires.front().beat.predicted);
    for (std::size_t i = 1; i < fires.size(); ++i) {
        INFO("fire " << i);
        CHECK(fires[i].beat.predicted);
        // Sent the lead ahead of its moment, to the round.
        CHECK(fires[i].sentAt == Approx(fires[i].beat.moment - 0.200).margin(0.0011));
        // Counted on, never repeated: what "every fourth beat" counts.
        CHECK(fires[i].beat.beats == fires[i - 1].beat.beats + 1);
        CHECK(fires[i].beat.event.beatInBar == (fires[i - 1].beat.event.beatInBar % 4) + 1);
        CHECK(fires[i].beat.event.downbeat == (fires[i].beat.event.beatInBar == 1));
    }
    // The bars count on as the tracker counts them.
    CHECK(fires.back().beat.bars == 17);
}

TEST_CASE("with no lead a locked beat is fired on its moment, not a pipeline late",
          "[output][scheduler]") {
    const std::vector<Fire> fires = run(128.0, 0.0, 32, 0.060);
    REQUIRE(fires.size() == 33);
    for (std::size_t i = 1; i < fires.size(); ++i) {
        CHECK(fires[i].beat.predicted);
        CHECK(fires[i].sentAt == Approx(fires[i].beat.moment).margin(0.0011));
    }
}

TEST_CASE("a hunting tracker's beats fire as they are heard", "[output][scheduler]") {
    // Nothing to predict with: the tempo is the hunt's and flips octaves. A beat goes the
    // moment it is heard, which is as early as it can.
    BeatScheduler scheduler;
    const double period = 60.0 / 128.0;
    std::size_t fired = 0;
    for (std::uint64_t n = 1; n <= 16; ++n) {
        const double moment = 1.0 + static_cast<double>(n) * period;
        const auto heard = scheduler.heard(beat(n, 128.0, false), moment, moment + 0.06, 0.35);
        REQUIRE(heard.has_value());
        CHECK_FALSE(heard->predicted);
        CHECK(heard->moment == Approx(moment));
        ++fired;
        CHECK_FALSE(scheduler.due(moment + 0.06 + period, -0.2, 0.35).has_value());
    }
    CHECK(fired == 16);
    CHECK(scheduler.predictedFires() == 0);
}

TEST_CASE("a beat with no timeline fires as it arrives, however close to the last",
          "[output][scheduler]") {
    // Offline, and in tests that feed audio faster than it plays, beats arrive a millisecond
    // apart with no host-clock stamp. Each is fired, and nothing is predicted from any of them.
    BeatScheduler scheduler;
    for (std::uint64_t n = 1; n <= 20; ++n) {
        const double now = 0.001 * static_cast<double>(n);
        const auto heard = scheduler.heard(beat(n, 128.0), std::nullopt, now, 0.35);
        REQUIRE(heard.has_value());
        CHECK(heard->moment == Approx(now));
        CHECK_FALSE(scheduler.due(now + 10.0, -0.5, 100.0).has_value());
    }
    CHECK(scheduler.heardFires() == 20);
}

TEST_CASE("a backlog of stale beats is not fired in a burst", "[output][scheduler]") {
    // The audit's M15: after the model worker stalls, the beats it owed arrive together, and
    // firing them all as "now" was a burst of cues for music long gone.
    BeatScheduler scheduler;
    const double period = 60.0 / 128.0;
    const double now = 10.0;
    std::size_t fired = 0;
    for (std::uint64_t n = 1; n <= 4; ++n) {
        const double moment = now - 2.0 + static_cast<double>(n) * period; // 1.53 s to 0.13 s old
        if (scheduler.heard(beat(n, 128.0), moment, now, 0.35)) {
            ++fired;
        }
    }
    // Only the one heard in time — the last, 0.13 s old.
    CHECK(fired == 1);
    CHECK(scheduler.stale() == 3);
}

TEST_CASE("a missed beat is carried over, and the prediction stops after two",
          "[output][scheduler]") {
    // The tracker misses a beat in a quiet passage: the outputs keep the beat through it.
    const std::vector<Fire> carried = run(128.0, -0.05, 24, 0.060, 0.0, {10});
    CHECK(carried.size() == 25); // every beat, the missed one included, and the one after the end
    for (std::size_t i = 1; i < carried.size(); ++i) {
        CHECK(carried[i].beat.beats == carried[i - 1].beat.beats + 1);
        CHECK(carried[i].beat.moment - carried[i - 1].beat.moment ==
              Approx(60.0 / 128.0).margin(1e-6));
    }

    SECTION("but a tracker gone quiet stops being predicted after two beats") {
        BeatScheduler scheduler;
        (void)scheduler.heard(beat(1, 128.0), 1.0, 1.06, 0.35);
        std::size_t predicted = 0;
        for (std::size_t round = 0; round < 5000; ++round) {
            const double now = 1.06 + 0.001 * static_cast<double>(round);
            while (scheduler.due(now, 0.0, 0.35)) {
                ++predicted;
            }
        }
        CHECK(predicted == BeatScheduler::kMaxAhead);
    }
}

TEST_CASE("a DOWNBEAT pressed on a beat already heard relabels the beats after it",
          "[output][scheduler]") {
    // The tracker's state changes between beats; the prediction counts from it.
    BeatScheduler scheduler;
    (void)scheduler.heard(beat(3, 128.0), 1.0, 1.06, 0.35); // beat 3 of the bar
    takt4::tracking::TempoState snapped = beat(3, 128.0).state;
    snapped.beatInBar = 1; // the operator says that one was the downbeat
    scheduler.restate(snapped);
    const auto next = scheduler.due(1.0 + 60.0 / 128.0, 0.0, 0.35);
    REQUIRE(next.has_value());
    CHECK(next->event.beatInBar == 2);
    CHECK_FALSE(next->event.downbeat);
}

TEST_CASE("below the confidence gate nothing is predicted", "[output][scheduler]") {
    BeatScheduler scheduler;
    EngineBeat held = beat(1, 128.0);
    held.state.holding = true;
    (void)scheduler.heard(held, 1.0, 1.06, 0.35);
    CHECK_FALSE(scheduler.due(3.0, 0.0, 10.0).has_value());
}

TEST_CASE("the beats are predicted at the spacing they arrive at, not at the tempo shown",
          "[output][scheduler]") {
    // ×2 doubles the number and cannot double the beats: the filter still calls them at the
    // tempo it hears. Predicting at sixty over the doubled number would fire a phantom beat
    // between every two real ones.
    EngineBeat doubled = beat(1, 256.0);
    doubled.state.calledBpm = 128.0;
    doubled.state.beatDivisor = 1;
    CHECK(takt4::output::beatSeconds(doubled.state, 256.0) == Approx(60.0 / 128.0));

    // A ÷2 divides the beats as well, so the spacing is the halved tempo's.
    EngineBeat halved = beat(1, 64.0);
    halved.state.calledBpm = 128.0;
    halved.state.beatDivisor = 2;
    CHECK(takt4::output::beatSeconds(halved.state, 64.0) == Approx(60.0 / 64.0));

    // A fold the music has not backed yet halves the number and leaves the beats alone.
    EngineBeat folded = beat(1, 87.0);
    folded.state.calledBpm = 174.0;
    folded.state.beatDivisor = 1;
    CHECK(takt4::output::beatSeconds(folded.state, 87.0) == Approx(60.0 / 174.0));

    BeatScheduler scheduler;
    (void)scheduler.heard(doubled, 1.0, 1.06, 0.35);
    const auto next = scheduler.due(2.0, 0.0, 10.0);
    REQUIRE(next.has_value());
    CHECK(next->moment == Approx(1.0 + 60.0 / 128.0));
}

TEST_CASE("a tracker that starts again starts the counts again", "[output][scheduler]") {
    BeatScheduler scheduler;
    for (std::uint64_t n = 1; n <= 8; ++n) {
        (void)scheduler.heard(beat(n, 128.0, false), static_cast<double>(n), static_cast<double>(n),
                              0.35);
    }
    // A Stop and a Start: the tracker's count is back at one.
    const auto first = scheduler.heard(beat(1, 128.0, false), 20.0, 20.0, 0.35);
    REQUIRE(first.has_value());
    CHECK(first->beats == 1);
}
