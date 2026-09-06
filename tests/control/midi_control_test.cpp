#include "core/audio/rates.hpp"
#include "core/control/control_action.hpp"
#include "core/control/midi_binding.hpp"
#include "core/control/midi_control.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/output/midi_ports.hpp"
#include "core/tracking/state_space.hpp"

#include "support/recording_rules.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::control::ControlAction;
using takt4::control::MidiBinding;
using takt4::control::MidiControl;
using takt4::control::MidiEvent;
using takt4::engine::BeatEngine;

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

std::unique_ptr<BeatEngine> makeEngine() {
    return std::make_unique<BeatEngine>(weights(), stateSpace());
}

void trackUntilLocked(BeatEngine& engine) {
    static const std::vector<float> samples =
        takt4::io::readWavFile(kTestData / "features" / "synthetic.wav").samples;
    const std::size_t hops = samples.size() / takt4::audio::kHopSize;
    for (std::size_t hop = 0; hop < hops && !engine.state().locked; ++hop) {
        engine.processHop(samples.data() + hop * takt4::audio::kHopSize, hop);
        (void)engine.step();
    }
}

MidiEvent note(std::uint8_t number, std::uint8_t channel = 1, std::uint8_t velocity = 100) {
    MidiEvent event;
    event.kind = MidiEvent::Kind::Note;
    event.channel = channel;
    event.number = number;
    event.value = velocity;
    return event;
}

MidiEvent cc(std::uint8_t number, std::uint8_t value, std::uint8_t channel = 1) {
    MidiEvent event;
    event.kind = MidiEvent::Kind::ControlChange;
    event.channel = channel;
    event.number = number;
    event.value = value;
    return event;
}

/// Disabled and with no port: nothing here opens hardware, and `dispatch` is the seam
/// that lets the table be driven without any.
MidiControl::Config offline() {
    return MidiControl::Config{};
}

} // namespace

TEST_CASE("learn mode binds the next control that moves", "[control][midi]") {
    auto engine = makeEngine();
    MidiControl control(*engine, offline());

    CHECK_FALSE(control.learning().has_value());
    control.learn(ControlAction::Downbeat);
    CHECK(control.learning() == ControlAction::Downbeat);

    // The gesture that assigns a control must not also fire it: an operator binding
    // `tempo/halve` would otherwise halve the tempo in the act of binding it.
    CHECK(control.dispatch(note(36, 10)));
    CHECK_FALSE(control.learning().has_value()); // and it disarms itself
    CHECK(control.handled() == 0);

    REQUIRE(control.bindings().size() == 1);
    // By value, not by reference: `bindings()` copies the table out from under its mutex,
    // so a reference to `.front()` would dangle the moment the full-expression ended.
    const MidiBinding learned = control.bindings().front();
    CHECK(learned.kind == MidiEvent::Kind::Note);
    CHECK(learned.number == 36);
    CHECK(learned.channel == 10);
    CHECK(learned.target.action == ControlAction::Downbeat);

    // And now it is that control.
    CHECK(control.dispatch(note(36, 10)));
    CHECK(control.handled() == 1);

    SECTION("a control learned twice changes its mind rather than doing both") {
        control.learn(ControlAction::Tap);
        CHECK(control.dispatch(note(36, 10)));
        REQUIRE(control.bindings().size() == 1);
        CHECK(control.bindings().front().target.action == ControlAction::Tap);
    }

    SECTION("cancelling leaves the table alone") {
        control.learn(ControlAction::Tap);
        control.cancelLearn();
        CHECK_FALSE(control.learning().has_value());
        // Not learned and not bound, so nothing happened to it — which is the whole
        // claim: a cancelled learn must not quietly bind the next thing pressed.
        CHECK_FALSE(control.dispatch(note(48, 10)));
        CHECK(control.bindings().size() == 1);
        CHECK(control.ignored() == 1);
    }
}

TEST_CASE("a bound control reaches the tracker", "[control][midi]") {
    auto engine = makeEngine();
    MidiControl control(*engine, offline());
    trackUntilLocked(*engine);
    REQUIRE(engine->state().locked);

    MidiBinding halve;
    halve.number = 40;
    halve.target = ControlAction::TempoHalve;
    MidiBinding redouble;
    redouble.number = 41;
    redouble.target = ControlAction::TempoDouble;
    REQUIRE(control.bind(halve));
    REQUIRE(control.bind(redouble));

    const double raw = engine->state().rawBpm;
    CHECK(control.dispatch(note(40)));
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(raw / 2.0, 1e-6));

    CHECK(control.dispatch(note(41)));
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(raw, 1e-6));

    // The channel is part of which control it is: the same note on another channel is
    // another button, which is what stops two controllers from fighting.
    CHECK_FALSE(control.dispatch(note(40, /*channel=*/2)));
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(raw, 1e-6));
    CHECK(engine->commandsDropped() == 0);
}

TEST_CASE("a switch bound to the lock pins with its own position", "[control][midi]") {
    auto engine = makeEngine();
    MidiControl control(*engine, offline());
    trackUntilLocked(*engine);
    REQUIRE(engine->state().locked);
    REQUIRE_FALSE(engine->state().pinned);

    MidiBinding lock;
    lock.kind = MidiEvent::Kind::ControlChange;
    lock.number = 64; // where a sustain pedal lives
    lock.target = ControlAction::Lock;
    REQUIRE(control.bind(lock));

    CHECK(control.dispatch(cc(64, 127)));
    (void)engine->step();
    CHECK(engine->state().pinned);

    CHECK(control.dispatch(cc(64, 0)));
    (void)engine->step();
    CHECK_FALSE(engine->state().pinned);
    CHECK_FALSE(engine->state().locked); // released, so the tracker has it back

    SECTION("a note bound to it is a press, so it pins and stays pinned") {
        // §5.7 spells lock `<0|1>` and a note has no 0 to send. That is not a gap: an
        // operator who wants one button for each end binds two notes, and one who wants a
        // toggle binds a CC. Inventing a toggle here would depend on a state the
        // controller cannot see.
        MidiBinding pad;
        pad.number = 50;
        pad.target = ControlAction::Lock;
        REQUIRE(control.bind(pad));
        CHECK(control.dispatch(note(50)));
        (void)engine->step();
        CHECK(engine->state().pinned);
        CHECK(control.dispatch(note(50)));
        (void)engine->step();
        CHECK(engine->state().pinned);
    }
}

TEST_CASE("a box of buttons can reach the rules as well as the tracker", "[control][midi]") {
    // §5.7: "Every one of these is also bindable to a MIDI note or CC through a learn
    // mode." That includes the two addresses whose subject is §5.8's rules rather than the
    // tracker, and they take a different route out of the surface to get there.
    auto engine = makeEngine();
    takt4::testing::RecordingRules rules;
    MidiControl control(*engine, offline(), &rules);

    SECTION("a pad bound to panic is a panic button, and stays one") {
        // A note carries a press and no release (`argumentOf`), so it engages every time.
        // For panic that is exactly right: a second press during a bad moment must not be
        // the thing that lets the rig go again.
        MidiBinding pad;
        pad.number = 36;
        pad.target = ControlAction::Panic;
        REQUIRE(control.bind(pad));

        CHECK(control.dispatch(note(36)));
        CHECK(control.dispatch(note(36)));
        CHECK(rules.panics() == std::vector<bool>{true, true});
    }

    SECTION("a switch bound to panic is a switch, so the same control lets go") {
        MidiBinding knob;
        knob.kind = MidiEvent::Kind::ControlChange;
        knob.number = 64;
        knob.target = ControlAction::Panic;
        REQUIRE(control.bind(knob));

        CHECK(control.dispatch(cc(64, 127)));
        CHECK(control.dispatch(cc(64, 0)));
        CHECK(rules.panics() == std::vector<bool>{true, false});
    }

    SECTION("a binding to a rule carries which rule, because a gesture cannot") {
        MidiBinding arm;
        arm.kind = MidiEvent::Kind::ControlChange;
        arm.number = 21;
        arm.target = takt4::control::ControlTarget(ControlAction::RuleEnable, "drop");
        REQUIRE(control.bind(arm));

        CHECK(control.dispatch(cc(21, 127)));
        CHECK(control.dispatch(cc(21, 0)));
        const std::vector<std::pair<std::string, bool>> expected{{"drop", true}, {"drop", false}};
        CHECK(rules.enables() == expected);
    }

    SECTION("learn mode arms a whole target, so the rule survives being bound") {
        // §5.9's editor is what will call this, from the card that already names the rule —
        // which is why `learn` takes a target rather than an action.
        control.learn(takt4::control::ControlTarget(ControlAction::RuleEnable, "intro"));
        REQUIRE(control.dispatch(note(48)));
        REQUIRE(control.bindings().size() == 1);
        CHECK(control.bindings().front().target ==
              takt4::control::ControlTarget(ControlAction::RuleEnable, "intro"));
        // Learned, not acted on — the same rule every other action follows.
        CHECK(rules.enables().empty());

        CHECK(control.dispatch(note(48)));
        const std::vector<std::pair<std::string, bool>> expected{{"intro", true}};
        CHECK(rules.enables() == expected);
    }
}

TEST_CASE("a rule control that is not there refuses rather than swallows", "[control][midi]") {
    // The default: a `MidiControl` built with no rules — Q8's headless mode before it has
    // an output runner, or any app that has none. A binding to panic then reports that it
    // did nothing, which is what puts "your panic button is not wired" in front of somebody
    // at the bench rather than during a set.
    auto engine = makeEngine();
    MidiControl control(*engine, offline());

    MidiBinding pad;
    pad.number = 36;
    pad.target = ControlAction::Panic;
    REQUIRE(control.bind(pad));

    // The *event* was still ours — it matched a binding — so it counts as handled; what it
    // asked for is what could not be done. Those are two different claims and the counters
    // keep them apart.
    CHECK(control.dispatch(note(36)));
    CHECK(control.handled() == 1);

    // And it becomes real the moment something is wired, with no rebinding.
    takt4::testing::RecordingRules rules;
    control.setRuleControl(&rules);
    CHECK(control.dispatch(note(36)));
    CHECK(rules.panics() == std::vector<bool>{true});
}

TEST_CASE("three taps on a pad make a tempo", "[control][midi]") {
    auto engine = makeEngine();
    MidiControl control(*engine, offline());

    MidiBinding tap;
    tap.number = 36;
    tap.target = ControlAction::Tap;
    REQUIRE(control.bind(tap));

    // The first two say nothing, which is not the same as failing: the control was
    // understood either way, so all three count as handled.
    CHECK(control.dispatch(note(36)));
    CHECK(control.dispatch(note(36)));
    CHECK(control.dispatch(note(36)));
    CHECK(control.handled() == 3);
    (void)engine->step();
    CHECK(engine->commandsDropped() == 0);
}

TEST_CASE("the binding table is restored the way it was saved", "[control][midi]") {
    auto engine = makeEngine();
    MidiControl control(*engine, offline());

    std::vector<MidiBinding> saved;
    for (const ControlAction action : takt4::control::kControlActions) {
        MidiBinding binding;
        binding.number = static_cast<std::uint8_t>(40 + saved.size());
        binding.channel = 3;
        binding.target = takt4::control::takesRuleId(action)
                             ? takt4::control::ControlTarget(action, "intro")
                             : takt4::control::ControlTarget(action);
        saved.push_back(binding);
    }
    control.setBindings(saved);
    CHECK(control.bindings() == saved);

    SECTION("two lines naming one control leave the later one, as learning twice does") {
        std::vector<MidiBinding> clashing;
        MidiBinding first;
        first.number = 60;
        first.target = ControlAction::Tap;
        MidiBinding second;
        second.number = 60;
        second.target = ControlAction::Downbeat;
        clashing.push_back(first);
        clashing.push_back(second);
        control.setBindings(clashing);
        REQUIRE(control.bindings().size() == 1);
        CHECK(control.bindings().front().target.action == ControlAction::Downbeat);
    }

    SECTION("an unusable line is dropped rather than kept as a control nothing can send") {
        std::vector<MidiBinding> bad;
        MidiBinding offChannel;
        offChannel.number = 60;
        offChannel.channel = 17;
        bad.push_back(offChannel);
        control.setBindings(bad);
        CHECK(control.bindings().empty());
        CHECK_FALSE(control.bind(offChannel));
    }

    SECTION("forgetting an action takes every control bound to it") {
        MidiBinding second;
        second.number = 99;
        second.channel = 3;
        second.target = ControlAction::Tap;
        REQUIRE(control.bind(second));
        CHECK(control.forget(ControlAction::Tap) == 2);
        for (const MidiBinding& binding : control.bindings()) {
            CHECK(binding.target.action != ControlAction::Tap);
        }
    }
}

TEST_CASE("what arrived is visible even when nothing is bound to it", "[control][midi]") {
    // A controller on the wrong channel looks exactly like a broken one until somebody can
    // see that its messages are arriving.
    auto engine = makeEngine();
    MidiControl control(*engine, offline());

    CHECK_FALSE(control.lastEvent().has_value());
    CHECK_FALSE(control.dispatch(cc(7, 64, /*channel=*/11)));
    CHECK(control.ignored() == 1);
    CHECK(control.handled() == 0);

    const auto seen = control.lastEvent();
    REQUIRE(seen.has_value());
    CHECK(seen->kind == MidiEvent::Kind::ControlChange);
    CHECK(seen->number == 7);
    CHECK(seen->channel == 11);
    CHECK(seen->value == 64);
}

TEST_CASE("a control that was never enabled opens no port", "[control][midi]") {
    // Off unless asked for, and a port has to be named: opening whatever MIDI input
    // happens to be first would bind a DAW's clock stream to somebody's tap button.
    auto engine = makeEngine();
    MidiControl control(*engine, offline());

    control.start();
    CHECK_FALSE(control.running());
    CHECK(control.portName().empty());

    control.stop();
    control.stop(); // safe twice, and safe having never started
    CHECK_FALSE(control.running());
}

TEST_CASE("changing the port keeps what was learned", "[control][midi]") {
    // The whole reason `setPort` exists rather than building a new MidiControl: an
    // operator moving from one controller to another is not asking to forget their
    // bindings, and in a picker those two are one keystroke apart.
    auto engine = makeEngine();
    MidiControl control(*engine, offline());

    MidiBinding tap;
    tap.number = 36;
    tap.channel = 10;
    tap.target = ControlAction::Tap;
    REQUIRE(control.bind(tap));

    control.setPort("some other controller");
    CHECK(control.config().port == "some other controller");
    CHECK(control.config().enabled);
    CHECK(control.bindings().size() == 1);
    CHECK(control.bindings().front() == tap);

    // And an empty name is "none", which stops it without touching the table.
    control.setPort("");
    CHECK_FALSE(control.config().enabled);
    CHECK_FALSE(control.running());
    CHECK(control.bindings().size() == 1);
}

TEST_CASE("a port that is not there is reported rather than silently dead",
          "[control][midi][hardware]") {
    auto engine = makeEngine();
    MidiControl::Config config;
    config.enabled = true;
    config.port = "no such port exists on any machine";
    MidiControl control(*engine, config);

    // Either there is no MIDI API at all on this box or there is no such port; both are
    // errors an operator has to be told about, and both come back as one.
    CHECK_THROWS(control.start());
    CHECK_FALSE(control.running());
}

TEST_CASE("the machine's MIDI inputs can be listed without opening one", "[control][midi]") {
    // Never throws, even where RtMidi has no usable API at all — it reports that by
    // throwing from its own constructor, and a listing has to swallow it.
    CHECK_NOTHROW(takt4::output::listMidiInputPorts());
    const std::vector<std::string> ports = takt4::output::listMidiInputPorts();
    for (const std::string& port : ports) {
        INFO(port);
        CHECK_FALSE(port.empty());
    }
}
