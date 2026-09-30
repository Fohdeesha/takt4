#include "core/output/midi_ports.hpp"
#include "core/output/rule_sink.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "core/trigger/trigger_engine.hpp"

#include "support/artnet_nodes.hpp"
#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
using takt4::testing::LoopbackReceiver;
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
    const LoopbackReceiver server; // the test's own, not a port nobody holds
    Transports::Config config;
    config.outputs = takt4::output::oscOutputs({{"127.0.0.1", server.port()}});
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

        // And the held ones go at the 1.8 they were given — not at 2.0, where the new offset
        // would have put them. The check above was before both, so it held either way (the
        // audit of 2026-09-25, T13).
        transports.advance(1.85, steady);
        CHECK(transports.osc().pending() == 0);
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
    // Receivers of the test's own, not ports nobody holds.
    const LoopbackReceiver first;
    const LoopbackReceiver second;
    const LoopbackReceiver third;

    transports.setOscTargets({{"127.0.0.1", first.port()}, {"127.0.0.1", second.port()}});
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
    transports.setOscTargets({{"127.0.0.1", third.port()}});
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
    /// Plugged in and held by another program, which is what a WinMM port another application
    /// has open is: listed, and refused.
    bool held = false;
    int opens = 0;
    std::uint64_t delivered = 0;
};

/// **Behaves as RtMidi's port does, closed as well as open** (the audit of 2026-09-25, C1).
/// The first version of this threw on every send while the cable was out, closed or not — and
/// RtMidi's WinMM port does not: `MidiOutWinMM::sendMessage` begins `if (!connected_) return;`.
/// So after one failed reopen every send "worked", the device read as back, and it was never
/// looked for again, while this test passed against a port that could not do that.
class UnpluggablePort final : public takt4::output::MidiPort {
public:
    explicit UnpluggablePort(std::shared_ptr<Cable> cable) : cable_(std::move(cable)) {}
    std::string open(std::string_view spec) override {
        ++cable_->opens;
        // What RtMidi's port throws for each, since the audit's C1 said to make the fakes honest.
        if (!cable_->plugged) {
            throw takt4::output::MidiPortMissing("MIDI output: no port matching \"" +
                                                 std::string(spec) + "\"");
        }
        if (cable_->held) {
            throw takt4::output::MidiPortBusy(
                "MIDI output: ", "Desk " + std::string(spec),
                "MidiOutWinMM::openPort: error creating Windows MM MIDI output port.");
        }
        open_ = true;
        return "Desk " + std::string(spec);
    }
    void close() noexcept override { open_ = false; }
    void send(std::span<const unsigned char>) override {
        if (!open_) {
            return; // RtMidi's WinMM port, closed: nothing sent, nothing said
        }
        if (!cable_->plugged) {
            throw std::runtime_error("the device has gone"); // a dead handle's driver error
        }
        ++cable_->delivered;
    }

private:
    std::shared_ptr<Cable> cable_;
    bool open_ = false;
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

TEST_CASE("a MIDI device another program holds is said to be held, not missing",
          "[output][midi]") {
    // 2026-09-28: "if it can't bind ... it should say so in the UI so it doesn't just silently
    // fail". A WinMM port is one program's at a time, so a DAW with every output open is the
    // usual reason a MIDI row reaches nothing — and the row said "no MIDI device called ... plug
    // it in" of a device the operator could see plugged in. Each reason is now its own, by row,
    // with the output's id so a window can put it on the row it is about.
    using takt4::output::OutputTarget;
    auto held = std::make_shared<Cable>();
    held->held = true;
    auto missing = std::make_shared<Cable>();
    missing->plugged = false;
    const auto output = [](std::string id, std::string name, OutputTarget::Kind kind,
                           std::string device) {
        OutputTarget target;
        target.id = std::move(id);
        target.name = std::move(name);
        target.kind = kind;
        target.device = std::move(device);
        return target;
    };
    Transports::Config config;
    config.openMidi = [held, missing](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(
            name, std::make_unique<UnpluggablePort>(name == "Held" ? held : missing));
    };
    Transports transports(config);
    CHECK_THROWS(transports.setOutputs(
        {output("o-00000001", "desk", OutputTarget::Kind::Midi, "Held"),
         output("o-00000002", "drums", OutputTarget::Kind::MidiClock, "Gone"),
         output("o-00000003", "unpicked", OutputTarget::Kind::Midi, "")}));

    const std::vector<Transports::Problem> problems = transports.problems();
    REQUIRE(problems.size() == 3);
    CHECK(problems[0].id == "o-00000001");
    INFO(problems[0].why);
    CHECK(problems[0].why.find("is on this machine but would not open") != std::string::npos);
    CHECK(problems[0].why.find("another program") != std::string::npos);
    CHECK(problems[0].why.find("no MIDI device called") == std::string::npos);
    CHECK(problems[0].text == "desk: " + problems[0].why);

    CHECK(problems[1].id == "o-00000002");
    CHECK(problems[1].why == "no MIDI device called \"Gone\" \xE2\x80\x94 plug it in and press RESCAN");

    CHECK(problems[2].id == "o-00000003");
    CHECK(problems[2].why == "no MIDI device chosen");

    // Let go of by the other program, the row stops saying so.
    held->held = false;
    CHECK_NOTHROW(
        transports.setOutputs({output("o-00000001", "desk", OutputTarget::Kind::Midi, "Held")}));
    CHECK(transports.problems().empty());
}

namespace {

/// The machine's list of ports, as RtMidi's Windows backend names them — each with its place in
/// the list on the end — which a device leaves when it is unplugged and rejoins at the end.
struct PortList {
    std::vector<std::string> names;
    std::string opened;
    std::uint64_t delivered = 0;
};

class ListedPort final : public takt4::output::MidiPort {
public:
    explicit ListedPort(std::shared_ptr<PortList> list) : list_(std::move(list)) {}
    std::string open(std::string_view spec) override {
        const std::optional<std::size_t> found = takt4::output::findMidiPort(list_->names, spec);
        if (!found) {
            throw std::runtime_error("MIDI output: no port matching \"" + std::string(spec) + "\"");
        }
        name_ = list_->names[*found];
        list_->opened = name_;
        return name_;
    }
    void close() noexcept override { name_.clear(); }
    void send(std::span<const unsigned char>) override {
        if (name_.empty()) {
            return; // closed: RtMidi's WinMM port says nothing
        }
        if (std::find(list_->names.begin(), list_->names.end(), name_) == list_->names.end()) {
            throw std::runtime_error("the device has gone");
        }
        ++list_->delivered;
    }

private:
    std::shared_ptr<PortList> list_;
    std::string name_;
};

} // namespace

TEST_CASE("a MIDI device that comes back at another number is found again", "[output][midi]") {
    // C1's other half. The port was saved as "USB MIDI 2"; unplugged and plugged back in beside
    // another device, it is "USB MIDI 3" — and a reconnect that looked for "USB MIDI 2" by what
    // it contained looked for ever.
    auto list = std::make_shared<PortList>();
    list->names = {"Microsoft GS Wavetable Synth 0", "loopMIDI Port 1", "USB MIDI 2"};
    Transports::Config config;
    config.midiClockPort = "USB MIDI 2";
    config.openMidi = [list](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(name, std::make_unique<ListedPort>(list));
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
    runFor(0.5);
    REQUIRE(list->delivered > 0);

    list->names = {"Microsoft GS Wavetable Synth 0", "loopMIDI Port 1"};
    runFor(2.5);
    REQUIRE(transports.lostMidiCount() == 1);

    list->names = {"Microsoft GS Wavetable Synth 0", "loopMIDI Port 1", "Launchpad 2", "USB MIDI 3"};
    const std::uint64_t before = list->delivered;
    runFor(1.5);
    CHECK(transports.lostMidiCount() == 0);
    CHECK(list->opened == "USB MIDI 3");
    CHECK(list->delivered > before);
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
        // What the output thread's PANIC does, in its order (`OutputRunner::apply`): the rules'
        // owed releases first, then everything held for an output's offset. This called only
        // the second, so the two note offs — owed by the rules, not held by the sink — were
        // never part of the test (the audit of 2026-09-25, T13).
        context.now = 20.0;
        context.moment = 20.4;
        context.beats = 2;
        wire->heard.clear();
        wire->now = 20.0;
        sink.setNow(20.0);
        triggers.onBeat(context);
        REQUIRE(sink.queued() > 0);
        triggers.panic(context);
        sink.flushQueued();
        CHECK(sink.queued() == 0);
        // Both presses and both releases, now — and each release after its own press, or a
        // note would be left sounding.
        REQUIRE(wire->heard.size() == 4);
        for (const auto& [heard, at] : wire->heard) {
            INFO(heard);
            CHECK(at == 20.0);
        }
        const auto order = [&wire](const std::string& what) {
            for (std::size_t i = 0; i < wire->heard.size(); ++i) {
                if (wire->heard[i].first == what) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        CHECK(order("laser on") >= 0);
        CHECK(order("desk on") >= 0);
        CHECK(order("laser on") < order("laser off"));
        CHECK(order("desk on") < order("desk off"));
    }
}

namespace {

/// A `TimedWire` per MIDI device, every one on the clock the test drives.
struct Wires {
    std::map<std::string, std::shared_ptr<TimedWire>> byDevice;

    void setNow(double now) {
        for (auto& entry : byDevice) {
            entry.second->now = now;
        }
    }
    std::function<std::unique_ptr<takt4::output::MidiOutput>(const std::string&)> opener() {
        return [this](const std::string& name) {
            std::shared_ptr<TimedWire>& wire = byDevice[name];
            if (wire == nullptr) {
                wire = std::make_shared<TimedWire>();
            }
            return std::make_unique<takt4::output::MidiOutput>(name,
                                                               std::make_unique<TimedPort>(wire));
        };
    }
    /// How many messages starting with `status` went down `device`.
    std::size_t count(const std::string& device, unsigned char status) const {
        const auto found = byDevice.find(device);
        if (found == byDevice.end()) {
            return 0;
        }
        std::size_t n = 0;
        for (const TimedWire::Message& message : found->second->messages) {
            if (!message.bytes.empty() && message.bytes[0] == status) {
                ++n;
            }
        }
        return n;
    }
};

takt4::output::OutputTarget clockTo(std::string name, std::string device, double delay) {
    takt4::output::OutputTarget clock;
    clock.id = "o-c10c" + std::to_string(name.size()) + std::to_string(device.size());
    clock.name = std::move(name);
    clock.kind = takt4::output::OutputTarget::Kind::MidiClock;
    clock.device = std::move(device);
    clock.delaySeconds = delay;
    return clock;
}

/// Music at 128 BPM, locked from its first beat, each beat reaching the transports a pipeline
/// after it was in the audio — played a millisecond at a time, so a test can change something
/// between two stretches of it.
struct Music {
    static constexpr double kBpm = 128.0;
    static constexpr double kPipeline = 0.060;
    std::vector<double> beats;
    std::size_t next = 0;
    double now = 0.0;

    explicit Music(std::size_t count) {
        for (std::size_t k = 0; k < count; ++k) {
            beats.push_back(0.137 + static_cast<double>(k) * 60.0 / kBpm);
        }
    }
    /// Plays until beat `until` has been heard, and a little past it.
    void play(Transports& transports, Wires& wires, std::size_t until) {
        const TempoState state;
        const double end = beats[std::min(until, beats.size() - 1)] + kPipeline + 0.05;
        while (now < end) {
            now += 0.001;
            wires.setNow(now);
            while (next < beats.size() && beats[next] + kPipeline <= now) {
                transports.publish(
                    beatAt(beats[next], static_cast<std::uint32_t>(next % 4) + 1, kBpm), 0,
                    beats[next]);
                ++next;
            }
            transports.advance(now, state);
        }
    }
};

/// How a receiver's beats — every 24th tick from the first after Start — sit against the music
/// played `delay` later, over the music's beats `from` to `to`.
struct Fit {
    double worst = 0.0;    ///< the furthest any was from the beat it is nearest
    std::size_t beats = 0; ///< how many were in range
    bool downbeats = true; ///< whether every 4th — the receiver's bar 1s — was on a downbeat
};

Fit fit(const TimedWire& wire, const Music& music, double delay, std::size_t from, std::size_t to) {
    std::size_t start = wire.messages.size();
    for (std::size_t i = 0; i < wire.messages.size(); ++i) {
        if (wire.messages[i].bytes[0] == takt4::output::MidiClock::kStart) {
            start = i;
            break;
        }
    }
    REQUIRE(start < wire.messages.size());
    std::vector<double> ticks;
    for (std::size_t i = start + 1; i < wire.messages.size(); ++i) {
        if (wire.messages[i].bytes[0] == takt4::output::MidiClock::kTick) {
            ticks.push_back(wire.messages[i].at);
        }
    }
    Fit result;
    for (std::size_t n = 0; n < ticks.size(); n += 24) {
        const double t = ticks[n] - delay;
        std::size_t nearest = 0;
        for (std::size_t k = 1; k < music.beats.size(); ++k) {
            if (std::abs(music.beats[k] - t) < std::abs(music.beats[nearest] - t)) {
                nearest = k;
            }
        }
        if (nearest < from || nearest > to) {
            continue;
        }
        result.worst = std::max(result.worst, std::abs(music.beats[nearest] - t));
        ++result.beats;
        if ((n / 24) % 4 == 0 && nearest % 4 != 0) {
            result.downbeats = false;
        }
    }
    return result;
}

} // namespace

TEST_CASE("every MIDI clock output ticks on the beat plus its own delay", "[output][midi]") {
    // The operator's call of 2026-09-25: a MIDI clock is an output like any other, as many of
    // them as there are things to clock, each with a delay that works as every output's does —
    // a DAW that plays 80 ms late wants its clock 80 ms later, a drum machine that is quick
    // wants it 60 ms early, and one number for the rig could not give both.
    Wires wires;
    Transports::Config config;
    config.outputs = {clockTo("DAW", "daw", 0.080), clockTo("drums", "drums", -0.060)};
    config.openMidi = wires.opener();
    Transports transports(config);
    CHECK(transports.clockCount() == 2);
    CHECK(transports.outputProblems().empty());
    // Neither asks for a beat early: a clock is a grid that runs on from a beat already heard.
    CHECK(transports.leadSeconds() == 0.0);

    transports.startOutputs(0.0);
    transports.startClock(0.0);
    Music music(64);
    music.play(transports, wires, 63);

    for (const auto& [device, delay] : {std::pair<std::string, double>{"daw", 0.080},
                                        std::pair<std::string, double>{"drums", -0.060}}) {
        INFO(device);
        REQUIRE(wires.byDevice.count(device) == 1);
        const TimedWire& wire = *wires.byDevice.at(device);
        // Once the grid has settled, every beat of it is the music's beat plus this delay —
        // within the millisecond a round is — and the receiver's bars are the music's.
        const Fit settled = fit(wire, music, delay, 16, 60);
        CHECK(settled.beats >= 40);
        CHECK(settled.worst < 0.004);
        CHECK(settled.downbeats);
        // And a whole delay away from where a clock without one would have put it.
        CHECK(fit(wire, music, 0.0, 16, 60).worst > 0.05);
        CHECK(wires.count(device, takt4::output::MidiClock::kStart) == 1);
    }
    transports.stopOutputs();
    CHECK(wires.count("daw", takt4::output::MidiClock::kStop) == 1);
    CHECK(wires.count("drums", takt4::output::MidiClock::kStop) == 1);
}

TEST_CASE("a second MIDI clock on a device already clocked is refused, and says so",
          "[output][midi]") {
    // Two clocks down one cable would tick it twice as fast, and a receiver would play at double
    // the tempo — so the second one is not built, and the row says which output already has it.
    Wires wires;
    Transports::Config config;
    config.outputs = {clockTo("DAW", "daw", 0.0), clockTo("again", "daw", 0.020)};
    config.openMidi = wires.opener();
    Transports transports(config);
    CHECK(transports.clockCount() == 1);
    const std::vector<std::string> problems = transports.outputProblems();
    REQUIRE(problems.size() == 1);
    CHECK(problems.front() == "again: \"DAW\" already sends the clock to that device");

    // One clock's worth of ticks: 48 a second at the clock's opening 120 BPM, not 96.
    transports.startOutputs(0.0);
    transports.startClock(0.0);
    const TempoState state;
    for (int ms = 1; ms <= 1000; ++ms) {
        wires.setNow(ms / 1000.0);
        transports.advance(ms / 1000.0, state);
    }
    const std::size_t ticks = wires.count("daw", takt4::output::MidiClock::kTick);
    CHECK(ticks >= 47);
    CHECK(ticks <= 49);
    transports.stopOutputs();
}

TEST_CASE("editing the outputs keeps every clock running, and one taken away is stopped",
          "[output][midi]") {
    // A receiver that is sent Stop and Start stops playing and starts again from bar 1. Renaming
    // an output, moving its delay, or adding a row beside it is not a reason for that.
    Wires wires;
    Transports::Config config;
    config.outputs = {clockTo("DAW", "daw", 0.0), clockTo("drums", "drums", 0.0)};
    config.openMidi = wires.opener();
    Transports transports(config);
    transports.startOutputs(0.0);
    transports.startClock(0.0);
    Music music(48);
    music.play(transports, wires, 16);
    REQUIRE(wires.count("daw", takt4::output::MidiClock::kStart) == 1);
    REQUIRE(wires.count("drums", takt4::output::MidiClock::kStart) == 1);

    // Renamed, moved later, and a note output on the same cable added beside it.
    std::vector<takt4::output::OutputTarget> edited = config.outputs;
    edited[0].name = "Ableton";
    edited[0].delaySeconds = 0.040;
    takt4::output::OutputTarget desk;
    desk.id = "o-de5c";
    desk.name = "desk";
    desk.kind = takt4::output::OutputTarget::Kind::Midi;
    desk.device = "daw";
    edited.push_back(desk);
    transports.setOutputs(edited);
    CHECK(transports.clockCount() == 2);
    CHECK(transports.midiTarget(2) != nullptr); // the note output shares the clock's device
    music.play(transports, wires, 32);
    for (const char* device : {"daw", "drums"}) {
        INFO(device);
        CHECK(wires.count(device, takt4::output::MidiClock::kStop) == 0);
        CHECK(wires.count(device, takt4::output::MidiClock::kStart) == 1);
    }

    // The drums' row taken away: its receiver is told to stop, and the DAW carries on.
    edited.erase(edited.begin() + 1);
    transports.setOutputs(edited);
    CHECK(transports.clockCount() == 1);
    CHECK(wires.count("drums", takt4::output::MidiClock::kStop) == 1);
    const std::size_t dawTicks = wires.count("daw", takt4::output::MidiClock::kTick);
    music.play(transports, wires, 40);
    CHECK(wires.count("daw", takt4::output::MidiClock::kTick) > dawTicks);
    CHECK(wires.count("daw", takt4::output::MidiClock::kStop) == 0);
    transports.stopOutputs();
}

TEST_CASE("moving a clock's delay moves its grid there without a Stop", "[output][midi]") {
    // What a dragged delay slider sends: `setOutputDelay`, which moves the one number and touches
    // nothing else. The grid is steered to the new place, as it is to every beat.
    Wires wires;
    Transports::Config config;
    config.outputs = {clockTo("DAW", "daw", 0.0)};
    config.openMidi = wires.opener();
    Transports transports(config);
    transports.startOutputs(0.0);
    transports.startClock(0.0);
    Music music(64);
    music.play(transports, wires, 24);
    CHECK(fit(*wires.byDevice.at("daw"), music, 0.0, 12, 22).worst < 0.004);

    CHECK(transports.setOutputDelay(config.outputs[0].id, 0.090));
    CHECK_FALSE(transports.setOutputDelay("o-ffffffff", 0.090));
    CHECK_THAT(transports.outputs()[0].delaySeconds, WithinAbs(0.090, 1e-12));
    music.play(transports, wires, 63);
    const Fit moved = fit(*wires.byDevice.at("daw"), music, 0.090, 44, 60);
    CHECK(moved.beats >= 12);
    CHECK(moved.worst < 0.004);
    CHECK(moved.downbeats);
    CHECK(wires.count("daw", takt4::output::MidiClock::kStop) == 0);
    CHECK(wires.count("daw", takt4::output::MidiClock::kStart) == 1);
    transports.stopOutputs();
}

TEST_CASE("a caller with one clock and a Link switch in mind still has them", "[output][midi]") {
    // `takt4-cli --midi-clock` and `--link`, and every older test: a port and a switch rather
    // than a list of outputs. They become outputs like the window's.
    Wires wires;
    Transports::Config config;
    config.midiClockPort = "Clock";
    config.link = true;
    config.openMidi = wires.opener();
    Transports transports(config);
    REQUIRE(transports.outputs().size() == 2);
    CHECK(transports.outputs()[0].kind == takt4::output::OutputTarget::Kind::Link);
    CHECK(transports.outputs()[0].enabled);
    CHECK(transports.linkEnabled());
    CHECK_FALSE(transports.link().enabled()); // not joined before the outputs start
    CHECK(transports.outputs()[1].kind == takt4::output::OutputTarget::Kind::MidiClock);
    CHECK(transports.midiClockPort() == std::optional<std::string>("Clock"));
    CHECK(transports.clockCount() == 1);
    CHECK(transports.midiClock() != nullptr);

    // Another port in place of it — one clock still, on the new device.
    transports.setMidiClockPort(std::string("Other"));
    CHECK(transports.clockCount() == 1);
    CHECK(transports.midiClockPort() == std::optional<std::string>("Other"));
    CHECK(transports.outputs().size() == 2);

    // None, and the Link output is left alone.
    transports.setMidiClockPort(std::nullopt);
    CHECK(transports.clockCount() == 0);
    CHECK_FALSE(transports.midiClockPort().has_value());
    REQUIRE(transports.outputs().size() == 1);
    CHECK(transports.outputs()[0].kind == takt4::output::OutputTarget::Kind::Link);

    // Link's own switch is the Link output's.
    transports.setLinkEnabled(false);
    CHECK_FALSE(transports.outputs()[0].enabled);
    CHECK_FALSE(transports.linkEnabled());
}

TEST_CASE("outputs with no Link output among them leave Link as it was", "[output]") {
    // A caller that never had a Link output switches Link with `setLinkEnabled`, and then
    // replaces its OSC targets — which used to switch Link straight back off, because the new
    // list had no Link output switched on.
    Transports transports{Transports::Config{}};
    transports.setLinkEnabled(true);
    transports.setOscTargets({{"127.0.0.1", 57000}});
    CHECK(transports.linkEnabled());
    CHECK(transports.outputs().size() == 1);

    // A list that has one is what Link follows, either way.
    std::vector<takt4::output::OutputTarget> outputs = transports.outputs();
    REQUIRE(takt4::output::ensureLinkOutput(outputs, false));
    outputs.front().delaySeconds = 0.015;
    transports.setOutputs(outputs);
    CHECK_FALSE(transports.linkEnabled());
    CHECK_THAT(transports.linkDelaySeconds(), WithinAbs(0.015, 1e-12));
    outputs.front().enabled = true;
    transports.setOutputs(outputs);
    CHECK(transports.linkEnabled());
    // And taken out of the list again, Link stays where the last list put it.
    transports.setOscTargets({});
    CHECK(transports.linkEnabled());
    CHECK_THAT(transports.linkDelaySeconds(), WithinAbs(0.015, 1e-12));
}

TEST_CASE("the Link output's delay moves the timeline under the beat", "[output][link][network]") {
    // Link's delay works as every output's does: the session's bar starts the delay after the
    // beat's own moment. Driven like "a beat reaches Link as a tempo and a bar position".
    std::vector<takt4::output::OutputTarget> outputs;
    REQUIRE(takt4::output::ensureLinkOutput(outputs, true));
    outputs.front().delaySeconds = 0.030;
    Transports::Config config;
    config.outputs = outputs;
    Transports transports(config);
    CHECK(transports.linkEnabled());
    CHECK_THAT(transports.linkDelaySeconds(), WithinAbs(0.030, 1e-12));
    transports.startOutputs(0.0);
    const auto paced = [&transports](const BeatEvent& event, std::int64_t at, double beatTime) {
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
        transports.publish(event, at, beatTime);
    };
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    const std::int64_t origin = transports.link().now().count();
    const auto micros = [origin](double beats) {
        return origin + static_cast<std::int64_t>(beats * 60.0e6 / 128.0);
    };
    // How far from the start of a bar Link's session is at `at`, either way round.
    const auto offBar = [&transports](std::int64_t at) {
        const double phase = transports.link().phaseAtTime(std::chrono::microseconds{at}, 4.0);
        return std::min(phase, 4.0 - phase);
    };

    paced(beatAt(1.0, 1, 128.0), micros(0.0), 1.0);
    CHECK(offBar(micros(0.0) + 30000) < 1e-4);
    // Not at the beat's own moment: 30 ms is 0.064 of a beat at 128 BPM.
    CHECK(offBar(micros(0.0)) > 0.05);

    // Moved earlier, and a DOWNBEAT to put the bar there: Link's bar now starts before the beat.
    const std::string id = transports.outputs().front().id;
    REQUIRE(transports.setOutputDelay(id, -0.020));
    CHECK_THAT(transports.linkDelaySeconds(), WithinAbs(-0.020, 1e-12));
    BeatEvent downbeat = beatAt(3.0, 1, 128.0);
    downbeat.snapped = true;
    paced(downbeat, micros(4.0), 3.0);
    CHECK(offBar(micros(4.0) - 20000) < 1e-4);
    transports.stopOutputs();
}

TEST_CASE("each Art-Net node has a beat's lighting on the beat plus its own delay",
          "[output][dmx][trigger]") {
    // The operator's call of 2026-09-25, end to end. Three nodes — one set 100 ms early, one on
    // time, one 50 ms late — and a colour rule fired on a beat in the music at 1.0, fired at 0.8
    // on a prediction as the output thread does while the tracker is locked. The lighting
    // starts 100 ms early, for the early node, and each other node is sent it its own delay
    // after that: so each node has it at the beat plus its own delay.
    using takt4::testing::kArtNetFrame;
    takt4::testing::ArtNetNodes nodes(3);
    Transports::Config config;
    takt4::dmx::Fixture par = takt4::dmx::fixtureFromMode("par", 1, 0, 1);
    par.id = "par";
    config.patch = {par};
    const double delays[] = {-0.100, 0.0, 0.050};
    for (std::size_t i = 0; i < 3; ++i) {
        takt4::output::OutputTarget node;
        node.id = "o-a" + std::to_string(i);
        node.name = "node " + std::to_string(i);
        node.kind = takt4::output::OutputTarget::Kind::ArtNet;
        node.host = "127.0.0.1";
        node.port = nodes.port(i);
        node.delaySeconds = delays[i];
        config.outputs.push_back(node);
    }
    Transports transports(config);
    CHECK_THAT(transports.leadSeconds(), WithinAbs(-0.100, 1e-12));
    CHECK_THAT(transports.lightingLeadSeconds(), WithinAbs(0.100, 1e-12));

    takt4::output::RuleSink sink(transports);
    takt4::trigger::TriggerEngine triggers(sink);
    takt4::trigger::Rule::Config rule;
    rule.id = "red";
    rule.trigger = takt4::trigger::Trigger::Beat;
    rule.sendKind = takt4::trigger::Message::Kind::Dmx;
    rule.dmx.fixtures = {"par"};
    rule.dmx.effect = takt4::dmx::EffectKind::Color;
    rule.dmx.color.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.dmx.color.fixed = takt4::trigger::Value::ofText("#ff0000");
    rule.dmx.level.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.dmx.level.fixed = takt4::trigger::Value::ofInt(255);
    rule.dmx.durationBeats = 0.0;
    triggers.setRules({rule});
    REQUIRE(triggers.rule(0).valid());
    triggers.rule(0).setFixtureMask(0b1);

    const TempoState state;
    const auto round = [&](double now) {
        sink.releaseDue(now);
        const std::uint64_t before = transports.artnet().sent();
        transports.advance(now, state);
        return static_cast<std::size_t>(transports.artnet().sent() - before);
    };
    nodes.run(round, 0, 800);
    takt4::trigger::Context context;
    context.bpm = 128.0;
    context.locked = true;
    context.meter = 4;
    context.beatInBar = 1;
    context.beats = 1;
    context.bars = 1;
    context.now = 0.8;
    context.moment = 1.0;
    sink.setNow(0.8);
    triggers.onBeat(context);
    nodes.run(round, 800, 1300);

    const double expected[] = {0.900, 1.000, 1.050};
    for (std::size_t node = 0; node < 3; ++node) {
        INFO("node " << node);
        CHECK(nodes.first(node, 255) >= expected[node] - 1e-9);
        CHECK(nodes.first(node, 255) < expected[node] + kArtNetFrame);
    }
}

TEST_CASE("each output's delay reaches that output by its id, with the Link row first",
          "[output][osc][dmx][trigger]") {
    // The audit of 2026-09-25, coverage gap 8. Every rig since the window was rebuilt around one
    // list of outputs has Link as its first row, so an output's place in the list is never its
    // place among the OSC targets or among the nodes — and every delay test before this one had
    // no Link row, where the two happen to agree. Here the delays are moved afterwards, by id, as
    // a dragged slider moves them, on a list laid out the way the window lays it out.
    using takt4::output::OutputTarget;
    using takt4::testing::kArtNetFrame;
    LoopbackReceiver prompt;
    LoopbackReceiver slow;
    takt4::testing::ArtNetNodes nodes(2);
    Transports::Config config;
    takt4::dmx::Fixture par = takt4::dmx::fixtureFromMode("par", 1, 0, 1);
    par.id = "par";
    config.patch = {par};
    OutputTarget link;
    link.id = "o-link";
    link.name = "Link";
    link.kind = OutputTarget::Kind::Link;
    config.outputs.push_back(link);
    for (const auto& [id, port] : {std::pair<std::string, std::uint16_t>{"o-prompt", prompt.port()},
                                   std::pair<std::string, std::uint16_t>{"o-slow", slow.port()}}) {
        OutputTarget osc;
        osc.id = id;
        osc.name = id.substr(2);
        osc.kind = OutputTarget::Kind::Osc;
        osc.host = "127.0.0.1";
        osc.port = port;
        config.outputs.push_back(osc);
    }
    for (std::size_t i = 0; i < 2; ++i) {
        OutputTarget node;
        node.id = "o-node" + std::to_string(i);
        node.name = "node " + std::to_string(i);
        node.kind = OutputTarget::Kind::ArtNet;
        node.host = "127.0.0.1";
        node.port = nodes.port(i);
        config.outputs.push_back(node);
    }
    Transports transports(config);
    REQUIRE(transports.outputs().size() == 5);
    REQUIRE(transports.outputs().front().kind == OutputTarget::Kind::Link);

    // The second OSC output 200 ms late, the second node 100 ms early — by id.
    REQUIRE(transports.setOutputDelay("o-slow", 0.200));
    REQUIRE(transports.setOutputDelay("o-node1", -0.100));
    CHECK_FALSE(transports.setOutputDelay("o-nobody", 0.1));
    CHECK_THAT(transports.lightingLeadSeconds(), WithinAbs(0.100, 1e-12));

    SECTION("the late OSC output is held, and the one beside it is not") {
        transports.osc().setNow(10.0);
        transports.osc().sendAddress("/cue");
        CHECK_FALSE(prompt.receive().empty());
        CHECK(slow.receive().empty());
        transports.osc().setNow(10.199);
        transports.osc().flushDue();
        CHECK(slow.receive().empty());
        transports.osc().setNow(10.2);
        transports.osc().flushDue();
        CHECK_FALSE(slow.receive().empty());
    }

    SECTION("a beat's lighting reaches the early node ahead of the beat, and the other on it") {
        takt4::output::RuleSink sink(transports);
        takt4::trigger::TriggerEngine triggers(sink);
        takt4::trigger::Rule::Config rule;
        rule.id = "red";
        rule.trigger = takt4::trigger::Trigger::Beat;
        rule.sendKind = takt4::trigger::Message::Kind::Dmx;
        rule.dmx.fixtures = {"par"};
        rule.dmx.effect = takt4::dmx::EffectKind::Color;
        rule.dmx.color.kind = takt4::trigger::GeneratorKind::Fixed;
        rule.dmx.color.fixed = takt4::trigger::Value::ofText("#ff0000");
        rule.dmx.level.kind = takt4::trigger::GeneratorKind::Fixed;
        rule.dmx.level.fixed = takt4::trigger::Value::ofInt(255);
        rule.dmx.durationBeats = 0.0;
        triggers.setRules({rule});
        REQUIRE(triggers.rule(0).valid());
        triggers.rule(0).setFixtureMask(0b1);

        const TempoState state;
        const auto round = [&](double now) {
            sink.releaseDue(now);
            const std::uint64_t before = transports.artnet().sent();
            transports.advance(now, state);
            return static_cast<std::size_t>(transports.artnet().sent() - before);
        };
        nodes.run(round, 0, 800);
        takt4::trigger::Context context;
        context.bpm = 128.0;
        context.locked = true;
        context.meter = 4;
        context.beatInBar = 1;
        context.beats = 1;
        context.bars = 1;
        context.now = 0.8;
        context.moment = 1.0;
        sink.setNow(0.8);
        triggers.onBeat(context);
        nodes.run(round, 800, 1300);
        // Node 0 is on time; node 1, set 100 ms early, has it 100 ms before the beat.
        CHECK(nodes.first(0, 255) >= 1.000 - 1e-9);
        CHECK(nodes.first(0, 255) < 1.000 + kArtNetFrame);
        CHECK(nodes.first(1, 255) >= 0.900 - 1e-9);
        CHECK(nodes.first(1, 255) < 0.900 + kArtNetFrame);
    }
}

TEST_CASE("MIDI Start waits for a beat whose place in the bar is known", "[output][midi]") {
    // The audit of 2026-09-25, M6. The tracker publishes `beatInBar` 0 until it has found a bar,
    // and the clock read that as beat 1: a locked beat with no bar started a receiver's bar there,
    // on whatever beat of the music it was, and the receiver kept that bar for the run.
    auto wire = std::make_shared<TimedWire>();
    Transports::Config config;
    config.midiClockPort = "Clock";
    config.openMidi = [wire](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(name, std::make_unique<TimedPort>(wire));
    };
    Transports transports(config);
    // Outputs set later than the audio: a beat is heard before the moment the clock puts it at,
    // which is when a claim about where the bar is can take effect at once. Without it, every
    // beat's claim is already history by the time it is heard and the next beat replaces it.
    constexpr double kLatency = 0.200;
    transports.setLatencySeconds(kLatency);
    transports.startOutputs(0.0);
    transports.startClock(0.0);

    constexpr double kBpm = 128.0;
    const double beat = 60.0 / kBpm;
    constexpr std::size_t kNoBarBeats = 8; // no bar known for the first eight beats
    const TempoState state;
    std::size_t next = 0;
    double firstBarKnown = -1.0;
    for (double now = 0.0; now < 24 * beat; now += 0.001) {
        while (static_cast<double>(next) * beat + 0.1 <= now) {
            const std::uint32_t inBar =
                next < kNoBarBeats ? 0 : static_cast<std::uint32_t>(next % 4) + 1;
            BeatEvent event = beatAt(static_cast<double>(next) * beat, inBar, kBpm);
            event.locked = true; // locked throughout: the bar, not the lock, is what is missing
            if (inBar != 0 && firstBarKnown < 0.0) {
                firstBarKnown = now;
            }
            transports.publish(event, 0, static_cast<double>(next) * beat);
            ++next;
        }
        wire->now = now;
        transports.advance(now, state);
    }
    double started = -1.0;
    double barOne = -1.0; // the first tick after Start: the receiver's bar 1
    for (const TimedWire::Message& message : wire->messages) {
        if (started < 0.0 && message.bytes[0] == takt4::output::MidiClock::kStart) {
            started = message.at;
        } else if (started >= 0.0 && message.bytes[0] == takt4::output::MidiClock::kTick) {
            barOne = message.at;
            break;
        }
    }
    INFO("Start at " << started << ", bar 1 at " << barOne << ", the first bar known at "
                     << firstBarKnown);
    REQUIRE(barOne >= 0.0);
    CHECK(started >= firstBarKnown);
    // And on a downbeat of the bar the tracker then named: beat 8 is beat 1 of its bar.
    const double sinceBarOne = std::fmod(barOne - kLatency - 8.0 * beat + 4.0 * beat, 4.0 * beat);
    CHECK((sinceBarOne < 0.015 || sinceBarOne > 4.0 * beat - 0.015));
    transports.stopOutputs();
}

TEST_CASE("a Link delay moved by more than the deadband is put under the beat at once",
          "[output][link]") {
    // The audit of 2026-09-25, M7. Moving the Link output's delay, or the rig's latency, was read
    // on the next beat as a phase error and nudged out an eighth a beat: 100 ms took nine seconds
    // to settle, with every peer's tempo up to 2.6 BPM off meanwhile — slow, wobbling feedback for
    // an operator dragging the slider to line Resolume up. It is a place to put the beat, and it
    // is put there on the next beat.
    //
    // Switched on and never joined, so nothing leaves this machine: Link keeps its timeline
    // locally, which is all that is asked of it here.
    Transports::Config config;
    config.link = true;
    Transports transports(config);
    REQUIRE(transports.linkEnabled());
    REQUIRE_FALSE(transports.link().enabled());
    std::string linkId;
    for (const takt4::output::OutputTarget& target : transports.outputs()) {
        if (target.kind == takt4::output::OutputTarget::Kind::Link) {
            linkId = target.id;
        }
    }
    REQUIRE_FALSE(linkId.empty());

    // Paced, as the network test is, so Link's own thread has finished with each commit.
    const std::int64_t origin = transports.link().now().count() + 1'000'000;
    constexpr double kBpm = 128.0;
    std::uint32_t k = 0;
    const auto nextBeat = [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
        const std::int64_t at = origin + static_cast<std::int64_t>(k * 60.0e6 / kBpm);
        transports.publish(beatAt(0.0, k % 4 + 1, kBpm), at, 0.0);
        ++k;
    };
    nextBeat();
    REQUIRE(transports.link().beatRequests() == 1); // the first locked beat snaps
    nextBeat();
    nextBeat();
    CHECK(transports.link().beatRequests() == 1); // and the beats on its grid do not

    SECTION("a delay moved by a hundred milliseconds snaps on the next beat") {
        REQUIRE(transports.setOutputDelay(linkId, 0.100));
        nextBeat();
        CHECK(transports.link().beatRequests() == 2);
        // The beat is where the delay puts it.
        const std::int64_t at = origin + static_cast<std::int64_t>((k - 1) * 60.0e6 / kBpm);
        const double phase =
            transports.link().phaseAtTime(std::chrono::microseconds{at + 100'000}, 4.0);
        CHECK_THAT(phase, WithinAbs(static_cast<double>((k - 1) % 4), 1e-6));
    }

    SECTION("a move inside the deadband is left to the nudge") {
        REQUIRE(transports.setOutputDelay(linkId, 0.005)); // under a hundredth of a beat
        nextBeat();
        CHECK(transports.link().beatRequests() == 1);
    }

    SECTION("so is the rig's latency") {
        transports.setLatencySeconds(-0.050);
        nextBeat();
        CHECK(transports.link().beatRequests() == 2);
    }
}

namespace {

/// A Transports with Link switched on and never joined — Link keeps its timeline locally, and
/// nothing leaves this machine — and a way to publish the beats of a 128 BPM grid on it, paced as
/// the network test is so Link's own thread has finished with each commit.
struct LocalLink {
    static constexpr double kBpm = 128.0;
    static constexpr double kPeriodMicros = 60.0e6 / kBpm;

    Transports transports{[] {
        Transports::Config config;
        config.link = true;
        return config;
    }()};
    const std::int64_t origin = transports.link().now().count() + 1'000'000;

    /// Beat `k` of the grid, `offBeats` of a beat late, numbered `beatInBar` in a bar of four.
    void beat(std::uint32_t k, std::uint32_t beatInBar, double offBeats = 0.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
        const auto at = origin + static_cast<std::int64_t>((k + offBeats) * kPeriodMicros);
        transports.publish(beatAt(0.0, beatInBar, kBpm), at, 0.0);
    }
    double phaseAtBeat(std::uint32_t k) {
        return transports.link().phaseAtTime(
            std::chrono::microseconds{origin + static_cast<std::int64_t>(k * kPeriodMicros)}, 4.0);
    }
};

} // namespace

TEST_CASE("a phase error is taken out an eighth a beat at a time, and never faster than 2%",
          "[output][link]") {
    // The audit of 2026-09-25, coverage gap 7: the nudge's size and its clamp, which nothing
    // measured. After the snap, a beat that arrives late against the session means the session
    // is ahead, and the tempo it is sent is lowered by an eighth of the error — so a peer is
    // pulled back over about eight beats — but never by more than a fiftieth, which a peer's
    // tempo display barely shows. Inside the deadband, a fiftieth of a beat, nothing is nudged.
    const double error = GENERATE(0.08, -0.08, 0.30, -0.30, 0.01);
    INFO("the beat " << error << " of a beat late");
    LocalLink link;
    REQUIRE_FALSE(link.transports.link().enabled());
    link.beat(0, 1);
    REQUIRE(link.transports.link().beatRequests() == 1); // the snap
    REQUIRE(link.transports.link().tempoUpdates() == 1);
    link.beat(1, 2, error);

    const double nudge = std::clamp(error / 8.0, -0.02, 0.02);
    CHECK(link.transports.link().beatRequests() == 1); // pulled, never jumped
    if (std::abs(error) < 0.02) {
        CHECK(link.transports.link().tempoUpdates() == 1);
        CHECK_THAT(link.transports.link().tempoBpm(), WithinAbs(LocalLink::kBpm, 1e-6));
    } else {
        CHECK(link.transports.link().tempoUpdates() == 2);
        CHECK_THAT(link.transports.link().tempoBpm(),
                   WithinAbs(LocalLink::kBpm * (1.0 - nudge), 1e-6));
    }
}

TEST_CASE("a bar the tracker counts differently is taken up after a bar of it, and not before",
          "[output][link]") {
    // Coverage gap 7's other half. After the snap the tracker changes its mind about where the bar
    // starts — every beat now numbered one on — which is a musical event, not drift: a nudge
    // would take a hundred beats over a whole beat. A bar's worth of disagreement in a row is
    // answered the way a DOWNBEAT is, with a snap; one beat of it is not.
    LocalLink link;
    link.beat(0, 1);
    REQUIRE(link.transports.link().beatRequests() == 1);
    REQUIRE_THAT(link.phaseAtBeat(0), WithinAbs(0.0, 1e-6));

    SECTION("four beats numbered one on, and the fourth snaps the bar to them") {
        for (std::uint32_t k = 1; k <= 3; ++k) {
            link.beat(k, (k + 1) % 4 + 1);
            INFO("beat " << k);
            CHECK(link.transports.link().beatRequests() == 1);
        }
        link.beat(4, (4 + 1) % 4 + 1);
        CHECK(link.transports.link().beatRequests() == 2);
        // Beat 4 of the grid is where the tracker says bar beat 2 is, so Link's bar says so too.
        CHECK_THAT(link.phaseAtBeat(4), WithinAbs(1.0, 1e-6));
        CHECK_THAT(link.transports.link().tempoBpm(), WithinAbs(LocalLink::kBpm, 1e-6));
    }

    SECTION("a beat of disagreement between agreeing ones is not a new bar") {
        // Four beats numbered one on, as many as the bar that would snap it — but never two in
        // a row, so the count starts again after each.
        for (std::uint32_t k = 1; k <= 8; ++k) {
            const bool disagree = k % 2 == 1;
            link.beat(k, disagree ? (k + 1) % 4 + 1 : k % 4 + 1);
        }
        CHECK(link.transports.link().beatRequests() == 1);
        CHECK_THAT(link.phaseAtBeat(8), WithinAbs(0.0, 1e-6));
    }
}
