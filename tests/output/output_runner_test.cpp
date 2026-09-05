#include "core/audio/rates.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/state_space.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using takt4::audio::kHopSize;
using takt4::engine::BeatEngine;
using takt4::output::OutputRunner;
using takt4::output::Transports;

namespace {

const std::filesystem::path kTestData{TAKT4_TEST_DATA_DIR};

const takt4::model::ModelWeights& weights() {
    static const takt4::model::ModelWeights loaded = takt4::model::ModelWeights::fromFile(
        std::filesystem::path(TAKT4_WEIGHTS_DIR) / "generic.bin");
    return loaded;
}

const takt4::tracking::StateSpaceModel& stateSpace() {
    static const takt4::tracking::StateSpaceModel loaded =
        takt4::tracking::StateSpaceModel::fromFile(std::filesystem::path(TAKT4_STATESPACE_DIR) /
                                                   "default.bin");
    return loaded;
}

/// The committed excerpt, which `takt4-cli track` reports as "499 frames, 21 beats
/// (5 downbeats)" — so the numbers here are the tracker's own, not this test's invention.
const std::vector<float>& excerpt() {
    static const std::vector<float> samples =
        takt4::io::readWavFile(kTestData / "features" / "synthetic.wav").samples;
    return samples;
}

constexpr std::uint64_t kExpectedBeats = 21;
constexpr std::uint64_t kExpectedDownbeats = 5;

/// Feeds the excerpt through a stopped engine on the calling thread.
///
/// `step()` is the offline half of the same code path the inference thread takes, so the
/// beats that land on the ring are the ones the tracker really called. The runner drains
/// that ring from its own thread meanwhile, which is exactly `rt::SpscRing`'s contract:
/// this thread is the one producer, the runner is the one consumer.
void feedExcerpt(BeatEngine& engine) {
    const std::vector<float>& samples = excerpt();
    const std::size_t hops = samples.size() / kHopSize;
    for (std::size_t hop = 0; hop < hops; ++hop) {
        engine.processHop(samples.data() + hop * kHopSize, hop);
        (void)engine.step();
    }
}

} // namespace

TEST_CASE("the output thread drains every beat the tracker called", "[output]") {
    // The whole point of §4.2's output thread: beats reach the transports without the
    // caller's loop deciding when. Nothing is configured to send, so what is under test
    // is the draining and the counting rather than any one transport.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner(*engine, Transports::Config{});
    const Transports& transports = runner.transports();

    runner.start();
    CHECK(runner.running());
    feedExcerpt(*engine);
    runner.stop();

    CHECK_FALSE(runner.running());
    CHECK(transports.beats() == kExpectedBeats);
    CHECK(transports.downbeats() == kExpectedDownbeats);
    CHECK(engine->beatsDropped() == 0);
    CHECK(runner.errors() == 0);
    // The thread really was the one doing it, rather than everything falling out of
    // stop()'s final drain.
    INFO(runner.rounds() << " rounds");
    CHECK(runner.rounds() > 1);
}

TEST_CASE("stopping drains the beats that were still waiting", "[output]") {
    // The last beats of a set are still beats. Everything is produced before the runner
    // is ever started, so the ring is full of them and only stop()'s final drain can
    // account for the ones its thread did not reach.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner(*engine, Transports::Config{});
    const Transports& transports = runner.transports();

    feedExcerpt(*engine);
    CHECK(transports.beats() == 0); // nothing drains a ring until something drains it

    runner.start();
    runner.stop();
    CHECK(transports.beats() == kExpectedBeats);
    CHECK(engine->beatsDropped() == 0);
}

TEST_CASE("the observer sees every beat, in the order they were called", "[output]") {
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner(*engine, Transports::Config{});

    std::mutex mutex;
    std::vector<takt4::engine::EngineBeat> seen;
    runner.setBeatObserver([&](const takt4::engine::EngineBeat& beat) {
        const std::lock_guard<std::mutex> lock(mutex);
        seen.push_back(beat);
    });

    runner.start();
    feedExcerpt(*engine);
    runner.stop();

    const std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(seen.size() == kExpectedBeats);
    std::uint64_t downbeats = 0;
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (i > 0) {
            // A console prints these and a rule engine will fire on them; out of order
            // would be worse than late.
            CHECK(seen[i].event.frameIndex > seen[i - 1].event.frameIndex);
        }
        if (seen[i].event.downbeat) {
            ++downbeats;
        }
    }
    CHECK(downbeats == kExpectedDownbeats);
}

TEST_CASE("the output thread runs on its own clock", "[output]") {
    // That the thread exists is one claim and that it drains correctly is another. This
    // is the first, and it is the one a caller doing the draining itself would also pass.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner(*engine, Transports::Config{});

    runner.start();
    const std::uint64_t before = runner.rounds();
    std::this_thread::sleep_for(std::chrono::milliseconds{120});
    const std::uint64_t after = runner.rounds();
    runner.stop();

    INFO(after - before << " rounds in 120 ms at " << OutputRunner::kPeriod.count() << " ms");
    // 120 ms is a hundred-odd periods. Assert a floor far under that so a busy machine
    // cannot fail it, and a ceiling that would catch a loop spinning without sleeping.
    CHECK(after - before >= 5);
    CHECK(after - before <= 2000);
}

TEST_CASE("a runner is safe to stop twice, and to never start", "[output]") {
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    {
        OutputRunner runner(*engine, Transports::Config{});
        runner.stop(); // never started
        CHECK_FALSE(runner.running());
        runner.start();
        runner.stop();
        runner.stop();
        CHECK_FALSE(runner.running());
    } // and the destructor stops a running one
    OutputRunner running(*engine, Transports::Config{});
    running.start();
    CHECK(running.running());
}

TEST_CASE("a change posted while stopped applies at once", "[output]") {
    // An app is configured before it is started, and an operator ticking Link with nothing
    // running should not have to press Start to find out whether it took.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner(*engine, Transports::Config{});
    REQUIRE_FALSE(runner.running());

    runner.post(takt4::output::OutputCommand::linkEnabled(true));
    CHECK(runner.transports().linkEnabled());
    runner.post(takt4::output::OutputCommand::oscTargets({{"127.0.0.1", 7000}}));
    CHECK(runner.transports().osc().targetCount() == 1);
    CHECK(runner.lastError().empty());
}

TEST_CASE("a change posted while running reaches the transports", "[output]") {
    // The other half: the output thread owns them once it is going, so the change has to
    // travel rather than be made by the caller.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner(*engine, Transports::Config{});
    runner.start();
    REQUIRE_FALSE(runner.transports().linkEnabled());

    runner.post(takt4::output::OutputCommand::linkEnabled(true));
    runner.post(takt4::output::OutputCommand::oscTargets({{"127.0.0.1", 7000}}));

    // The thread applies them at the top of a round, so within a period or two.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    while (std::chrono::steady_clock::now() < until && !runner.transports().linkEnabled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(runner.transports().linkEnabled());
    CHECK(runner.transports().osc().targetCount() == 1);
    CHECK(runner.lastError().empty());

    runner.stop();
}

TEST_CASE("a MIDI port that is not there is reported rather than thrown away", "[output]") {
    // The transports keep working — Transports leaves what was open alone — so the only
    // evidence is the message, and an operator has to be given it.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner(*engine, Transports::Config{});

    runner.post(takt4::output::OutputCommand::midiClockPort(
        std::string("takt4 test - no such MIDI port exists")));
    CHECK_FALSE(runner.lastError().empty());
    CHECK(runner.transports().midiClock() == nullptr);

    // And a change that works clears it again.
    runner.post(takt4::output::OutputCommand::linkEnabled(true));
    CHECK(runner.lastError().empty());
}
