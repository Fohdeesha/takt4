#include "ui/rules_controller.hpp"

#include "core/engine/beat_engine.hpp"
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/rule.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

using Catch::Approx;
using takt4::engine::BeatEngine;
using takt4::output::OutputRunner;
using takt4::output::Transports;
using takt4::trigger::GeneratorKind;
using takt4::trigger::Pool;
using takt4::trigger::Rule;
using takt4::trigger::Trigger;
using takt4::ui::RulesController;

namespace {

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

/// An engine and an output runner, which is everything a `RulesController` needs. Nothing
/// is started: the runner applies commands on the calling thread while it is stopped, which
/// is what lets a test read the rules back through `triggers()`.
struct Rig {
    std::unique_ptr<BeatEngine> engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner{*engine, Transports::Config{}};
};

/// Where the trigger names sit in the dropdown, which is `trigger::kTriggers`' order.
int indexOf(Trigger which) {
    for (std::size_t i = 0; i < takt4::trigger::kTriggers.size(); ++i) {
        if (takt4::trigger::kTriggers[i] == which) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

TEST_CASE("Phase 6's exit criterion, built by clicking", "[ui][trigger]") {
    // §8: "A rule that fires a non-repeating random clip on every fourth downbeat can be
    // built entirely by clicking, in under a minute, by someone who has not read the docs."
    //
    // Every step below is a window callback and nothing else — no `Rule::Config` is
    // assembled by hand. What it produces is then checked against what §5.6 and §5.8 say
    // that rule has to be.
    Rig rig;
    RulesController editor(rig.runner, {});
    REQUIRE(editor.rules().empty());

    // 1. A rule.
    editor.add();
    REQUIRE(editor.rules().size() == 1);
    CHECK(editor.selected() == 0);
    editor.rename("Random clip on downbeat");

    // 2. Every fourth bar.
    editor.pickTrigger(indexOf(Trigger::Bar));
    editor.setEvery(4);

    // 3. Resolume's clip address, from §5.6's own preset rather than typed.
    editor.pickHostPreset(1);

    // 4. The layer, which for Resolume is one number and not a sequence. A preset fills
    //    the address and leaves both chips on §5.8's default, so this is the operator
    //    saying "this one does not vary".
    const int fixedKind = 4;
    REQUIRE(takt4::trigger::kGeneratorKinds[fixedKind] == GeneratorKind::Fixed);
    editor.pickSlotKind(0, fixedKind);
    editor.setSlotFixed(0, "3");

    // 5. The clips, as a sequence the operator picked. Typing a list is itself the
    //    instruction to draw from one.
    editor.setSlotValues(1, "3, 7, 1, 12");

    // 6. And on, which is the last thing rather than the first: a new rule is built
    //    switched off so a half-finished one cannot fire into the rig.
    CHECK_FALSE(editor.rules().front().enabled);
    editor.setEnabled(true);

    const Rule::Config& built = editor.rules().front();
    CHECK(built.name == "Random clip on downbeat");
    CHECK(built.enabled);
    CHECK(built.trigger == Trigger::Bar);
    CHECK(built.every == 4);
    CHECK(built.address == "/composition/layers/{layer}/clips/{clip}/connect");
    REQUIRE(built.segments.size() == 2);

    // Non-repeating, which is §5.8's whole point about Shuffle: "every repeat reads to an
    // audience as a bug".
    CHECK(built.segments[1].kind == GeneratorKind::Shuffle);
    CHECK(built.segments[1].pool == Pool::List);
    REQUIRE(built.segments[1].values.size() == 4);
    CHECK(built.segments[1].values[0].asInt() == 3);
    CHECK(built.segments[1].values[3].asInt() == 12);

    // §7.4: connect takes 1 then 0, and a rule that sends only the 1 leaves the clip held.
    // The preset brings the release with it rather than leaving the trap to be found live.
    CHECK(built.followUp);
    CHECK(built.followUpValue.asInt() == 0);
    CHECK(built.value.fixed.asInt() == 1);

    // And it is a rule that will actually fire.
    CHECK(Rule(built).valid());
    REQUIRE(rig.runner.triggers().ruleCount() == 1);
    CHECK(rig.runner.triggers().rule(0).valid());

    SECTION("and what it sends is the sequence, in the order it was written") {
        // Through the real engine, from the real config: `test()` is §5.9's [test] button,
        // which fires past the trigger and past the conditions on purpose.
        for (int i = 0; i < 4; ++i) {
            editor.test();
        }
        const std::vector<OutputRunner::Fired> fired = rig.runner.takeFired();
        // Four fires, each with its release — §5.6's press-then-release. The releases are
        // queued for 50 ms and only leave on a later round, so what is here is the presses.
        REQUIRE(fired.size() >= 4);

        std::vector<std::string> presses;
        for (const OutputRunner::Fired& entry : fired) {
            if (entry.message.back() == '1') {
                presses.push_back(entry.message);
            }
        }
        REQUIRE(presses.size() == 4);
        // A bag of four, drawn without replacement: every clip exactly once, and the layer
        // fixed at 3 throughout.
        for (const int clip : {3, 7, 1, 12}) {
            const std::string expected =
                "/composition/layers/3/clips/" + std::to_string(clip) + "/connect 1";
            INFO(expected);
            CHECK(std::count(presses.begin(), presses.end(), expected) == 1);
        }
    }
}

TEST_CASE("three Resolume layers, from one pick", "[ui][trigger]") {
    // The ask this was built for, 2026-09-06: "send osc to resolume to randomly trigger
    // clips on all three resolume layers". One pick in the rig picker, and the three rules
    // that come out are checked against what 5.6 and 7.4 say they have to be.
    Rig rig;
    RulesController editor(rig.runner, {});

    editor.addRig(1); // "Resolume: clips on 3 layers"
    REQUIRE(editor.rules().size() == 3);

    for (int layer = 1; layer <= 3; ++layer) {
        const Rule::Config& rule = editor.rules()[static_cast<std::size_t>(layer - 1)];
        INFO("layer " << layer);
        CHECK(rule.address == "/composition/layers/{layer}/clips/{clip}/connect");
        REQUIRE(rule.segments.size() == 2);
        // The layer is fixed and the clip is shuffled, which is what "randomly trigger clips
        // on all three layers" means: three layers, each picking its own clip.
        CHECK(rule.segments[0].kind == GeneratorKind::Fixed);
        CHECK(rule.segments[0].fixed.asInt() == layer);
        CHECK(rule.segments[1].kind == GeneratorKind::Shuffle);
        // 7.4: connect is a mouse click, and without the release the clip stays held.
        CHECK(rule.followUp);
        CHECK(rule.followUpValue.asInt() == 0);
        // Off until the operator says so, like every rule the editor makes.
        CHECK_FALSE(rule.enabled);
        CHECK(Rule(rule).valid());
    }

    SECTION("with three seeds, or all three layers fire the same clip as each other") {
        CHECK(editor.rules()[0].seed != editor.rules()[1].seed);
        CHECK(editor.rules()[1].seed != editor.rules()[2].seed);
    }

    SECTION("and three periods, so the three do not all change on one downbeat") {
        // Three layers changing together is one event, not three.
        CHECK(editor.rules()[0].every != editor.rules()[1].every);
        CHECK(editor.rules()[1].every != editor.rules()[2].every);
    }

    SECTION("adding the same rig twice keeps both, with ids that stay unique") {
        // 5.7 addresses a rule by id and `TriggerEngine::find` takes the first of a pair,
        // so a duplicate id would make `/ctl/rule/<id>/enable` mean the wrong rule.
        editor.addRig(1);
        REQUIRE(editor.rules().size() == 6);
        std::set<std::string> ids;
        for (const Rule::Config& rule : editor.rules()) {
            ids.insert(rule.id);
        }
        CHECK(ids.size() == 6);
    }

    SECTION("and what they send is three layers of clips, over a real socket") {
        // End to end: the rules the preset built, fired through the engine, at a runner that
        // really sends. Nothing about this is a mock.
        for (const Rule::Config& rule : editor.rules()) {
            rig.runner.post(takt4::output::OutputCommand::testRule(rule.id));
        }
        const std::vector<takt4::output::OutputRunner::Fired> fired = rig.runner.takeFired();

        std::set<std::string> layers;
        for (const takt4::output::OutputRunner::Fired& entry : fired) {
            const std::size_t at = entry.message.find("/layers/");
            if (at != std::string::npos) {
                layers.insert(entry.message.substr(at + 8, 1));
            }
        }
        CHECK(layers == std::set<std::string>{"1", "2", "3"});
    }
}

TEST_CASE("the other rigs a pick builds", "[ui][trigger]") {
    Rig rig;
    RulesController editor(rig.runner, {});

    SECTION("Resolume's own tempo, from 5.6's verified addresses") {
        editor.addRig(2);
        REQUIRE(editor.rules().size() == 2);
        CHECK(editor.rules()[0].address == "/composition/tempocontroller/tempo");
        // 5.6 and A.4: "float, normalised 0-1 across 20-500 BPM", which is exactly what the
        // Live generator's BpmNormalised source exists for.
        CHECK(editor.rules()[0].value.kind == GeneratorKind::Live);
        CHECK(editor.rules()[0].value.source == takt4::trigger::LiveSource::BpmNormalised);
        CHECK(editor.rules()[0].value.normaliseLow == Approx(20.0));
        CHECK(editor.rules()[0].value.normaliseHigh == Approx(500.0));
        CHECK(editor.rules()[1].address == "/composition/tempocontroller/resync");
    }

    SECTION("a dashboard parameter that breathes rather than jumping") {
        editor.addRig(3);
        REQUIRE(editor.rules().size() == 1);
        const Rule::Config& rule = editor.rules()[0];
        CHECK(rule.value.kind == GeneratorKind::Ramp);
        CHECK(rule.value.rampFloat);
        // On every beat: a ramp is only as smooth as it is sampled.
        CHECK(rule.trigger == Trigger::Beat);
        CHECK(Rule(rule).valid());
    }

    SECTION("a euclidean pattern out to MIDI") {
        editor.addRig(4);
        REQUIRE(editor.rules().size() == 1);
        const Rule::Config& rule = editor.rules()[0];
        CHECK(rule.trigger == Trigger::Euclid);
        CHECK(rule.every == 8);
        CHECK(rule.pulses == 3); // the tresillo
        CHECK(rule.sendKind == takt4::trigger::Message::Kind::MidiNote);
        CHECK(Rule(rule).valid());
        // The pattern shows in the editor as something a person can read before it plays.
        editor.pick(0);
        CHECK(std::string(editor.window().get_euclid_pattern()) == "x..x..x.");
    }

    SECTION("and the picker's own label adds nothing") {
        editor.addRig(0);
        CHECK(editor.rules().empty());
    }
}

TEST_CASE("a rule is routed by naming outputs", "[ui][trigger]") {
    Rig rig;
    RulesController editor(rig.runner, {});
    std::vector<takt4::output::OutputTarget> targets(2);
    targets[0].name = "deck";
    targets[0].port = 7000;
    targets[1].name = "wall";
    targets[1].port = 7001;
    editor.setTargets(targets);

    editor.add();
    editor.setAddress("/fire");

    // Empty is everywhere, and says which everywhere is — an operator who has just added a
    // second output otherwise has no way to know the rule now reaches it too.
    CHECK(editor.rules()[0].outputs.empty());
    CHECK(std::string(editor.window().get_outputs_available()) == "every output: deck, wall");

    editor.setOutputs("deck, wall");
    CHECK(editor.rules()[0].outputs == std::vector<std::string>{"deck", "wall"});
    CHECK(std::string(editor.window().get_outputs_available()) == "reaches 2 outputs");

    SECTION("a name this rig does not have is kept, and said") {
        // A preset written where there was a "lights" output should still say "lights", so
        // that plugging it back in restores the routing rather than needing it retyped.
        editor.setOutputs("lights");
        CHECK(editor.rules()[0].outputs == std::vector<std::string>{"lights"});
        CHECK(std::string(editor.window().get_outputs_available()) == "no output called lights");
    }

    SECTION("clearing the field is back to everywhere") {
        editor.setOutputs("deck");
        editor.setOutputs("");
        CHECK(editor.rules()[0].outputs.empty());
    }
}

TEST_CASE("a list typed into the editor is the sequence that comes out", "[ui][trigger]") {
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/fire/{what}");
    REQUIRE(editor.rules().front().segments.size() == 1);

    SECTION("numbers stay numbers and names stay names") {
        // §5.8's generators produce `trigger::Value`, so a sequence can be of anything a
        // rule can send — which is what lets an address name its own sections.
        editor.setSlotValues(0, "intro, build, drop");
        const takt4::trigger::Generator::Config& config = editor.rules().front().segments[0];
        REQUIRE(config.values.size() == 3);
        CHECK(config.values[0].kind() == takt4::trigger::Value::Kind::Text);
        CHECK(config.values[0].text() == "intro");

        editor.setSlotValues(0, "3, 7, 1, 12");
        CHECK(editor.rules().front().segments[0].values[0].kind() ==
              takt4::trigger::Value::Kind::Int);
    }

    SECTION("it is written the way it was typed, so the field reads back") {
        editor.setSlotValues(0, "  3 ,7,  1 , 12 ");
        REQUIRE(editor.rules().front().segments[0].values.size() == 4);
        // Whitespace forgiven, because a person typed it.
        CHECK(editor.rules().front().segments[0].values[2].asInt() == 1);
    }

    SECTION("in order is a different rule from shuffled, and both are one click") {
        editor.setSlotValues(0, "3, 7, 1, 12");
        editor.pickSlotKind(0, 2); // Cycle
        CHECK(editor.rules().front().segments[0].kind == GeneratorKind::Cycle);
        CHECK(editor.rules().front().segments[0].pool == Pool::List);
    }

    SECTION("what the editor shows back is what the generator accepted") {
        // §5.8's clamp-never-refuse policy, made visible. A range typed backwards is a
        // range, not an error — and an operator who cannot see it was swapped has no way
        // to know it happened.
        editor.setSlotRange(0, "12 - 3");
        const SlotRow shown = *editor.window().get_slots()->row_data(0);
        CHECK(shown.low == 3);
        CHECK(shown.high == 12);

        // And a no-repeat wider than the pool can satisfy comes back cut to what can.
        editor.setSlotValues(0, "1, 2, 3");
        editor.setSlotNoRepeat(0, 40);
        CHECK(editor.window().get_slots()->row_data(0)->no_repeat == 2);
    }

    SECTION("switching a range to a list starts from the range that was there") {
        // Otherwise the operator ticks the box and their values vanish, which reads as the
        // control having lost them.
        editor.setSlotRange(0, "1 - 5");
        editor.setSlotPool(0, true);
        const std::vector<takt4::trigger::Value>& values =
            editor.rules().front().segments[0].values;
        REQUIRE(values.size() == 5);
        CHECK(values[0].asInt() == 1);
        CHECK(values[4].asInt() == 5);
    }
}

TEST_CASE("the address and its chips stay in step", "[ui][trigger]") {
    // §5.8's validity is mostly "one generator per placeholder", so an edit that adds a
    // `{}` should hand the operator a chip rather than a rule that refuses to fire.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();

    editor.setAddress("/a/{one}/b");
    CHECK(editor.rules().front().segments.size() == 1);
    CHECK(Rule(editor.rules().front()).valid());

    editor.setAddress("/a/{one}/b/{two}");
    CHECK(editor.rules().front().segments.size() == 2);
    CHECK(Rule(editor.rules().front()).valid());

    editor.setAddress("/a/b");
    CHECK(editor.rules().front().segments.empty());
    CHECK(Rule(editor.rules().front()).valid());
}

TEST_CASE("the rule list is edited by clicking too", "[ui][trigger]") {
    Rig rig;
    RulesController editor(rig.runner, {});

    editor.add();
    editor.rename("first");
    editor.add();
    editor.rename("second");
    REQUIRE(editor.rules().size() == 2);
    CHECK(editor.selected() == 1);

    SECTION("ids are distinct, because §5.7 addresses a rule by one") {
        CHECK(editor.rules()[0].id != editor.rules()[1].id);
        // And each is usable as an OSC address segment, which is what `Rule` checks first.
        // Both rules are still invalid — a new one has no address — so this asks the
        // narrower question the ids are actually about.
        CHECK(Rule(editor.rules()[0]).problem().find("id") == std::string::npos);
        CHECK(Rule(editor.rules()[1]).problem().find("id") == std::string::npos);
    }

    SECTION("a new rule is switched off and says what it still needs") {
        // §5.8: an invalid rule is held and shown rather than refused. A brand-new one is
        // exactly that state, and the list's own dot is where it shows.
        CHECK_FALSE(editor.rules()[0].enabled);
        const Rule fresh(editor.rules()[0]);
        CHECK_FALSE(fresh.valid());
        CHECK(fresh.problem() == "an OSC rule needs an address");
    }

    SECTION("seeds are distinct, or every rule fires the same clip as every other") {
        CHECK(editor.rules()[0].seed != editor.rules()[1].seed);
    }

    SECTION("a copy is a different rule, not the same one twice") {
        editor.pick(0);
        editor.duplicate();
        REQUIRE(editor.rules().size() == 3);
        CHECK(editor.selected() == 1); // the copy, ready to edit
        CHECK(editor.rules()[1].id != editor.rules()[0].id);
        CHECK(editor.rules()[1].seed != editor.rules()[0].seed);
        CHECK(editor.rules()[1].name == "first copy");
    }

    SECTION("deleting keeps a selection where it can") {
        editor.remove();
        REQUIRE(editor.rules().size() == 1);
        CHECK(editor.selected() == 0);
        CHECK(editor.rules()[0].name == "first");

        editor.remove();
        CHECK(editor.rules().empty());
        CHECK(editor.selected() == -1);
        // And every edit is a no-op rather than a crash with nothing selected.
        editor.rename("nobody");
        editor.setEvery(4);
        editor.setSlotValues(0, "1, 2");
        CHECK(editor.rules().empty());
    }

    SECTION("what the editor has is what the output thread has") {
        CHECK(rig.runner.triggers().ruleCount() == 2);
        editor.remove();
        CHECK(rig.runner.triggers().ruleCount() == 1);
    }
}

TEST_CASE("the conditions an operator types are read leniently", "[ui][trigger]") {
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();

    SECTION("a BPM range, however it is written") {
        for (const char* text : {"120 - 140", "120-140", "120 to 140", "140 - 120", " 120  140 "}) {
            INFO(text);
            editor.setBpmRange(text);
            CHECK(editor.rules().front().conditions.minBpm == Approx(120.0));
            CHECK(editor.rules().front().conditions.maxBpm == Approx(140.0));
        }
    }

    SECTION("and cleared back to any") {
        editor.setBpmRange("120 - 140");
        editor.setBpmRange("any");
        CHECK(editor.rules().front().conditions.minBpm == Approx(0.0));
        CHECK(editor.rules().front().conditions.maxBpm == Approx(1000.0));
    }

    SECTION("something that is not a range is refused rather than half-applied") {
        editor.setBpmRange("120 - 140");
        editor.setBpmRange("fast");
        CHECK(editor.rules().front().conditions.minBpm == Approx(120.0));
        CHECK(editor.window().get_status_is_error());
    }

    SECTION("milliseconds on screen, seconds in the rule") {
        editor.setCooldownMs(500);
        CHECK(editor.rules().front().conditions.cooldownSeconds == Approx(0.5));
        editor.setFollowUpMs(50);
        CHECK(editor.rules().front().followUpDelaySeconds == Approx(0.05));
    }

    SECTION("intensity is a set, indexed by §5.6's own wire numbers") {
        editor.setIntensity(0, false);
        CHECK_FALSE(editor.rules().front().conditions.allows(takt4::features::Intensity::Calm));
        CHECK(editor.rules().front().conditions.allows(takt4::features::Intensity::Normal));
    }
}

TEST_CASE("PANIC is reachable from the editor and latches", "[ui][trigger]") {
    // §5.8: "reachable from the UI, a keyboard shortcut, OSC and MIDI. Non-negotiable for
    // live use." This is the UI, and it is a latch rather than a flush.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/fire");

    CHECK_FALSE(rig.runner.panicked());
    editor.panic();
    CHECK(rig.runner.panicked());
    CHECK(editor.window().get_panicked());

    editor.panic();
    CHECK_FALSE(rig.runner.panicked());
}

TEST_CASE("what fired reaches the editor's log and its last-fired line", "[ui][trigger]") {
    // §5.9: "the last-fired line on each rule card, showing the actually-sent message with a
    // timestamp ... That single line turns a config screen into an instrument."
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/composition/layers/3/clips/{clip}/connect");
    editor.setSlotValues(0, "7");

    editor.test();
    editor.tick();

    const std::string fired(editor.window().get_last_fired());
    INFO(fired);
    // The message as it went out, with the placeholder filled in — not the template.
    CHECK(fired.find("/composition/layers/3/clips/7/connect") != std::string::npos);
    CHECK_FALSE(std::string(editor.window().get_last_fired_ago()).empty());
    CHECK(editor.window().get_log()->row_count() > 0);

    SECTION("and the log can be cleared without touching the rules") {
        editor.clearLog();
        CHECK(editor.window().get_log()->row_count() == 0);
        CHECK(editor.rules().size() == 1);
    }
}

TEST_CASE("a preset load replaces what the editor is showing", "[ui][trigger]") {
    Rig rig;
    Rule::Config loaded;
    loaded.id = "from-a-file";
    loaded.name = "Loaded";
    loaded.address = "/loaded";

    RulesController editor(rig.runner, {loaded});
    REQUIRE(editor.rules().size() == 1);
    CHECK(editor.selected() == 0);
    CHECK(std::string(editor.window().get_rule_name()) == "Loaded");

    Rule::Config other;
    other.id = "later";
    other.name = "Later";
    editor.setRules({other});
    CHECK(editor.rules().size() == 1);
    CHECK(editor.rules()[0].id == "later");
    CHECK(std::string(editor.window().get_rule_name()) == "Later");
}
