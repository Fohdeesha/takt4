#include "core/audio/rates.hpp"
#include "core/control/rule_control.hpp"
#include "core/dmx/fixture.hpp"
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
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

/// The same for a rule's own count, which is what a test about firing rather than about
/// draining wants. Returns what it reached, so a caller can say how many rather than only
/// that it waited. Safe to read here for `triggers()`' own reason — the count is a plain
/// integer the output thread only ever increments, and the assertions that matter are made
/// after the stop.
std::uint64_t waitForFires(const OutputRunner& runner, std::uint64_t fires) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < until && runner.triggers().ruleCount() > 0 &&
           runner.triggers().rule(0).fires() < fires) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return runner.triggers().ruleCount() > 0 ? runner.triggers().rule(0).fires() : 0;
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

TEST_CASE("a change posted while stopped applies at once", "[output][network]") {
    // An app is configured before it is started, and an operator ticking Link with nothing
    // running should not have to press Start to find out whether it took.
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
    REQUIRE_FALSE(runner.transports().linkEnabled());

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

TEST_CASE("what a UI reads while the thread sends is a snapshot", "[output][network]") {
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

namespace {

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

TEST_CASE("the MIDI clock starts and stops with the tracker, not with the outputs",
          "[output][midi]") {
    // The outputs run from launch (H5); a drum machine given a Start then would play at the
    // clock's opening tempo before anything was listening.
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

    runner.setTracking(true);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    CHECK(count(takt4::output::MidiClock::kStart) == 1);
    CHECK(count(takt4::output::MidiClock::kTick) > 5);

    runner.setTracking(false);
    REQUIRE(runner.sync());
    CHECK(count(takt4::output::MidiClock::kStop) == 1);
    const auto ticks = count(takt4::output::MidiClock::kTick);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    CHECK(count(takt4::output::MidiClock::kTick) == ticks);
    runner.stop();
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
