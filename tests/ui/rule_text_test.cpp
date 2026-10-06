#include "core/dmx/effect.hpp"
#include "core/dmx/fixture.hpp"
#include "core/dmx/liberation.hpp"
#include "core/output/output_target.hpp"
#include "core/trigger/rule.hpp"
#include "core/trigger/value.hpp"
#include "ui/rule_presets.hpp"
#include "ui/rule_text.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The rule editor's words, on their own: what `rule_text` makes of what is typed into a box,
// and what it says back. These ran only through the window until they had a file of their own
// (the 2026-09-22 audit's suggested split), so the window tests are where the gestures are;
// these are the answers.

using Catch::Approx;
using takt4::output::OutputTarget;
using takt4::trigger::Message;
using takt4::trigger::Rule;
using takt4::trigger::Value;
namespace rule_text = takt4::ui::rule_text;
namespace rule_presets = takt4::ui::rule_presets;

namespace {

OutputTarget target(std::string id, OutputTarget::Kind kind, bool enabled = true) {
    OutputTarget out;
    out.id = id;
    out.name = "out " + id;
    out.kind = kind;
    out.enabled = enabled;
    return out;
}

} // namespace

TEST_CASE("rule text: a list typed into a box is split and trimmed", "[ui][trigger]") {
    CHECK(rule_text::split("3, 7,, 1 ,12", ",") ==
          std::vector<std::string_view>{"3", "7", "1", "12"});
    CHECK(rule_text::split(" 70 - 140 ", "-") == std::vector<std::string_view>{"70", "140"});
    CHECK(rule_text::split("   ", ",").empty());
    CHECK(rule_text::trim("\t intro \n") == "intro");
}

TEST_CASE("rule text: only a finite number is a number", "[ui][trigger]") {
    // The 2026-09-25 audit's L5: `from_chars` reads these as numbers, and each one broke a rule.
    CHECK_FALSE(rule_text::readNumber("nan"));
    CHECK_FALSE(rule_text::readNumber("inf"));
    CHECK_FALSE(rule_text::readNumber("-inf"));
    CHECK_FALSE(rule_text::readNumber(""));
    CHECK_FALSE(rule_text::readNumber("12x"));
    CHECK(rule_text::readNumber("12") == Approx(12.0));
    CHECK(rule_text::readNumber("-0.25") == Approx(-0.25));

    // And a finite one past an int's range is clamped rather than converted, which is
    // undefined behaviour.
    CHECK(rule_text::clampedInt(1e12) == std::numeric_limits<std::int32_t>::max());
    CHECK(rule_text::clampedInt(-1e12) == std::numeric_limits<std::int32_t>::min());
    CHECK(rule_text::clampedInt(7.9) == 7);

    // A number box's keystrokes: trimmed, rounded, nothing for anything else.
    CHECK(rule_text::typedNumber(" 12 ") == 12);
    CHECK(rule_text::typedNumber("2.6") == 3);
    CHECK_FALSE(rule_text::typedNumber("twelve"));
    CHECK_FALSE(rule_text::typedNumber("inf"));
}

TEST_CASE("rule text: an entry's type is how it is written", "[ui][trigger]") {
    CHECK(rule_text::parseValue("7") == Value::ofInt(7));
    CHECK(rule_text::parseValue(" -3 ") == Value::ofInt(-3));
    CHECK(rule_text::parseValue("0.5") == Value::ofFloat(0.5f));
    // The point is what asks for a float; a clip index has to come back as an int.
    CHECK(rule_text::parseValue("3.0").kind() == Value::Kind::Float);
    CHECK(rule_text::parseValue("1e3").kind() == Value::Kind::Float);
    // Too big for an int, and still a number.
    CHECK(rule_text::parseValue("99999999999").kind() == Value::Kind::Float);
    CHECK(rule_text::parseValue("intro") == Value::ofText("intro"));
    CHECK(rule_text::parseValue("true") == Value::ofBool(true));
    // "nan" is not a number here, so it is a name, which is what an address segment can be.
    CHECK(rule_text::parseValue("nan") == Value::ofText("nan"));
    // A float past a float's range is clamped to it, not converted (L5).
    const Value huge = rule_text::parseValue("1e300");
    REQUIRE(huge.kind() == Value::Kind::Float);
    CHECK(huge.asFloat() == std::numeric_limits<float>::max());

    SECTION("and a list comes back the way it was typed") {
        std::vector<Value> values;
        for (const std::string_view entry : rule_text::split("3, 7, intro, 0.5", ",")) {
            values.push_back(rule_text::parseValue(entry));
        }
        CHECK(rule_text::spellValues(values) == "3, 7, intro, 0.5");
    }
}

TEST_CASE("rule text: numbers are spelled without trailing zeroes", "[ui][trigger]") {
    CHECK(rule_text::spellNumber(2.0) == "2");
    CHECK(rule_text::spellNumber(-3.0) == "-3");
    CHECK(rule_text::spellNumber(0.25) == "0.25");
    CHECK(rule_text::spellNumber(1.5) == "1.5");
}

TEST_CASE("rule text: a rate says how often, not the multiplier", "[ui][trigger]") {
    // The multiplier is on the interval, so 2 is half as often.
    CHECK(rule_text::describeRate(1.0).empty());
    CHECK(rule_text::describeRate(2.0) == "2× slower");
    CHECK(rule_text::describeRate(0.5) == "2× faster");
    CHECK(rule_text::describeRate(0.25) == "4× faster");
}

TEST_CASE("rule text: a rule is offered only the outputs it can reach", "[ui][trigger]") {
    const OutputTarget osc = target("a", OutputTarget::Kind::Osc);
    const OutputTarget midi = target("b", OutputTarget::Kind::Midi);
    const OutputTarget artnet = target("c", OutputTarget::Kind::ArtNet);

    CHECK(rule_text::targetTakes(Message::Kind::Osc, osc));
    CHECK_FALSE(rule_text::targetTakes(Message::Kind::Osc, midi));
    CHECK(rule_text::targetTakes(Message::Kind::MidiNote, midi));
    CHECK(rule_text::targetTakes(Message::Kind::MidiCc, midi));
    CHECK_FALSE(rule_text::targetTakes(Message::Kind::MidiNote, osc));
    // Nothing routes to an Art-Net node by name, and a lighting rule picks fixtures instead.
    for (const Message::Kind kind :
         {Message::Kind::Osc, Message::Kind::MidiNote, Message::Kind::Dmx}) {
        CHECK_FALSE(rule_text::targetTakes(kind, artnet));
    }
    CHECK_FALSE(rule_text::targetTakes(Message::Kind::Dmx, osc));
}

TEST_CASE("rule text: the routing line says what a rule reaches", "[ui][trigger]") {
    const std::vector<OutputTarget> none;
    CHECK(rule_text::describeRouting(Message::Kind::Osc, {}, none) == "no outputs yet");

    const std::vector<OutputTarget> rig{target("a", OutputTarget::Kind::Osc),
                                        target("b", OutputTarget::Kind::Osc, false),
                                        target("m", OutputTarget::Kind::Midi)};
    CHECK(rule_text::describeRouting(Message::Kind::MidiNote, {}, {rig[0]}) ==
          "no MIDI output yet");

    // Everything, named, with the one switched off said rather than counted (L12).
    CHECK(rule_text::describeRouting(Message::Kind::Osc, {}, rig) ==
          "every output: out a; out b is switched off");
    CHECK(rule_text::describeRouting(Message::Kind::Osc, {"a"}, rig) == "reaches 1 output");
    CHECK(rule_text::describeRouting(Message::Kind::Osc, {"a", "b"}, rig) ==
          "reaches 1 output; out b is switched off");
    // Gone and the wrong kind are different mistakes, and said differently.
    CHECK(rule_text::describeRouting(Message::Kind::Osc, {"gone"}, rig) ==
          "1 output it was routed to is gone");
    CHECK(rule_text::describeRouting(Message::Kind::Osc, {"a", "m"}, rig) ==
          "reaches 1 output; out m is not OSC");
}

TEST_CASE("rule text: a release is spelled as what it sends", "[ui][trigger]") {
    CHECK(rule_text::releasedAs(Message::Kind::MidiNote) == Message::Kind::MidiNoteOff);
    CHECK(rule_text::releasedAs(Message::Kind::MidiCc) == Message::Kind::MidiCc);
    CHECK(rule_text::releasedAs(Message::Kind::Osc) == Message::Kind::Osc);
    CHECK(rule_text::releaseLabelOf(Message::Kind::MidiNote) == "release (same note)");
    CHECK(rule_text::releaseLabelOf(Message::Kind::Osc) == "release (same address)");
    CHECK(std::string(rule_text::numberLabelOf(Message::Kind::MidiProgramChange)) == "program");
    CHECK(std::string(rule_text::valueLabelOf(Message::Kind::MidiPitchBend)) == "bend");
}

TEST_CASE("rule text: a Euclidean rule is drawn as its pattern", "[ui][trigger]") {
    Rule::Config rule;
    rule.trigger = takt4::trigger::Trigger::Euclid;
    rule.every = 8;
    rule.pulses = 3;
    CHECK(rule_text::spellEuclid(rule) == "x..x..x.");

    rule.pulses = 0;
    CHECK(rule_text::spellEuclid(rule) == "........");

    // Past 64 steps a pattern is too long to read, and is said instead.
    rule.every = 65;
    rule.pulses = 3;
    CHECK(rule_text::spellEuclid(rule) == "3 in 65");

    // And a rule on any other trigger has no pattern at all.
    rule.trigger = takt4::trigger::Trigger::Beat;
    CHECK(rule_text::spellEuclid(rule).empty());
}

TEST_CASE("rule presets: the picker follows the address", "[ui][trigger]") {
    for (std::size_t i = 1; i < rule_presets::kHostPresets.size(); ++i) {
        CHECK(rule_presets::presetOf(rule_presets::kHostPresets[i].address) == static_cast<int>(i));
    }
    // An empty address is "custom", and so is one edited by a character.
    CHECK(rule_presets::presetOf("") == 0);
    CHECK(rule_presets::presetOf("/composition/tempocontroller/resync ") == 0);

    // Every rig preset adds rules, and the menu offers exactly as many as there are — but the
    // Liberation entry, which asks first and adds nothing by itself (`planLiberation`).
    for (std::size_t i = 1; i <= rule_presets::kRigPresets.size(); ++i) {
        INFO("preset " << i);
        CHECK(rule_presets::rigPresetRules(i).empty() == (i == rule_presets::kLiberationPreset));
    }
    CHECK(std::string_view(rule_presets::kRigPresets[rule_presets::kLiberationPreset - 1])
              .rfind("Liberation", 0) == 0);
    CHECK(rule_presets::rigPresetRules(0).empty());
    CHECK(rule_presets::rigPresetRules(rule_presets::kRigPresets.size() + 1).empty());
}

TEST_CASE("a folded section of the editor reads what it holds in one line", "[ui][trigger]") {
    // HANDOFF §0.5: each of A to D folds, and folded it still reports, in the words the open
    // section would be read as.
    using takt4::trigger::Trigger;

    SECTION("A, when") {
        Rule::Config rule;
        rule.trigger = Trigger::Bar;
        rule.every = 4;
        CHECK(rule_text::describeWhen(rule, 1.0) == "every 4 bars, counting from the first");
        rule.every = 1;
        CHECK(rule_text::describeWhen(rule, 1.0) == "every bar");
        rule.trigger = Trigger::Beat;
        rule.every = 2;
        // Laid on the bar, from the beat it names (2026-10-06).
        CHECK(rule_text::describeWhen(rule, 1.0) == "every 2 beats from beat 1 of the bar");
        rule.onBeat = 2;
        CHECK(rule_text::describeWhen(rule, 1.0) == "every 2 beats from beat 2 of the bar");
        // Slowed from a control surface: said where the trigger counts.
        CHECK(rule_text::describeWhen(rule, 16.0) ==
              "every 2 beats from beat 2 of the bar · 16× slower");
        // A bar on a beat of its own, and the wait in the unit it was set in.
        rule.trigger = Trigger::Bar;
        rule.every = 1;
        rule.onBeat = 3;
        CHECK(rule_text::describeWhen(rule, 1.0) == "every bar, on beat 3");
        rule.every = 2;
        CHECK(rule_text::describeWhen(rule, 1.0) == "every 2 bars, on beat 3, counting from the first");
        rule.delayOn = true;
        rule.delayUnit = takt4::trigger::DelayUnit::Beats;
        rule.delayBeats = 0.5;
        CHECK(rule_text::describeWhen(rule, 1.0) ==
              "every 2 bars, on beat 3, counting from the first · sent 0.5 beats later");
        rule.delayUnit = takt4::trigger::DelayUnit::Milliseconds;
        rule.delaySeconds = 0.12;
        CHECK(rule_text::describeWhen(rule, 1.0) ==
              "every 2 bars, on beat 3, counting from the first · sent 120 ms later");
        rule.delayUnit = takt4::trigger::DelayUnit::Bars;
        rule.delayBeats = 1.0;
        CHECK(rule_text::describeWhen(rule, 1.0) ==
              "every 2 bars, on beat 3, counting from the first · sent 1 bar later");
        rule.delayOn = false; // switched off, what it holds is not what it does
        rule.onBeat = 1;
        rule.trigger = Trigger::Beat;
        rule.every = 2;
        // A cooldown is said either way on a trigger that comes in bursts...
        rule.trigger = Trigger::Onset;
        CHECK(rule_text::describeWhen(rule, 1.0) == "onset · no limit");
        rule.cooldownSeconds = 0.25;
        CHECK(rule_text::describeWhen(rule, 1.0) == "onset · at most once every 250 ms");
        // ...and not at all on one that counts, which never reads it.
        rule.trigger = Trigger::Bar;
        rule.every = 4;
        CHECK(rule_text::describeWhen(rule, 1.0) == "every 4 bars, counting from the first");
    }

    SECTION("B, only if") {
        Rule::Config rule;
        CHECK(rule_text::describeOnlyIf(rule) == "off — fires every time A comes round");
        // Switched off, what it holds is not what it does.
        rule.conditions.minConfidence = 0.7;
        CHECK(rule_text::describeOnlyIf(rule) == "off — fires every time A comes round");
        rule.conditionsOn = true;
        rule.conditions.minConfidence = 0.0;
        CHECK(rule_text::describeOnlyIf(rule) ==
              "on — nothing set yet, so it fires every time A comes round");
        rule.conditions.minConfidence = 0.7;
        rule.conditions.probability = 0.9;
        rule.conditions.intensities = {false, true, true};
        rule.conditions.minBpm = 120.0;
        rule.conditions.maxBpm = 140.0;
        CHECK(rule_text::describeOnlyIf(rule) ==
              "confidence over 0.7 · 90% · normal, intense · 120 - 140 BPM");
        rule.conditions = {};
        rule.conditions.intensities = {false, false, false};
        CHECK(rule_text::describeOnlyIf(rule) == "no intensity ticked, so never");
    }

    SECTION("C, send") {
        const std::vector<OutputTarget> targets = {target("o-1", OutputTarget::Kind::Osc),
                                                   target("o-2", OutputTarget::Kind::Midi)};
        Rule::Config rule;
        rule.sendKind = Message::Kind::Osc;
        rule.address = "/composition/layers/1/clear";
        CHECK(rule_text::describeSend(rule, targets, {}) ==
              "OSC to every output · /composition/layers/1/clear");
        // By the names the rig has now, and a routing to something gone says so.
        rule.outputs = {"o-1", "o-9"};
        CHECK(rule_text::describeSend(rule, targets, {}) ==
              "OSC to out o-1, an output that is gone · /composition/layers/1/clear");
        rule.address.clear();
        CHECK(rule_text::describeSend(rule, targets, {}) ==
              "OSC to out o-1, an output that is gone · no address yet");
        rule.sendKind = Message::Kind::MidiNote;
        rule.channel = 10;
        rule.outputs = {"o-2"};
        CHECK(rule_text::describeSend(rule, targets, {}) == "MIDI note ch 10 to out o-2");

        takt4::dmx::Fixture heads;
        heads.id = "f-00000001";
        heads.name = "heads";
        rule.sendKind = Message::Kind::Dmx;
        rule.dmx.effect = takt4::dmx::EffectKind::Color;
        CHECK(rule_text::describeSend(rule, targets, {heads}) ==
              "color on no fixtures — it sends nowhere");
        rule.dmx.fixtures = {"f-00000001", "f-00000002"};
        CHECK(rule_text::describeSend(rule, targets, {heads}) ==
              "color on heads, a fixture that is gone");
    }

    SECTION("D, then send") {
        Rule::Config rule;
        CHECK(rule_text::describeThen(rule) == "nothing — this trigger sends once and is done");
        takt4::trigger::FollowUp release;
        release.unit = takt4::trigger::DelayUnit::Beats;
        release.delayBeats = 1.0;
        takt4::trigger::FollowUp cc;
        cc.kind = Message::Kind::MidiCc;
        cc.number = 21;
        cc.unit = takt4::trigger::DelayUnit::Bars;
        cc.delayBeats = 2.0;
        takt4::trigger::FollowUp quick;
        quick.delaySeconds = 0.05;
        rule.followUps = {release, cc, quick};
        CHECK(rule_text::describeThen(rule) ==
              "release after 1 beat · MIDI CC 21 after 2 bars · release after 50 ms");
    }
}

// --- the Liberation preset's plan -------------------------------------------------------------

namespace {

namespace liberation = takt4::dmx::liberation;
using rule_presets::LiberationAsk;
using rule_presets::LiberationPlan;
using rule_presets::planLiberation;

/// The fixture of `plan`'s patch a laser's rules aim at. A copy: a reference returned from a call
/// with a temporary among its arguments is a shape GCC's -Wdangling-reference flags.
takt4::dmx::Fixture zoneOf(const LiberationPlan& plan, std::size_t laser) {
    const takt4::dmx::Fixture* const zone =
        takt4::dmx::findFixture(plan.patch, plan.zones.at(laser));
    REQUIRE(zone != nullptr);
    return *zone;
}

} // namespace

TEST_CASE("the Liberation plan stacks a zone per laser as Liberation does",
          "[ui][trigger][liberation]") {
    LiberationAsk ask;
    ask.lasers = 4;
    const LiberationPlan plan = planLiberation(ask, {});
    REQUIRE(plan.problem.empty());
    REQUIRE(plan.zones.size() == 4);
    for (std::size_t laser = 0; laser < 4; ++laser) {
        const takt4::dmx::Fixture zone = zoneOf(plan, laser);
        CHECK(liberation::isZone(zone));
        CHECK(zone.universe == 0); // Liberation's universe 1
        CHECK(zone.address == 1 + 32 * laser);
        CHECK(zone.group == "lasers");
        CHECK(zone.name == "laser " + std::to_string(laser + 1));
    }
    CHECK(plan.where[0] == "universe 1 · 1-32");
    CHECK(plan.where[3] == "universe 1 · 97-128");
    CHECK(plan.clips[0] == "101 clips");

    // One clip rule each, aimed at its own zone, staggered as the operator asked: a beat, two
    // beats, a bar, two bars.
    REQUIRE(plan.rules.size() == 4);
    const std::array<std::pair<takt4::trigger::Trigger, std::uint32_t>, 4> stagger{{
        {takt4::trigger::Trigger::Beat, 1},
        {takt4::trigger::Trigger::Beat, 2},
        {takt4::trigger::Trigger::Bar, 1},
        {takt4::trigger::Trigger::Bar, 2},
    }};
    std::set<std::uint64_t> seeds;
    for (std::size_t laser = 0; laser < 4; ++laser) {
        const Rule::Config& rule = plan.rules[laser];
        INFO("laser " << laser + 1);
        CHECK(rule.sendKind == Message::Kind::Dmx);
        CHECK(rule.dmx.effect == takt4::dmx::EffectKind::Clip);
        CHECK(rule.dmx.fixtures == std::vector<std::string>{plan.zones[laser]});
        CHECK(rule.trigger == stagger[laser].first);
        CHECK(rule.every == stagger[laser].second);
        CHECK(rule.dmx.clip.kind == takt4::trigger::GeneratorKind::Shuffle);
        CHECK(rule.dmx.clip.low == liberation::indexOf({1, 1}));
        CHECK(rule.dmx.clip.high == liberation::indexOf({21, 1}));
        CHECK(Rule(rule).valid());
        seeds.insert(rule.seed);
    }
    CHECK(seeds.size() == 4); // or every laser would shuffle the same clips as every other

    // What to set up in Liberation, in its numbering.
    CHECK(plan.instructions.find("add 4 profiles, Extended 32ch, universe 1, addresses 1, 33, 65 "
                                 "and 97") != std::string::npos);
    CHECK(plan.instructions.find("Ableton Link") != std::string::npos);
}

TEST_CASE("the Liberation plan spills onto the next universe as Liberation does",
          "[ui][trigger][liberation]") {
    LiberationAsk ask;
    ask.lasers = 3;
    ask.universe = 2;
    ask.address = 449; // 449-480 fits, 481-512 fits, the third would cross 512
    const LiberationPlan plan = planLiberation(ask, {});
    REQUIRE(plan.problem.empty());
    CHECK(zoneOf(plan, 0).universe == 1);
    CHECK(zoneOf(plan, 0).address == 449);
    CHECK(zoneOf(plan, 1).address == 481);
    CHECK(zoneOf(plan, 2).universe == 2);
    CHECK(zoneOf(plan, 2).address == 1);
    CHECK(plan.where[2] == "universe 3 · 1-32");
    CHECK(plan.instructions.find("universe 2 address 449, universe 2 address 481 and universe 3 "
                                 "address 1") != std::string::npos);
}

TEST_CASE("the Liberation plan says what is wrong and adds nothing until it is put right",
          "[ui][trigger][liberation]") {
    SECTION("a clip that is not one") {
        LiberationAsk ask;
        ask.lasers = 2;
        ask.to[1] = "21-7";
        const LiberationPlan plan = planLiberation(ask, {});
        CHECK(plan.problem.find("laser 2") != std::string::npos);
        CHECK(plan.problem.find("\"21-7\" is not a clip") != std::string::npos);
        CHECK(plan.rules.empty());
        CHECK(plan.clips[1].empty());
    }
    SECTION("no address for Liberation") {
        LiberationAsk ask;
        ask.host = "  ";
        CHECK_FALSE(planLiberation(ask, {}).problem.empty());
    }
    SECTION("a fixture already on the channels") {
        takt4::dmx::Fixture par = takt4::dmx::fixtureFromMode("Bedroom RGB", 1, 0, 40);
        par.id = "f-par";
        LiberationAsk ask;
        ask.lasers = 2;
        const LiberationPlan plan = planLiberation(ask, {par});
        CHECK(plan.problem.find("laser 2") != std::string::npos);
        CHECK(plan.problem.find("Bedroom RGB") != std::string::npos);
        CHECK(plan.problem.find("channel 40") != std::string::npos);
        CHECK(plan.rules.empty());
        // Somewhere else, and it is fine.
        ask.universe = 2;
        CHECK(planLiberation(ask, {par}).problem.empty());
        // And switched off, it is in nobody's way.
        par.enabled = false;
        ask.universe = 1;
        CHECK(planLiberation(ask, {par}).problem.empty());
    }
    SECTION("a range typed backwards is turned round") {
        LiberationAsk ask;
        ask.from[0] = "21-1";
        ask.to[0] = "1-1";
        const LiberationPlan plan = planLiberation(ask, {});
        REQUIRE(plan.problem.empty());
        CHECK(plan.rules[0].dmx.clip.low == 6);
        CHECK(plan.rules[0].dmx.clip.high == 106);
    }
}

TEST_CASE("the Liberation plan uses the zones the patch already has", "[ui][trigger][liberation]") {
    takt4::dmx::Fixture mine = liberation::zone("left laser", 0, 33);
    mine.id = "f-mine";
    mine.enabled = false;
    LiberationAsk ask;
    ask.lasers = 2;
    const LiberationPlan plan = planLiberation(ask, {mine});
    REQUIRE(plan.problem.empty());
    CHECK(plan.reused == 1);
    CHECK(plan.patch.size() == 2); // the one it had, and laser 1's
    CHECK(plan.zones[1] == "f-mine");
    const takt4::dmx::Fixture reused = zoneOf(plan, 1);
    CHECK(reused.name == "left laser"); // the operator's name kept
    CHECK(reused.enabled);              // and switched on, since driving it is what was asked
}

TEST_CASE("the Liberation plan moves each laser within the amount asked",
          "[ui][trigger][liberation]") {
    LiberationAsk ask;
    ask.lasers = 2;
    ask.move = true;
    ask.amount = 40;
    ask.bars = 8;
    for (int shape = 0; shape < 4; ++shape) {
        ask.moveShape = shape;
        const LiberationPlan plan = planLiberation(ask, {});
        REQUIRE(plan.problem.empty());
        REQUIRE(plan.rules.size() == 4); // clips, move; clips, move
        for (std::size_t laser = 0; laser < 2; ++laser) {
            const Rule::Config& move = plan.rules[laser * 2 + 1];
            INFO("shape " << shape << ", laser " << laser + 1);
            CHECK(move.dmx.fixtures == std::vector<std::string>{plan.zones[laser]});
            CHECK(move.trigger == takt4::trigger::Trigger::Bar);
            CHECK(move.every == 8);
            CHECK(move.dmx.unit == takt4::trigger::DelayUnit::Bars);
            CHECK(move.dmx.durationBeats == 8.0);
            CHECK(move.dmx.effect ==
                  (shape == 3 ? takt4::dmx::EffectKind::Position : takt4::dmx::EffectKind::Path));
            CHECK(Rule(move).valid());
            // 40% of the way to the edge either side of the centre.
            const takt4::dmx::Fixture zone = zoneOf(plan, laser);
            CHECK(zone.panMin == Approx(0.3));
            CHECK(zone.panMax == Approx(0.7));
            CHECK(zone.tiltMin == Approx(0.3));
            CHECK(zone.tiltMax == Approx(0.7));
        }
    }
    // Not asked to move, the zones keep their whole window.
    ask.move = false;
    const LiberationPlan still = planLiberation(ask, {});
    CHECK(still.rules.size() == 2);
    CHECK(zoneOf(still, 0).panMin == 0.0);
    CHECK(zoneOf(still, 0).panMax == 1.0);
}
