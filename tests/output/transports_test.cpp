#include "core/output/transports.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
    config.outputs = takt4::output::oscOutputs({{"127.0.0.1", 7000}});
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

    // A beat at 120 BPM, where a beat is 500 ms: 200 ms early is a 300 ms hold. Nothing has
    // left yet, which is the assertion — before this, everything went out immediately.
    transports.publish(beatAt(1.0, 1, 120.0), 0, 1.0);
    const std::size_t held = transports.osc().pending();
    CHECK(held > 0);
    CHECK(transports.osc().messagesSent() == 0);

    transports.advance(1.2999, steady);
    CHECK(transports.osc().messagesSent() == 0);
    CHECK(transports.osc().pending() == held);

    transports.advance(1.3001, steady);
    CHECK(transports.osc().messagesSent() == held);
    CHECK(transports.osc().pending() == 0);

    SECTION("and moving it mid-set leaves what is already queued where it was") {
        // A held message keeps the deadline it was given: a target's queue is FIFO, and
        // rewriting deadlines under it would reorder a rig in the middle of a bar.
        transports.publish(beatAt(2.0, 2, 120.0), 0, 2.0);
        const std::size_t queued = transports.osc().pending();
        REQUIRE(queued > 0);

        transports.setLatencySeconds(0.0);
        transports.advance(2.1, steady); // before the 2.3 those are due at
        CHECK(transports.osc().pending() == queued);

        // The next beat feels the new offset and goes out at once.
        const std::uint64_t before = transports.osc().messagesSent();
        transports.publish(beatAt(2.5, 3, 120.0), 0, 2.5);
        CHECK(transports.osc().messagesSent() > before);
        CHECK(transports.osc().pending() == queued);
    }

    transports.stopOutputs();
}

TEST_CASE("Link is built whether or not it is switched on", "[output][link]") {
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

TEST_CASE("switching Link on before the outputs start does not join yet", "[output][link]") {
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

    transports.setOscTargets({{"127.0.0.1", 7000}, {"127.0.0.1", 7001}});
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
    transports.setOscTargets({{"127.0.0.1", 7002}});
    CHECK(transports.osc().targetCount() == 1);
    transports.advance(0.3, state);
    CHECK(transports.osc().messagesSent() > afterFirst);

    transports.setOscTargets({});
    CHECK(transports.osc().targetCount() == 0);
    CHECK_FALSE(transports.any());
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

TEST_CASE("a beat reaches Link as a tempo and a bar position", "[output][link]") {
    // The wiring only, held to Link's own counters so that nothing here depends on
    // timing. What those calls then *mean* — that only the phase survives a capture, that
    // a request is moved to where the phase already matches — is link_session_test.cpp's
    // business.
    Transports::Config config;
    config.link = true;
    Transports transports(config);
    transports.startOutputs(0.0);

    const std::int64_t hostMicros = transports.link().now().count();

    // Offline, there is no host clock to align to, so Link is deliberately left alone.
    transports.publish(beatAt(0.5, 1, 120.0), 0, 0.5);
    CHECK(transports.link().tempoUpdates() == 0);
    CHECK(transports.link().beatRequests() == 0);

    // Live, the same beat is a tempo and a bar position.
    transports.publish(beatAt(1.0, 1, 128.0), hostMicros, 1.0);
    CHECK(transports.link().tempoUpdates() == 1);
    CHECK(transports.link().beatRequests() == 1);

    // A tempo that has not moved is not resent: the refined tempo wobbles by hundredths of
    // a BPM on almost every beat, and no peer can act on that.
    transports.publish(beatAt(1.5, 2, 128.001), hostMicros, 1.5);
    CHECK(transports.link().tempoUpdates() == 1);
    CHECK(transports.link().beatRequests() == 2);

    // A tempo that really moved is.
    transports.publish(beatAt(2.0, 3, 130.0), hostMicros, 2.0);
    CHECK(transports.link().tempoUpdates() == 2);
    CHECK(transports.link().beatRequests() == 3);

    // Before the first downbeat the bar phase is unknown, and publishing a guess would put
    // every peer on the wrong beat of the bar. Nothing goes out for it.
    BeatEvent unphased = beatAt(2.5, 0, 130.0);
    unphased.beatsPerBar = 0;
    transports.publish(unphased, hostMicros, 2.5);
    CHECK(transports.link().beatRequests() == 3);
    CHECK(transports.link().tempoUpdates() == 2);

    // Switched off, a beat stops reaching it at all.
    transports.setLinkEnabled(false);
    transports.publish(beatAt(3.0, 4, 135.0), hostMicros, 3.0);
    CHECK(transports.link().tempoUpdates() == 2);
    CHECK(transports.link().beatRequests() == 3);

    // Switched on again, the tempo is resent even though it has not changed since: the
    // session rejoined knowing nothing.
    transports.setLinkEnabled(true);
    transports.publish(beatAt(3.5, 1, 135.0), hostMicros, 3.5);
    CHECK(transports.link().tempoUpdates() == 3);

    transports.stopOutputs();
    CHECK(transports.beats() == 7);
}
