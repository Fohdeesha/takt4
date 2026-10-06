#include "core/audio/rates.hpp"
#include "core/control/rule_control.hpp"
#include "core/dmx/fixture.hpp"
#include "core/dmx/liberation.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/output/midi_ports.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/rule_sink.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/rule.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
/// Of those, what reaches the rig. Nothing does before the tracker has earned a lock
/// (`TempoState::acquired`, the operator's call of 2026-10-03), and a lock needs a bar of beats
/// (`TempoTracker::Options::lockBeats`), so the first three beats, none of them a downbeat, are
/// called and not sent. "the observer sees every beat" checks the split rather than assuming it.
constexpr std::uint64_t kSentBeats = 18;
constexpr std::uint64_t kSentDownbeats = 5;

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

/// The first rule's count, read between two of the output thread's rounds. A plain read of
/// `triggers()` while the thread runs is a data race even on an integer that only ever goes
/// up — TSan reported exactly that here — so a running runner is read through `inspect`.
std::uint64_t firesOf(const OutputRunner& runner) {
    return runner.inspect([](const takt4::trigger::TriggerEngine& rules, const auto&, const auto&) {
        return rules.ruleCount() > 0 ? rules.rule(0).fires() : std::uint64_t{0};
    });
}

/// The same for a rule's own count, which is what a test about firing rather than about
/// draining wants. Returns what it reached, so a caller can say how many rather than only
/// that it waited.
std::uint64_t waitForFires(const OutputRunner& runner, std::uint64_t fires) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < until && firesOf(runner) < fires) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return firesOf(runner);
}

/// Waits until the output thread has made `count` more whole rounds from now — each of which
/// reads what the engine has published — rather than for a length of time, which is a guess
/// about how busy the machine is (the audit of 2026-09-25, T17). The first round counted may
/// be one already under way, so the round after it is the first known to start afterwards.
void waitForRounds(const OutputRunner& runner, std::uint64_t count) {
    const std::uint64_t target = runner.rounds() + count + 1;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < until && runner.rounds() < target) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(runner.rounds() >= target);
}

} // namespace

TEST_CASE("starting the engine leaves the beat ring to its one consumer", "[output]") {
    // The audit of 2026-09-25, L42. `BeatEngine::start` drained the beat ring from the UI thread
    // while the output thread — the ring's one consumer, draining it all the time — could be
    // doing the same, and two threads taking from a single-consumer ring race on its read
    // position. Now what a run leaves there stays for the consumer, stamped with its run.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    feedExcerpt(*engine);
    const std::uint64_t before = engine->runNumber();
    engine->start();
    engine->stop();
    CHECK(engine->runNumber() == before + 1);
    std::uint64_t left = 0;
    std::uint64_t fromBefore = 0;
    takt4::engine::EngineBeat beat;
    while (engine->popBeat(beat)) {
        ++left;
        fromBefore += beat.run == before ? 1U : 0U;
    }
    CHECK(left == kExpectedBeats);
    CHECK(fromBefore == kExpectedBeats);
}

TEST_CASE("a beat the last run left in the ring is not sent in the next", "[output]") {
    // The other half of L42: the consumer drops what a run that is over left behind — a
    // flash or a clip for a beat of the track before the STOP.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    feedExcerpt(*engine);
    engine->start(); // a new run, with the last one's beats still on the ring
    engine->stop();
    OutputRunner runner(*engine, Transports::Config{});
    runner.start();
    waitForRounds(runner, 3);
    CHECK(runner.transports().beats() == 0);
    runner.stop();
    CHECK(runner.transports().beats() == 0); // nor did the final sweep send them
    takt4::engine::EngineBeat beat;
    CHECK_FALSE(engine->popBeat(beat)); // taken, and dropped
}

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
    while (std::chrono::steady_clock::now() < until && transports.beats() < kSentBeats) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    INFO(runner.rounds() << " rounds, " << transports.beats() << " beats before the stop");
    CHECK(transports.beats() == kSentBeats);
    CHECK(transports.downbeats() == kSentDownbeats);

    runner.stop();
    CHECK_FALSE(runner.running());
    CHECK(transports.beats() == kSentBeats); // and the final sweep found nothing left
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
    CHECK(transports.beats() == kSentBeats);
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
    std::uint64_t sent = 0;
    std::uint64_t sentDownbeats = 0;
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (i > 0) {
            // A console prints these and a rule engine will fire on them; out of order
            // would be worse than late.
            CHECK(seen[i].event.frameIndex > seen[i - 1].event.frameIndex);
            // And once a lock has been earned the rig hears every beat after it.
            CHECK((seen[i].event.acquired || !seen[i - 1].event.acquired));
        }
        if (seen[i].event.downbeat) {
            ++downbeats;
        }
        if (seen[i].event.acquired) {
            ++sent;
            sentDownbeats += seen[i].event.downbeat ? 1U : 0U;
        }
    }
    CHECK(downbeats == kExpectedDownbeats);
    CHECK(sent == kSentBeats);
    CHECK(sentDownbeats == kSentDownbeats);
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
    // running should not have to press Start to find out whether it took. Not [network]: the
    // runner is never started, so nothing is joined or sent (the audit of 2026-09-25, T5).
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    REQUIRE_FALSE(runner.running());

    runner.post(takt4::output::OutputCommand::linkEnabled(true));
    CHECK(runner.transports().linkEnabled());
    runner.post(takt4::output::OutputCommand::oscTargets({{"127.0.0.1", 57000}}));
    CHECK(runner.transports().osc().targetCount() == 1);
    CHECK(runner.lastError().empty());
}

TEST_CASE("a change posted while running reaches the transports", "[output][network]") {
    // The other half: the output thread owns them once it is going, so the change has to
    // travel rather than be made by the caller.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    runner.start();
    // Read through `inspect`, which waits for the gap between two rounds: the transports are
    // the output thread's now, and a plain read of them from here is the data race TSan
    // reported on this very test.
    const auto linkOn = [&runner] {
        return runner.inspect([](const auto&, const Transports& transports, const auto&) {
            return transports.linkEnabled();
        });
    };
    const auto oscTargets = [&runner] {
        return runner.inspect([](const auto&, const Transports& transports, const auto&) {
            return transports.osc().targetCount();
        });
    };
    REQUIRE_FALSE(linkOn());

    runner.post(takt4::output::OutputCommand::linkEnabled(true));
    runner.post(takt4::output::OutputCommand::oscTargets({{"127.0.0.1", 57000}}));

    // The thread applies them at the top of a round, so within a period or two.
    //
    // Wait for **both**, not for the first and then assume the second. They are applied
    // one after another rather than atomically, so a reader on this thread can see
    // `linkEnabled` already true while `oscTargets` is still a few instructions behind —
    // and a runner that preempts the output thread between the two turns that into a
    // failure. macOS did exactly that at `66173912`. Waiting on the conjunction is also
    // simply the honest thing: these are the two claims, so both are what to wait for.
    const auto applied = [&] { return linkOn() && oscTargets() == 1; };
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < until && !applied()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(linkOn());
    CHECK(oscTargets() == 1);
    CHECK(runner.lastError().empty());

    runner.stop();
}

TEST_CASE("what a UI reads while the thread sends is a snapshot", "[output][hardware]") {
    // `transports()` hands back references the output thread replaces whole — a vector of
    // targets, an optional port — so a reader at redraw rate races with a freed buffer
    // rather than merely with a stale number. `snapshot()` is taken under a lock, and is
    // taken again after every command, so it is at worst one round behind.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});

    // Filled at construction rather than at the first command: a reader must never find it
    // uninitialised.
    CHECK(runner.snapshot().outputs.empty());
    CHECK_FALSE(runner.snapshot().link);
    CHECK_FALSE(runner.snapshot().midiClockOpen);

    takt4::output::OutputTarget deck;
    deck.name = "deck";
    deck.host = "127.0.0.1";
    deck.port = 57000;
    runner.post(OutputCommand::outputs({deck}));
    runner.post(OutputCommand::linkEnabled(true));

    const OutputRunner::Snapshot live = runner.snapshot();
    CHECK(live.outputs.size() == 1);
    CHECK(live.outputs.front().name == "deck");
    CHECK(live.link);

    const std::vector<std::string> ports = takt4::output::listMidiOutputPorts();
    if (ports.empty()) {
        SKIP("no MIDI output on this machine");
    }
    runner.post(OutputCommand::midiClockPort(ports.front()));

    // **A port that enumerates is not a port that opens**, and this test asserted the first
    // while needing the second. A hosted Windows runner lists the Microsoft GS Wavetable
    // Synth and then refuses to open it — `MidiOutWinMM::openPort: error creating Windows MM
    // MIDI output port` — so the guard above passed and the two checks below failed on a
    // machine that simply has no MIDI hardware. It went unseen because no CI run reached the
    // test step between this test being written and 2026-09-14: every one before that was
    // stopped at the billing gate seconds after starting, and every developer machine here
    // has a real port.
    //
    // So the environment is skipped on the error the open actually reported, carrying it in
    // the message. A regression that stops a *working* port opening does not become a silent
    // pass: it becomes a skip on a machine where the rest of the suite opens ports happily,
    // and the reason is printed rather than guessed at.
    if (const std::string failed = runner.lastError(); !failed.empty()) {
        SKIP("a MIDI output is listed but will not open here: " << failed);
    }

    const OutputRunner::Snapshot withClock = runner.snapshot();
    CHECK(withClock.midiClockPort == ports.front());
    CHECK(withClock.midiClockOpen);
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

    // And a change that works clears it again. (Any change: this one used to be switching
    // Link on, which joined the real session on whatever network the suite ran on.)
    runner.post(takt4::output::OutputCommand::oscTargets({}));
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
    waitForBeats(runner, kSentBeats);
    runner.stop();

    // The excerpt has five downbeats, all of them after the lock, and the rule is on downbeats.
    CHECK(runner.triggers().rule(0).fires() == kSentDownbeats);
    CHECK(runner.ruleSink().delivered() == kSentDownbeats);
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
    CHECK(matched == kSentDownbeats);
}

TEST_CASE("a stage of a round that throws is counted and named, and the round carries on",
          "[output][trigger]") {
    // The audit's M12. One try block held the whole round, so a stage that threw took every
    // stage after it down with it for that round — every round, if it threw every time — and
    // all anybody could have seen was a count that nothing displayed. Here a stage throws on
    // every beat, and every beat still reaches the rules and the transports, and the window's
    // copy says which stage it was and what it said.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    runner.setBeatObserver(
        [](const takt4::engine::EngineBeat&) { throw std::runtime_error("the observer broke"); });
    Rule::Config rule;
    rule.id = "clips";
    rule.trigger = takt4::trigger::Trigger::Downbeat;
    rule.address = "/clips";
    runner.post(OutputCommand::rules({rule}));

    runner.start();
    feedExcerpt(*engine);
    waitForBeats(runner, kSentBeats);
    // While it runs, with no command to take a snapshot: the window's copy catches up on its
    // own, a refresh or two later.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (runner.snapshot().trouble.roundErrors < kExpectedBeats &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    CHECK(runner.snapshot().trouble.roundErrors == kExpectedBeats);
    runner.stop();
    REQUIRE(runner.sync()); // a command applied is a snapshot taken

    CHECK(runner.transports().beats() == kSentBeats);
    CHECK(runner.triggers().rule(0).fires() == kSentDownbeats);
    CHECK(runner.errors() == kExpectedBeats); // the observer sees every beat, sent or not
    const OutputRunner::Snapshot::Trouble trouble = runner.snapshot().trouble;
    CHECK(trouble.roundErrors == kExpectedBeats);
    CHECK(trouble.lastRoundError == "the beat observer: the observer broke");
    // And the rule's messages, which had no OSC target to go to, are counted where the window
    // reads them.
    CHECK(trouble.undeliverable == kSentDownbeats);
}

TEST_CASE("the fired log keeps the newest messages, in order, when nobody drains it",
          "[output][trigger]") {
    // It holds `kFiredCapacity` and drops the oldest past that — which it did by erasing the
    // front of a vector, every message once it was full (the audit's Low items). Six hundred
    // fired with nothing reading: the last 512, oldest first.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    Rule::Config rule;
    rule.id = "count";
    rule.address = "/n/{}";
    rule.sendValue = false; // the address alone, so each entry is just the count
    takt4::trigger::Generator::Config counter;
    counter.kind = takt4::trigger::GeneratorKind::Cycle;
    counter.low = 1;
    counter.high = 1000;
    rule.segments = {counter};
    runner.post(OutputCommand::rules({rule}));
    constexpr int kFired = 600;
    for (int i = 0; i < kFired; ++i) {
        runner.post(OutputCommand::testRule("count")); // stopped, so applied at once
    }
    const std::vector<OutputRunner::Fired> fired = runner.takeFired();
    REQUIRE(fired.size() == OutputRunner::kFiredCapacity);
    const int first = kFired - static_cast<int>(OutputRunner::kFiredCapacity) + 1;
    for (std::size_t i = 0; i < fired.size(); ++i) {
        INFO("entry " << i);
        REQUIRE(fired[i].message == "/n/" + std::to_string(first + static_cast<int>(i)));
    }
    CHECK(runner.takeFired().empty());
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
        waitForBeats(runner, halve ? kSentBeats / 2 : kSentBeats);
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
    CHECK(whole == kSentBeats);

    const std::uint64_t halved = beatsOnTheWire(true);
    // Half the beats, on the wire, for a control that halves the tempo. Before
    // `Options::foldBeats` this was every beat either way.
    CHECK(halved >= kSentBeats / 2 - 1);
    CHECK(halved <= kSentBeats / 2 + 1);
}

TEST_CASE("rules still fire after a stop and a start", "[output][trigger]") {
    // Reported from a rig on 2026-09-12: *"when hitting stop on the main window, when I hit
    // start again, none of the triggers started firing again."*
    //
    // `elapsed()` — which is `Context::now` — used to be taken from `start()`, so it went
    // back to zero on every Start while `Rule::lastFired_` kept the time of the last fire of
    // the previous run. §5.8's cooldown test then read `now - lastFired` as a large
    // *negative* number, which is below even a zero cooldown, and every rule that had ever
    // fired was blocked until the clock climbed back past where it had been.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});

    Rule::Config rule;
    rule.id = "lasers";
    rule.trigger = takt4::trigger::Trigger::Downbeat;
    rule.address = "/lasers";
    runner.post(OutputCommand::rules({rule}));
    REQUIRE(runner.triggers().ruleCount() == 1);

    runner.start();
    feedExcerpt(*engine);
    const std::uint64_t before = waitForFires(runner, 1);
    runner.stop();
    REQUIRE(before >= 1);

    // The clock only ever goes forwards, which is the fix itself and the one thing a rule's
    // cooldown depends on.
    const double stopped = runner.elapsed();
    runner.start();
    CHECK(runner.elapsed() >= stopped);

    // The rules are deliberately **not** posted again: re-posting rebuilds every `Rule` and
    // clears `lastFired_` with it, which is exactly what would hide the bug. What the
    // operator does is press Start.
    feedExcerpt(*engine);
    waitForFires(runner, before + 1);
    runner.stop();

    const std::uint64_t after = runner.triggers().rule(0).fires() - before;
    INFO(runner.rounds() << " rounds, elapsed " << runner.elapsed());
    CHECK(after >= 1);
    CHECK(runner.errors() == 0);
}

TEST_CASE("a DOWNBEAT pressed just after a beat fires that bar's downbeat rules",
          "[output][trigger]") {
    // The audit's M4, as the operator meets it: they press DOWNBEAT on the downbeat they hear,
    // a moment after the tracker called that beat as beat 3. The beat becomes the bar's first
    // — but it has gone out already, so a downbeat rule (a laser on the one) used to skip that
    // bar altogether. It fires on the press now, late by the press.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    std::mutex mutex;
    std::uint64_t publishedDownbeats = 0;
    runner.setBeatObserver([&](const takt4::engine::EngineBeat& beat) {
        const std::lock_guard<std::mutex> lock(mutex);
        publishedDownbeats += beat.event.acquired && beat.event.beatInBar == 1 ? 1 : 0;
    });
    Rule::Config rule;
    rule.id = "lasers";
    rule.trigger = takt4::trigger::Trigger::Downbeat;
    rule.address = "/lasers";
    runner.post(OutputCommand::rules({rule}));
    runner.start();

    // The excerpt a hop at a time, pressing on the first frame after a beat that is not a
    // bar's first — the press the operator makes, and the one M4 is about.
    const std::vector<float>& samples = excerpt();
    const std::size_t hops = samples.size() / kHopSize;
    std::uint64_t beatsSeen = 0;
    bool pressed = false;
    for (std::size_t hop = 0; hop < hops; ++hop) {
        engine->processHop(samples.data() + hop * kHopSize, hop);
        (void)engine->step();
        const takt4::tracking::TempoState state = engine->state();
        if (!pressed && state.beats > beatsSeen) {
            beatsSeen = state.beats;
            // Once a lock has been earned: before one, nothing fires the rig, a declared bar
            // included.
            if (state.acquired && state.bars >= 1 && state.beatInBar > 1) {
                REQUIRE(engine->post(takt4::engine::Command::snapDownbeat()));
                pressed = true;
            }
        }
    }
    REQUIRE(pressed);
    REQUIRE(engine->state().barsDeclared == 1);
    waitForBeats(runner, kSentBeats);
    // **Never `firesOf` under `mutex`** (the audit of 2026-09-25, T14): the observer takes
    // `mutex` inside the runner's own lock, and `firesOf` takes the runner's lock — so holding
    // this one while asking was the other order, and a beat arriving between them hung the test.
    const auto published = [&mutex, &publishedDownbeats] {
        const std::lock_guard<std::mutex> lock(mutex);
        return publishedDownbeats;
    };
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < until) {
        const std::uint64_t fires = firesOf(runner);
        if (fires >= published() + 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    runner.stop();

    const std::uint64_t downbeats = published();
    INFO(downbeats << " downbeats published");
    // Every downbeat that went out, and the one the press declared.
    CHECK(runner.triggers().rule(0).fires() == downbeats + 1);
}

TEST_CASE("a DOWNBEAT pressed before the first lock fires nothing until there is one",
          "[output][trigger]") {
    // Nothing reaches the rig until the tracker has earned a lock (`TempoState::acquired`, the
    // operator's call of 2026-10-03), and a bar a press declares is no exception: a downbeat rule
    // fired for it would be the one flash in seconds of a hunt. "The Galaxist" has a downbeat at
    // 0.6 s and other beats after it, under this configuration, long before its first lock at 3.8.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    std::mutex mutex;
    std::uint64_t sentDownbeats = 0;
    runner.setBeatObserver([&](const takt4::engine::EngineBeat& beat) {
        const std::lock_guard<std::mutex> lock(mutex);
        sentDownbeats += beat.event.acquired && beat.event.beatInBar == 1 ? 1 : 0;
    });
    Rule::Config rule;
    rule.id = "lasers";
    rule.trigger = takt4::trigger::Trigger::Downbeat;
    rule.address = "/lasers";
    runner.post(OutputCommand::rules({rule}));
    runner.start();

    const std::vector<float> samples =
        takt4::io::readWavFile(kTestData / "features" / "the-galaxist.wav").samples;
    const std::size_t hops = samples.size() / kHopSize;
    std::uint64_t beatsSeen = 0;
    bool pressed = false;
    for (std::size_t hop = 0; hop < hops; ++hop) {
        engine->processHop(samples.data() + hop * kHopSize, hop);
        (void)engine->step();
        const takt4::tracking::TempoState state = engine->state();
        if (!pressed && state.beats > beatsSeen) {
            beatsSeen = state.beats;
            if (!state.acquired && state.bars >= 1 && state.beatInBar > 1) {
                REQUIRE(engine->post(takt4::engine::Command::snapDownbeat()));
                pressed = true;
            }
        }
    }
    REQUIRE(pressed);
    REQUIRE(engine->state().barsDeclared == 1);
    REQUIRE(engine->state().acquired); // the lock came later in the excerpt
    // Every round after the press has read the declared bar — none fires it, and nothing after
    // the lock may fire it late either.
    waitForRounds(runner, 5);
    runner.stop();
    const std::uint64_t sent = [&] {
        const std::lock_guard<std::mutex> lock(mutex);
        return sentDownbeats;
    }();
    INFO(sent << " downbeats sent after the lock");
    CHECK(runner.triggers().rule(0).fires() == sent);
}

TEST_CASE("starting the tracker again does not fire the onset rules", "[output][trigger]") {
    // The runner keeps going across a Stop and a Start (the audit's H5); the engine zeroes its
    // onset count on every start. Compared for plain inequality, the first round of every run
    // after the first saw the old count "move" to zero and fired every onset rule once.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    Rule::Config rule;
    rule.id = "hits";
    rule.trigger = takt4::trigger::Trigger::Onset;
    rule.address = "/hits";
    runner.post(OutputCommand::rules({rule}));
    runner.start();

    feedExcerpt(*engine);
    REQUIRE(engine->intensity().onsets > 0);
    waitForFires(runner, 1);
    waitForRounds(runner, 3); // every onset seen
    const std::uint64_t fired = firesOf(runner);
    REQUIRE(fired >= 1);

    // A new run, with nothing played in it: the engine zeroes its counts as Start does.
    engine->start();
    engine->stop();
    REQUIRE(engine->intensity().onsets == 0);
    waitForRounds(runner, 20); // the zeroed count read, and read again
    runner.stop();
    CHECK(runner.triggers().rule(0).fires() == fired);
}

namespace {

/// A MIDI port that keeps every message it is sent, whole.
class KeepingPort final : public takt4::output::MidiPort {
public:
    explicit KeepingPort(std::shared_ptr<std::vector<std::vector<unsigned char>>> sent)
        : sent_(std::move(sent)) {}
    std::string open(std::string_view spec) override { return std::string(spec); }
    void close() noexcept override {}
    void send(std::span<const unsigned char> message) override {
        sent_->emplace_back(message.begin(), message.end());
    }

private:
    std::shared_ptr<std::vector<std::vector<unsigned char>>> sent_;
};

/// An OSC int argument's value, from a datagram that carries exactly one: the last four bytes,
/// big-endian.
int lastInt(const std::string& datagram) {
    if (datagram.size() < 4) {
        return -1;
    }
    const auto byte = [&datagram](std::size_t at) {
        return static_cast<int>(static_cast<std::uint8_t>(datagram[at]));
    };
    const std::size_t at = datagram.size() - 4;
    return (byte(at) << 24) | (byte(at + 1) << 16) | (byte(at + 2) << 8) | byte(at + 3);
}

} // namespace

TEST_CASE("stopping sends the releases it still owes", "[output][trigger]") {
    // A note on whose note off has not yet come round is a laser still lit, and an operator
    // pressing Stop has said the opposite. `TriggerEngine::flushFollowUps` on the way down —
    // the same argument `panic` and `setRules` already make.
    //
    // **To a port, not only to the log.** This test used to have no outputs at all and read
    // the runner's record of what it fired, which says the release was *owed* and nothing
    // about whether it left — and leaving is what the audit's H6 found it did not do (T1).
    auto sent = std::make_shared<std::vector<std::vector<unsigned char>>>();
    Transports::Config config;
    takt4::output::OutputTarget desk;
    desk.id = "o-0000a5e7";
    desk.name = "laser desk";
    desk.kind = takt4::output::OutputTarget::Kind::Midi;
    desk.device = "Laser";
    config.outputs = {desk};
    config.openMidi = [sent](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(name, std::make_unique<KeepingPort>(sent));
    };
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);

    Rule::Config rule;
    rule.id = "laser";
    rule.trigger = takt4::trigger::Trigger::Manual;
    rule.sendKind = takt4::trigger::Message::Kind::MidiNote;
    rule.channel = 1;
    rule.number.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.number.fixed = takt4::trigger::Value::ofInt(60);
    rule.value.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.value.fixed = takt4::trigger::Value::ofInt(100);
    takt4::trigger::FollowUp release;
    release.unit = takt4::trigger::DelayUnit::Milliseconds;
    release.delaySeconds = 600.0; // never due within the test; only the stop can pay it out
    rule.followUps.push_back(release);
    runner.post(OutputCommand::rules({rule}));
    REQUIRE(runner.triggers().rule(0).valid());

    runner.start();
    runner.post(OutputCommand::testRule("laser"));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    // Read between the output thread's rounds — it is running, and it owns the rules.
    const auto pending = [&runner] {
        return runner.inspect([](const auto& rules, const auto&, const auto&) { return rules.pending(); });
    };
    while (std::chrono::steady_clock::now() < until && pending() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    runner.stop();

    std::vector<OutputRunner::Fired> owed;
    for (const OutputRunner::Fired& entry : runner.takeFired()) {
        if (entry.followUp) {
            owed.push_back(entry);
        }
    }
    REQUIRE(owed.size() == 1);
    CHECK(owed[0].followUp);
    CHECK(owed[0].ruleId == "laser");
    // A real Note Off, not the Note On with velocity zero the rig's laser controller
    // ignores. See `trigger::Message::Kind`.
    CHECK(owed[0].message == "note off 60 ch 1");
    CHECK(runner.triggers().pending() == 0);

    // And on the cable: the Note On, then its Note Off, and nothing left owing.
    const std::vector<std::vector<unsigned char>> want{{0x90, 60, 100}, {0x80, 60, 0}};
    CHECK(*sent == want);
}

TEST_CASE("an output switched off or deleted is sent what it is owed first", "[output][trigger]") {
    // The operator, 2026-09-30: "if I uncheck a midi target for instance, it immediately dies, so
    // it never sends the note off, and the midi device keeps playing that note". Unticking a row
    // closed its port with the note off still owed to it, and the note off went nowhere a beat
    // later. Now what is owed to an output that is going — a rule's release, a note held for the
    // output's delay, an OSC datagram held for its — goes to it before it goes. What is owed to
    // the outputs that stay is left for its own time.
    using Kind = takt4::output::OutputTarget::Kind;
    using Bytes = std::vector<std::vector<unsigned char>>;
    std::map<std::string, std::shared_ptr<Bytes>> cables;
    for (const char* device : {"Laser", "Synth", "Other"}) {
        cables[device] = std::make_shared<Bytes>();
    }
    LoopbackReceiver deckSocket;
    LoopbackReceiver stageSocket;

    const auto output = [](const char* id, Kind kind, double delay) {
        takt4::output::OutputTarget target;
        target.id = id;
        target.name = id;
        target.kind = kind;
        target.delaySeconds = delay;
        return target;
    };
    auto desk = output("o-desk", Kind::Midi, 0.0);
    desk.device = "Laser";
    // Held 400 ms by its own delay, so its note on has not left yet either.
    auto synth = output("o-synth", Kind::Midi, 0.4);
    synth.device = "Synth";
    auto deck = output("o-deck", Kind::Osc, 0.4);
    deck.host = "127.0.0.1";
    deck.port = deckSocket.port();
    auto stage = output("o-stage", Kind::Osc, 0.0);
    stage.host = "127.0.0.1";
    stage.port = stageSocket.port();

    Transports::Config config;
    config.outputs = {desk, synth, deck, stage};
    config.openMidi = [cables](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(
            name, std::make_unique<KeepingPort>(cables.at(name)));
    };
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);

    takt4::trigger::FollowUp release;
    release.unit = takt4::trigger::DelayUnit::Milliseconds;
    release.delaySeconds = 600.0; // never due within the test
    Rule::Config laser;
    laser.id = "laser";
    laser.trigger = takt4::trigger::Trigger::Manual;
    laser.sendKind = takt4::trigger::Message::Kind::MidiNote;
    laser.number.kind = takt4::trigger::GeneratorKind::Fixed;
    laser.number.fixed = takt4::trigger::Value::ofInt(60);
    laser.value.kind = takt4::trigger::GeneratorKind::Fixed;
    laser.value.fixed = takt4::trigger::Value::ofInt(100);
    laser.followUps = {release};
    laser.outputs = {"o-desk", "o-synth"};
    Rule::Config clip;
    clip.id = "clip";
    clip.trigger = takt4::trigger::Trigger::Manual;
    clip.address = "/clip";
    clip.value.kind = takt4::trigger::GeneratorKind::Fixed;
    clip.value.fixed = takt4::trigger::Value::ofInt(1);
    clip.followUps = {release};
    clip.outputs = {"o-deck", "o-stage"};
    runner.post(OutputCommand::rules({laser, clip}));
    REQUIRE(runner.triggers().rule(0).valid());
    REQUIRE(runner.triggers().rule(1).valid());

    // Not started, so each command is applied as it is posted and nothing comes due.
    runner.post(OutputCommand::testRule("laser"));
    runner.post(OutputCommand::testRule("clip"));
    const Bytes noteOn{{0x90, 60, 100}};
    const Bytes noteOnAndOff{{0x90, 60, 100}, {0x80, 60, 0}};
    REQUIRE(*cables["Laser"] == noteOn);
    REQUIRE(cables["Synth"]->empty()); // held for its delay
    REQUIRE(runner.triggers().pending() == 2);
    const std::string pressed = stageSocket.receive();
    REQUIRE(pressed.rfind("/clip", 0) == 0);
    REQUIRE(lastInt(pressed) == 1);
    REQUIRE_FALSE(deckSocket.ready(50)); // held for its delay

    // What the OSC deck must now hear, in order: its held press, then its release.
    const auto deckGotPressAndRelease = [&] {
        const std::string first = deckSocket.receive();
        const std::string second = deckSocket.receive();
        CHECK(first.rfind("/clip", 0) == 0);
        CHECK(lastInt(first) == 1);
        CHECK(second.rfind("/clip", 0) == 0);
        CHECK(lastInt(second) == 0);
    };

    SECTION("switched off") {
        auto off = config.outputs;
        off[0].enabled = false;
        off[1].enabled = false;
        off[2].enabled = false;
        runner.post(OutputCommand::outputs(off));
        CHECK(*cables["Laser"] == noteOnAndOff);
        CHECK(*cables["Synth"] == noteOnAndOff);
        deckGotPressAndRelease();
        // The stage stays, and its release is still owed at its own time, once — not sent
        // early, and not handed to the publisher to hold as well as being owed by the rule.
        CHECK_FALSE(stageSocket.ready(50));
        CHECK(runner.triggers().pending() == 1);
        CHECK(runner.transports().osc().pending() == 0);
        CHECK(runner.ruleSink().queued() == 0);
    }
    SECTION("deleted") {
        runner.post(OutputCommand::outputs({stage}));
        CHECK(*cables["Laser"] == noteOnAndOff);
        CHECK(*cables["Synth"] == noteOnAndOff);
        deckGotPressAndRelease();
        CHECK_FALSE(stageSocket.ready(50));
        CHECK(runner.triggers().pending() == 1);
        CHECK(runner.transports().osc().pending() == 0);
        CHECK(runner.ruleSink().queued() == 0);
    }
    SECTION("the same row pointed at another device") {
        auto moved = config.outputs;
        moved[0].device = "Other";
        runner.post(OutputCommand::outputs(moved));
        // The note off to the laser that was sent the note on; nothing to the other device.
        CHECK(*cables["Laser"] == noteOnAndOff);
        CHECK(cables["Other"]->empty());
        CHECK(cables["Synth"]->empty());
        CHECK_FALSE(deckSocket.ready(50));
    }
    SECTION("renamed, which is not going anywhere") {
        auto renamed = config.outputs;
        for (auto& target : renamed) {
            target.name += " (renamed)";
        }
        runner.post(OutputCommand::outputs(renamed));
        CHECK(*cables["Laser"] == noteOn);
        CHECK(cables["Synth"]->empty());
        CHECK_FALSE(deckSocket.ready(50));
        CHECK(runner.triggers().pending() == 2);
    }
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

// No quotes in the name, and no characters the console codepage cannot carry: ctest hands a
// test's name back to the binary as a filter through the command line, so anything that does
// not survive that round trip "fails" by matching nothing at all.
TEST_CASE("the rule id all means every rule", "[output][trigger][control]") {
    // §5.7's `/ctl/rule/all/mute`. The *surface* does not know what rules exist — only the
    // thing behind `RuleControl` does — so "all" travels as a name and is expanded here.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});

    std::vector<Rule::Config> rules;
    for (const char* id : {"intro", "drop", "outro"}) {
        Rule::Config rule;
        rule.id = id;
        rule.trigger = takt4::trigger::Trigger::Beat;
        rule.address = "/fire";
        rules.push_back(rule);
    }
    runner.post(OutputCommand::rules(rules));
    REQUIRE(runner.triggers().ruleCount() == 3);

    runner.post(OutputCommand::ruleMuted(std::string(takt4::output::kAllRules), true));
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(runner.triggers().rule(i).muted());
    }

    // And one rule at a time still means one rule.
    runner.post(OutputCommand::ruleMuted("drop", false));
    CHECK(runner.triggers().rule(0).muted());
    CHECK_FALSE(runner.triggers().rule(1).muted());
    CHECK(runner.triggers().rule(2).muted());

    SECTION("a rate gesture reaches all of them too, and compounds where it is relative") {
        runner.post(OutputCommand::ruleRate(std::string(takt4::output::kAllRules), 2.0, true));
        runner.post(OutputCommand::ruleRate(std::string(takt4::output::kAllRules), 2.0, true));
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(runner.triggers().rule(i).rate() == 4.0);
        }
        // Absolute replaces rather than compounding, which is what a reset is.
        runner.post(OutputCommand::ruleRate(std::string(takt4::output::kAllRules), 1.0, false));
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(runner.triggers().rule(i).rate() == 1.0);
        }
    }

    SECTION("an edit to one rule does not undo the gestures made to the others") {
        // §5.9's editor replaces the whole set on every keystroke, and the live gestures are
        // not part of what it hands over — so without `setRules` carrying them by id, renaming
        // one rule would unmute the rig.
        rules[1].name = "renamed mid-set";
        runner.post(OutputCommand::rules(rules));
        CHECK(runner.triggers().rule(0).muted());
        CHECK_FALSE(runner.triggers().rule(1).muted());
        CHECK(runner.triggers().rule(2).muted());
    }

    SECTION("a preset load brings new ids, so nothing carries over") {
        std::vector<Rule::Config> preset;
        Rule::Config fresh;
        fresh.id = "from-a-preset";
        fresh.address = "/fire";
        preset.push_back(fresh);
        runner.post(OutputCommand::rules(preset));
        REQUIRE(runner.triggers().ruleCount() == 1);
        CHECK_FALSE(runner.triggers().rule(0).muted());
        CHECK(runner.triggers().rule(0).rate() == 1.0);
    }
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

TEST_CASE("a target list that would not open still moves the routing with it",
          "[output][trigger]") {
    // `Transports::setOutputs` replaces its list and *then* raises whatever would not open,
    // so the bit a rule's name resolves to has already moved when it throws. Resolving the
    // routing only on the way out of a successful call left every rule holding the bit its
    // old neighbour used to occupy — one unplugged MIDI device was enough to send a rule's
    // clips to the lighting desk instead.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());

    takt4::output::OutputTarget wall;
    wall.id = "o-000000aa";
    wall.name = "wall";
    wall.host = "127.0.0.1";
    wall.port = 57000;

    Transports::Config config;
    config.outputs = {wall};
    OutputRunner runner(*engine, config);

    Rule::Config rule;
    rule.id = "wall-rule";
    rule.trigger = takt4::trigger::Trigger::Beat;
    rule.address = "/wall";
    rule.outputs = {wall.id};
    runner.post(OutputCommand::rules({rule}));
    REQUIRE(runner.triggers().ruleCount() == 1);
    REQUIRE(runner.triggers().rule(0).outputMask() == 1); // "wall" is target 0

    // A new list where "wall" has moved to index 1 and index 0 is a device this machine has
    // not got, so the replacement reports a failure part-way through.
    takt4::output::OutputTarget broken;
    broken.id = "o-000000bb";
    broken.name = "lights";
    broken.kind = takt4::output::OutputTarget::Kind::Midi;
    broken.device = "takt4 test - no such MIDI device exists";
    runner.post(OutputCommand::outputs({broken, wall}));

    INFO("lastError: " << runner.lastError());
    CHECK_FALSE(runner.lastError().empty());            // the failure is still reported
    CHECK(runner.triggers().rule(0).outputMask() == 2); // and "wall" is target 1 now

    SECTION("and the targets that did open are still there") {
        const OutputRunner::Snapshot live = runner.snapshot();
        REQUIRE(live.outputs.size() == 2);
        CHECK(live.outputs[1].name == "wall");
    }
}

TEST_CASE("two control surfaces can post to a stopped runner at once", "[output][trigger]") {
    // `post` applies on the *caller's* thread while the runner is stopped — which is what
    // lets an app be configured before it is started, and is the state an operator binding
    // buttons before a set is in. §5.7's `panic` and `rule/<id>/enable` are documented safe
    // from RtMidi's callback and from the OSC receiver's loop, so two of them really can be
    // inside `apply` at once, and the rules editor commits the whole set on every keystroke
    // from a third. All of it mutates one vector of rules.
    //
    // A soak rather than a proof: a race is not something a test can observe directly, so
    // what this does is run the shapes that would collide and check that what comes out is
    // intact. Under `ownerMutex_` it is; without it, it is a vector being cleared under a
    // walk.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});

    std::vector<Rule::Config> set;
    for (int i = 0; i < 32; ++i) {
        Rule::Config rule;
        // Long enough to be a heap allocation rather than a small string, so a walk over a
        // set being rebuilt would be reading freed memory rather than a stale byte.
        rule.id = "a-rule-id-past-the-small-string-buffer-" + std::to_string(i);
        rule.trigger = takt4::trigger::Trigger::Manual;
        rule.address = "/r" + std::to_string(i);
        set.push_back(rule);
    }
    const std::string last = set.back().id;
    runner.post(OutputCommand::rules(set));
    REQUIRE(runner.triggers().ruleCount() == set.size());

    std::atomic<bool> go{false};
    const auto wait = [&go] {
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    // The editor: the whole set again on every keystroke.
    std::thread editing([&] {
        wait();
        for (int i = 0; i < 2000; ++i) {
            set.front().name = std::string(static_cast<std::size_t>(i % 24) + 1, 'x');
            runner.post(OutputCommand::rules(set));
        }
    });
    // A Stream Deck on the OSC socket, and a pad on RtMidi's callback thread.
    std::thread surface([&] {
        wait();
        for (int i = 0; i < 20000; ++i) {
            runner.setRuleEnabled(last, i % 2 == 0);
        }
    });
    std::thread pad([&] {
        wait();
        for (int i = 0; i < 20000; ++i) {
            runner.panic(i % 2 == 0);
        }
    });
    go.store(true, std::memory_order_release);
    editing.join();
    surface.join();
    pad.join();

    // Intact, and still the set that was posted: every id in place and readable, which is
    // what a walk over a vector being rebuilt underneath would not have left.
    REQUIRE(runner.triggers().ruleCount() == set.size());
    for (std::size_t i = 0; i < set.size(); ++i) {
        INFO("rule " << i);
        REQUIRE(runner.triggers().rule(i).id() == set[i].id);
    }
    CHECK(runner.triggers().rule(set.size() - 1).id() == last);
    runner.panic(false);
    CHECK_FALSE(runner.panicked());
}

TEST_CASE("a negative offset lands a beat's cue before that beat from live audio",
          "[output][trigger][slow]") {
    // The audit's H4 at the far end of the chain, where an operator meets it: the excerpt fed
    // in real time through the real engine and the real output thread, stamped on Link's clock
    // as a sound card's audio is, and a rule sending each beat's place in the bar to a media
    // server set 200 ms early.
    //
    // The *value* is what tells the fix from what it replaced. The old hold also delivered a
    // cue about 200 ms before a beat — but it was the cue for the beat *before*, held for what
    // was left of it: the `2` of a bar arrived just before beat 3, and a bar-1 clip cue landed
    // before beat 2. Here the cue arriving 200 ms before a beat has to be that beat's own.
    LoopbackReceiver media;
    Transports::Config config;
    takt4::output::OutputTarget server;
    server.name = "media";
    server.host = "127.0.0.1";
    server.port = media.port();
    server.delaySeconds = -0.200;
    config.outputs = {server};

    // The default decoder, which is what ships.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner(*engine, config);
    engine->setHostTimeSource(&runner.hostTimeClock());

    Rule::Config rule;
    rule.id = "position";
    rule.trigger = takt4::trigger::Trigger::Beat;
    rule.address = "/cue";
    rule.value = takt4::trigger::Generator::Config{};
    rule.value.kind = takt4::trigger::GeneratorKind::Live;
    rule.value.source = takt4::trigger::LiveSource::BeatInBar;
    runner.post(OutputCommand::rules({rule}));
    REQUIRE(runner.triggers().rule(0).valid());

    // Every beat heard, with its moment put on the runner's clock the way the runner does it.
    struct Heard {
        double moment = 0.0;
        std::uint32_t beatInBar = 0;
        bool locked = false;
    };
    std::mutex mutex;
    std::vector<Heard> heard;
    runner.setBeatObserver([&](const takt4::engine::EngineBeat& beat) {
        const double moment =
            runner.elapsed() -
            static_cast<double>(runner.transports().link().now().count() - beat.hostMicros) / 1e6;
        const std::lock_guard<std::mutex> lock(mutex);
        heard.push_back({moment, beat.event.beatInBar, beat.state.locked});
    });

    // Every cue as it arrives, on the same clock.
    struct Cue {
        double at = 0.0;
        int value = 0;
    };
    std::vector<Cue> cues;
    std::atomic<bool> listening{true};
    std::thread listener([&] {
        while (listening.load(std::memory_order_acquire)) {
            const std::string datagram = media.receive();
            if (datagram.rfind("/cue", 0) == 0) {
                const double at = runner.elapsed();
                const std::lock_guard<std::mutex> lock(mutex);
                cues.push_back({at, lastInt(datagram)});
            }
        }
    });

    runner.start();
    engine->start();
    const std::vector<float>& samples = excerpt();
    const std::size_t hops = samples.size() / kHopSize;
    const double hopSeconds = static_cast<double>(kHopSize) / takt4::audio::kInternalSampleRate;
    const auto began = std::chrono::steady_clock::now();
    for (std::size_t hop = 0; hop < hops; ++hop) {
        std::this_thread::sleep_until(
            began + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(static_cast<double>(hop + 1) * hopSeconds)));
        engine->processHop(samples.data() + hop * kHopSize, hop);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{600});
    engine->stop();
    runner.stop();
    listening.store(false, std::memory_order_release);
    listener.join();

    // Every beat heard while locked after the one before it was — so there was a lock to
    // predict from — should have had its own cue 200 ms before its moment.
    std::size_t predictable = 0;
    std::size_t onTime = 0;
    std::size_t beforeItsBeat = 0;
    for (std::size_t i = 1; i < heard.size(); ++i) {
        if (!heard[i].locked || !heard[i - 1].locked || heard[i].beatInBar == 0) {
            continue;
        }
        ++predictable;
        for (const Cue& cue : cues) {
            if (cue.value != static_cast<int>(heard[i].beatInBar)) {
                continue;
            }
            const double early = heard[i].moment - cue.at;
            if (early > 0.0 && early < 0.45) {
                ++beforeItsBeat;
            }
            if (std::abs(early - 0.200) < 0.035) {
                ++onTime;
                break;
            }
        }
    }
    INFO(heard.size() << " beats heard, " << predictable << " of them predictable, " << onTime
                      << " with their own cue 200 ms early, " << cues.size() << " cues");
    CHECK(runner.errors() == 0);
    CHECK(predictable >= 8);
    // Every one, give or take the beat where the tracker's tempo was still settling.
    CHECK(onTime + 1 >= predictable);
    CHECK(beforeItsBeat >= onTime);
    CHECK(runner.scheduler().predictedFires() >= predictable);
}

namespace {

/// The 512 levels of one ArtDmx datagram, or nothing for anything that is not one.
std::vector<std::uint8_t> artDmxLevels(const std::string& datagram) {
    if (datagram.size() < 18 || datagram.compare(0, 8, std::string("Art-Net\0", 8)) != 0) {
        return {};
    }
    const std::size_t length = static_cast<std::size_t>(static_cast<std::uint8_t>(datagram[16])) *
                                   256 +
                               static_cast<std::uint8_t>(datagram[17]);
    if (datagram.size() < 18 + length) {
        return {};
    }
    return std::vector<std::uint8_t>(datagram.begin() + 18,
                                     datagram.begin() + 18 + static_cast<std::ptrdiff_t>(length));
}

/// An RGB par at address 1 of universe 0, and an Art-Net node on loopback that feeds it.
Transports::Config parOnNode(std::uint16_t port) {
    Transports::Config config;
    takt4::output::OutputTarget node;
    node.name = "truss";
    node.kind = takt4::output::OutputTarget::Kind::ArtNet;
    node.host = "127.0.0.1";
    node.port = port;
    config.outputs.push_back(node);
    config.patch = {takt4::dmx::fixtureFromMode("par", 1, 0, 1)};
    return config;
}

/// Reads frames off `node` until one's first three channels are `rgb`, or the node goes quiet.
bool sawRgb(LoopbackReceiver& node, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        const std::string datagram = node.receive();
        if (datagram.empty()) {
            return false;
        }
        const std::vector<std::uint8_t> levels = artDmxLevels(datagram);
        if (levels.size() >= 3 && levels[0] == r && levels[1] == g && levels[2] == b) {
            return true;
        }
    }
    return false;
}

/// The par to a colour, the way the patch editor's IDENTIFY and colour preview reach it.
OutputCommand paint(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    takt4::dmx::Payload payload;
    payload.kind = takt4::dmx::EffectKind::Color;
    payload.color = takt4::dmx::Color{r, g, b};
    return OutputCommand::effect(0b1, payload);
}

/// A MIDI device that notes every message sent down it.
class NotingPort final : public takt4::output::MidiPort {
public:
    explicit NotingPort(std::shared_ptr<std::vector<unsigned char>> heard) : heard_(std::move(heard)) {}
    std::string open(std::string_view spec) override { return std::string(spec); }
    void close() noexcept override {}
    void send(std::span<const unsigned char> message) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!message.empty()) {
            heard_->push_back(message[0]);
        }
    }

private:
    std::shared_ptr<std::vector<unsigned char>> heard_;
    std::mutex mutex_;
};

} // namespace

TEST_CASE("the outputs run with the tracker stopped", "[output][dmx]") {
    // The audit's H5. The runner started and stopped with the tracker, so before Start nothing
    // was transmitted at all: IDENTIFY and a colour preview changed a buffer nobody sent. It
    // runs for the application's life now, and the tracker's Start and Stop are `setTracking`.
    LoopbackReceiver node;
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, parOnNode(node.port()));
    runner.start();
    REQUIRE_FALSE(runner.tracking());

    runner.post(paint(255, 0, 0));
    CHECK(sawRgb(node, 255, 0, 0));

    SECTION("and a Stop blacks the lights out and goes on sending") {
        // The operator's call of 2026-09-23 (Q3): Stop is a blackout — every light-emitting
        // channel to zero — and the node goes on hearing takt4, so it does not fall back to a
        // failsafe of its own.
        runner.setTracking(true);
        runner.post(paint(0, 0, 255));
        REQUIRE(sawRgb(node, 0, 0, 255));
        runner.setTracking(false);
        CHECK(sawRgb(node, 0, 0, 0));
        REQUIRE(runner.sync());
        CHECK_FALSE(runner.tracking());
        // And the keep-alive: frames keep coming with nothing moving.
        int frames = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{2200};
        while (std::chrono::steady_clock::now() < until) {
            if (!artDmxLevels(node.receive()).empty()) {
                ++frames;
            }
        }
        CHECK(frames >= 2);
    }

    SECTION("and PANIC freezes rather than blacking out") {
        // PANIC keeps the operator's call of 2026-09-16: the lights stay where they are.
        runner.post(OutputCommand::panic(true));
        REQUIRE(runner.sync());
        const std::vector<std::uint8_t> levels =
            runner.inspect([](const auto&, const Transports& transports, const auto&) {
                const std::span<const std::uint8_t> frame = transports.dmx().levels(0);
                return std::vector<std::uint8_t>(frame.begin(), frame.begin() + 3);
            });
        CHECK(levels == std::vector<std::uint8_t>{255, 0, 0});
    }
    runner.stop();
}

TEST_CASE("a node whose only fixture is switched off is sent dark frames, then left alone",
          "[output][dmx][slow]") {
    // The audit of 2026-09-25, H4, on the wire. One par on a node: switching it off took its
    // universe out of the patch, nothing was ever sent there again, and the node held the lamp
    // lit. The operator's answer to the audit's Q2: zeros for a few seconds, then leave it.
    LoopbackReceiver node;
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    Transports::Config config = parOnNode(node.port());
    OutputRunner runner(*engine, config);
    runner.start();
    runner.post(paint(255, 0, 0));
    REQUIRE(sawRgb(node, 255, 0, 0));

    takt4::dmx::Fixture off = config.patch.front();
    off.enabled = false;
    runner.post(OutputCommand::patch({off}));
    const auto posted = std::chrono::steady_clock::now();

    // Everything that arrives for the next four seconds, and when.
    int dark = 0;
    int lit = 0;
    double lastDark = 0.0;
    const auto until = posted + std::chrono::seconds{4};
    while (std::chrono::steady_clock::now() < until) {
        const std::vector<std::uint8_t> levels = artDmxLevels(node.receive());
        if (levels.size() < 3) {
            continue;
        }
        const double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - posted).count();
        if (levels[0] == 0 && levels[1] == 0 && levels[2] == 0) {
            ++dark;
            lastDark = at;
        } else if (dark > 0) {
            ++lit; // lit after it went dark: the release did not hold
        }
    }
    runner.stop();
    INFO(dark << " dark frames, the last " << lastDark << " s after the switch");
    // Many, so a dropped datagram or two costs nothing: the node is fed at its 44 Hz ceiling.
    CHECK(dark >= 30);
    CHECK(lit == 0);
    // For about three seconds, and not for ever — something else may take the universe over.
    CHECK(lastDark > 2.5);
    CHECK(lastDark < 3.6);
}

TEST_CASE("quitting sends the blackout frame itself", "[output][dmx]") {
    // The way out of the application: the lights out (Q3), and that frame actually sent. Art-Net
    // only went out in `advance`, paced at 44 Hz, so a change made a moment before quitting was
    // never transmitted and the node held the rig lit after takt4 had gone (the audit's H6).
    LoopbackReceiver node;
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, parOnNode(node.port()));
    runner.start();
    runner.post(paint(255, 255, 255));
    REQUIRE(sawRgb(node, 255, 255, 255));
    runner.stop();

    std::vector<std::uint8_t> last;
    for (std::string datagram = node.receive(); !datagram.empty(); datagram = node.receive()) {
        if (std::vector<std::uint8_t> levels = artDmxLevels(datagram); !levels.empty()) {
            last = std::move(levels);
        }
    }
    REQUIRE(last.size() >= 3);
    CHECK(last[0] == 0);
    CHECK(last[1] == 0);
    CHECK(last[2] == 0);
}

TEST_CASE("the frame sent on quitting keeps the 44 Hz spacing", "[output][dmx]") {
    // The audit's L7: the blackout on the way out went straight after the round's own frame, a
    // millisecond apart, and a node that drops frames arriving faster than 44 Hz dropped it and
    // kept the look. So a change is posted and the runner stopped at once — the change's frame
    // and the blackout are then as close as they can be — and every frame's arrival is stamped.
    LoopbackReceiver node;
    const auto origin = std::chrono::steady_clock::now();
    std::vector<std::pair<double, std::vector<std::uint8_t>>> frames;
    std::atomic<bool> listening{true};
    std::thread reader([&] {
        while (listening.load()) {
            const std::string datagram = node.receive();
            if (std::vector<std::uint8_t> levels = artDmxLevels(datagram); !levels.empty()) {
                frames.emplace_back(
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - origin)
                        .count(),
                    std::move(levels));
            }
        }
    });
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, parOnNode(node.port()));
    runner.start();
    runner.post(paint(255, 255, 255));
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    runner.post(paint(0, 255, 0));
    runner.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    listening = false;
    reader.join();

    REQUIRE(frames.size() >= 2);
    const auto& last = frames.back().second;
    REQUIRE(last.size() >= 3);
    CHECK(last[0] == 0); // the blackout, and the last word
    CHECK(last[1] == 0);
    CHECK(last[2] == 0);
    const double gap = frames.back().first - frames[frames.size() - 2].first;
    INFO(frames.size() << " frames; the last came " << gap * 1000.0 << " ms after the one before");
    CHECK(gap >= 0.020); // 1/44 s is 22.7 ms; a little is left for the stamps' own jitter
}

TEST_CASE("quitting sends a release held for a delayed output", "[output][trigger]") {
    // The audit's H6: a release parked for an OSC target's delay went out at the *next* Start,
    // so a Resolume clip stayed latched until then.
    LoopbackReceiver server;
    Transports::Config config;
    takt4::output::OutputTarget media;
    media.name = "media";
    media.host = "127.0.0.1";
    media.port = server.port();
    media.delaySeconds = 0.8;
    config.outputs = {media};
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);

    Rule::Config rule;
    rule.id = "clip";
    rule.trigger = takt4::trigger::Trigger::Manual;
    rule.address = "/clip";
    takt4::trigger::FollowUp release;
    release.unit = takt4::trigger::DelayUnit::Milliseconds;
    release.delaySeconds = 0.05;
    rule.followUps.push_back(release);
    runner.post(OutputCommand::rules({rule}));
    runner.start();
    runner.post(OutputCommand::testRule("clip"));
    // Past the release's own delay, and well short of the output's: both are held.
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    runner.stop();

    int clips = 0;
    for (std::string datagram = server.receive(); !datagram.empty(); datagram = server.receive()) {
        if (datagram.rfind("/clip", 0) == 0) {
            ++clips;
        }
    }
    CHECK(clips == 2); // the press and its release, not the press alone
}

TEST_CASE("STOP leaves the lights out, fires nothing after it and lets go of what is held",
          "[output][dmx][slow]") {
    // The audit of 2026-09-25, H3, and the operator's answer to its Q3. STOP blacked the lights
    // out once — and then up to two predicted beats still fired their rules, because nothing
    // reset the scheduler and the engine's last locked state stays published after it stops; and
    // a lighting release owed went on to relight a lamp at its release level. The lights have to
    // go out and stay out. What is owed to a media server or a laser — a clip's release — goes out
    // at once, as it does on quit, so nothing is left latched.
    //
    // Fed in real time, on the runner's own clock, so there is a lock to predict from: offline,
    // with no timeline, nothing is ever predicted and the bug cannot happen.
    LoopbackReceiver node;
    LoopbackReceiver media;
    Transports::Config config = parOnNode(node.port());
    config.patch.front().group = "washes";
    takt4::output::OutputTarget server;
    server.name = "media";
    server.host = "127.0.0.1";
    server.port = media.port();
    server.sendsNamespace = true; // its /takt4/beat is what says the beats went on to the stop
    config.outputs.push_back(server);

    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner(*engine, config);
    engine->setHostTimeSource(&runner.hostTimeClock());

    // Every beat the washes go white, and 1.5 s later down to a quarter of that: a release that
    // lights the lamp, and always one owed.
    Rule::Config flash;
    flash.id = "flash";
    flash.trigger = takt4::trigger::Trigger::Beat;
    flash.sendKind = takt4::trigger::Message::Kind::Dmx;
    flash.dmx.fixtures = {"washes"};
    flash.dmx.effect = takt4::dmx::EffectKind::Color;
    flash.dmx.color.kind = takt4::trigger::GeneratorKind::Fixed;
    flash.dmx.color.fixed = takt4::trigger::Value::ofText("#ffffff");
    flash.dmx.unit = takt4::trigger::DelayUnit::Milliseconds;
    flash.dmx.durationSeconds = 0.0;
    takt4::trigger::FollowUp dim;
    dim.value = takt4::trigger::Value::ofInt(64);
    dim.unit = takt4::trigger::DelayUnit::Milliseconds;
    dim.delaySeconds = 1.5;
    flash.followUps = {dim};
    // And a clip on every bar, released three seconds on — longer than a bar, so one is owed.
    Rule::Config clip;
    clip.id = "clip";
    clip.trigger = takt4::trigger::Trigger::Bar;
    clip.address = "/clip";
    clip.value = takt4::trigger::Generator::Config{};
    clip.value.kind = takt4::trigger::GeneratorKind::Fixed;
    clip.value.fixed = takt4::trigger::Value::ofInt(1);
    takt4::trigger::FollowUp release;
    release.unit = takt4::trigger::DelayUnit::Milliseconds;
    release.delaySeconds = 3.0;
    clip.followUps = {release};
    runner.post(OutputCommand::rules({flash, clip}));

    // Every clip message as it arrives, on the runner's clock — and every beat the transports
    // publish, which a predicted beat also sends: the rules are not the only thing it fires.
    struct Cue {
        double at = 0.0;
        int value = 0;
    };
    std::mutex mutex;
    std::vector<Cue> cues;
    std::vector<double> beatsHeard;
    std::atomic<bool> listening{true};
    std::thread listener([&] {
        while (listening.load(std::memory_order_acquire)) {
            const std::string datagram = media.receive();
            const double at = runner.elapsed();
            const std::lock_guard<std::mutex> lock(mutex);
            if (datagram.rfind("/clip", 0) == 0) {
                cues.push_back({at, lastInt(datagram)});
            } else if (datagram.rfind(std::string("/takt4/beat") + '\0', 0) == 0) {
                beatsHeard.push_back(at);
            }
        }
    });

    runner.start();
    runner.setTracking(true);
    engine->start();
    const std::vector<float>& samples = excerpt();
    const std::size_t hops = samples.size() / kHopSize;
    const double hopSeconds = static_cast<double>(kHopSize) / takt4::audio::kInternalSampleRate;
    const auto began = std::chrono::steady_clock::now();
    for (std::size_t hop = 0; hop < hops; ++hop) {
        std::this_thread::sleep_until(
            began + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(static_cast<double>(hop + 1) * hopSeconds)));
        engine->processHop(samples.data() + hop * kHopSize, hop);
    }

    // What the node was sent during the run is of no interest, and it starts dark — the par is
    // parked at zero until the first beat — so it is read away first.
    while (node.ready(0)) {
        (void)node.receive();
    }
    // STOP as the window does it: the tracker, then the runner told.
    engine->stop();
    REQUIRE(engine->state().locked); // what the scheduler would go on predicting from
    const double stoppedAt = runner.elapsed();
    runner.setTracking(false);
    REQUIRE(runner.sync());

    // Two seconds of what the node is sent. A frame or two sent just before the STOP may still
    // be queued on the socket; from the first dark one on, every one must be dark.
    bool dark = false;
    int litAfter = 0;
    int frames = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < until) {
        const std::vector<std::uint8_t> levels = artDmxLevels(node.receive());
        if (levels.size() < 3) {
            continue;
        }
        const bool black = levels[0] == 0 && levels[1] == 0 && levels[2] == 0;
        dark = dark || black;
        if (dark) {
            ++frames;
            litAfter += black ? 0 : 1;
        }
    }
    runner.stop();
    listening.store(false, std::memory_order_release);
    listener.join();

    CHECK(dark);
    CHECK(frames >= 2); // the keep-alive: still sent, still dark
    CHECK(litAfter == 0);
    const std::lock_guard<std::mutex> lock(mutex);
    int pressesAfter = 0;
    bool releasedAtStop = false;
    for (const Cue& cue : cues) {
        if (cue.at >= stoppedAt && cue.value != 0) {
            ++pressesAfter;
        }
        if (cue.value == 0 && cue.at >= stoppedAt && cue.at < stoppedAt + 0.25) {
            releasedAtStop = true;
        }
    }
    INFO(cues.size() << " clip messages");
    CHECK(pressesAfter == 0);
    CHECK(releasedAtStop);
    // And no beat published after it, beyond one already on its way as the STOP was posted.
    int beatsAfter = 0;
    for (const double at : beatsHeard) {
        beatsAfter += at > stoppedAt + 0.05 ? 1 : 0;
    }
    CHECK(beatsHeard.size() > 4); // the run's own, so the address is right
    CHECK(beatsAfter == 0);
}

TEST_CASE("the MIDI clock starts and stops with the tracker, not with the outputs",
          "[output][midi]") {
    // The outputs run from launch (H5); a drum machine given a Start then would play at the
    // clock's opening tempo before anything was listening. Nothing is fed here, so this is
    // about the ticks; when Start goes is the transports' test.
    auto heard = std::make_shared<std::vector<unsigned char>>();
    Transports::Config config;
    config.midiClockPort = "Clock";
    config.openMidi = [heard](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(name, std::make_unique<NotingPort>(heard));
    };
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);
    runner.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    REQUIRE(runner.sync());
    const auto count = [&heard, &runner](unsigned char status) {
        // Read between rounds: the output thread is the one writing.
        return runner.inspect([&](const auto&, const auto&, const auto&) {
            return std::count(heard->begin(), heard->end(), status);
        });
    };
    CHECK(count(takt4::output::MidiClock::kStart) == 0);
    CHECK(count(takt4::output::MidiClock::kTick) == 0);

    // Ticking from the press, so a receiver has a tempo; but not started, because nothing has
    // been heard to say where a bar begins (the audit's M19 — see "a MIDI receiver's bar 1 is a
    // downbeat of the music").
    runner.setTracking(true);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    CHECK(count(takt4::output::MidiClock::kStart) == 0);
    CHECK(count(takt4::output::MidiClock::kTick) > 5);

    runner.setTracking(false);
    REQUIRE(runner.sync());
    // A receiver never told to play is not told to stop — and the ticks stop.
    CHECK(count(takt4::output::MidiClock::kStop) == 0);
    const auto ticks = count(takt4::output::MidiClock::kTick);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    CHECK(count(takt4::output::MidiClock::kTick) == ticks);
    runner.stop();
}

namespace {

/// A MIDI port that notes when each clock tick left, on the steady clock, from the thread that
/// sent it — the output thread. Read once that thread has been stopped.
class TickTimer final : public takt4::output::MidiPort {
public:
    explicit TickTimer(std::shared_ptr<std::vector<double>> ticks) : ticks_(std::move(ticks)) {}
    std::string open(std::string_view spec) override { return std::string(spec); }
    void close() noexcept override {}
    void send(std::span<const unsigned char> message) override {
        if (!message.empty() && message[0] == takt4::output::MidiClock::kTick) {
            ticks_->push_back(std::chrono::duration<double>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
        }
    }

private:
    std::shared_ptr<std::vector<double>> ticks_;
};

} // namespace

TEST_CASE("the model, the tracker and the output thread each ask to run ahead",
          "[output][engine]") {
    // The audit's M13, read off the threads themselves: each asks for its own priority as it
    // starts, and the model's also reads denormals as zero. What that does is the next test's.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    engine->start();
    runner.start();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!(engine->workerRaised() && engine->activations().workerRaised() &&
             runner.threadRaised()) &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
#if defined(_WIN32)
    CHECK(engine->workerRaised());
    CHECK(engine->activations().workerRaised());
    CHECK(runner.threadRaised());
#endif
#if defined(_M_X64) || defined(__x86_64__)
    CHECK(engine->activations().workerFlushesDenormals());
#endif
    runner.stop();
    engine->stop();
}

TEST_CASE("the MIDI clock keeps its time while every core is busy", "[output][midi][hardware]") {
    // The audit's M13 as a receiver meets it. The output thread ran at the priority every
    // thread starts at, the window's included, so on a machine with its cores busy — Skia
    // drawing, a video server, another program — a tick waited its turn behind them. Here
    // every core is kept busy at that ordinary priority for three seconds, and the gaps between
    // ticks are held to the tempo's spacing. Tagged `hardware` because it loads the whole
    // machine, which on a live rig is as disruptive as opening its interface.
    //
    // Measured on the 16-thread rig, 2026-09-24: with nothing else running, 99% of gaps are
    // within 1.4 ms of the spacing either way — the millisecond rounds. With every core busy and
    // the priority raised, 99% within 1.7 to 2.2 ms over fifteen runs; without it, 20.8 ms every
    // run, which is a tick a whole tick late and the next bunched up behind it. So the 99% is
    // what tells the two apart. The single worst gap is not: raised, it was 1.7 ms in most runs
    // and 8 to 15 ms in three of fifteen, where the first version of this test held it to 8 ms
    // and failed one run in five. It is held to less than a whole tick, a tick the receiver
    // still counts in its place; the 99% is the check that fails without the priority.
    auto ticks = std::make_shared<std::vector<double>>();
    Transports::Config config;
    config.midiClockPort = "Clock";
    config.openMidi = [ticks](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(name, std::make_unique<TickTimer>(ticks));
    };
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);
    runner.start();
    runner.setTracking(true); // 120 BPM until a beat says otherwise: a tick every 1/48 s

    std::atomic<bool> busy{true};
    std::vector<std::thread> load;
    const unsigned int cores = std::max(1U, std::thread::hardware_concurrency());
    for (unsigned int i = 0; i < cores; ++i) {
        load.emplace_back([&busy] {
            volatile double sum = 0.0;
            while (busy.load(std::memory_order_relaxed)) {
                sum = sum + 1.0;
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds{3});
    busy.store(false, std::memory_order_relaxed);
    for (std::thread& thread : load) {
        thread.join();
    }
    runner.stop(); // the output thread is gone, so the ticks can be read

    REQUIRE(ticks->size() > 100);
    const double spacing = 60.0 / (120.0 * 24.0);
    std::vector<double> errors;
    for (std::size_t i = 1; i < ticks->size(); ++i) {
        errors.push_back(std::abs((*ticks)[i] - (*ticks)[i - 1] - spacing));
    }
    std::sort(errors.begin(), errors.end());
    const double worst = errors.back();
    const double p99 = errors[errors.size() * 99 / 100];
    INFO(ticks->size() << " ticks over " << cores << " busy cores: 99% within " << p99 * 1000.0
                       << " ms of the spacing, the worst " << worst * 1000.0 << " ms");
    CHECK(p99 < 0.004);
    CHECK(worst < spacing);
}

TEST_CASE("PANIC leaves a lamp where it is, whatever its release would have done",
          "[output][dmx]") {
    // The audit's M6. PANIC pays out every release it owes, so that a clip or a laser note lets
    // go, and then freezes the lights (the operator's call, 2026-09-16). A lighting release is
    // the fired effect again at the release level over the rule's own fade time, so paying it
    // could turn off a lamp whose rule snapped while leaving one whose rule faded. Here the
    // rule snaps to full with a release owed, and an OSC rule owes one too.
    //
    // Two ways the release can stand at the moment of the PANIC. Owed well ahead, the sink
    // holds it until its moment and the PANIC drops what is held. But owed a little ahead on
    // a rig whose offset is further ahead still, its moment has passed once the offset is
    // applied, and the sink would start it the moment PANIC paid it out.
    struct Case {
        const char* what;
        double releaseAfter;
        double offset;
    };
    for (const Case& c : {Case{"owed ten seconds ahead", 10.0, 0.0},
                          Case{"owed 0.2 s ahead, with the rig 0.5 s early", 0.2, -0.5}}) {
        INFO(c.what);
        LoopbackReceiver server;
        Transports::Config config;
        config.patch = {takt4::dmx::fixtureFromMode("par", 1, 0, 1)};
        config.patch[0].id = "par"; // what a rule aims at
        takt4::output::OutputTarget deck;
        deck.id = "o-00000dec";
        deck.name = "deck";
        deck.host = "127.0.0.1";
        deck.port = server.port();
        config.outputs = {deck};
        auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
        OutputRunner runner(*engine, config); // stopped, so every post applies at once
        runner.setLatencySeconds(c.offset);

        Rule::Config lamp;
        lamp.id = "lamp";
        lamp.sendKind = takt4::trigger::Message::Kind::Dmx;
        lamp.dmx.effect = takt4::dmx::EffectKind::Level;
        lamp.dmx.role = takt4::dmx::Role::Dimmer;
        lamp.dmx.level = takt4::trigger::fixedNumber(255);
        lamp.dmx.unit = takt4::trigger::DelayUnit::Milliseconds;
        lamp.dmx.durationSeconds = 0.0; // a snap: its release would be one too
        lamp.dmx.fixtures = {"par"};
        takt4::trigger::FollowUp release;
        release.delaySeconds = c.releaseAfter;
        lamp.followUps = {release};
        Rule::Config clip;
        clip.id = "clip";
        clip.address = "/clip";
        clip.followUps = {release};
        runner.post(OutputCommand::rules({lamp, clip}));
        runner.post(OutputCommand::testRule("lamp"));
        runner.post(OutputCommand::testRule("clip"));
        // The brightest of the par's channels, wherever the level lands on this fixture.
        const auto lit = [&runner] {
            const auto levels = runner.transports().dmx().levels(0);
            return *std::max_element(levels.begin(), levels.begin() + 8);
        };
        REQUIRE(lit() == 255);
        int before = 0;
        for (std::string datagram = server.receive(); !datagram.empty();
             datagram = server.receive()) {
            before += datagram.rfind("/clip", 0) == 0 ? 1 : 0;
        }
        REQUIRE(before == 1); // the press; its release is still owed

        runner.post(OutputCommand::panic(true));
        CHECK(lit() == 255);
        int released = 0;
        for (std::string datagram = server.receive(); !datagram.empty();
             datagram = server.receive()) {
            released += datagram.rfind("/clip", 0) == 0 ? 1 : 0;
        }
        CHECK(released == 1); // and the clip's release went, as PANIC promises
    }
}

TEST_CASE("PANIC drops held lighting rather than starting it frozen", "[output][dmx]") {
    // A beat fired ahead of time holds its lighting until the beat. PANIC freezes the lights,
    // and starting a held flash only to freeze it on its first frame would hold a lamp at full.
    Transports::Config config;
    config.patch = {takt4::dmx::fixtureFromMode("par", 1, 0, 1)};
    Transports transports(config);
    takt4::output::RuleSink sink(transports);
    takt4::trigger::Message flash;
    flash.kind = takt4::trigger::Message::Kind::Dmx;
    flash.fixtures = 0b1;
    flash.payload.kind = takt4::dmx::EffectKind::Flash;
    flash.payload.role = takt4::dmx::Role::Dimmer;
    flash.payload.level = 255;
    flash.payload.durationSeconds = 0.4f;
    flash.moment = 10.5;
    sink.setNow(10.2);
    sink.send(flash);
    REQUIRE(sink.queued() == 1);
    sink.dropQueuedLighting();
    sink.flushQueued();
    CHECK(sink.queued() == 0);
    CHECK(transports.dmx().running() == 0);
    CHECK(transports.dmx().levels(0)[0] == 0);
}

TEST_CASE("a hand-fired effect or channel test sends nothing while PANIC is engaged",
          "[output][dmx]") {
    // The audit's M17. The color-picker preview, IDENTIFY and a channel TEST go to the lights
    // past the rules — and so past the halt too: a drag on the picker lit fixtures in the
    // middle of a panic. Run on stopped runners, where a post applies at once.
    Transports::Config config;
    config.patch = {takt4::dmx::fixtureFromMode("par", 1, 0, 1)};
    takt4::dmx::Payload white;
    white.kind = takt4::dmx::EffectKind::Color;
    white.color = {255, 255, 255};
    white.durationSeconds = 0.0f;
    // The brightest of the par's channels, whichever of them the colour lands on.
    const auto lit = [](const OutputRunner& runner) {
        const auto levels = runner.transports().dmx().levels(0);
        return *std::max_element(levels.begin(), levels.begin() + 8);
    };

    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    {
        // Without a panic the preview lands — which is what makes the check below mean
        // something.
        OutputRunner runner(*engine, config);
        runner.post(OutputCommand::effect(0b1, white));
        CHECK(lit(runner) == 255);
    }
    OutputRunner runner(*engine, config);
    runner.post(OutputCommand::panic(true));
    REQUIRE(runner.panicked());
    runner.post(OutputCommand::effect(0b1, white));
    CHECK(lit(runner) == 0);
    runner.post(OutputCommand::channelTest(0, 1, 255, 3.0));
    CHECK(runner.transports().dmx().running() == 0);
    CHECK(lit(runner) == 0);

    SECTION("and after RELEASE it is sent again") {
        runner.post(OutputCommand::panic(false));
        runner.post(OutputCommand::effect(0b1, white));
        CHECK(lit(runner) == 255);
    }
}

TEST_CASE("a name server that does not answer does not stop the output thread",
          "[output][network]") {
    // The audit's H12. An output typed as a host name was resolved on the output thread — the
    // one with the MIDI clock, Link, Art-Net's keep-alive and every rule on it — so a name the
    // network could not answer stopped all of that for the resolver's timeout. A missing
    // ".local" name takes about nine seconds to fail on the machine this was written on.
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, Transports::Config{});
    runner.start();
    takt4::output::OutputTarget slow;
    slow.id = "o-000000d1";
    slow.name = "slow";
    slow.host = "takt4-no-such-host.local";
    slow.port = 9000;
    runner.post(OutputCommand::outputs({slow}));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::uint64_t before = runner.rounds();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const std::uint64_t during = runner.rounds() - before;
    runner.stop();
    INFO("rounds in half a second: " << during);
    // A thread stuck in the lookup does next to none in that half second — the lookup takes
    // seconds — and a free one does 250 to 500, one a millisecond where the timer allows it.
    // So the line is well below the second and far above the first: a CI runner gave exactly
    // 250 on 2026-09-23, a coarser timer rather than a stuck thread, and failed `> 250`.
    CHECK(during > 100);
    // And the target is there, waiting for its address rather than refused.
    CHECK(runner.snapshot().outputs.size() == 1);
}

TEST_CASE("a delay slider moves one output's delay and drops nothing held", "[output][osc]") {
    // The audit's H12, the other half: the slider posted the whole target list on every pixel
    // of a drag, and every one of those cleared what was queued for a delayed target.
    LoopbackReceiver server;
    Transports::Config config;
    takt4::output::OutputTarget media;
    media.id = "o-000000e1";
    media.name = "media";
    media.host = "127.0.0.1";
    media.port = server.port();
    media.delaySeconds = 0.8;
    config.outputs = {media};
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);

    Rule::Config rule;
    rule.id = "clip";
    rule.address = "/clip";
    runner.post(OutputCommand::rules({rule}));
    runner.post(OutputCommand::testRule("clip"));
    REQUIRE(runner.transports().osc().pending() == 1);

    runner.post(OutputCommand::outputDelay(media.id, 0.3));
    CHECK(runner.transports().osc().pending() == 1);
    CHECK(runner.snapshot().outputs[0].delaySeconds == 0.3);

    // And the whole list again, as the other edits send it, with nothing about this output
    // changed: its sender and what it holds are kept.
    media.delaySeconds = 0.3;
    runner.post(OutputCommand::outputs({media}));
    CHECK(runner.transports().osc().pending() == 1);
}

TEST_CASE("a release owed to an output follows it when another is added above it",
          "[output][trigger]") {
    // The audit's H12, and the operator's rule that nothing hangs off something editable: a
    // follow-up carries its routing as a bit, and the bit is the output's place in the list.
    // An output added above it moved every place below — so a release owed to the deck went
    // to whatever took the deck's place.
    LoopbackReceiver deck;
    LoopbackReceiver fresh;
    takt4::output::OutputTarget deckTarget;
    deckTarget.id = "o-000000f1";
    deckTarget.name = "deck";
    deckTarget.host = "127.0.0.1";
    deckTarget.port = deck.port();
    takt4::output::OutputTarget freshTarget = deckTarget;
    freshTarget.id = "o-000000f2";
    freshTarget.name = "fresh";
    freshTarget.port = fresh.port();

    Transports::Config config;
    config.outputs = {deckTarget};
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);

    Rule::Config rule;
    rule.id = "clip";
    rule.address = "/clip";
    rule.outputs = {deckTarget.id};
    takt4::trigger::FollowUp release;
    release.value = takt4::trigger::Value::ofInt(0);
    release.unit = takt4::trigger::DelayUnit::Milliseconds;
    release.delaySeconds = 0.3;
    rule.followUps.push_back(release);
    runner.post(OutputCommand::rules({rule}));
    runner.post(OutputCommand::testRule("clip"));
    REQUIRE(runner.triggers().pending() == 1);
    REQUIRE(deck.receive().find("/clip") != std::string::npos); // the press

    runner.post(OutputCommand::outputs({freshTarget, deckTarget}));
    CHECK(runner.triggers().pending() == 1); // still owed, at its own time

    // Running now, so the generic namespace goes to both as well — what matters is where the
    // release to "/clip" lands.
    runner.start();
    bool releasedAtDeck = false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!releasedAtDeck && std::chrono::steady_clock::now() < until) {
        releasedAtDeck = deck.receive().find("/clip") != std::string::npos;
    }
    runner.stop();
    bool releasedAtFresh = false;
    for (std::string datagram = fresh.receive(); !datagram.empty(); datagram = fresh.receive()) {
        releasedAtFresh = releasedAtFresh || datagram.find("/clip") != std::string::npos;
    }
    CHECK(releasedAtDeck);
    CHECK_FALSE(releasedAtFresh);
}

TEST_CASE("the log of a rule that moves one head says which", "[output][dmx][heads]") {
    Transports::Config config;
    takt4::dmx::Fixture twin;
    twin.id = "f-twin";
    twin.name = "twin";
    twin.address = 1;
    twin.channels = {takt4::dmx::Role::Pan, takt4::dmx::Role::Tilt, takt4::dmx::Role::Pan,
                     takt4::dmx::Role::Tilt};
    twin.parked = {128, 128, 128, 128};
    config.patch = {twin};
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config); // stopped, so every post applies at once

    Rule::Config right;
    right.id = "right";
    right.sendKind = takt4::trigger::Message::Kind::Dmx;
    right.dmx.effect = takt4::dmx::EffectKind::Home;
    right.dmx.fixtures = {"f-twin"};
    right.dmx.heads = 0b10; // head 2
    right.dmx.spread = 0.5;
    Rule::Config both = right;
    both.id = "both";
    both.dmx.heads = 0;
    both.dmx.spread = 0.0;
    runner.post(OutputCommand::rules({right, both}));
    runner.post(OutputCommand::testRule("right"));
    runner.post(OutputCommand::testRule("both"));

    std::vector<std::string> lines;
    for (const OutputRunner::Fired& entry : runner.takeFired()) {
        lines.push_back(entry.message);
    }
    REQUIRE(lines.size() == 2);
    CHECK_THAT(lines[0], Catch::Matchers::ContainsSubstring("head 2"));
    CHECK_THAT(lines[0], Catch::Matchers::ContainsSubstring("spread 50%"));
    CHECK_THAT(lines[1], !Catch::Matchers::ContainsSubstring("head"));
    CHECK_THAT(lines[1], !Catch::Matchers::ContainsSubstring("spread"));
}

namespace {

/// A Liberation zone at channel 1 and a dimmer at 100, the zone armed on clip 4-2 and the lamp at
/// 180 — through the runner's own queue, as the rule editor's preview reaches them.
void armZoneBesideLamp(OutputRunner& runner) {
    takt4::dmx::Payload clip;
    clip.kind = takt4::dmx::EffectKind::Clip;
    clip.clip = static_cast<std::uint16_t>(takt4::dmx::liberation::indexOf({4, 2}) + 1);
    clip.level = 255;
    runner.post(OutputCommand::effect(0b01, clip));
    takt4::dmx::Payload lamp;
    lamp.kind = takt4::dmx::EffectKind::Level;
    lamp.role = takt4::dmx::Role::Dimmer;
    lamp.level = 180;
    runner.post(OutputCommand::effect(0b10, lamp));
}

Transports::Config zoneBesideLamp() {
    Transports::Config config;
    config.patch = {takt4::dmx::liberation::zone("laser", 0, 1),
                    takt4::dmx::fixtureFromMode("lamp", 0, 0, 100)};
    config.patch[0].id = "laser";
    config.patch[1].id = "lamp";
    return config;
}

/// Arm, Gobo Select and the lamp, read between two of the output thread's rounds.
std::array<std::uint8_t, 3> zoneAndLamp(const OutputRunner& runner) {
    return runner.inspect([](const auto&, const Transports& transports, const auto&) {
        const auto levels = transports.dmx().levels(0);
        return std::array<std::uint8_t, 3>{levels[0], levels[3], levels[99]};
    });
}

} // namespace

TEST_CASE("PANIC disarms the lasers and freezes the lamps", "[output][dmx][liberation]") {
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, zoneBesideLamp()); // stopped, so every post applies at once
    armZoneBesideLamp(runner);
    const std::uint8_t select =
        takt4::dmx::liberation::goboOf(takt4::dmx::liberation::indexOf({4, 2})).select;
    REQUIRE(zoneAndLamp(runner) == std::array<std::uint8_t, 3>{255, select, 180});

    runner.post(OutputCommand::panic(true));
    // The laser disarmed with its clip taken off; the lamp where PANIC found it.
    CHECK(zoneAndLamp(runner) == std::array<std::uint8_t, 3>{0, 0, 180});

    runner.post(OutputCommand::panic(false));
    armZoneBesideLamp(runner);
    CHECK(zoneAndLamp(runner)[0] == 255); // and the next clip arms it again
}

TEST_CASE("the input going quiet disarms the lasers and leaves the lamps",
          "[output][dmx][liberation]") {
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, zoneBesideLamp());
    armZoneBesideLamp(runner);
    REQUIRE(zoneAndLamp(runner)[0] == 255);

    runner.start();
    // Digital silence, past the four seconds that say there is no signal.
    const std::vector<float> silence(
        static_cast<std::size_t>(6.0 * takt4::audio::kInternalSampleRate), 0.0f);
    for (std::size_t hop = 0; hop < silence.size() / kHopSize; ++hop) {
        engine->processHop(silence.data() + hop * kHopSize, hop);
        (void)engine->step();
    }
    REQUIRE(engine->state().noSignal);
    waitForRounds(runner, 3);
    CHECK(zoneAndLamp(runner) == std::array<std::uint8_t, 3>{0, 0, 180});
    runner.stop();
}

TEST_CASE("Liberation hears the clip a rule picked, armed and lit, on the wire",
          "[output][dmx][liberation]") {
    // Verified as the operator asked on 2026-10-05: not by launching Liberation, but by a listener
    // of the test's own on a loopback port, reading every ArtDmx frame and decoding it with the
    // formula Liberation's own document gives. Two zones, at 1 and 33; only the first is fired.
    namespace liberation = takt4::dmx::liberation;
    LoopbackReceiver node;
    Transports::Config config;
    takt4::output::OutputTarget target;
    target.id = "o-0000libe";
    target.name = "Liberation";
    target.kind = takt4::output::OutputTarget::Kind::ArtNet;
    target.host = "127.0.0.1";
    target.port = node.port();
    config.outputs = {target};
    config.patch = {liberation::zone("laser 1", 0, 1), liberation::zone("laser 2", 0, 33)};
    config.patch[0].id = "z1";
    config.patch[1].id = "z2";
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);
    runner.start();

    // A frame with both zones in it, read off the wire until `wanted` says it is the one.
    const auto frameWhere = [&node](auto wanted) {
        for (int attempt = 0; attempt < 400; ++attempt) {
            const std::string datagram = node.receive();
            if (datagram.empty()) {
                break;
            }
            const std::vector<std::uint8_t> levels = artDmxLevels(datagram);
            if (levels.size() >= 64 && wanted(levels)) {
                return levels;
            }
        }
        return std::vector<std::uint8_t>{};
    };

    // Parked: both disarmed, dark and on no clip — and at full scale, which is what renders.
    const std::vector<std::uint8_t> parked = frameWhere([](const auto&) { return true; });
    REQUIRE(parked.size() >= 64);
    CHECK(parked[0] == 0);
    CHECK(parked[1] == 0);
    CHECK(parked[3] == 0);
    CHECK(parked[9] == 255);
    CHECK(parked[10] == 255);
    CHECK(parked[32] == 0);

    Rule::Config rule;
    rule.id = "laser1";
    rule.sendKind = takt4::trigger::Message::Kind::Dmx;
    rule.dmx.effect = takt4::dmx::EffectKind::Clip;
    rule.dmx.fixtures = {"z1"};
    rule.dmx.clip.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.dmx.clip.fixed = takt4::trigger::Value::ofInt(liberation::indexOf({21, 1}));
    rule.dmx.level = takt4::trigger::fixedNumber(200);
    runner.post(OutputCommand::rules({rule}));
    runner.post(OutputCommand::testRule("laser1"));

    // Armed (Liberation renders at 250 and up), lit, and on 21-1 as Liberation decodes it.
    const std::vector<std::uint8_t> lit =
        frameWhere([](const auto& levels) { return levels[0] >= 250; });
    REQUIRE(lit.size() >= 64);
    CHECK(lit[1] == 200);
    CHECK(liberation::decode(lit[2], lit[3]) == liberation::Clip{21, 1});
    CHECK(lit[32] == 0); // the second zone, untouched
    CHECK_FALSE(liberation::decode(lit[34], lit[35]).has_value());

    // And PANIC takes it off the air.
    runner.post(OutputCommand::panic(true));
    const std::vector<std::uint8_t> off =
        frameWhere([](const auto& levels) { return levels[0] == 0; });
    REQUIRE(off.size() >= 64);
    CHECK_FALSE(liberation::decode(off[2], off[3]).has_value());
    runner.stop();
}

namespace {

using Bytes = std::vector<std::vector<unsigned char>>;

/// A laser desk on one MIDI cable, recorded, beside a Liberation zone at channel 1 and a lamp at
/// 100: the two things takt4 sends that stay on until something takes them off.
Transports::Config heldRig(const std::shared_ptr<Bytes>& cable) {
    Transports::Config config = zoneBesideLamp();
    takt4::output::OutputTarget desk;
    desk.id = "o-0000desk";
    desk.name = "laser desk";
    desk.kind = takt4::output::OutputTarget::Kind::Midi;
    desk.device = "Laser";
    config.outputs = {desk};
    config.openMidi = [cable](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(name,
                                                           std::make_unique<KeepingPort>(cable));
    };
    return config;
}

/// A note on with nothing to let go of it — no follow-up — on the desk.
Rule::Config heldNote() {
    Rule::Config rule;
    rule.id = "note";
    rule.trigger = takt4::trigger::Trigger::Manual;
    rule.sendKind = takt4::trigger::Message::Kind::MidiNote;
    rule.channel = 1;
    rule.number.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.number.fixed = takt4::trigger::Value::ofInt(60);
    rule.value.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.value.fixed = takt4::trigger::Value::ofInt(100);
    return rule;
}

/// A Liberation clip on the zone, with nothing to take it off: the preset's own rule.
Rule::Config heldClip(std::string id = "clip") {
    Rule::Config rule;
    rule.id = std::move(id);
    rule.trigger = takt4::trigger::Trigger::Manual;
    rule.sendKind = takt4::trigger::Message::Kind::Dmx;
    rule.dmx.effect = takt4::dmx::EffectKind::Clip;
    rule.dmx.fixtures = {"laser"};
    rule.dmx.clip.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.dmx.clip.fixed =
        takt4::trigger::Value::ofInt(takt4::dmx::liberation::indexOf({4, 2}));
    rule.dmx.level = takt4::trigger::fixedNumber(255);
    return rule;
}

/// The releases the log shows, as "rule: message".
std::vector<std::string> releasesLogged(OutputRunner& runner) {
    std::vector<std::string> out;
    for (const OutputRunner::Fired& entry : runner.takeFired()) {
        if (entry.followUp) {
            out.push_back(entry.ruleId + ": " + entry.message);
        }
    }
    return out;
}

} // namespace

TEST_CASE("a rule that stops sending lets go of what it holds", "[output][trigger][release]") {
    // The operator, 2026-10-06: "if I mute a trigger by clicking its green circle in the rules
    // editor, the laser will sometimes still play. might do this for midi shit too, anything that
    // needs an excpicit release/disarm". A Liberation clip arms its zone until something takes it
    // off, and a note on sounds until its note off; a rule muted, switched off, deleted or aimed
    // elsewhere would never send either again. Not started, so every post applies at once.
    auto cable = std::make_shared<Bytes>();
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, heldRig(cable));
    const Rule::Config note = heldNote();
    const Rule::Config clip = heldClip();
    runner.post(OutputCommand::rules({note, clip}));
    runner.post(OutputCommand::testRule("note"));
    runner.post(OutputCommand::testRule("clip"));
    REQUIRE(*cable == Bytes{{0x90, 60, 100}});
    REQUIRE(zoneAndLamp(runner)[0] == 255);
    REQUIRE(runner.holds().notes == 1);
    REQUIRE(runner.holds().zones == 1);
    (void)runner.takeFired();

    SECTION("muted from the editor's dot") {
        runner.post(OutputCommand::ruleMuted("note", true));
        runner.post(OutputCommand::ruleMuted("clip", true));
    }
    SECTION("muted from a control surface, all at once") {
        runner.setRuleMuted("all", true);
    }
    SECTION("switched off") {
        runner.post(OutputCommand::ruleEnabled("note", false));
        runner.post(OutputCommand::ruleEnabled("clip", false));
    }
    SECTION("deleted") {
        runner.post(OutputCommand::rules({}));
    }
    SECTION("aimed elsewhere: another channel, another fixture") {
        Rule::Config moved = note;
        moved.channel = 2;
        Rule::Config lamp = clip;
        lamp.dmx.fixtures = {"lamp"};
        runner.post(OutputCommand::rules({moved, lamp}));
    }
    SECTION("made another kind or effect") {
        Rule::Config cc = note;
        cc.sendKind = takt4::trigger::Message::Kind::MidiCc;
        Rule::Config move = clip;
        move.dmx.effect = takt4::dmx::EffectKind::Position;
        runner.post(OutputCommand::rules({cc, move}));
    }
    SECTION("replaced by a show loaded") {
        runner.post(OutputCommand::rules({note, clip}, true));
    }
    SECTION("PANIC") {
        runner.post(OutputCommand::panic(true));
    }
    SECTION("Stop") {
        runner.post(OutputCommand::tracking(false));
    }
    // The note's own Note Off on the cable, and the zone disarmed with its clip taken off.
    CHECK(*cable == Bytes{{0x90, 60, 100}, {0x80, 60, 0}});
    CHECK(zoneAndLamp(runner)[0] == 0);
    CHECK(zoneAndLamp(runner)[1] == 0);
    CHECK(runner.holds().notes == 0);
    CHECK(runner.holds().zones == 0);
}

TEST_CASE("the log shows a release beside the rule that held it", "[output][trigger][release]") {
    auto cable = std::make_shared<Bytes>();
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, heldRig(cable));
    runner.post(OutputCommand::rules({heldNote(), heldClip()}));
    runner.post(OutputCommand::testRule("note"));
    runner.post(OutputCommand::testRule("clip"));
    (void)runner.takeFired();
    runner.post(OutputCommand::ruleMuted("note", true));
    runner.post(OutputCommand::ruleMuted("clip", true));
    const std::vector<std::string> logged = releasesLogged(runner);
    REQUIRE(logged.size() == 2);
    CHECK(logged[0] == "note: note off 60 ch 1");
    CHECK_THAT(logged[1], Catch::Matchers::StartsWith("clip: ") &&
                              Catch::Matchers::EndsWith(" none"));
}

TEST_CASE("an edit that keeps a rule's aim leaves what it holds", "[output][trigger][release]") {
    // Every keystroke in the editor posts the whole set. A rename must not cut a note or drop a
    // laser between two of a rule's own fires.
    auto cable = std::make_shared<Bytes>();
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, heldRig(cable));
    Rule::Config note = heldNote();
    Rule::Config clip = heldClip();
    runner.post(OutputCommand::rules({note, clip}));
    runner.post(OutputCommand::testRule("note"));
    runner.post(OutputCommand::testRule("clip"));
    note.name = "renamed";
    clip.name = "renamed too";
    clip.dmx.fixtures = {"laser", "lamp"}; // aimed at more, and still at the zone
    runner.post(OutputCommand::rules({note, clip}));
    CHECK(*cable == Bytes{{0x90, 60, 100}});
    CHECK(zoneAndLamp(runner)[0] == 255);
    CHECK(runner.holds().notes == 1);
    CHECK(runner.holds().zones == 1);

    SECTION("and a rule muted lets go of only its own") {
        // A second clip rule arms the zone after the first: the zone is the second's now, and
        // muting the first leaves it lit.
        runner.post(OutputCommand::rules({note, clip, heldClip("other")}));
        runner.post(OutputCommand::testRule("other"));
        runner.post(OutputCommand::ruleMuted("clip", true));
        CHECK(zoneAndLamp(runner)[0] == 255);
        runner.post(OutputCommand::ruleMuted("other", true));
        CHECK(zoneAndLamp(runner)[0] == 0);
    }
}

TEST_CASE("a MIDI output switched off is sent the note offs it is owed", "[output][trigger][release]") {
    // Held on two outputs, one switched off: its note off goes to that one alone, before its port
    // closes, and the other still holds the note.
    using Kind = takt4::output::OutputTarget::Kind;
    std::map<std::string, std::shared_ptr<Bytes>> cables{{"Laser", std::make_shared<Bytes>()},
                                                         {"Synth", std::make_shared<Bytes>()}};
    const auto midi = [](const char* id, const char* device) {
        takt4::output::OutputTarget target;
        target.id = id;
        target.name = id;
        target.kind = Kind::Midi;
        target.device = device;
        return target;
    };
    Transports::Config config;
    config.outputs = {midi("o-laser", "Laser"), midi("o-synth", "Synth")};
    config.openMidi = [cables](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(
            name, std::make_unique<KeepingPort>(cables.at(name)));
    };
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);
    runner.post(OutputCommand::rules({heldNote()}));
    runner.post(OutputCommand::testRule("note"));
    REQUIRE(*cables["Laser"] == Bytes{{0x90, 60, 100}});
    REQUIRE(*cables["Synth"] == Bytes{{0x90, 60, 100}});

    takt4::output::OutputTarget off = midi("o-laser", "Laser");
    off.enabled = false;
    runner.post(OutputCommand::outputs({off, midi("o-synth", "Synth")}));
    CHECK(*cables["Laser"] == Bytes{{0x90, 60, 100}, {0x80, 60, 0}});
    CHECK(*cables["Synth"] == Bytes{{0x90, 60, 100}});
    CHECK(runner.holds().notes == 1); // still on the synth

    // And PANIC lets go of it there — the synth now the first output in the list.
    runner.post(OutputCommand::outputs({midi("o-synth", "Synth")}));
    runner.post(OutputCommand::panic(true));
    CHECK(*cables["Synth"] == Bytes{{0x90, 60, 100}, {0x80, 60, 0}});
    CHECK(runner.holds().notes == 0);
}

TEST_CASE("the input going quiet lets go of the notes it holds", "[output][trigger][release]") {
    // As it disarms the lasers: a note on is a Liberation clip too, on a rig that drives it by
    // MIDI, and nothing fires again until the next lock.
    auto cable = std::make_shared<Bytes>();
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, heldRig(cable));
    runner.post(OutputCommand::rules({heldNote()}));
    runner.post(OutputCommand::testRule("note"));
    REQUIRE(runner.holds().notes == 1);

    runner.start();
    const std::vector<float> silence(
        static_cast<std::size_t>(6.0 * takt4::audio::kInternalSampleRate), 0.0f);
    for (std::size_t hop = 0; hop < silence.size() / kHopSize; ++hop) {
        engine->processHop(silence.data() + hop * kHopSize, hop);
        (void)engine->step();
    }
    REQUIRE(engine->state().noSignal);
    waitForRounds(runner, 3);
    runner.stop();
    CHECK(*cable == Bytes{{0x90, 60, 100}, {0x80, 60, 0}});
}

TEST_CASE("an Art-Net node switched off is sent zeros: its lights dark, its lasers disarmed",
          "[output][dmx][liberation][release]") {
    // The operator, 2026-10-06: "unchecking the liberation artnet output in the main window
    // doesnt disarm them either" — the node held the last frame it was sent, which armed the zone
    // — and then "untickng artnet should black out lights / send zeroed". Read off the wire with
    // a listener of the test's own, never Liberation (the operator's rule).
    LoopbackReceiver node;
    Transports::Config config = zoneBesideLamp();
    takt4::output::OutputTarget target;
    target.id = "o-0000libe";
    target.name = "Liberation";
    target.kind = takt4::output::OutputTarget::Kind::ArtNet;
    target.host = "127.0.0.1";
    target.port = node.port();
    config.outputs = {target};
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config);
    runner.start();
    armZoneBesideLamp(runner);

    const auto frameWhere = [&node](auto wanted) {
        for (int attempt = 0; attempt < 400; ++attempt) {
            const std::string datagram = node.receive();
            if (datagram.empty()) {
                break;
            }
            const std::vector<std::uint8_t> levels = artDmxLevels(datagram);
            if (levels.size() >= 100 && wanted(levels)) {
                return levels;
            }
        }
        return std::vector<std::uint8_t>{};
    };
    REQUIRE_FALSE(frameWhere([](const auto& levels) {
                      return levels[0] == 255 && levels[99] == 180;
                  }).empty());

    takt4::output::OutputTarget off = target;
    off.enabled = false;
    runner.post(OutputCommand::outputs({off}));
    // Every channel zero: the zone disarmed with its clip taken off, the lamp dark — and the
    // zone's scale, parked at full, zero too: nothing of the frame before is left.
    const auto dark = [](const std::vector<std::uint8_t>& levels) {
        return levels.size() == 512 &&
               std::all_of(levels.begin(), levels.end(), [](std::uint8_t v) { return v == 0; });
    };
    REQUIRE_FALSE(frameWhere(dark).empty());
    const auto first = std::chrono::steady_clock::now();
    // And then nothing: every frame until the line goes quiet is the same, for a released
    // universe's three seconds, and then it goes quiet.
    int after = 0;
    bool lit = false;
    auto last = first;
    for (std::string datagram = node.receive(); !datagram.empty(); datagram = node.receive()) {
        ++after;
        last = std::chrono::steady_clock::now();
        lit = lit || !dark(artDmxLevels(datagram));
        REQUIRE(after < 400); // three seconds at 44 Hz is 132; this is a node that never stops
    }
    CHECK_FALSE(lit);
    const double lasted = std::chrono::duration<double>(last - first).count();
    INFO("zeros for " << lasted << " s, " << after << " frames after the first");
    CHECK(lasted > 2.5);
    CHECK(lasted < 3.5);
    CHECK(runner.inspect([](const auto&, const Transports& transports, const auto&) {
        return transports.artnet().leaving();
    }) == 0);
    // The engine still has the zone armed: switched back on, the node is fed what takt4 holds.
    CHECK(zoneAndLamp(runner)[0] == 255);
    runner.stop();
}

TEST_CASE("a node switched back on within its farewell is fed, not fought",
          "[output][dmx][liberation][release]") {
    LoopbackReceiver node;
    Transports::Config config = zoneBesideLamp();
    takt4::output::OutputTarget target;
    target.id = "o-0000libe";
    target.name = "Liberation";
    target.kind = takt4::output::OutputTarget::Kind::ArtNet;
    target.host = "127.0.0.1";
    target.port = node.port();
    config.outputs = {target};
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, config); // stopped: nothing is sent, and every post applies now
    takt4::output::OutputTarget off = target;
    off.enabled = false;
    runner.post(OutputCommand::outputs({off}));
    const auto leaving = [&runner] {
        return runner.inspect([](const auto&, const Transports& transports, const auto&) {
            return transports.artnet().leaving();
        });
    };
    CHECK(leaving() == 1);
    runner.post(OutputCommand::outputs({target}));
    CHECK(leaving() == 0);
}
