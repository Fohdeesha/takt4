#include "core/engine/beat_source.hpp"
#include "core/engine/control.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"

#include "support/fake_beat_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using takt4::engine::BeatSourceSwitch;
using takt4::engine::Command;
using takt4::engine::EngineBeat;
using takt4::output::OutputRunner;
using takt4::output::Transports;
using takt4::testing::FakeBeatSource;

namespace {

/// The `n`th beat of a bar of four at `bpm`, stamped `hostMicros` on Link's clock. Allowed to
/// fire (`acquired`) and **not locked**, so the output thread fires it as it is heard and predicts
/// nothing from it — every beat a test counts is one it pushed.
EngineBeat heardBeat(std::uint64_t n, double bpm, std::int64_t hostMicros) {
    EngineBeat beat;
    beat.event.bpm = bpm;
    beat.event.gridBpm = bpm;
    beat.event.beatsPerBar = 4;
    beat.event.beatInBar = static_cast<std::uint32_t>((n - 1) % 4 + 1);
    beat.event.downbeat = beat.event.beatInBar == 1;
    beat.event.acquired = true;
    beat.state.bpm = bpm;
    beat.state.gridBpm = bpm;
    beat.state.acquired = true;
    beat.state.beatsPerBar = 4;
    beat.state.beatInBar = beat.event.beatInBar;
    beat.state.beats = n;
    beat.hostMicros = hostMicros;
    return beat;
}

/// Waits until the runner's transports have counted `beats`, or gives up.
void waitForBeats(const OutputRunner& runner, std::uint64_t beats) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < until && runner.transports().beats() < beats) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

/// Waits until the output thread has made `count` more whole rounds from now.
void waitForRounds(const OutputRunner& runner, std::uint64_t count) {
    const std::uint64_t target = runner.rounds() + count + 1;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < until && runner.rounds() < target) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(runner.rounds() >= target);
}

} // namespace

TEST_CASE("the beat source switch hands everything to the source selected and nothing to the other",
          "[engine][follow]") {
    FakeBeatSource tracker;
    FakeBeatSource followed;
    followed.start(); // a run number of its own
    BeatSourceSwitch beats(tracker);
    REQUIRE(beats.isSelected(tracker));

    REQUIRE(tracker.push(heardBeat(1, 120.0, 1)));
    REQUIRE(followed.push(heardBeat(7, 90.0, 1)));
    CHECK(beats.state().bpm == 120.0);
    CHECK(beats.runNumber() == tracker.runNumber());
    CHECK(beats.post(Command::halve()));
    CHECK(tracker.posted() == std::vector<Command::Kind>{Command::Kind::Halve});
    CHECK(followed.posted().empty());

    EngineBeat out;
    REQUIRE(beats.popBeat(out));
    CHECK(out.state.bpm == 120.0);
    CHECK_FALSE(beats.popBeat(out)); // the tracker's ring is empty, and the other is not drained
    CHECK(followed.waiting() == 1);

    beats.select(followed);
    CHECK(beats.isSelected(followed));
    CHECK(beats.state().bpm == 90.0);
    CHECK(beats.runNumber() == followed.runNumber());
    REQUIRE(beats.popBeat(out));
    CHECK(out.state.beats == 7);
    CHECK(beats.post(Command::redouble()));
    CHECK(followed.posted() == std::vector<Command::Kind>{Command::Kind::Redouble});
    CHECK(tracker.posted().size() == 1); // nothing more reached the one not selected

    // A refusal is the source's to give, and reaches the caller.
    followed.setAccepts(false);
    CHECK_FALSE(beats.post(Command::seedTempo(128.0)));
}

TEST_CASE("the output thread fires the selected source's beats and drops what a run before left",
          "[output][follow]") {
    // The seam the window stands on: the output thread holds the switch, never a source, so the
    // beats it fires come from whichever source is selected — and only from that one. A source
    // selected anew may still have beats on its ring from a run of its own before; its start
    // makes a new run, and those are dropped rather than fired as the new run's.
    FakeBeatSource tracker;
    FakeBeatSource followed;
    BeatSourceSwitch beats(tracker);
    OutputRunner runner(beats, Transports::Config{});
    runner.start();
    const auto linkNow = [&runner] { return runner.transports().link().now().count(); };

    REQUIRE(tracker.push(heardBeat(1, 120.0, linkNow())));
    waitForBeats(runner, 1);
    CHECK(runner.transports().beats() == 1);

    // Pushed to the source not selected: nobody drains it, and nothing fires.
    REQUIRE(followed.push(heardBeat(1, 126.0, linkNow())));
    waitForRounds(runner, 5);
    CHECK(runner.transports().beats() == 1);
    CHECK(followed.waiting() == 1);

    // STOP, the other source selected, START: what the window does when the operator changes
    // where the timing comes from.
    runner.setTracking(false);
    REQUIRE(runner.sync());
    beats.select(followed);
    followed.start();
    runner.setTracking(true);
    REQUIRE(runner.sync());
    waitForRounds(runner, 5);
    CHECK(followed.waiting() == 0); // drained by the output thread, the ring's one consumer
    CHECK(runner.transports().beats() == 1); // and dropped: it was the run before's

    REQUIRE(followed.push(heardBeat(1, 126.0, linkNow())));
    waitForBeats(runner, 2);
    CHECK(runner.transports().beats() == 2);

    // And now the tracker is the one nobody drains.
    REQUIRE(tracker.push(heardBeat(2, 120.0, linkNow())));
    waitForRounds(runner, 5);
    CHECK(runner.transports().beats() == 2);
    CHECK(tracker.waiting() == 1);

    runner.stop();
    CHECK(runner.errors() == 0);
}
