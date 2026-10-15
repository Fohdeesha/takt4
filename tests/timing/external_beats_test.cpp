#include "core/engine/control.hpp"
#include "core/timing/beat_grid.hpp"
#include "core/timing/external_beats.hpp"

#include "support/fake_grid_source.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

using Catch::Approx;
using takt4::engine::Command;
using takt4::engine::EngineBeat;
using takt4::testing::FakeGridSource;
using takt4::timing::BeatGrid;
using takt4::timing::ExternalBeats;

namespace {

std::int64_t steadyMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Waits until `done` or two seconds have gone.
template <typename Done>
bool waitFor(Done done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < until) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return done();
}

} // namespace

TEST_CASE("a followed grid's beats reach the ring stamped with their own moments, never early",
          "[timing][follow]") {
    // The timing worker on its own thread, reading a source once a millisecond. Each beat it
    // pushes carries its moment on the source's grid exactly, and none is on the ring before that
    // moment has come: a beat is sent as it passes, and the output thread predicts from there.
    constexpr double kBpm = 240.0; // a beat every quarter second
    const double zero = static_cast<double>(steadyMicros()) + 100'000.0;
    FakeGridSource source(steadyMicros, kBpm, zero);
    ExternalBeats beats;
    beats.start(source, BeatGrid::Options{});
    CHECK(beats.running());

    std::vector<EngineBeat> taken;
    const auto take = [&] {
        EngineBeat beat;
        while (beats.popBeat(beat)) {
            CHECK(steadyMicros() >= beat.hostMicros); // not before its moment
            taken.push_back(beat);
        }
    };
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{1600};
    while (std::chrono::steady_clock::now() < until) {
        take();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    beats.stop();
    CHECK_FALSE(beats.running());
    take(); // what the last rounds pushed

    REQUIRE(taken.size() >= 5);
    const double period = 60e6 / kBpm;
    const double first =
        std::round((static_cast<double>(taken.front().hostMicros) - zero) / period);
    for (std::size_t i = 0; i < taken.size(); ++i) {
        INFO("beat " << i);
        const double moment = zero + (first + static_cast<double>(i)) * period;
        CHECK(std::abs(static_cast<double>(taken[i].hostMicros) - moment) <= 1.0);
        CHECK(taken[i].run == beats.runNumber());
        CHECK(taken[i].event.acquired);
        CHECK(taken[i].event.gridBpm == kBpm);
        if (i > 0) {
            CHECK(taken[i].state.beats == taken[i - 1].state.beats + 1);
        }
    }
    CHECK(beats.beatsSent() == taken.size());
    CHECK(beats.beatsDropped() == 0);
    CHECK(beats.rounds() > 800); // a round a millisecond, give or take a loaded machine
    CHECK(beats.state().locked);
    CHECK(beats.state().confidence == 1.0);
    CHECK(beats.intensity().onsets == 0);
    CHECK(source.begun() == 1);
    CHECK(source.ended() == 1);
    CHECK(source.beatsPerBar() == 4);
}

TEST_CASE("a followed grid refuses a tap, a pin and a tempo hold, and counts them",
          "[timing][follow]") {
    // The tempo is the source's, and the lock is the source being there: what the operator
    // cannot change is said to be refused rather than silently ignored. ÷2, ×2 and DOWNBEAT are
    // the operator's to press; the tracker's settings are taken and touch nothing here.
    ExternalBeats beats;
    CHECK_FALSE(beats.post(Command::seedTempo(128.0)));
    CHECK_FALSE(beats.post(Command::setLockPinned(true)));
    CHECK_FALSE(beats.post(Command::holdTempo(128.0)));
    CHECK(beats.commandsRefused() == 3);
    CHECK(beats.post(Command::halve()));
    CHECK(beats.post(Command::redouble()));
    CHECK(beats.post(Command::snapDownbeat()));
    CHECK(beats.post(Command::setTempoOptions({})));
    CHECK(beats.commandsRefused() == 3);
}

TEST_CASE("a followed grid's presses take effect while it runs, and not from before it starts",
          "[timing][follow]") {
    const double zero = static_cast<double>(steadyMicros());
    FakeGridSource source(steadyMicros, 120.0, zero);
    ExternalBeats beats;
    // A ÷2 from a control surface between sets is not for the set about to start.
    REQUIRE(beats.post(Command::halve()));
    beats.start(source, BeatGrid::Options{});
    REQUIRE(waitFor([&] { return beats.state().locked; }));
    CHECK(beats.state().octaveShift == 0);
    CHECK(beats.state().bpm == Approx(120.0));

    REQUIRE(beats.post(Command::halve()));
    CHECK(waitFor([&] { return beats.state().octaveShift == -1; }));
    CHECK(beats.state().bpm == Approx(60.0));
    REQUIRE(beats.post(Command::redouble()));
    REQUIRE(beats.post(Command::redouble()));
    CHECK(waitFor([&] { return beats.state().octaveShift == 1; }));
    CHECK(beats.state().gridBpm == Approx(240.0));
    beats.stop();
}

TEST_CASE("each start of a followed grid is a run of its own", "[timing][follow]") {
    const double zero = static_cast<double>(steadyMicros());
    FakeGridSource source(steadyMicros, 600.0, zero); // ten beats a second
    ExternalBeats beats;
    beats.start(source, BeatGrid::Options{3, 4.0});
    const std::uint64_t first = beats.runNumber();
    REQUIRE(waitFor([&] { return beats.beatsSent() >= 3; }));
    beats.stop();
    CHECK(source.begun() == 1);
    CHECK(source.ended() == 1);
    CHECK(source.beatsPerBar() == 3);

    beats.start(source, BeatGrid::Options{});
    CHECK(beats.runNumber() == first + 1);
    // What the last run left on the ring is still there for its one consumer, stamped with that
    // run — and the new run counts from nothing.
    EngineBeat beat;
    REQUIRE(beats.popBeat(beat));
    CHECK(beat.run == first);
    CHECK(beat.state.beats == 1);
    beats.stop();
    CHECK(source.begun() == 2);
    CHECK(source.ended() == 2);
}

TEST_CASE("a followed grid that goes is coasted on the timing thread too", "[timing][follow]") {
    const double zero = static_cast<double>(steadyMicros());
    FakeGridSource source(steadyMicros, 120.0, zero);
    ExternalBeats beats;
    beats.start(source, BeatGrid::Options{});
    REQUIRE(waitFor([&] { return beats.state().locked; }));
    CHECK_FALSE(beats.coasting());
    source.setPresent(false);
    CHECK(waitFor([&] { return beats.coasting(); }));
    CHECK(beats.state().locked); // the lock stays while it coasts
    source.setStopped(true);
    CHECK(waitFor([&] { return beats.state().noSignal; }));
    CHECK_FALSE(beats.state().locked);
    beats.stop();
}
