#include "core/audio/rates.hpp"
#include "core/control/rule_control.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/rule.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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
using takt4::output::OutputCommand;
using takt4::output::OutputRunner;
using takt4::output::Transports;
using takt4::testing::LoopbackReceiver;
using takt4::trigger::Rule;

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

/// The committed excerpt, which `takt4-cli track --decoder pf` reports as "499 frames, 21
/// beats (5 downbeats)" — so the numbers here are the tracker's own, not this test's
/// invention.
const std::vector<float>& excerpt() {
    static const std::vector<float> samples =
        takt4::io::readWavFile(kTestData / "features" / "synthetic.wav").samples;
    return samples;
}

constexpr std::uint64_t kExpectedBeats = 21;
constexpr std::uint64_t kExpectedDownbeats = 5;

/// Every engine here runs the particle filter. These tests are about the output thread
/// — draining, counting, routing — and want a beat stream that never moves; the particle
/// filter's is held to `tools/pf_reference.py` frame for frame, where the default decoder's
/// changes with its tuning. See `tracking::Decoder`.
BeatEngine::Options particleOptions() {
    BeatEngine::Options options;
    options.decoder = takt4::tracking::Decoder::ParticleFilter;
    return options;
}

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

/// Waits until the runner's transports have counted `beats`, or gives up.
///
/// Waiting rather than assuming, for the reason the first test below spells out at length:
/// feeding the excerpt pegs a core, and on a small CI runner the output thread gets very
/// little of the other one.
void waitForBeats(const OutputRunner& runner, std::uint64_t beats) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < until && runner.transports().beats() < beats) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

} // namespace

TEST_CASE("the output thread drains every beat the tracker called", "[output]") {
    // The whole point of §4.2's output thread: beats reach the transports without the
    // caller's loop deciding when. Nothing is configured to send, so what is under test
    // is the draining and the counting rather than any one transport.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    const Transports& transports = runner.transports();

    runner.start();
    CHECK(runner.running());
    feedExcerpt(*engine);

    // Waited for rather than assumed, and checked *before* stop(): this is what says the
    // thread drained them rather than stop()'s final sweep, which is a different claim
    // and has a test of its own below.
    //
    // Waiting is the point. The first version of this asserted that the thread had made
    // some rounds by the time the feed finished, and CI disagreed on 5b2b2b6: pushing the
    // excerpt through the network and the filter pegs a core, and on a two-core runner the
    // output thread can get almost none of the other one. That is the machine's business,
    // not the code's — what the code owes is that the beats arrive, which they did there
    // too (every other assertion passed).
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < until && transports.beats() < kExpectedBeats) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    INFO(runner.rounds() << " rounds, " << transports.beats() << " beats before the stop");
    CHECK(transports.beats() == kExpectedBeats);
    CHECK(transports.downbeats() == kExpectedDownbeats);

    runner.stop();
    CHECK_FALSE(runner.running());
    CHECK(transports.beats() == kExpectedBeats); // and the final sweep found nothing left
    CHECK(engine->beatsDropped() == 0);
    CHECK(runner.errors() == 0);
}

TEST_CASE("stopping drains the beats that were still waiting", "[output]") {
    // The last beats of a set are still beats. Everything is produced before the runner
    // is ever started, so the ring is full of them and only stop()'s final drain can
    // account for the ones its thread did not reach.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
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
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
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
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
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
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
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
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
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
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    runner.start();
    REQUIRE_FALSE(runner.transports().linkEnabled());

    runner.post(takt4::output::OutputCommand::linkEnabled(true));
    runner.post(takt4::output::OutputCommand::oscTargets({{"127.0.0.1", 7000}}));

    // The thread applies them at the top of a round, so within a period or two.
    //
    // Wait for **both**, not for the first and then assume the second. They are applied
    // one after another rather than atomically, so a reader on this thread can see
    // `linkEnabled` already true while `oscTargets` is still a few instructions behind —
    // and a runner that preempts the output thread between the two turns that into a
    // failure. macOS did exactly that at `66173912`. Waiting on the conjunction is also
    // simply the honest thing: these are the two claims, so both are what to wait for.
    const auto applied = [&runner] {
        return runner.transports().linkEnabled() && runner.transports().osc().targetCount() == 1;
    };
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < until && !applied()) {
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
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});

    runner.post(takt4::output::OutputCommand::midiClockPort(
        std::string("takt4 test - no such MIDI port exists")));
    CHECK_FALSE(runner.lastError().empty());
    CHECK(runner.transports().midiClock() == nullptr);

    // And a change that works clears it again.
    runner.post(takt4::output::OutputCommand::linkEnabled(true));
    CHECK(runner.lastError().empty());
}

TEST_CASE("a rule fires from the real beats, on the output thread", "[output][trigger]") {
    // 5.8: "Rules are evaluated on the output thread, never the audio thread." This is that
    // claim end to end — the real excerpt through the real tracker, the beats it really
    // called, a rule the runner was handed by command, and a socket that really receives.
    LoopbackReceiver receiver;
    Transports::Config config;
    config.outputs = takt4::output::oscOutputs({{"127.0.0.1", receiver.port()}});

    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);

    Rule::Config rule;
    rule.id = "downbeat-clip";
    rule.trigger = takt4::trigger::Trigger::Downbeat;
    rule.address = "/composition/layers/1/clips/{c}/connect";
    takt4::trigger::Generator::Config clip;
    clip.low = 1;
    clip.high = 4;
    rule.segments = {clip};
    rule.value = takt4::trigger::Generator::Config{};
    rule.value.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.value.fixed = takt4::trigger::Value::ofInt(1);
    runner.post(OutputCommand::rules({rule}));
    REQUIRE(runner.triggers().ruleCount() == 1);
    REQUIRE(runner.triggers().rule(0).valid());

    runner.start();
    feedExcerpt(*engine);
    waitForBeats(runner, kExpectedBeats);
    runner.stop();

    // The excerpt has five downbeats, and the rule is on downbeats.
    CHECK(runner.triggers().rule(0).fires() == kExpectedDownbeats);
    CHECK(runner.ruleSink().delivered() == kExpectedDownbeats);
    CHECK(runner.ruleSink().undeliverable() == 0);
    CHECK(runner.triggers().dropped() == 0);

    // And the addresses really left the machine, with the segment filled in.
    //
    // Counted out of everything that arrived rather than taken as the first five: §5.6's
    // generic namespace is going to the same target on the same socket, so a rule's
    // datagrams are interleaved with `/takt4/bpm` and the rest. That interleaving is the
    // design — one OSC target, both namespaces — and a test that assumed otherwise would be
    // asserting an ordering nothing promises.
    std::uint64_t matched = 0;
    std::uint64_t seen = 0;
    for (std::string datagram = receiver.receive(); !datagram.empty();
         datagram = receiver.receive()) {
        ++seen;
        if (datagram.find("/composition/layers/1/clips/") != std::string::npos) {
            ++matched;
        }
    }
    INFO(seen << " datagrams arrived in all");
    CHECK(matched == kExpectedDownbeats);
}

TEST_CASE("the octave fold reaches the wire, not just the readout", "[output][trigger]") {
    // The user's report of 2026-09-06, at the far end of the chain it actually mattered at.
    // A fold that moves the number and leaves `TrackedFrame::emitted` alone is invisible in
    // the tracker's own tests and *deafening* here: the readout says 93 and every OSC
    // datagram, MIDI clock tick and trigger rule fires at 186. So this asserts the halving
    // where an operator would see it — on a socket, in the datagrams that really left.
    //
    // Driven from ÷2 rather than from the automatic fold because the excerpt is ten seconds
    // long and the automatic one deliberately waits five for the music to agree with it
    // (Options::foldSupportFrames); the operator's ÷2 is the same divisor without the wait.
    LoopbackReceiver receiver;
    Transports::Config config;
    config.outputs = takt4::output::oscOutputs({{"127.0.0.1", receiver.port()}});

    const auto beatsOnTheWire = [&](bool halve) {
        auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
        OutputRunner runner(*engine, config);
        Rule::Config rule;
        rule.id = "flash";
        rule.trigger = takt4::trigger::Trigger::Beat;
        rule.address = "/flash";
        rule.sendValue = false;
        runner.post(OutputCommand::rules({rule}));
        runner.start();
        if (halve) {
            REQUIRE(engine->post(takt4::engine::Command::halve()));
        }
        feedExcerpt(*engine);
        waitForBeats(runner, halve ? kExpectedBeats / 2 : kExpectedBeats);
        runner.stop();

        std::uint64_t flashes = 0;
        for (std::string datagram = receiver.receive(); !datagram.empty();
             datagram = receiver.receive()) {
            if (datagram.find("/flash") != std::string::npos) {
                ++flashes;
            }
        }
        // What the rule fired, what the sink delivered and what a socket really received are
        // three different claims, and a fold that reached only two of them would be the same
        // bug one layer further down.
        CHECK(runner.triggers().rule(0).fires() == flashes);
        CHECK(runner.ruleSink().delivered() == flashes);
        return flashes;
    };

    const std::uint64_t whole = beatsOnTheWire(false);
    CHECK(whole == kExpectedBeats);

    const std::uint64_t halved = beatsOnTheWire(true);
    // Half the beats, on the wire, for a control that halves the tempo. Before
    // `Options::foldBeats` this was 21 either way.
    CHECK(halved >= kExpectedBeats / 2 - 1);
    CHECK(halved <= kExpectedBeats / 2 + 1);
}

TEST_CASE("panic reaches the rules through the same queue as everything else",
          "[output][trigger]") {
    // 5.8 calls PANIC "non-negotiable for live use", and 5.7 gives it an address. Both of
    // those land here, on the thread that owns the rules.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});

    Rule::Config rule;
    rule.id = "every-beat";
    rule.trigger = takt4::trigger::Trigger::Beat;
    rule.address = "/fire";
    runner.post(OutputCommand::rules({rule}));
    runner.post(OutputCommand::panic(true));
    CHECK(runner.panicked());

    runner.start();
    feedExcerpt(*engine);
    waitForBeats(runner, kExpectedBeats);
    runner.stop();
    CHECK(runner.triggers().rule(0).fires() == 0);

    // And 5.7's `/ctl/rule/<id>/enable` takes the same road.
    runner.post(OutputCommand::ruleEnabled("every-beat", false));
    CHECK_FALSE(runner.triggers().rule(0).enabled());
    runner.post(OutputCommand::ruleEnabled("every-beat", true));
    CHECK(runner.triggers().rule(0).enabled());
    runner.post(OutputCommand::ruleEnabled("no-such-rule", false));
    CHECK(runner.triggers().rule(0).enabled()); // and an id nobody has is not an error
}

TEST_CASE("a control surface reaches the rules without knowing what a runner is",
          "[output][trigger][control]") {
    // The far end of 5.7's two remaining addresses. `control::ControlSurface` is written
    // against `RuleControl` and this is the only implementation an app has: the runner owns
    // the rules, so there is nowhere else for the route to end.
    //
    // Driven through the *interface* deliberately, rather than through `post`. A surface
    // holds a `RuleControl*` and never sees an OutputRunner, so what has to be checked is
    // that the two calls it can make do what the commands do.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});

    Rule::Config rule;
    rule.id = "every-beat";
    rule.trigger = takt4::trigger::Trigger::Beat;
    rule.address = "/fire";
    runner.post(OutputCommand::rules({rule}));

    takt4::control::RuleControl& rules = runner;

    SECTION("while stopped, both take effect at once") {
        rules.panic(true);
        CHECK(runner.panicked());
        rules.panic(false);
        CHECK_FALSE(runner.panicked());

        rules.setRuleEnabled("every-beat", false);
        CHECK_FALSE(runner.triggers().rule(0).enabled());
        rules.setRuleEnabled("every-beat", true);
        CHECK(runner.triggers().rule(0).enabled());

        // A Stream Deck holding a button for a rule the current preset no longer has is an
        // ordinary state of the world, not something to report.
        rules.setRuleEnabled("no-such-rule", false);
        CHECK(runner.triggers().rule(0).enabled());
    }

    SECTION("while running, a panic from another thread stops the rules firing") {
        // The live case: the call arrives on RtMidi's callback thread or the OSC receiver's
        // while the output thread is mid-set, and the queue is what makes that safe.
        runner.start();

        // Panic before any beat is fed, so what is being measured is the halt and not a
        // race with the excerpt. The rules are read back after the stop, which is what
        // `triggers()` documents as the only time it is safe to.
        std::thread surface([&rules] { rules.panic(true); });
        surface.join();

        feedExcerpt(*engine);
        waitForBeats(runner, kExpectedBeats);
        runner.stop();

        CHECK(runner.panicked());
        CHECK(runner.triggers().rule(0).fires() == 0);
        CHECK(runner.errors() == 0);
    }
}
