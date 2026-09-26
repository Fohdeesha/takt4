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
#include "support/tap_rhythm.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
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

    // Half of what is showing, and back (the audit of 2026-09-25, H5).
    const double shown = engine->state().bpm;
    CHECK(control.dispatch(note(40)));
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(shown / 2.0, 1e-9));

    CHECK(control.dispatch(note(41)));
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(shown, 1e-9));

    // The channel is part of which control it is: the same note on another channel is
    // another button, which is what stops two controllers from fighting.
    CHECK_FALSE(control.dispatch(note(40, /*channel=*/2)));
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(shown, 1e-9));
    CHECK(engine->commandsDropped() == 0);
}

TEST_CASE("the bytes a controller sends reach the tracker as RtMidi hands them over",
          "[control][midi]") {
    // The audit of 2026-09-25, coverage gap 13: the callback's own path — raw bytes, read as a
    // gesture or not, then dispatched — which every other test here skips by building the event
    // itself. RtMidi delivering them needs a MIDI loopback this machine has not got; everything
    // after that is here.
    auto engine = makeEngine();
    MidiControl control(*engine, offline());
    trackUntilLocked(*engine);
    REQUIRE(engine->state().locked);
    MidiBinding halve;
    halve.number = 40;
    halve.target = ControlAction::TempoHalve;
    MidiBinding redouble;
    redouble.kind = MidiEvent::Kind::ControlChange;
    redouble.channel = 3;
    redouble.number = 21;
    redouble.target = ControlAction::TempoDouble;
    REQUIRE(control.bind(halve));
    REQUIRE(control.bind(redouble));
    const double shown = engine->state().bpm;
    const auto receive = [&control](std::vector<unsigned char> bytes) {
        control.receive(std::span<const unsigned char>(bytes.data(), bytes.size()));
    };

    receive({0x90, 40, 100}); // note on, channel 1
    CHECK(control.handled() == 1);
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(shown / 2.0, 1e-9));

    // The release of that press, both ways a keyboard spells it: it reaches the binding, as a
    // CC pad's release does (the audit's L6), and a button does nothing with it — the tempo is
    // halved once, not again.
    receive({0x80, 40, 0});
    receive({0x90, 40, 0});
    CHECK(control.handled() == 3);
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(shown / 2.0, 1e-9));

    // And what a controller sends that is no gesture at all: none of it acts, and none of it
    // is counted as a control ignored.
    receive({0xF8});         // a clock tick
    receive({0xFE});         // active sensing
    receive({0xF0, 1, 0xF7}); // system exclusive
    receive({0x90, 40});     // cut short
    receive({});
    CHECK(control.handled() == 3);
    CHECK(control.ignored() == 0);

    // A CC on channel 3, bound, doubles it back.
    receive({0xB2, 21, 127});
    CHECK(control.handled() == 4);
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(shown, 1e-9));

    // The same note on channel 3 is another button, and not one of ours.
    receive({0x92, 40, 100});
    CHECK(control.handled() == 4);
    CHECK(control.ignored() == 1);
    REQUIRE(control.lastEvent().has_value());
    CHECK(control.lastEvent()->channel == 3);
    CHECK(control.lastEvent()->number == 40);
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

    SECTION("a pad bound to it pins while it is held, and lets go when it is released") {
        // The audit of 2026-09-25, L6. A note used to carry its press and no release, so a pad
        // bound to the lock pinned it and nothing on the controller could let go — "bind two
        // notes, one for each end" pinned it twice. Its release is the note at 0 now, as a CC
        // pad's release is a CC at 0, and the lock follows the pad exactly as it follows the
        // CC above. Still no toggle: that would depend on a state the controller cannot see.
        MidiBinding pad;
        pad.number = 50;
        pad.target = ControlAction::Lock;
        REQUIRE(control.bind(pad));
        CHECK(control.dispatch(note(50)));
        (void)engine->step();
        CHECK(engine->state().pinned);
        CHECK(control.dispatch(note(50, 1, 0))); // the release
        (void)engine->step();
        CHECK_FALSE(engine->state().pinned);

        // And from the wire, where a release is spelled either way hardware spells it.
        const auto receive = [&control](std::vector<unsigned char> bytes) {
            control.receive(std::span<const unsigned char>(bytes.data(), bytes.size()));
        };
        for (const unsigned char off : std::initializer_list<unsigned char>{0x80, 0x90}) {
            receive({0x90, 50, 100});
            (void)engine->step();
            CHECK(engine->state().pinned);
            receive({off, 50, 0});
            (void)engine->step();
            CHECK_FALSE(engine->state().pinned);
        }
    }

    SECTION("learning a pad for it does not let go of a lock pinned from elsewhere") {
        // The release of the press being learned reaches the bindings now (L6), and is swallowed
        // as a CC pad's is (H1): otherwise learning a pad for the lock unpinned it on the way.
        CHECK(control.dispatch(cc(64, 127)));
        (void)engine->step();
        REQUIRE(engine->state().pinned);
        control.learn(ControlAction::Lock);
        CHECK(control.dispatch(note(51)));
        CHECK(control.dispatch(note(51, 1, 0)));
        (void)engine->step();
        CHECK(engine->state().pinned);
        // And the pad is the lock's from then on.
        CHECK(control.dispatch(note(51, 1, 0)));
        (void)engine->step();
        CHECK_FALSE(engine->state().pinned);
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
        // Every press engages, and neither a second press nor lifting the finger lets go: a
        // note's release reaches the binding since the audit's L6, and PANIC from MIDI skips
        // it, as it skips a CC pad's (H1). Releasing is the window's RELEASE.
        MidiBinding pad;
        pad.number = 36;
        pad.target = ControlAction::Panic;
        REQUIRE(control.bind(pad));

        CHECK(control.dispatch(note(36)));
        CHECK(control.dispatch(note(36, 1, 0)));
        CHECK(control.dispatch(note(36)));
        CHECK(control.dispatch(note(36, 1, 0)));
        CHECK(rules.panics() == std::vector<bool>{true, true});
    }

    SECTION("a CC bound to panic engages it, and letting go of the pad does not release it") {
        // The audit's H1. A CC reads below 64 as "off", which made a momentary CC pad —
        // 127 on the press, 0 on the release, the commonest pad there is — hold-to-panic: the
        // halt let go the moment the finger came up. PANIC from a control surface engages and
        // nothing else, as the window's has since H18; RELEASE is the window's, or
        // `panic/release`.
        MidiBinding knob;
        knob.kind = MidiEvent::Kind::ControlChange;
        knob.number = 64;
        knob.target = ControlAction::Panic;
        REQUIRE(control.bind(knob));

        CHECK(control.dispatch(cc(64, 127)));
        CHECK(control.dispatch(cc(64, 0)));
        CHECK(rules.panics() == std::vector<bool>{true});
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
    // keep them apart. The second was claimed here and counted nowhere: this asserted only
    // the first, which is the swallow the name says it is not (the audit of 2026-09-25, T13).
    CHECK(control.dispatch(note(36)));
    CHECK(control.handled() == 1);
    CHECK(control.refused() == 1);

    // And it becomes real the moment something is wired, with no rebinding.
    takt4::testing::RecordingRules rules;
    control.setRuleControl(&rules);
    CHECK(control.dispatch(note(36)));
    CHECK(rules.panics() == std::vector<bool>{true});
    CHECK(control.handled() == 2);
    CHECK(control.refused() == 1);
}

TEST_CASE("three taps on a pad make a tempo, even from a pad that chatters", "[control][midi]") {
    // This used to stop at "handled", which is how a pad that tapped twice per press went
    // unseen (the audit's H1 and T1). A rhythm played in real time, every press retriggering
    // 5 ms later the way a worn pad does, and where the tracker's tempo window ends up: centred
    // on the tempo tapped (a seed, §7) — which, without the bounce guard, it is not.
    auto engine = makeEngine();
    MidiControl control(*engine, offline());

    MidiBinding tap;
    tap.number = 36;
    tap.target = ControlAction::Tap;
    REQUIRE(control.bind(tap));

    const takt4::testing::TappedRhythm rhythm = takt4::testing::tapWithBounces(
        [&control] { CHECK(control.dispatch(note(36))); }, 3, std::chrono::milliseconds{400});
    const double heard = rhythm.tempoOfThree();
    INFO("tapped at " << heard << " BPM (" << rhythm.slowestOfThree() << " to "
                      << rhythm.fastestOfThree() << " as the surface could have stamped it); "
                      << "the slowest bounce came " << rhythm.longestBounce()
                      << " s after its press");
    REQUIRE(rhythm.longestBounce() < 0.1);
    REQUIRE(heard == Approx(150.0).margin(8.0));

    // Every press was understood, the bounces included: a bounce is ignored, not refused.
    CHECK(control.handled() == 6);
    (void)engine->step();
    CHECK(engine->commandsDropped() == 0);
    // Within what the surface's own stamps allow — see `TappedRhythm::slowestOfThree`.
    const auto window = engine->tempoOptions();
    const double centre = std::sqrt(window.minBpm * window.maxBpm);
    CHECK(centre >= rhythm.slowestOfThree() - 0.5);
    CHECK(centre <= rhythm.fastestOfThree() + 0.5);
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

TEST_CASE("a real MIDI input opens and closes again and again", "[control][midi][hardware]") {
    // The audit's M14: RtMidi's WinMM close held its lock across `midiInReset`, which hands
    // pending SysEx buffers back through the input callback — which takes the same lock
    // (cmake/rtmidi_patch.cmake). The deadlock itself wants a device sending SysEx at the
    // moment of the close, which no test can arrange; what this does run is the patched close
    // and open, against a real port and the real driver, over and over: every close returns,
    // and every reopen works — which upstream's fix alone would not have kept, since it never
    // lowered its flag again.
    const std::vector<std::string> ports = takt4::output::listMidiInputPorts();
    if (ports.empty()) {
        SKIP("no MIDI input on this machine");
    }
    auto engine = makeEngine();
    MidiControl::Config config;
    config.enabled = true;
    config.port = ports.front();
    MidiControl control(*engine, config);
    INFO("port " << ports.front());
    for (int round = 0; round < 5; ++round) {
        INFO("round " << round);
        REQUIRE_NOTHROW(control.start());
        CHECK(control.running());
        const auto closing = std::chrono::steady_clock::now();
        control.stop();
        CHECK_FALSE(control.running());
        CHECK(std::chrono::steady_clock::now() - closing < std::chrono::seconds(2));
    }
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

TEST_CASE("a CC pad's release is not a second press", "[control][midi]") {
    // The audit's H1. A pad in CC mode sends 127 on the press and 0 on the release, and every
    // CC value used to be dispatched — so ÷2 halved twice, a tap button tapped twice about
    // 120 ms apart, and a downbeat snapped again, late, on the release.
    auto engine = makeEngine();
    MidiControl control(*engine, offline());
    trackUntilLocked(*engine);
    REQUIRE(engine->state().locked);

    MidiBinding halve;
    halve.kind = MidiEvent::Kind::ControlChange;
    halve.number = 20;
    halve.target = ControlAction::TempoHalve;
    REQUIRE(control.bind(halve));

    const double shown = engine->state().bpm;
    CHECK(control.dispatch(cc(20, 127)));
    CHECK(control.dispatch(cc(20, 0)));
    (void)engine->step();
    // Halved once, not quartered.
    CHECK_THAT(engine->state().bpm, WithinAbs(shown / 2.0, 1e-9));
}

TEST_CASE("learning a CC fires it neither on the press nor on the release", "[control][midi]") {
    // Learn promises the gesture that assigns a control does not also fire it. The press was
    // swallowed; the release that followed went through the brand-new binding and fired it —
    // which for a state action is not a no-op: learning a pad for a rule's mute, on a rule the
    // operator had muted, unmuted it on the way up.
    auto engine = makeEngine();
    takt4::testing::RecordingRules rules;
    MidiControl control(*engine, offline(), &rules);

    control.learn(takt4::control::ControlTarget(ControlAction::RuleMute, "stabs"));
    CHECK(control.dispatch(cc(21, 127)));
    CHECK(control.dispatch(cc(21, 0)));
    CHECK(rules.mutes().empty());
    REQUIRE(control.bindings().size() == 1);

    // And from then on it is that control: a switch, as a state action is.
    CHECK(control.dispatch(cc(21, 127)));
    CHECK(control.dispatch(cc(21, 0)));
    const std::vector<std::pair<std::string, bool>> expected{{"stabs", true}, {"stabs", false}};
    CHECK(rules.mutes() == expected);

    SECTION("only the learned control's own release is swallowed") {
        control.learn(takt4::control::ControlTarget(ControlAction::RuleMute, "pads"));
        CHECK(control.dispatch(cc(22, 127)));
        // Another control moving in between is its own gesture, and acts.
        CHECK(control.dispatch(cc(21, 127)));
        CHECK(rules.mutes().size() == 3);
    }
}

TEST_CASE("a push button that sends 1 then 0 taps once a press", "[control]") {
    // TouchOSC's push button, a Stream Deck's, and a CC pad through `argumentOf`: 1 on the
    // press and 0 on the release. Driven with the times supplied, the way `TapTempo` is built
    // to be, and the release 120 ms after the press — past the bounce window, so this is the
    // release being recognised and not the bounce filter hiding it.
    auto engine = makeEngine();
    takt4::testing::RecordingRules rules;
    takt4::control::ControlSurface surface(*engine, &rules);
    const takt4::control::ControlTarget tap(ControlAction::Tap);
    for (int press = 0; press < 4; ++press) {
        const double at = 10.0 + 0.5 * press; // 120 BPM
        CHECK(surface.apply(tap, 1.0, at));
        CHECK(surface.apply(tap, 0.0, at + 0.120));
    }
    CHECK(surface.taps().taps() == 4);
    CHECK_THAT(surface.taps().bpm(), WithinAbs(120.0, 1e-6));

    SECTION("but the four actions that hold a state still read 0 as off") {
        const takt4::control::ControlTarget arm(ControlAction::RuleEnable, "intro");
        CHECK(surface.apply(arm, 1.0, 20.0));
        CHECK(surface.apply(arm, 0.0, 20.2));
        const std::vector<std::pair<std::string, bool>> expected{{"intro", true}, {"intro", false}};
        CHECK(rules.enables() == expected);
    }

    SECTION("and panic/release lets go, where a panic 0 no longer does") {
        // The audit of 2026-09-25, L15 (Q1): a 0 to panic was a release, so a push button's
        // own release undid the halt. A panic engages whatever it carries; letting go is an
        // action of its own.
        const takt4::control::ControlTarget panic(ControlAction::Panic);
        const takt4::control::ControlTarget release(ControlAction::PanicRelease);
        CHECK(surface.apply(panic, 1.0, 30.0));
        CHECK(surface.apply(panic, 0.0, 30.5));
        CHECK(rules.panics() == std::vector<bool>{true, true});
        CHECK(surface.apply(release, std::nullopt, 31.0));
        CHECK(rules.panics() == std::vector<bool>{true, true, false});
    }
}
