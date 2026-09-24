#include "core/output/rule_sink.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "core/trigger/trigger_engine.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::output::Transports;
using takt4::tracking::BeatEvent;
using takt4::tracking::TempoState;

namespace {

/// A beat of the shape the tempo state machine produces: on the beat, in a bar of four.
BeatEvent beatAt(double time, std::uint32_t beatInBar, double bpm) {
    BeatEvent event;
    event.time = time;
    event.bpm = bpm;
    event.beatInBar = beatInBar;
    event.beatsPerBar = 4;
    event.downbeat = beatInBar == 1;
    event.locked = true;
    event.confidence = 0.8;
    return event;
}

/// A port name nothing on any machine can be called, for the failure path.
const std::string kNoSuchPort = "takt4 test — no such MIDI port exists";

} // namespace

TEST_CASE("transports with nothing switched on send nothing and still count", "[output]") {
    // The state an app is in before anyone has set an output up, which is most of the time
    // it is open. It has to be as harmless as it is uninteresting.
    Transports transports{Transports::Config{}};

    CHECK_FALSE(transports.any());
    CHECK_FALSE(transports.linkEnabled());
    CHECK(transports.osc().targetCount() == 0);
    CHECK(transports.midiClock() == nullptr);
    CHECK(transports.midiPort() == nullptr);
    CHECK_FALSE(transports.midiClockPort().has_value());

    transports.startOutputs(0.0);
    for (std::uint32_t beat = 1; beat <= 8; ++beat) {
        transports.publish(beatAt(0.5 * beat, (beat - 1) % 4 + 1, 120.0), 0, 0.5 * beat);
        transports.advance(0.5 * beat, TempoState{});
    }
    transports.stopOutputs();

    CHECK(transports.beats() == 8);
    CHECK(transports.downbeats() == 2); // beats 1 and 5
}

TEST_CASE("the latency offset round-trips and is what the transports fire on", "[output]") {
    // §5.5: the tracker applies the offset to event.time, and this is the same number
    // applied to the host times the transports fire on. The two drifting apart is the
    // failure — a beat reported at one time and sent at another.
    Transports::Config config;
    config.latencySeconds = -0.030;
    Transports transports(config);
    CHECK_THAT(transports.latencySeconds(), WithinAbs(-0.030, 1e-9));

    transports.setLatencySeconds(0.012);
    CHECK_THAT(transports.latencySeconds(), WithinAbs(0.012, 1e-9));
}

TEST_CASE("the latency offset reaches OSC and not only the two clocks", "[output][osc]") {
    // The user's report of 2026-09-07: *"I was going to use it to make the resolume clip
    // changing end up on beat but it didn't seem to work how I expected."* It did not,
    // because it moved Link and the MIDI clock and never touched the publisher — and
    // Resolume listens to OSC. This is the wiring that closed that.
    Transports::Config config;
    config.outputs = takt4::output::oscOutputs({{"127.0.0.1", 57000}});
    config.latencySeconds = -0.200;
    Transports transports(config);
    transports.startOutputs(0.0);

    // The state the beats below are reporting, so `advance` publishes nothing of its own and
    // every message counted here came from a beat.
    TempoState steady;
    steady.bpm = 120.0;
    steady.confidence = 0.8;
    steady.locked = true;
    steady.beatsPerBar = 4;

    // A beat in the music at 1.0, fired at 0.7 on a prediction — which is what the output
    // thread does while the tracker is locked (`BeatScheduler`). 200 ms early is 0.8, and
    // nothing has left before then.
    transports.publishBeat(beatAt(1.0, 1, 120.0), 1.0, 0.7);
    const std::size_t held = transports.osc().pending();
    CHECK(held > 0);
    CHECK(transports.osc().messagesSent() == 0);

    transports.advance(0.7999, steady);
    CHECK(transports.osc().messagesSent() == 0);
    CHECK(transports.osc().pending() == held);

    transports.advance(0.8001, steady);
    CHECK(transports.osc().messagesSent() == held);
    CHECK(transports.osc().pending() == 0);

    SECTION("a beat heard after its moment goes at once, not most of a beat later") {
        // Hunting, nothing is predicted, and a beat reaches the outputs a pipeline after its
        // moment. It used to be held for what was left of a beat after the offset — 300 ms
        // here — so a cue for this beat landed just before the next one (the audit's H4).
        const std::uint64_t before = transports.osc().messagesSent();
        transports.publish(beatAt(2.0, 2, 120.0), 0, 2.0);
        CHECK(transports.osc().messagesSent() > before);
        CHECK(transports.osc().pending() == 0);
    }

    SECTION("a positive offset holds even a beat heard as it happens") {
        transports.setLatencySeconds(0.050);
        transports.publish(beatAt(3.0, 3, 120.0), 0, 3.0);
        const std::size_t queued = transports.osc().pending();
        CHECK(queued > 0);
        transports.advance(3.0499, steady);
        CHECK(transports.osc().pending() == queued);
        transports.advance(3.0501, steady);
        CHECK(transports.osc().pending() == 0);
    }

    SECTION("and moving it mid-set leaves what is already queued where it was") {
        // A held message keeps the deadline it was given: rewriting deadlines under a queue
        // would reorder a rig in the middle of a bar.
        transports.publishBeat(beatAt(2.0, 2, 120.0), 2.0, 1.7);
        const std::size_t queued = transports.osc().pending();
        REQUIRE(queued > 0);

        transports.setLatencySeconds(0.0);
        transports.advance(1.75, steady); // before the 1.8 those are due at
        CHECK(transports.osc().pending() == queued);

        // The next beat feels the new offset: at its own moment, which is now.
        const std::uint64_t before = transports.osc().messagesSent();
        transports.publish(beatAt(1.76, 3, 120.0), 0, 1.76);
        CHECK(transports.osc().messagesSent() > before);
        CHECK(transports.osc().pending() == queued);
    }

    transports.stopOutputs();
}

TEST_CASE("Link is built whether or not it is switched on", "[output][link][network]") {
    // The invariant the audio thread depends on. `BeatEngine::setHostTimeSource` is handed
    // this session and reads it every hop (§4.3), so building it on demand would mean
    // destroying one under a running audio thread the first time Link was switched off.
    // It is built once, switched, and never replaced.
    Transports transports{Transports::Config{}};
    CHECK_FALSE(transports.linkEnabled());

    const auto* before = &transports.link();
    transports.startOutputs(0.0);
    transports.setLinkEnabled(true);
    CHECK(transports.linkEnabled());
    CHECK(transports.any());
    transports.setLinkEnabled(false);
    CHECK_FALSE(transports.linkEnabled());
    transports.stopOutputs();

    CHECK(&transports.link() == before); // the same session throughout
}

TEST_CASE("switching Link on before the outputs start does not join yet", "[output][link][network]") {
    // Setting something up is not the same as doing it: an operator ticks Link while the
    // tracker is stopped, and nothing should appear to peers until Start.
    Transports transports{Transports::Config{}};
    transports.setLinkEnabled(true);
    CHECK(transports.linkEnabled());
    CHECK_FALSE(transports.link().enabled());

    transports.startOutputs(0.0);
    CHECK(transports.link().enabled());
    transports.stopOutputs();
    CHECK_FALSE(transports.link().enabled());
}

TEST_CASE("OSC targets can be replaced, and a new one is told the state", "[output]") {
    Transports transports{Transports::Config{}};
    CHECK(transports.osc().targetCount() == 0);

    transports.setOscTargets({{"127.0.0.1", 57000}, {"127.0.0.1", 57001}});
    CHECK(transports.osc().targetCount() == 2);
    CHECK(transports.oscTargets().size() == 2);
    CHECK(transports.any());

    transports.startOutputs(0.0);
    TempoState state;
    state.bpm = 128.0;
    state.confidence = 0.7;
    state.locked = true;
    state.beatsPerBar = 4;
    transports.advance(0.1, state);
    const std::uint64_t afterFirst = transports.osc().messagesSent();
    CHECK(afterFirst > 0);

    // Nothing moved, so nothing is resent.
    transports.advance(0.2, state);
    CHECK(transports.osc().messagesSent() == afterFirst);

    // A target added mid-set has never been told the tempo. It must not have to wait for
    // the tempo to change before it learns what it is.
    transports.setOscTargets({{"127.0.0.1", 57002}});
    CHECK(transports.osc().targetCount() == 1);
    transports.advance(0.3, state);
    CHECK(transports.osc().messagesSent() > afterFirst);

    transports.setOscTargets({});
    CHECK(transports.osc().targetCount() == 0);
    CHECK_FALSE(transports.any());
    transports.stopOutputs();
}

TEST_CASE("an output the network will not send to says so, and the others carry on",
          "[output][osc]") {
    // The audit's T3 — sends failing because the network is down had no test — and the part
    // of M12 that goes with it: nothing said so, and the target just went quiet. The socket
    // refuses every send to `kUnsendableHost`, which is what a network that is down does to
    // all of them.
    takt4::testing::LoopbackReceiver receiver;
    takt4::output::OutputTarget deck;
    deck.id = "o-0000d0c0";
    deck.name = "deck";
    deck.host = takt4::testing::kUnsendableHost;
    deck.port = 57000;
    takt4::output::OutputTarget lights;
    lights.id = "o-0000e1e1";
    lights.name = "lights";
    lights.host = "127.0.0.1";
    lights.port = receiver.port();
    Transports transports{Transports::Config{}};
    transports.setOutputs({deck, lights});
    transports.startOutputs(0.0);
    TempoState state;
    state.bpm = 128.0;
    state.confidence = 0.7;
    state.locked = true;
    state.beatsPerBar = 4;
    transports.advance(0.1, state);

    // The one that works got the tempo; the one that cannot is named, with why.
    CHECK_FALSE(receiver.receive().empty());
    std::vector<std::string> problems = transports.outputProblems();
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].rfind("deck: sends are failing: ", 0) == 0);
    CHECK(problems[0].find(takt4::testing::kUnsendableReason) != std::string::npos);

    // Pointed somewhere it can reach, it stops saying so.
    deck.host = "127.0.0.1";
    deck.port = receiver.port();
    transports.setOutputs({deck, lights});
    state.bpm = 130.0;
    transports.advance(0.2, state);
    CHECK(transports.outputProblems().empty());
    transports.stopOutputs();
}

TEST_CASE("a MIDI port that will not open leaves the transports as they were", "[output]") {
    // The one reconfiguration that can fail, and the operator has to be able to keep
    // working when it does.
    Transports transports{Transports::Config{}};
    CHECK_THROWS(transports.setMidiClockPort(kNoSuchPort));
    CHECK(transports.midiClock() == nullptr);
    CHECK_FALSE(transports.midiClockPort().has_value());
    CHECK_FALSE(transports.any());

    // And it is still usable afterwards.
    transports.startOutputs(0.0);
    transports.publish(beatAt(0.5, 1, 120.0), 0, 0.5);
    CHECK(transports.beats() == 1);
    transports.stopOutputs();
}

TEST_CASE("a config naming a MIDI port that is not there throws", "[output]") {
    Transports::Config config;
    config.midiClockPort = kNoSuchPort;
    CHECK_THROWS(Transports{config});
}

namespace {

/// A MIDI device that can be pulled out and plugged back in, which nothing on a test machine
/// can do for real. Shared state, because the transports own the port and the test has to
/// reach the cable.
struct Cable {
    bool plugged = true;
    int opens = 0;
    std::uint64_t delivered = 0;
};

class UnpluggablePort final : public takt4::output::MidiPort {
public:
    explicit UnpluggablePort(std::shared_ptr<Cable> cable) : cable_(std::move(cable)) {}
    std::string open(std::string_view spec) override {
        ++cable_->opens;
        if (!cable_->plugged) {
            throw std::runtime_error("MIDI output: no port matching \"" + std::string(spec) + "\"");
        }
        return "Desk " + std::string(spec);
    }
    void close() noexcept override {}
    void send(std::span<const unsigned char>) override {
        if (!cable_->plugged) {
            throw std::runtime_error("the device has gone");
        }
        ++cable_->delivered;
    }

private:
    std::shared_ptr<Cable> cable_;
};

} // namespace

TEST_CASE("a MIDI device pulled out mid-set comes back on its own", "[output][midi]") {
    // The audit's H11a. A send to a port whose device has gone failed and was counted, and
    // nothing ever opened the port again — so a USB interface pulled out and pushed back in
    // mid-show stayed silent until takt4 was restarted. Driven through the clock, which sends
    // 24 times a beat and so finds a dead port within a beat.
    auto cable = std::make_shared<Cable>();
    Transports::Config config;
    config.midiClockPort = "Clock";
    config.openMidi = [cable](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(name,
                                                           std::make_unique<UnpluggablePort>(cable));
    };
    Transports transports(config);
    const TempoState state;
    transports.startOutputs(0.0);
    transports.startClock(0.0);
    double now = 0.0;
    const auto runFor = [&](double seconds) {
        const double until = now + seconds;
        while (now < until) {
            now += 0.001;
            transports.advance(now, state);
        }
    };

    runFor(1.0);
    REQUIRE(cable->delivered > 0);
    CHECK(transports.lostMidiDevices().empty());

    cable->plugged = false;
    runFor(0.5);
    const std::vector<std::string> lost = transports.lostMidiDevices();
    REQUIRE(lost.size() == 1);
    CHECK(lost.front() == "Clock");
    CHECK(transports.lostMidiCount() == 1);

    // Looked for again once a second, not once a tick: five seconds unplugged is about five
    // tries, not five thousand.
    const int opensBefore = cable->opens;
    runFor(5.0);
    CHECK(cable->opens - opensBefore >= 4);
    CHECK(cable->opens - opensBefore <= 6);
    CHECK(transports.lostMidiCount() == 1);

    // Plugged back in: within a second it is found, and the clock is sending again without
    // anybody touching anything.
    cable->plugged = true;
    const std::uint64_t deliveredBefore = cable->delivered;
    runFor(1.5);
    CHECK(transports.lostMidiDevices().empty());
    CHECK(cable->delivered > deliveredBefore);
    transports.stopOutputs();
}

TEST_CASE("picking a lost MIDI port again reopens it at once", "[output][midi]") {
    // Re-picking the port that was already selected did nothing, because it was "already
    // open" — which is exactly what an operator does after plugging the cable back in.
    auto cable = std::make_shared<Cable>();
    Transports::Config config;
    config.midiClockPort = "Clock";
    config.openMidi = [cable](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(name,
                                                           std::make_unique<UnpluggablePort>(cable));
    };
    Transports transports(config);
    const TempoState state;
    transports.startOutputs(0.0);
    transports.startClock(0.0);
    transports.advance(0.1, state);
    cable->plugged = false;
    for (double now = 0.101; now < 0.4; now += 0.001) {
        transports.advance(now, state);
    }
    REQUIRE(transports.lostMidiCount() == 1);

    cable->plugged = true;
    transports.setMidiClockPort(std::string("Clock"));
    CHECK(transports.lostMidiCount() == 0);
    transports.stopOutputs();
}

namespace {

/// Every MIDI message the transports send, and when, on the clock the test drives.
struct TimedWire {
    struct Message {
        double at = 0.0;
        std::vector<unsigned char> bytes;
    };
    double now = 0.0;
    std::vector<Message> messages;
};

class TimedPort final : public takt4::output::MidiPort {
public:
    explicit TimedPort(std::shared_ptr<TimedWire> wire) : wire_(std::move(wire)) {}
    std::string open(std::string_view spec) override { return std::string(spec); }
    void close() noexcept override {}
    void send(std::span<const unsigned char> message) override {
        wire_->messages.push_back(
            TimedWire::Message{wire_->now, std::vector<unsigned char>(message.begin(), message.end())});
    }

private:
    std::shared_ptr<TimedWire> wire_;
};

} // namespace

TEST_CASE("a MIDI receiver's bar 1 is a downbeat of the music", "[output][midi]") {
    // The audit's M19, end to end: beats heard a pipeline late with their stamps jittered, the
    // tracker unlocked for its first eight, and the clock ticking from the press all along. A
    // receiver counts its bars from the first tick after Start, so that tick has to be on a
    // downbeat — and every 96th after it, or the bars have slipped.
    for (const double latency : {0.0, -0.040}) {
        INFO("latency " << latency * 1000.0 << " ms");
        auto wire = std::make_shared<TimedWire>();
        Transports::Config config;
        config.midiClockPort = "Clock";
        config.openMidi = [wire](const std::string& name) {
            return std::make_unique<takt4::output::MidiOutput>(name, std::make_unique<TimedPort>(wire));
        };
        Transports transports(config);
        transports.setLatencySeconds(latency);
        transports.startOutputs(0.0);
        transports.startClock(0.0);

        constexpr double kBpm = 128.0;
        constexpr double kPipeline = 0.060;
        constexpr std::size_t kBeats = 64;
        constexpr std::size_t kLockedFrom = 8;
        const double beat = 60.0 / kBpm;
        std::vector<double> beats;
        for (std::size_t k = 0; k < kBeats; ++k) {
            beats.push_back(0.137 + static_cast<double>(k) * beat);
        }
        std::mt19937 random(20260924);
        const TempoState state;
        std::size_t next = 0;
        double firstLockedHeard = -1.0;
        for (double now = 0.0; now < beats.back() + 1.0; now += 0.001) {
            while (next < kBeats && beats[next] + kPipeline <= now) {
                BeatEvent event = beatAt(beats[next], static_cast<std::uint32_t>(next % 4) + 1, kBpm);
                event.locked = next >= kLockedFrom;
                if (event.locked && firstLockedHeard < 0.0) {
                    firstLockedHeard = now;
                }
                const double jitter = (static_cast<double>(random()) / 4294967296.0) * 0.020 - 0.010;
                transports.publish(event, 0, beats[next] + jitter);
                ++next;
            }
            wire->now = now;
            transports.advance(now, state);
        }

        std::size_t start = wire->messages.size();
        for (std::size_t i = 0; i < wire->messages.size(); ++i) {
            if (wire->messages[i].bytes[0] == takt4::output::MidiClock::kStart) {
                start = i;
                break;
            }
        }
        REQUIRE(start < wire->messages.size());
        // Not before the tracker said where the bars are.
        CHECK(wire->messages[start].at >= firstLockedHeard);
        REQUIRE(start >= 1);
        CHECK(wire->messages[start - 1].bytes ==
              std::vector<unsigned char>{takt4::output::MidiClock::kSongPosition, 0x00, 0x00});

        // The receiver's bars: every 96th tick from the first after Start, while there is music.
        std::vector<double> ticks;
        for (std::size_t i = start + 1; i < wire->messages.size(); ++i) {
            if (wire->messages[i].bytes[0] == takt4::output::MidiClock::kTick) {
                ticks.push_back(wire->messages[i].at);
            }
        }
        std::size_t bars = 0;
        for (std::size_t n = 0; n < ticks.size(); n += 96) {
            const double t = ticks[n] - latency;
            if (t > beats.back() + 0.05) {
                break;
            }
            std::size_t nearest = 0;
            for (std::size_t k = 1; k < kBeats; ++k) {
                if (std::abs(beats[k] - t) < std::abs(beats[nearest] - t)) {
                    nearest = k;
                }
            }
            INFO("receiver's bar " << bars + 1 << " at " << t << ", nearest beat " << nearest);
            CHECK(nearest % 4 == 0);
            CHECK(std::abs(beats[nearest] - t) < 0.015);
            ++bars;
        }
        // Started within a couple of bars of the lock, and counted bars to the end.
        CHECK(bars >= (kBeats - kLockedFrom) / 4 - 3);
        transports.stopOutputs();
        CHECK(wire->messages.back().bytes[0] == takt4::output::MidiClock::kStop);
    }
}

TEST_CASE("a beat reaches Link as a tempo and a bar position", "[output][link][network]") {
    // The wiring, held to Link's own counters and to its own timeline. What the calls then
    // *mean* between two peers is link_peers_test.cpp's business; this is about which calls
    // are made, and when — the audit's C3 and H15.
    Transports::Config config;
    config.link = true;
    Transports transports(config);
    transports.startOutputs(0.0);
    // Link's own thread resets the timeline once when first enabled, and writes each commit's
    // result back as it handles it. Real beats are half a second apart and come seconds after
    // Start; these are paced enough for Link to have finished with each before the next.
    const auto paced = [&transports](const BeatEvent& event, std::int64_t at, double beatTime) {
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
        transports.publish(event, at, beatTime);
    };
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    // Beats on a 128 BPM grid on Link's own clock, so the phase Link reports is the phase the
    // beats were put at.
    const std::int64_t origin = transports.link().now().count();
    const auto micros = [origin](double beats, double bpm) {
        return origin + static_cast<std::int64_t>(beats * 60.0e6 / bpm);
    };

    // Offline, there is no host clock to align to, so Link is deliberately left alone.
    paced(beatAt(0.5, 1, 120.0), 0, 0.5);
    CHECK(transports.link().tempoUpdates() == 0);
    CHECK(transports.link().beatRequests() == 0);

    // Hunting, nothing either: not the tempo, which is the hunt's and flips octaves, and not a
    // phase. Every Start used to overwrite Resolume's tempo with it for a few seconds.
    BeatEvent hunting = beatAt(0.6, 1, 64.0);
    hunting.locked = false;
    paced(hunting, micros(0.0, 128.0), 0.6);
    CHECK(transports.link().tempoUpdates() == 0);
    CHECK(transports.link().beatRequests() == 0);

    // The first locked beat snaps: the tempo, and the phase *forced* under the beat. A request
    // would be moved to wherever a peer's phase already matched, which is no phase at all.
    paced(beatAt(1.0, 1, 128.0), micros(0.0, 128.0), 1.0);
    CHECK(transports.link().tempoUpdates() == 1);
    CHECK(transports.link().beatRequests() == 1);
    CHECK_THAT(transports.link().phaseAtTime(std::chrono::microseconds{micros(0.0, 128.0)}, 4.0),
               WithinAbs(0.0, 1e-6));

    // A beat on the grid it set moves nothing: no request, and a tempo that has not moved is
    // not resent — the refined tempo wobbles by hundredths of a BPM, and no peer can act on it.
    paced(beatAt(1.5, 2, 128.001), micros(1.0, 128.0), 1.5);
    CHECK(transports.link().tempoUpdates() == 1);
    CHECK(transports.link().beatRequests() == 1);

    // A tempo that really moved is sent.
    paced(beatAt(2.0, 3, 130.0), micros(2.0, 128.0), 2.0);
    CHECK(transports.link().tempoUpdates() == 2);
    CHECK(transports.link().beatRequests() == 1);

    // A beat 20 ms late against Link's timeline is the session ahead of the music, and it is
    // pulled back with the tempo — a little, never with a jump.
    const std::int64_t beat4 = micros(2.0, 128.0) + static_cast<std::int64_t>(60.0e6 / 130.0) + 20000;
    paced(beatAt(2.5, 4, 130.0), beat4, 2.5);
    CHECK(transports.link().tempoUpdates() == 3);
    CHECK(transports.link().beatRequests() == 1);
    CHECK(transports.link().tempoBpm() < 130.0);
    CHECK(transports.link().tempoBpm() > 130.0 * 0.98);

    // Before the first downbeat the bar phase is unknown, and publishing a guess would put
    // every peer on the wrong beat of the bar. Only the tempo goes.
    BeatEvent unphased = beatAt(3.0, 0, 130.0);
    unphased.beatsPerBar = 0;
    paced(unphased, beat4, 3.0);
    CHECK(transports.link().beatRequests() == 1);

    // Losing the lock and finding it again snaps again: whatever the session did in between,
    // it is put back under the music.
    BeatEvent lost = beatAt(3.2, 1, 130.0);
    lost.locked = false;
    paced(lost, beat4, 3.2);
    const std::uint64_t tempos = transports.link().tempoUpdates();
    CHECK(transports.link().beatRequests() == 1);
    paced(beatAt(3.5, 1, 130.0), beat4, 3.5);
    CHECK(transports.link().beatRequests() == 2);
    CHECK(transports.link().tempoUpdates() == tempos + 1);

    // Switched off, a beat stops reaching it at all.
    transports.setLinkEnabled(false);
    paced(beatAt(4.0, 2, 135.0), beat4, 4.0);
    CHECK(transports.link().beatRequests() == 2);

    // Switched on again, it snaps and resends the tempo even though nothing has changed: the
    // session rejoined knowing nothing, or knowing a peer's timeline.
    transports.setLinkEnabled(true);
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    const std::uint64_t before = transports.link().tempoUpdates();
    paced(beatAt(4.5, 1, 130.0), beat4, 4.5);
    CHECK(transports.link().tempoUpdates() == before + 1);
    CHECK(transports.link().beatRequests() == 3);

    transports.stopOutputs();
    CHECK(transports.beats() == 11);
}

namespace {

/// A MIDI device that notes when each message reached it, and what it was.
struct Wire {
    double now = 0.0;
    std::vector<std::pair<std::string, double>> heard; ///< "desk on", "laser off", ...
};

class RecordingPort final : public takt4::output::MidiPort {
public:
    RecordingPort(std::string name, std::shared_ptr<Wire> wire)
        : name_(std::move(name)), wire_(std::move(wire)) {}
    std::string open(std::string_view spec) override { return std::string(spec); }
    void close() noexcept override {}
    void send(std::span<const unsigned char> message) override {
        const bool on = !message.empty() && (message[0] & 0xF0) == 0x90;
        wire_->heard.emplace_back(name_ + (on ? " on" : " off"), wire_->now);
    }

private:
    std::string name_;
    std::shared_ptr<Wire> wire_;
};

} // namespace

TEST_CASE("a press and its release keep their gap on every output however each is offset",
          "[output][midi][trigger]") {
    // The audit's H4, at the rule's end, and M9's MIDI half. Two MIDI devices: a desk with no
    // lag and a laser controller set 150 ms early. A note rule fires on a beat in the music at
    // 10.5 — fired at 10.35 on a prediction, the earliest either output needs it — with its
    // note off 100 ms later. Each device must hear the note at the beat's moment plus its own
    // offset, and the note off exactly 100 ms after that: measuring the release from the round
    // that sends it would put the desk's note off *before* the desk's note on.
    auto wire = std::make_shared<Wire>();
    Transports::Config config;
    for (const auto& [name, delay] : {std::pair<std::string, double>{"desk", 0.0},
                                      std::pair<std::string, double>{"laser", -0.150}}) {
        takt4::output::OutputTarget target;
        target.name = name;
        target.kind = takt4::output::OutputTarget::Kind::Midi;
        target.device = name;
        target.delaySeconds = delay;
        config.outputs.push_back(target);
    }
    config.openMidi = [wire](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(
            name, std::make_unique<RecordingPort>(name, wire));
    };
    Transports transports(config);
    CHECK_THAT(transports.leadSeconds(), WithinAbs(-0.150, 1e-12));
    CHECK_THAT(transports.tailSeconds(), WithinAbs(0.0, 1e-12));

    takt4::output::RuleSink sink(transports);
    takt4::trigger::TriggerEngine triggers(sink);
    takt4::trigger::Rule::Config rule;
    rule.id = "laser-hit";
    rule.trigger = takt4::trigger::Trigger::Beat;
    rule.sendKind = takt4::trigger::Message::Kind::MidiNote;
    takt4::trigger::FollowUp release;
    release.unit = takt4::trigger::DelayUnit::Milliseconds;
    release.delaySeconds = 0.100;
    rule.followUps.push_back(release);
    triggers.setRules({rule});
    REQUIRE(triggers.rule(0).valid());

    takt4::trigger::Context context;
    context.bpm = 128.0;
    context.locked = true;
    context.meter = 4;
    context.beatInBar = 1;
    context.beats = 1;
    context.bars = 1;
    context.now = 10.35;
    context.moment = 10.5;
    wire->now = context.now;
    sink.setNow(context.now);
    triggers.onBeat(context);

    takt4::trigger::Context round = context;
    round.moment.reset();
    for (int step = 0; step <= 700; ++step) {
        const double now = 10.35 + 0.001 * step;
        wire->now = now;
        sink.setNow(now);
        round.now = now;
        triggers.advance(round);
        sink.releaseDue(now);
    }

    const auto when = [&wire](const std::string& what) {
        for (const auto& [heard, at] : wire->heard) {
            if (heard == what) {
                return at;
            }
        }
        return -1.0;
    };
    REQUIRE(wire->heard.size() == 4);
    CHECK_THAT(when("laser on"), WithinAbs(10.35, 0.0011));
    CHECK_THAT(when("laser off"), WithinAbs(10.45, 0.0011));
    CHECK_THAT(when("desk on"), WithinAbs(10.50, 0.0011));
    CHECK_THAT(when("desk off"), WithinAbs(10.60, 0.0011));
    CHECK(sink.queued() == 0);

    SECTION("a PANIC sends what is held at once rather than leaving it to go later") {
        context.now = 20.0;
        context.moment = 20.4;
        context.beats = 2;
        wire->heard.clear();
        wire->now = 20.0;
        sink.setNow(20.0);
        triggers.onBeat(context);
        REQUIRE(sink.queued() > 0);
        sink.flushQueued();
        CHECK(sink.queued() == 0);
        CHECK(wire->heard.size() >= 2);
    }
}
