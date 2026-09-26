#include "core/control/control_action.hpp"
#include "core/control/midi_binding.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using takt4::control::ControlAction;
using takt4::control::MidiBinding;
using takt4::control::MidiEvent;

namespace {

std::optional<MidiEvent> read(std::vector<unsigned char> bytes) {
    return takt4::control::readMidiEvent(
        std::span<const unsigned char>(bytes.data(), bytes.size()));
}

} // namespace

TEST_CASE("a press is a note, and its release the same note at 0", "[control][midi]") {
    // Note-on, channel 10 as the hardware prints it, note 36, velocity 100.
    const auto press = read({0x99, 36, 100});
    REQUIRE(press.has_value());
    CHECK(press->kind == MidiEvent::Kind::Note);
    CHECK(press->channel == 10);
    CHECK(press->number == 36);
    CHECK(press->value == 100);
    CHECK(argumentOf(*press) == 1.0);

    // Its release, both ways hardware spells it: velocity 0, which most send, and an actual
    // note-off, whose velocity is how fast the key came up and not a press. Both are the note
    // at 0, as a CC pad's release is a CC at 0 — a button ignores it and a state follows it
    // (the audit of 2026-09-25, L6: dropped, a pad bound to the lock could never let go).
    for (const auto& release : {read({0x99, 36, 0}), read({0x89, 36, 100})}) {
        REQUIRE(release.has_value());
        CHECK(release->kind == MidiEvent::Kind::Note);
        CHECK(release->channel == 10);
        CHECK(release->number == 36);
        CHECK(release->value == 0);
        CHECK(argumentOf(*release) == 0.0);
    }

    const auto cc = read({0xB0, 64, 127});
    REQUIRE(cc.has_value());
    CHECK(cc->kind == MidiEvent::Kind::ControlChange);
    CHECK(cc->channel == 1); // 0x0 in the status byte is channel 1 to a person
    CHECK(cc->number == 64);
    CHECK(cc->value == 127);

    // A CC of 0 is a real message — a switch being released — unlike a note of velocity 0.
    CHECK(read({0xB0, 64, 0}).has_value());

    SECTION("everything that is not a button or a knob is refused") {
        CHECK_FALSE(read({0xF8}).has_value());         // clock tick
        CHECK_FALSE(read({0xFE}).has_value());         // active sensing
        CHECK_FALSE(read({0xE0, 0, 64}).has_value());  // pitch bend
        CHECK_FALSE(read({0xC0, 5, 0}).has_value());   // program change
        CHECK_FALSE(read({0xD0, 64, 0}).has_value());  // channel pressure
        CHECK_FALSE(read({0xA0, 36, 64}).has_value()); // polyphonic aftertouch
        CHECK_FALSE(read({0x99, 36}).has_value());     // truncated
        CHECK_FALSE(read({}).has_value());
    }
}

TEST_CASE("a binding survives being written down and read back", "[control][midi]") {
    for (const ControlAction action : takt4::control::kControlActions) {
        for (const MidiEvent::Kind kind : {MidiEvent::Kind::Note, MidiEvent::Kind::ControlChange}) {
            MidiBinding binding;
            binding.kind = kind;
            binding.channel = 16;
            binding.number = 127;
            // The one action that names something as well as doing it has to carry the name
            // through the file too, or a restored binding would point at no rule at all.
            binding.target = takt4::control::takesRuleId(action)
                                 ? takt4::control::ControlTarget(action, "intro")
                                 : takt4::control::ControlTarget(action);

            const std::string text = takt4::control::formatMidiBinding(binding);
            INFO(text);
            const std::optional<MidiBinding> back = takt4::control::parseMidiBinding(text);
            REQUIRE(back.has_value());
            CHECK(*back == binding);
        }
    }

    // The form it takes is a sentence, because Q8's headless mode expects one to be typed.
    MidiBinding tap;
    tap.number = 36;
    tap.channel = 10;
    tap.target = ControlAction::Tap;
    CHECK(takt4::control::formatMidiBinding(tap) == "note 36 ch 10 -> tap");

    MidiBinding arm;
    arm.kind = MidiEvent::Kind::ControlChange;
    arm.number = 21;
    arm.channel = 1;
    arm.target = takt4::control::ControlTarget(ControlAction::RuleEnable, "drop");
    CHECK(takt4::control::formatMidiBinding(arm) == "cc 21 ch 1 -> rule/drop/enable");
}

TEST_CASE("a line that is not a binding is refused rather than half-read", "[control][midi]") {
    using takt4::control::parseMidiBinding;
    // This reads a file a person may have edited, so every one of these is reachable.
    CHECK_FALSE(parseMidiBinding("").has_value());
    CHECK_FALSE(parseMidiBinding("note 36 ch 10").has_value());         // no action
    CHECK_FALSE(parseMidiBinding("note 36 ch 10 -> ").has_value());     // no verb
    CHECK_FALSE(parseMidiBinding("note 36 -> tap").has_value());        // no channel
    CHECK_FALSE(parseMidiBinding("pad 36 ch 10 -> tap").has_value());   // not a kind we know
    CHECK_FALSE(parseMidiBinding("note 300 ch 10 -> tap").has_value()); // out of range
    CHECK_FALSE(parseMidiBinding("note 36 ch 0 -> tap").has_value());   // channels are 1-16
    CHECK_FALSE(parseMidiBinding("note 36 ch 17 -> tap").has_value());
    CHECK_FALSE(parseMidiBinding("note x ch 10 -> tap").has_value());
    CHECK_FALSE(parseMidiBinding("note 36x ch 10 -> tap").has_value()); // trailing rubbish

    // `rule/<id>/enable` is the one verb with a hole in the middle, so its malformed shapes
    // are their own list. The last is the one worth having: `rule/` and `/enable` overlap
    // on it, so both halves match and there is still no rule being named.
    CHECK_FALSE(parseMidiBinding("note 36 ch 10 -> rule").has_value());
    CHECK_FALSE(parseMidiBinding("note 36 ch 10 -> rule/intro").has_value());
    CHECK_FALSE(parseMidiBinding("note 36 ch 10 -> intro/enable").has_value());
    CHECK_FALSE(parseMidiBinding("note 36 ch 10 -> rule/intro/disable").has_value());
    CHECK_FALSE(parseMidiBinding("note 36 ch 10 -> rule//enable").has_value());
    CHECK_FALSE(parseMidiBinding("note 36 ch 10 -> rule/enable").has_value());
    CHECK_FALSE(parseMidiBinding("note 36 ch 10 -> rule/x/enable/enable").has_value());

    // Whitespace is forgiven, because a person typed it.
    CHECK(parseMidiBinding("  cc 7 ch 1  ->  downbeat  ").has_value());
}

TEST_CASE("what a control hands an action that wants a 0 or a 1", "[control][midi]") {
    MidiEvent press;
    press.kind = MidiEvent::Kind::Note;
    press.value = 1; // even the softest press is a press
    CHECK(takt4::control::argumentOf(press) == 1.0);

    // A CC is a position, and 64 is where every hardware switch already puts its
    // threshold — so a sustain pedal or a toggle button works with no configuration.
    MidiEvent knob;
    knob.kind = MidiEvent::Kind::ControlChange;
    knob.value = 63;
    CHECK(takt4::control::argumentOf(knob) == 0.0);
    knob.value = 64;
    CHECK(takt4::control::argumentOf(knob) == 1.0);
}

TEST_CASE("the verbs a binding and an OSC address share", "[control]") {
    using takt4::control::ControlTarget;
    using takt4::control::targetOf;
    using takt4::control::verbFor;

    // One table, so the two surfaces cannot drift apart about what an action is called.
    for (const ControlAction action : takt4::control::kControlActions) {
        const ControlTarget target = takt4::control::takesRuleId(action)
                                         ? ControlTarget(action, "intro")
                                         : ControlTarget(action);
        const std::string verb = verbFor(target);
        INFO(verb);
        CHECK_FALSE(verb.empty());
        CHECK(targetOf(verb) == target);
        CHECK_FALSE(takt4::control::labelOf(action).empty());
    }
    CHECK_FALSE(targetOf("").has_value());
    CHECK_FALSE(targetOf("tap/").has_value());
    CHECK_FALSE(targetOf("preset").has_value()); // Q7's, and refused rather than half-done

    // §5.7's own spellings: `lock <0|1>` and `rule/<id>/enable <0|1>` are written with an
    // argument and `panic` is written bare, and that difference is the rule here. A panic
    // button that had to send a value would not be a panic button.
    CHECK(takt4::control::takesArgument(ControlAction::Lock));
    CHECK(takt4::control::takesArgument(ControlAction::RuleEnable));
    CHECK_FALSE(takt4::control::takesArgument(ControlAction::Panic));
    CHECK_FALSE(takt4::control::takesArgument(ControlAction::Tap));
    CHECK_FALSE(takt4::control::takesArgument(ControlAction::Downbeat));

    // Only `rule/<id>/enable` names something, and a UI's learn list turns on this being
    // the one it cannot arm from a gesture.
    CHECK(takt4::control::takesRuleId(ControlAction::RuleEnable));
    CHECK_FALSE(takt4::control::takesRuleId(ControlAction::Panic));
}
