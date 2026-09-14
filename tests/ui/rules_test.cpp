#include "core/engine/beat_engine.hpp"
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/rule_sink.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/random.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/rule.hpp"
#include "ui/model_watch.hpp"
#include "ui/rules_controller.hpp"
#include "ui/window_state.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
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
using takt4::tests::ModelWatch;

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

    // 6. And on — which it already is. A new rule arrives armed now: it is invalid until it
    //    has an address, so it cannot fire while it is half-built whatever this says, and
    //    every rule and every preset arriving inert behind an unlabelled tick box was the
    //    thing an operator could not see the reason for. Left as an explicit step because it
    //    is still the last question §5.9's editor asks.
    CHECK(editor.rules().front().enabled);
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
    REQUIRE(built.followUps.size() == 1);
    CHECK_FALSE(built.followUps.front().kind.has_value()); // a release: the same address again
    CHECK(built.followUps.front().value.asInt() == 0);
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

    // The menu offers the presets themselves and nothing else — it used to carry an "add a
    // preset..." entry at the front because a ComboBox has to sit on something. The window
    // adds the one back on (`rig-added(i + 1)`), so the two counts have to agree here or a
    // pick lands on the preset next to the one that was clicked.
    const auto offered = editor.window().get_rig_presets();
    REQUIRE(offered->row_count() == 4);
    CHECK(std::string(*offered->row_data(0)) == "Resolume: clips on 3 layers");

    editor.addRig(1); // "Resolume: clips on 3 layers"
    REQUIRE(editor.rules().size() == 3);

    // **Highest layer first**, because that is the way Resolume stacks them: 3 above 2 above
    // 1, so a list running 1, 2, 3 down the screen is the operator's rig upside down.
    // Reported on 2026-09-12 as "the order it adds them is backwards, which can be
    // confusing"; the layer numbers, the periods and the seeds are unchanged.
    for (int row = 0; row < 3; ++row) {
        const int layer = 3 - row;
        const Rule::Config& rule = editor.rules()[static_cast<std::size_t>(row)];
        INFO("row " << row << " should be layer " << layer);
        CHECK(rule.address == "/composition/layers/{layer}/clips/{clip}/connect");
        REQUIRE(rule.segments.size() == 2);
        // The layer is fixed and the clip is shuffled, which is what "randomly trigger clips
        // on all three layers" means: three layers, each picking its own clip.
        CHECK(rule.segments[0].kind == GeneratorKind::Fixed);
        CHECK(rule.segments[0].fixed.asInt() == layer);
        CHECK(rule.segments[1].kind == GeneratorKind::Shuffle);
        // 7.4: connect is a mouse click, and without the release the clip stays held.
        REQUIRE(rule.followUps.size() == 1);
        CHECK_FALSE(rule.followUps.front().kind.has_value());
        CHECK(rule.followUps.front().value.asInt() == 0);
        // Armed, like every rule the editor makes. A preset is a rig somebody asked for by
        // name; arriving switched off behind an unlabelled tick box was the state nobody
        // could see the reason for.
        CHECK(rule.enabled);
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

TEST_CASE("the routing is ticked from the rig's own list of outputs", "[ui][trigger]") {
    // It used to be a field of comma-separated names, which asks an operator to remember
    // what their outputs are called and to spell each the same way twice — and a typo there
    // is a rule that silently sends nowhere. The rig knows what it has; this offers them.
    Rig rig;
    RulesController editor(rig.runner, {});
    std::vector<takt4::output::OutputTarget> targets(2);
    targets[0].name = "deck";
    targets[1].name = "wall";
    editor.setTargets(targets);

    editor.add();
    editor.setAddress("/fire");

    const auto choices = [&editor] { return editor.window().get_output_choices(); };

    // A rule that names nothing reaches every output, and that is the state the list opens
    // in: the "every output" line ticked, and the outputs themselves not.
    CHECK(editor.window().get_outputs_all());
    CHECK(std::string(editor.window().get_outputs_summary()) == "every output");
    REQUIRE(choices()->row_count() == 2);
    CHECK(std::string(choices()->row_data(0)->name) == "deck");
    CHECK_FALSE(choices()->row_data(0)->chosen);
    CHECK_FALSE(choices()->row_data(1)->chosen);

    SECTION("ticking one routes the rule to it alone") {
        editor.setOutputChosen("deck", true);
        CHECK(editor.rules()[0].outputs == std::vector<std::string>{"deck"});
        CHECK_FALSE(editor.window().get_outputs_all());
        CHECK(std::string(editor.window().get_outputs_summary()) == "deck");
        CHECK(choices()->row_data(0)->chosen);
        CHECK_FALSE(choices()->row_data(1)->chosen);

        // And more than one, which is the whole reason this is not a ComboBox.
        editor.setOutputChosen("wall", true);
        CHECK(editor.rules()[0].outputs == std::vector<std::string>{"deck", "wall"});
        CHECK(std::string(editor.window().get_outputs_summary()) == "deck, wall");
    }

    SECTION("ticking the same one twice does not name it twice") {
        editor.setOutputChosen("deck", true);
        editor.setOutputChosen("deck", true);
        CHECK(editor.rules()[0].outputs == std::vector<std::string>{"deck"});
    }

    SECTION("unticking the last one is every output again") {
        // An empty list *is* everywhere (`output::resolveOutputs`), so there is no third
        // state to get wrong: the two cannot disagree because there is only one of them.
        editor.setOutputChosen("deck", true);
        editor.setOutputChosen("deck", false);
        CHECK(editor.rules()[0].outputs.empty());
        CHECK(editor.window().get_outputs_all());
        CHECK(std::string(editor.window().get_outputs_summary()) == "every output");
    }

    SECTION("and every output is one click from anywhere") {
        editor.setOutputChosen("wall", true);
        editor.chooseAllOutputs();
        CHECK(editor.rules()[0].outputs.empty());
        CHECK(editor.window().get_outputs_all());
    }

    SECTION("a name this rig has not got is listed, marked, and can only be taken off") {
        // A preset written where there was a "lights" output. It cannot be typed back — the
        // list is the rig's — so it has to be visible while it is still named.
        editor.setOutputs("lights");
        REQUIRE(choices()->row_count() == 3);
        CHECK(std::string(choices()->row_data(2)->name) == "lights");
        CHECK(choices()->row_data(2)->chosen);
        CHECK(choices()->row_data(2)->missing);
        CHECK_FALSE(choices()->row_data(0)->missing);

        editor.setOutputChosen("lights", false);
        CHECK(editor.rules()[0].outputs.empty());
        CHECK(choices()->row_count() == 2);
    }

    SECTION("an output added to the rig appears in the list") {
        targets.emplace_back();
        targets[2].name = "haze";
        editor.setTargets(targets);
        REQUIRE(choices()->row_count() == 3);
        CHECK(std::string(choices()->row_data(2)->name) == "haze");
    }

    SECTION("and the list follows the selected rule") {
        editor.setOutputChosen("wall", true);
        editor.add();
        CHECK(editor.window().get_outputs_all());
        editor.pick(0);
        CHECK(std::string(editor.window().get_outputs_summary()) == "wall");
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

TEST_CASE("the host preset picker says what the rule is, and custom empties it",
          "[ui][trigger]") {
    // Reported from a rig on 2026-09-06: pick the Resolume preset, change the picker back to
    // "custom", and the Resolume address, its two chips and its press-then-release are all
    // still there under a box that says "custom". Both halves of that were wrong — the box
    // never followed the rule, and "custom" did nothing.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();

    editor.pickHostPreset(1); // Resolume 7 — clip
    REQUIRE(editor.rules().front().address ==
            "/composition/layers/{layer}/clips/{clip}/connect");
    REQUIRE(editor.rules().front().segments.size() == 2);
    CHECK(editor.rules().front().followUps.size() == 1);
    // The picker names the preset the address is, rather than whatever was last clicked.
    CHECK(editor.window().get_host_preset_index() == 1);

    SECTION("custom clears the address, its chips and the host's release") {
        editor.pickHostPreset(0);
        CHECK(editor.rules().front().address.empty());
        CHECK(editor.rules().front().segments.empty());
        // §7.4's press-then-release is a fact about Resolume's `connect`, not about OSC.
        CHECK(editor.rules().front().followUps.empty());
        CHECK(editor.window().get_host_preset_index() == 0);
        CHECK(std::string(editor.window().get_address()).empty());
        // And it says so, on a status line that until now nothing drew.
        CHECK(std::string(editor.window().get_status()).find("Cleared") != std::string::npos);
        CHECK_FALSE(editor.window().get_status_is_error());
    }

    SECTION("editing one character of a preset's address is no longer that preset") {
        editor.setAddress("/composition/layers/{layer}/clips/{clip}/select");
        CHECK(editor.window().get_host_preset_index() == 0);
        // What the operator typed is kept; only the picker's claim about it changed.
        CHECK(editor.rules().front().address ==
              "/composition/layers/{layer}/clips/{clip}/select");
        CHECK(editor.rules().front().segments.size() == 2);
    }

    SECTION("and selecting another rule brings that rule's answer, not this one's") {
        editor.add();
        CHECK(editor.window().get_host_preset_index() == 0);
        editor.pick(0);
        CHECK(editor.window().get_host_preset_index() == 1);
    }

    SECTION("the next edit that works takes the status line back down") {
        editor.pickHostPreset(0);
        REQUIRE(!std::string(editor.window().get_status()).empty());
        editor.rename("something");
        CHECK(std::string(editor.window().get_status()).empty());
    }
}

TEST_CASE("a new rule arrives with a name, numbered", "[ui][trigger]") {
    // A list of "(unnamed)" rows is a list nobody can read, and naming a rule is a step an
    // operator building one will skip. The number at least says which of them this is.
    Rig rig;
    RulesController editor(rig.runner, {});

    editor.add();
    editor.add();
    editor.add();
    REQUIRE(editor.rules().size() == 3);
    CHECK(editor.rules()[0].name == "Trigger #1");
    CHECK(editor.rules()[1].name == "Trigger #2");
    CHECK(editor.rules()[2].name == "Trigger #3");

    SECTION("and the number is not one already taken") {
        // Deleting from the middle would otherwise make two rules called the same thing,
        // since the next number comes from how many there are.
        editor.pick(1);
        editor.remove();
        editor.add();
        REQUIRE(editor.rules().size() == 3);
        CHECK(editor.rules()[2].name == "Trigger #4");
    }
}

TEST_CASE("several rules are chosen at once, and acted on at once", "[ui][trigger]") {
    // Asked for on 2026-09-08: "you should be able to ctrl click or shift click in this
    // trigger list to duplicate or kill multiple of them at once".
    Rig rig;
    RulesController editor(rig.runner, {});
    for (int i = 0; i < 5; ++i) {
        editor.add();
    }
    REQUIRE(editor.rules().size() == 5);

    SECTION("a plain click is one row, as it always was") {
        editor.pick(2);
        CHECK(editor.chosen() == std::vector<int>{2});
        CHECK(editor.selected() == 2);
    }

    SECTION("control adds and removes one row at a time") {
        editor.pick(1);
        editor.pickWith(3, true, false);
        editor.pickWith(4, true, false);
        CHECK(editor.chosen() == std::vector<int>{1, 3, 4});
        // The editor follows the row that was clicked, whichever way the click went.
        CHECK(editor.selected() == 4);
        editor.pickWith(3, true, false);
        CHECK(editor.chosen() == std::vector<int>{1, 4});
    }

    SECTION("control never empties the selection") {
        // With nothing chosen there is no rule for the editor to show and none for the marks
        // to act on, so the last one stays.
        editor.pick(2);
        editor.pickWith(2, true, false);
        CHECK(editor.chosen() == std::vector<int>{2});
    }

    SECTION("shift takes the run from the anchor, and the anchor stays put") {
        editor.pick(1);
        editor.pickWith(3, false, true);
        CHECK(editor.chosen() == std::vector<int>{1, 2, 3});
        // Again from the same anchor rather than from where the last shift-click landed, so
        // the run shrinks instead of walking off down the list.
        editor.pickWith(2, false, true);
        CHECK(editor.chosen() == std::vector<int>{1, 2});
        // And backwards over the anchor.
        editor.pickWith(0, false, true);
        CHECK(editor.chosen() == std::vector<int>{0, 1});
    }

    SECTION("deleting takes every chosen rule") {
        editor.pick(1);
        editor.pickWith(3, false, true);
        const std::string kept = editor.rules()[4].id;
        editor.remove();
        REQUIRE(editor.rules().size() == 2);
        CHECK(editor.rules()[1].id == kept);
        // The row that moved up into the first hole, which is where the eye is.
        CHECK(editor.selected() == 1);
        CHECK(editor.chosen() == std::vector<int>{1});
    }

    SECTION("duplicating takes every chosen rule, in order, after the last of them") {
        editor.pick(0);
        editor.pickWith(1, false, true);
        const std::string first = editor.rules()[0].id;
        const std::string second = editor.rules()[1].id;
        editor.duplicate();
        REQUIRE(editor.rules().size() == 7);
        CHECK(editor.rules()[2].id == first + "-copy");
        CHECK(editor.rules()[3].id == second + "-copy");
        // Distinct ids, or §5.7's /ctl/rule/<id>/enable is ambiguous — including between two
        // copies made in the same gesture.
        CHECK(editor.rules()[2].id != editor.rules()[3].id);
        // The copies are what the operator is looking at now.
        CHECK(editor.chosen() == std::vector<int>{2, 3});
    }

    SECTION("a mark on a row outside the selection acts on that row alone") {
        editor.pick(0);
        editor.pickWith(1, false, true);
        REQUIRE(editor.chosen().size() == 2);
        editor.removeAt(4);
        CHECK(editor.rules().size() == 4); // one gone, not three
    }

    SECTION("a mark on a row inside the selection acts on all of it") {
        editor.pick(0);
        editor.pickWith(2, false, true);
        editor.removeAt(1);
        CHECK(editor.rules().size() == 2);
    }

    SECTION("a preset arrives selected whole, so it can be undone in one gesture") {
        editor.addRig(1); // three Resolume layers
        REQUIRE(editor.rules().size() == 8);
        CHECK(editor.chosen() == std::vector<int>{5, 6, 7});
        editor.remove();
        CHECK(editor.rules().size() == 5);
    }
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

    SECTION("a new rule is armed, named, and says what it still needs") {
        // §5.8: an invalid rule is held and shown rather than refused. A brand-new one is
        // exactly that state, and the list's own dot is where it shows — which is why being
        // armed costs nothing: it cannot fire until it has an address.
        CHECK(editor.rules()[0].enabled);
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
        editor.setCooldown("500");
        CHECK(editor.rules().front().conditions.cooldownSeconds == Approx(0.5));
        // The same for a follow-up's own delay, which is two numbers rather than one — see
        // `trigger::FollowUp::delayBeats` — so the unit is picked before the number is typed.
        editor.addFollowUp();
        editor.pickFollowUnit(0, 0); // milliseconds
        editor.setFollowDelay(0, "50");
        REQUIRE(editor.rules().front().followUps.size() == 1);
        CHECK(editor.rules().front().followUps.front().delaySeconds == Approx(0.05));
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

TEST_CASE("a rule firing does not rebuild the boxes being typed into", "[ui][trigger]") {
    // Reported from a set: *"editing triggers, like the text input boxes etc, was almost
    // impossible, every beat of the music, it would kick me out of the edit box"*.
    //
    // `tick` runs on the redraw timer and republishes the rules and their generator slots
    // every time a rule fires — which, for a rule on beats, is every beat. It did that with
    // `set_vector`, which resets the model, which makes the repeater destroy and rebuild
    // every row. The generator chips are rows, and they are full of text boxes.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/composition/layers/3/clips/{clip}/connect");
    editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Shuffle));
    editor.setSlotRange(0, "1 - 8");

    // Settle first, so what is counted is the steady state a set is actually in and not the
    // one-off publishing that building the rule caused.
    editor.test();
    editor.tick();

    const auto slots = editor.window().get_slots();
    const auto rules = editor.window().get_rules();
    // The `{clip}` placeholder and the value beside it — two chips, both editable.
    REQUIRE(slots->row_count() == 2);
    const auto slotWatch = std::make_shared<ModelWatch>();
    const auto ruleWatch = std::make_shared<ModelWatch>();
    slots->attach_peer(slotWatch);
    rules->attach_peer(ruleWatch);
    const auto chipBefore = *slots->row_data(0);

    // A couple of bars' worth, watching what the chip's readout says as it goes.
    std::set<std::string> readouts;
    for (int beat = 0; beat < 16; ++beat) {
        editor.test();
        editor.tick();
        readouts.insert(std::string(slots->row_data(0)->last));
    }

    // **Not one rebuild, on either list.** This is the assertion that would have caught the
    // bug. A reset makes the repeater destroy its items and build new ones, and a new item
    // is a new element tree — which is what took the keyboard away from whichever box had
    // it. A row *changing* leaves the element alone, and Slint drops a `text:` binding the
    // moment somebody types into the box, so a half-typed field is not overwritten either.
    CHECK(slotWatch->resets == 0);
    CHECK(ruleWatch->resets == 0);
    CHECK(slotWatch->added == 0);
    CHECK(slotWatch->removed == 0);

    // What the rows are *allowed* to carry: both lists have a live readout on them now — the
    // fire count on a rule, the last value on a chip — so they do get written, which is the
    // point of them. Every field an operator can edit has to be untouched.
    CHECK(slotWatch->changes > 0);
    CHECK(ruleWatch->changes > 0);

    const auto chipAfter = *slots->row_data(0);
    CHECK(chipAfter.kind_index == chipBefore.kind_index);
    CHECK(chipAfter.low == chipBefore.low);
    CHECK(chipAfter.high == chipBefore.high);
    CHECK(chipAfter.no_repeat == chipBefore.no_repeat);
    CHECK(std::string(chipAfter.values) == std::string(chipBefore.values));
    CHECK(std::string(chipAfter.fixed) == std::string(chipBefore.fixed));
    CHECK(chipAfter.ramp_bars == chipBefore.ramp_bars);

    // ...and the one field that moved is the readout, which really is following the draws:
    // a shuffle over 1-8 does not hand back the same number sixteen times.
    CHECK_FALSE(std::string(chipAfter.last).empty());
    CHECK(readouts.size() > 1);

    SECTION("and a rule really added still reaches the list") {
        editor.add();
        CHECK(ruleWatch->resets == 0);
        CHECK(ruleWatch->added == 1);
        CHECK(rules->row_count() == 2);
    }

    SECTION("and a rule removed still leaves the list") {
        editor.remove();
        CHECK(ruleWatch->resets == 0);
        CHECK(ruleWatch->removed == 1);
        CHECK(rules->row_count() == 0);
    }
}

TEST_CASE("every card counts its own fires, and a release is not one", "[ui][trigger]") {
    // The count in the rule list was drawn from a vector nothing ever incremented, so it
    // read 0 forever — a number on screen that was not a number. The trap in wiring it up is
    // §7.4's press-and-release: a Resolume connect sends a 1 and then a 0, both of which
    // reach the fire observer, and counting the pair would say every clip rule had fired
    // twice as often as it had.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.addRig(1); // three layers, each a connect with a release
    REQUIRE(editor.rules().size() == 3);

    const auto rules = editor.window().get_rules();
    REQUIRE(rules->row_count() == 3);
    CHECK(rules->row_data(0)->fires == 0);

    // The first rule only, five times.
    editor.pick(0);
    for (int i = 0; i < 5; ++i) {
        editor.test();
        editor.tick();
    }

    CHECK(rules->row_data(0)->fires == 5);
    // And only its own: the other two cards have not been touched.
    CHECK(rules->row_data(1)->fires == 0);
    CHECK(rules->row_data(2)->fires == 0);

    // Whether a *release* is counted is settled where the releases actually come out — see
    // "a release reaches an observer marked as one" in tests/trigger. The runner applies
    // commands on the calling thread while it is stopped, so nothing here brings a round
    // round, and a follow-up sits in the engine's queue for the whole of this test.

    SECTION("and a count follows its rule when one above it is deleted") {
        // The counts used to be a vector indexed by row. Deleting a rule from the middle
        // shifted every count below it up one, so a card would inherit a number that
        // belonged to the rule that had just been removed.
        editor.pick(1);
        for (int i = 0; i < 3; ++i) {
            editor.test();
            editor.tick();
        }
        REQUIRE(rules->row_data(1)->fires == 3);

        editor.pick(0);
        editor.remove();
        REQUIRE(rules->row_count() == 2);
        // What was the second rule is now the first, and it brought its own three with it.
        CHECK(rules->row_data(0)->fires == 3);
        CHECK(rules->row_data(1)->fires == 0);

        // And the deleted rule's five did not go to whoever gets its id next. `add` numbers
        // ids from the set's size, so a rule added now is called what the deleted one was.
        editor.add();
        REQUIRE(rules->row_count() == 3);
        CHECK(rules->row_data(2)->fires == 0);
    }

    SECTION("and a preset load starts the counts again") {
        // A preset can reuse the ids of the set it replaces, so keeping the counts would
        // hand a card a number belonging to a rule that is no longer loaded.
        REQUIRE(rules->row_data(0)->fires == 5);
        std::vector<Rule::Config> fresh;
        Rule::Config one;
        one.id = "rule1"; // the same id the counted rule had
        one.name = "From a file";
        fresh.push_back(one);
        editor.setRules(std::move(fresh));
        REQUIRE(rules->row_count() == 1);
        CHECK(rules->row_data(0)->fires == 0);
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

TEST_CASE("the panes either side of the editor are dragged to size", "[ui]") {
    // The one thing about a splitter worth testing is whether it moves, and that cannot be
    // asked of the markup — only of a drag. So this is a real press, a real move and a real
    // release, dispatched into the window; nothing here calls the handler directly.
    //
    // No controller: the splitters are markup, and a `RulesController` wants an output
    // runner — a Link session and three sockets — which a question about a pane's width has
    // no business opening.
    auto window = RulesWindow::create();
    window->show();
    window->window().dispatch_scale_factor_change_event(1.0f);
    // The size `takt4_ui_tests`' headless platform reports; the layout is that size whatever
    // is dispatched, so asking for it is only saying so out loud.
    window->window().dispatch_resize_event(slint::LogicalSize({900.0f, 520.0f}));

    const auto drag = [&window](float from, float to) {
        window->window().dispatch_pointer_press_event(slint::LogicalPosition({from, 300.0f}),
                                                      slint::PointerEventButton::Left);
        window->window().dispatch_pointer_move_event(slint::LogicalPosition({to, 300.0f}));
        window->window().dispatch_pointer_release_event(slint::LogicalPosition({to, 300.0f}),
                                                        slint::PointerEventButton::Left);
    };

    SECTION("the rule list narrows when its divider is pulled left") {
        const float was = window->get_list_width();
        drag(was + 2.0f, was - 58.0f);
        CHECK(window->get_list_width() == Approx(was - 60.0f).margin(2.0f));
    }

    SECTION("and the log widens when the divider on its left is") {
        const float was = window->get_log_width();
        const float divider = 900.0f - was - 3.0f;
        drag(divider, divider - 60.0f);
        CHECK(window->get_log_width() == Approx(was + 60.0f).margin(2.0f));
    }

    SECTION("neither can be dragged away entirely — the divider is what drags it back") {
        const float was = window->get_list_width();
        drag(was + 2.0f, 0.0f);
        CHECK(window->get_list_width() >= 100.0f);
    }
}

TEST_CASE("a box the controller rewrote comes back bound", "[ui][trigger]") {
    // The other half of "a rule firing does not rebuild the boxes being typed into".
    //
    // Updating a row in place keeps the element — and keeps its *dead* binding with it,
    // because Slint drops a `text:` binding the moment somebody types into the box. Within
    // one edit that is right: what was typed is what is meant. Across an edit the controller
    // made *itself* — a range swapped back the right way round, a guard cut to what can be
    // satisfied, another rule's values — the box would go on showing the last thing anybody
    // typed anywhere, which is what a rig met as "when I edit the value range on trigger 1,
    // it changes all the triggers".
    //
    // So a surviving row whose content moved is rebuilt, and the rebuild is deferred to the
    // next `tick`: a publisher runs inside the callback of the very widget it would destroy.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/deck/{clip}");
    editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Shuffle));
    editor.setSlotRange(0, "1 - 8");
    editor.tick();

    const auto slots = editor.window().get_slots();
    REQUIRE(slots->row_count() == 2);
    const auto watch = std::make_shared<ModelWatch>();
    slots->attach_peer(watch);

    // A guard wider than the pool can satisfy. §5.8 clamps rather than refuses, so what the
    // rule holds is not what was typed — and the box has to be told, or it goes on offering
    // a number the generator is not using.
    editor.setSlotNoRepeat(0, 40);
    CHECK(slots->row_data(0)->no_repeat == 7); // one less than the eight distinct values
    CHECK(watch->resets == 0);                 // not from inside the callback that caused it
    editor.tick();
    CHECK(watch->resets > 0);

    SECTION("and so does picking another rule, whose chips are different boxes entirely") {
        editor.add();
        editor.tick();
        const auto second = std::make_shared<ModelWatch>();
        editor.window().get_slots()->attach_peer(second);
        editor.pick(0);
        editor.tick();
        CHECK(second->resets > 0);
        CHECK(editor.window().get_slots()->row_count() == 2);
    }
}

TEST_CASE("the BPM range and the cooldown follow the rule that is selected", "[ui][trigger]") {
    // Both boxes are two-way bound, so the *property* is the box: the controller spells the
    // text (`publishSelected`) and parses it back. A one-way binding died on the first
    // keystroke and left rule 1's range showing over every rule picked after it.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.add();

    editor.pick(0);
    editor.setBpmRange("120 - 140");
    editor.setCooldown("500");
    CHECK(std::string(editor.window().get_bpm_range()) == "120 - 140");
    CHECK(std::string(editor.window().get_cooldown_ms()) == "500");
    CHECK(editor.rules().front().conditions.minBpm == Approx(120.0));
    CHECK(editor.rules().front().conditions.cooldownSeconds == Approx(0.5));

    // The second rule has neither, and has to say so rather than showing the first's.
    editor.pick(1);
    CHECK(std::string(editor.window().get_bpm_range()) == "0 - 1000");
    CHECK(std::string(editor.window().get_cooldown_ms()) == "0");
    CHECK(editor.rules().back().conditions.cooldownSeconds == Approx(0.0));

    // And back, unchanged by the excursion.
    editor.pick(0);
    CHECK(std::string(editor.window().get_bpm_range()) == "120 - 140");
    CHECK(std::string(editor.window().get_cooldown_ms()) == "500");

    SECTION("a range given backwards is read the way round it was meant") {
        editor.setBpmRange("140 - 120");
        CHECK(editor.rules().front().conditions.minBpm == Approx(120.0));
        CHECK(editor.rules().front().conditions.maxBpm == Approx(140.0));
    }

    SECTION("and an empty box undoes it") {
        editor.setBpmRange("");
        CHECK(editor.rules().front().conditions.minBpm == Approx(0.0));
        CHECK(editor.rules().front().conditions.maxBpm == Approx(1000.0));
    }
}

TEST_CASE("the editor never shows a rule the marks would not act on", "[ui][trigger]") {
    // Control-clicking a chosen row takes it *out* of the selection. The editor used to stay
    // on it anyway, so the card on screen was a rule the × and copy marks would not touch:
    // press × on any lit row and the others go while the one being read stays. Found by the
    // soak test below, which is the only reason it is written down rather than reported.
    Rig rig;
    RulesController editor(rig.runner, {});
    for (int i = 0; i < 4; ++i) {
        editor.add();
    }
    REQUIRE(editor.rules().size() == 4);

    // Rows 1, 2 and 3 chosen, the editor on 3.
    editor.pick(1);
    editor.pickWith(2, /*control=*/true, false);
    editor.pickWith(3, /*control=*/true, false);
    REQUIRE(editor.chosen() == std::vector<int>{1, 2, 3});
    REQUIRE(editor.selected() == 3);

    // Take 3 back out. The selection is 1 and 2, so the editor has to be on one of them.
    editor.pickWith(3, /*control=*/true, false);
    CHECK(editor.chosen() == std::vector<int>{1, 2});
    CHECK(editor.selected() == 2); // the nearest still chosen, which is where the eye is

    SECTION("and a control-click that would empty the selection leaves that row selected") {
        editor.pick(0);
        REQUIRE(editor.chosen() == std::vector<int>{0});
        editor.pickWith(0, /*control=*/true, false);
        CHECK(editor.chosen() == std::vector<int>{0});
        CHECK(editor.selected() == 0);
    }

    SECTION("and the marks act on the whole selection, from any row in it") {
        editor.removeAt(1); // row 1 is chosen, so this is "delete 1 and 2"
        CHECK(editor.rules().size() == 2);
        CHECK(editor.selected() >= 0);
        CHECK(editor.chosen().size() == 1);
    }

    SECTION("and a mark on a row outside the selection acts on that row alone") {
        editor.removeAt(0); // row 0 is not chosen
        CHECK(editor.rules().size() == 3);
    }
}

TEST_CASE("the editor survives being hammered", "[ui][trigger]") {
    // A soak, seeded so a failure is reproducible. It is not looking for a particular bug —
    // it is looking for the class of them: an index kept across an edit that moved it, a
    // selection left pointing at a rule that no longer exists, a slot addressed on a rule
    // whose send kind changed underneath it. Every one of those is a crash rather than a
    // wrong answer, so the assertions are the invariants and the test is the exercise.
    Rig rig;
    RulesController editor(rig.runner, {});
    takt4::tracking::Xoshiro256pp random(20260913);

    const auto invariants = [&editor] {
        const std::vector<int> chosen = editor.chosen();
        const auto rules = static_cast<int>(editor.rules().size());
        if (rules == 0) {
            REQUIRE(editor.selected() == -1);
            REQUIRE(chosen.empty());
            return;
        }
        // Something is always selected, it is always in range, and it is always one of the
        // rows the marks would act on — see the test above.
        REQUIRE(editor.selected() >= 0);
        REQUIRE(editor.selected() < rules);
        REQUIRE_FALSE(chosen.empty());
        for (const int row : chosen) {
            REQUIRE(row >= 0);
            REQUIRE(row < rules);
        }
        REQUIRE(std::find(chosen.begin(), chosen.end(), editor.selected()) != chosen.end());
        // §5.7 addresses a rule by id, and `TriggerEngine::find` takes the first of a pair,
        // so the editor must never make two the same.
        std::set<std::string> ids;
        for (const auto& rule : editor.rules()) {
            ids.insert(rule.id);
        }
        REQUIRE(ids.size() == editor.rules().size());
    };

    for (int step = 0; step < 400; ++step) {
        const auto rules = static_cast<int>(editor.rules().size());
        const int row =
            rules == 0 ? 0 : static_cast<int>(random.bounded(static_cast<std::uint64_t>(rules)));
        switch (random.bounded(16)) {
        case 0:
            editor.add();
            break;
        case 1:
            editor.addRig(static_cast<int>(random.bounded(4)) + 1);
            break;
        case 2:
            editor.remove();
            break;
        case 3:
            editor.duplicate();
            break;
        case 4:
            editor.removeAt(row);
            break;
        case 5:
            editor.duplicateAt(row);
            break;
        case 6:
            editor.pickWith(row, random.bounded(2) != 0, random.bounded(2) != 0);
            break;
        case 7:
            editor.pickTrigger(static_cast<int>(random.bounded(takt4::trigger::kTriggers.size())));
            break;
        case 8:
            editor.pickSend(static_cast<int>(random.bounded(takt4::trigger::kMessageKinds.size())));
            break;
        case 9:
            editor.setAddress(random.bounded(2) != 0 ? "/deck/{a}/{b}" : "/deck/go");
            break;
        case 10:
            editor.pickSlotKind(
                static_cast<int>(random.bounded(3)),
                static_cast<int>(random.bounded(takt4::trigger::kGeneratorKinds.size())));
            break;
        case 11:
            editor.setSlotRange(static_cast<int>(random.bounded(3)), "9 - 2");
            break;
        case 12:
            editor.addFollowUp();
            break;
        case 13:
            editor.removeFollowUp(static_cast<int>(random.bounded(3)));
            break;
        case 14:
            editor.pickHostPreset(static_cast<int>(random.bounded(5)));
            break;
        default:
            editor.tick();
            break;
        }
        invariants();
    }

    // And it is still a working editor rather than merely an intact one.
    editor.tick();
    if (editor.rules().empty()) {
        editor.add();
    }
    editor.pick(0);
    editor.rename("after the storm");
    CHECK(editor.rules().front().name == "after the storm");
    CHECK(rig.runner.triggers().ruleCount() == editor.rules().size());
}

TEST_CASE("a trigger sends a note on and a real note off, from one rule", "[ui][trigger]") {
    // The rig's ask, built by clicking: *"I need a note on trigger to trigger Liberation
    // laser clips. when the beat is done, I need the option to send a note off. in the same
    // trigger"* — and then, exactly right, *"we should just replace the 'then send' machinery
    // with a more fleshed out second send box"*.
    //
    // A **real** 0x80. The velocity-zero convention is widely understood and not universal;
    // that controller holds its clip until a note off arrives, so the rig never released.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.pickSend(1); // MIDI note, in `kMessageKinds` order
    editor.setChannel(10);
    REQUIRE(editor.rules().front().sendKind == takt4::trigger::Message::Kind::MidiNote);

    // Slot 0 is the note number for a MIDI rule, slot 1 the velocity — `slotConfig`'s order.
    editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Fixed));
    editor.setSlotFixed(0, "36");
    editor.pickSlotKind(1, static_cast<int>(GeneratorKind::Fixed));
    editor.setSlotFixed(1, "110");

    // One follow-up: a release, which for a note is a note off on the note that fired.
    editor.addFollowUp();
    editor.pickFollowUnit(0, 0); // milliseconds
    editor.setFollowDelay(0, "1");
    REQUIRE(editor.rules().front().followUps.size() == 1);
    CHECK_FALSE(editor.rules().front().followUps.front().kind.has_value());

    // What the row says it will do, which is the line the editor added for the rig that read
    // the old tick box as making note on and note off exclusive.
    const auto follows = editor.window().get_follow_ups();
    REQUIRE(follows->row_count() == 1);
    CHECK(std::string(follows->row_data(0)->summary) == "note off, same note, ch 10");

    // And what really leaves. The runner has to be going for a follow-up to come due, since
    // it is a clock rather than a beat that pays it out.
    rig.runner.start();
    editor.test();
    std::vector<std::string> lines;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < until && lines.size() < 2) {
        editor.tick();
        const auto log = editor.window().get_log();
        lines.clear();
        for (std::size_t i = 0; i < log->row_count(); ++i) {
            lines.push_back(std::string(*log->row_data(i)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    rig.runner.stop();

    // Newest first, so the release is line 0 and the press line 1.
    REQUIRE(lines.size() >= 2);
    INFO("log: " << lines[0] << " | " << lines[1]);
    CHECK(lines[0].find("note off 36 ch 10") != std::string::npos);
    CHECK(lines[1].find("note on 36 ch 10 = 110") != std::string::npos);

    // The status byte the two put on the wire, which is the half a counter cannot see: a
    // rule can decide to release and still send 0x90.
    CHECK(takt4::output::midiStatusFor(takt4::trigger::Message::Kind::MidiNote, 10) == 0x99);
    CHECK(takt4::output::midiStatusFor(takt4::trigger::Message::Kind::MidiNoteOff, 10) == 0x89);
}

TEST_CASE("a shuffled note is let go of on the note that was drawn", "[ui][trigger]") {
    // The rig's second report, which is the first one's other half: *"its making me set a
    // static note number to send note off to. I need it to send a note off to whatever note it
    // just sent a note on to, which will change every trigger because its set to
    // shuffle/random."* The rule above sends a fixed 36, so it could not tell these apart.
    //
    // It was never missing — it is what "release" has always meant — but the editor let them
    // pick the explicit *MIDI note off* instead, which comes with a box demanding one number,
    // and nothing on that row said the number would be wrong. So this checks the behaviour
    // **and** the three labels that now point at it.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.pickSend(1); // MIDI note
    editor.setChannel(3);
    editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Shuffle));
    editor.setSlotRange(0, "73 - 104");
    editor.pickSlotKind(1, static_cast<int>(GeneratorKind::Fixed));
    editor.setSlotFixed(1, "127");

    editor.addFollowUp();
    editor.pickFollowUnit(0, 0); // milliseconds, so the release comes due without a tempo
    editor.setFollowDelay(0, "1");

    // The dropdown's first entry names what it inherits, rather than the bare "release" that
    // was read as some other gesture entirely.
    const auto kinds = editor.window().get_follow_kinds();
    REQUIRE(kinds->row_count() >= 1);
    CHECK(std::string(*kinds->row_data(0)) == "release (same note)");

    // And the row's own boxes say what they are. The value box held a bare 0 and nothing else:
    // *"theres an unlabled box next to it with 0 in the field? wtf is that?"* — it is the
    // release velocity, and now it says so.
    {
        const auto rows = editor.window().get_follow_ups();
        REQUIRE(rows->row_count() == 1);
        const auto row = *rows->row_data(0);
        CHECK_FALSE(row.takes_number); // a release has no number of its own — that is the point
        CHECK(row.takes_value);
        CHECK(std::string(row.value_label) == "velocity");
        CHECK(std::string(row.summary) == "note off, same note, ch 3");
    }

    // Picking the explicit note off instead is the trap that was reported. It is still allowed
    // — a second, deliberate note off is a real thing to want — but the row now says what it
    // will really do, beside the fixed number it is asking for.
    editor.pickFollowKind(0, 2); // "MIDI note off" in `followKinds_` order, past release
    {
        const auto rows = editor.window().get_follow_ups();
        REQUIRE(rows->row_count() == 1);
        const auto row = *rows->row_data(0);
        CHECK(row.takes_number);
        CHECK(std::string(row.number_label) == "note");
        CHECK(std::string(row.value_label) == "velocity");
        const std::string summary(row.summary);
        INFO("summary: " << summary);
        CHECK(summary.find("the trigger sends a different one each time") != std::string::npos);
        CHECK(summary.find("release") != std::string::npos);
    }
    editor.pickFollowKind(0, 0); // back to the release, which is what the rig wants
    REQUIRE_FALSE(editor.rules().front().followUps.front().kind.has_value());

    // Now the behaviour itself: fire it several times and check every release names the note
    // its own press drew. One fire proves nothing — a shuffle can draw the number a stale
    // field happened to hold.
    rig.runner.start();
    std::vector<std::string> lines;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    for (int i = 0; i < 6; ++i) {
        editor.test();
        while (std::chrono::steady_clock::now() < until &&
               lines.size() < static_cast<std::size_t>(i + 1) * 2) {
            editor.tick();
            const auto log = editor.window().get_log();
            lines.clear();
            for (std::size_t j = 0; j < log->row_count(); ++j) {
                lines.push_back(std::string(*log->row_data(j)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }
    rig.runner.stop();

    // The log is newest first, so walking it in pairs gives release then press.
    const auto noteIn = [](const std::string& line, const std::string& after) {
        const std::size_t at = line.find(after);
        if (at == std::string::npos) {
            return -1;
        }
        return std::atoi(line.c_str() + at + after.size());
    };
    REQUIRE(lines.size() >= 12);
    int drawn = 0;
    for (std::size_t i = 0; i + 1 < 12; i += 2) {
        const int off = noteIn(lines[i], "note off ");
        const int on = noteIn(lines[i + 1], "note on ");
        INFO("pair " << i << ": " << lines[i] << " | " << lines[i + 1]);
        REQUIRE(on >= 73);
        REQUIRE(on <= 104);
        CHECK(off == on); // the whole ask: let go of the note that was actually played
        drawn = drawn == 0 || drawn == on ? on : -1;
    }
    // And it really was shuffling, or the check above passes on a rule that sends one note.
    CHECK(drawn == -1);
}

TEST_CASE("what was typed is kept when the operator clicks away", "[ui][trigger]") {
    // **Driven by real pointer and key events**, not by calling the callback: what is under
    // test is the markup, and the markup is where the bug was.
    //
    // Every box here commits on Enter. That is the ordinary contract and it hides a trap: an
    // operator who types and then clicks something else has *finished*, not cancelled, and
    // until now what they typed sat in the box until the next publish wrote over it and the
    // edit was gone without a word. Reported alongside "when I hit enter, nothing happens, it
    // stays in edit mode", which is the same box seen from the other side.
    //
    // The cooldown box, because it holds one number: a digit typed at whatever position the
    // click leaves the caret in still makes a number, so the test does not depend on where
    // inside the box it landed.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    auto& window = editor.window();
    window.show();
    window.window().dispatch_scale_factor_change_event(1.0f);
    // The size `takt4_ui_tests`' headless platform reports, said out loud so the sweep below
    // is over the layout an operator would get.
    window.window().dispatch_resize_event(slint::LogicalSize({900.0f, 520.0f}));
    window.window().dispatch_window_active_changed_event(true);
    REQUIRE(std::string(window.get_cooldown_ms()) == "0");

    // **Found rather than written down.** A coordinate in this file would be a number that
    // rots the first time a row moves, and a test that then clicked on nothing would go on
    // passing for ever. This sweeps the conditions block until a typed digit lands in the
    // cooldown box, which is proof that box has the keyboard.
    std::string typed;
    for (float y = 250.0f; y < 345.0f && typed.empty(); y += 2.0f) {
        for (float x = 540.0f; x < 660.0f && typed.empty(); x += 20.0f) {
            const slint::LogicalPosition at({x, y});
            window.window().dispatch_pointer_move_event(at);
            window.window().dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
            window.window().dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
            window.window().dispatch_key_press_event(slint::SharedString("5"));
            window.window().dispatch_key_release_event(slint::SharedString("5"));
            if (std::string(window.get_cooldown_ms()) != "0") {
                typed = std::string(window.get_cooldown_ms());
            }
        }
    }
    INFO("the cooldown box never took a keystroke — the sweep found no text field");
    REQUIRE_FALSE(typed.empty());

    // The box has it and the **rule has not**: a box commits when the edit is finished, not
    // on every keystroke.
    CHECK(editor.rules().front().conditions.cooldownSeconds == Approx(0.0));

    // Tab away, which is finishing rather than cancelling.
    const slint::SharedString tab(std::string(1, '\t'));
    window.window().dispatch_key_press_event(tab);
    window.window().dispatch_key_release_event(tab);

    INFO("box held \"" << typed << "\"");
    CHECK(editor.rules().front().conditions.cooldownSeconds > 0.0);
    CHECK_FALSE(window.get_status_is_error());
}

TEST_CASE("the send-to ticks follow the rule that is selected", "[ui][trigger]") {
    // **Driven by real clicks**, because what is under test is a `CheckBox` losing its
    // binding — a fact about the markup and the toolkit rather than about the controller,
    // whose model rows were right the whole time.
    //
    // A tick box drops `checked: choice.chosen` the moment somebody clicks it, and the
    // routing popup was the one repeater no publisher rebuilt. So after routing one rule to
    // an output, every rule selected afterwards showed that output already ticked — and
    // clicking it to route *that* rule un-ticked it and did nothing at all. Measured: with
    // the rebuild taken out, the second rule below stays unrouted.
    Rig rig;
    RulesController editor(rig.runner, {});
    takt4::output::OutputTarget wall;
    wall.name = "wall";
    wall.host = "127.0.0.1";
    wall.port = 7000;
    editor.setTargets({wall});
    editor.add(); // rule 0
    editor.add(); // rule 1
    REQUIRE(editor.rules().size() == 2);

    auto& window = editor.window();
    window.show();
    window.window().dispatch_scale_factor_change_event(1.0f);
    window.window().dispatch_resize_event(
        slint::LogicalSize({takt4::ui::kRulesWindowWidth, takt4::ui::kRulesWindowHeight}));
    window.window().dispatch_window_active_changed_event(true);

    const auto click = [&window](float x, float y) {
        const slint::LogicalPosition at({x, y});
        window.window().dispatch_pointer_move_event(at);
        window.window().dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
        window.window().dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
    };

    // **Found rather than written down**, for the reason the cooldown sweep gives: a
    // coordinate in this file rots the first time a row moves, and a test that then clicked
    // on nothing would go on passing for ever.
    //
    // The control is a closed field that opens a popup, so a candidate is only the opener if
    // a click just below it lands on the popup's first row — "every output", which clears
    // the rule's routing and is what says both clicks landed. Anything already open is
    // closed first, or a stray click from the last candidate would be credited to this one.
    editor.pick(0);
    float openX = -1.0f;
    float openY = -1.0f;
    float everyOutputY = -1.0f;
    for (float y = 250.0f; y < 760.0f && everyOutputY < 0.0f; y += 4.0f) {
        for (float x = 150.0f; x < 420.0f && everyOutputY < 0.0f; x += 10.0f) {
            editor.setOutputs("wall"); // something for "every output" to clear
            click(700.0f, 40.0f);      // close whatever is open; outside every popup
            click(x, y);
            for (float row = y + 20.0f; row < y + 80.0f && everyOutputY < 0.0f; row += 3.0f) {
                click(x + 30.0f, row);
                if (editor.rules().front().outputs.empty()) {
                    openX = x;
                    openY = y;
                    everyOutputY = row;
                }
            }
        }
    }
    INFO("no pair of clicks reached the popup's \"every output\" row");
    REQUIRE(everyOutputY >= 0.0f);
    INFO("opener at " << openX << "," << openY << ", every-output row at " << everyOutputY);

    // The popup is still open — the last click was inside it — so the ticks are below the
    // separator under that row. The one for "wall" is the one that routes the rule.
    const float tickX = openX + 30.0f;
    float tickY = -1.0f;
    for (float row = everyOutputY + 4.0f; row < everyOutputY + 90.0f && tickY < 0.0f; row += 2.0f) {
        click(tickX, row);
        if (!editor.rules().front().outputs.empty()) {
            tickY = row;
        }
    }
    INFO("no tick below the \"every output\" row routed the rule");
    REQUIRE(tickY >= 0.0f);
    REQUIRE(editor.rules().front().outputs == std::vector<std::string>{"wall"});

    // And now the question. Rule 1 names no output, so the box has to come up clear — one
    // click on it, in the same place, routes this rule rather than un-ticking the last.
    editor.pick(1);
    editor.tick(); // the rebuild is deferred by a round, as every other one is
    REQUIRE(editor.rules().back().outputs.empty());
    click(tickX, tickY);
    INFO("tick at " << tickX << "," << tickY);
    CHECK(editor.rules().back().outputs == std::vector<std::string>{"wall"});

    // And the first rule is untouched by any of it.
    CHECK(editor.rules().front().outputs == std::vector<std::string>{"wall"});
}

TEST_CASE("a rig preset added again brings its own streams", "[ui][trigger]") {
    // `Generator::Config::seed` exists so that two rules in a preset do not fire the same
    // clip as each other. A renamed copy stepped its seed by a flat amount, so the *third*
    // rig added shared the second's — two layers drawing the same clips, which is the one
    // thing the seed is for.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.addRig(1);
    editor.addRig(1);
    editor.addRig(1);
    REQUIRE(editor.rules().size() == 9);

    std::set<std::string> ids;
    std::set<std::uint64_t> seeds;
    for (const Rule::Config& rule : editor.rules()) {
        INFO(rule.id << "  seed " << rule.seed);
        ids.insert(rule.id);
        seeds.insert(rule.seed);
    }
    CHECK(ids.size() == editor.rules().size());
    CHECK(seeds.size() == editor.rules().size());
}

TEST_CASE("a text value too long for its buffer still reaches the window", "[ui][trigger]") {
    // **Every string the window is given goes through `slint::SharedString`, and Slint's own
    // `slint_shared_string_from_bytes` is `core::str::from_utf8(..).unwrap()`** — a Rust
    // panic across the C ABI, which is an abort rather than an exception.
    //
    // So a `Value` cut through the middle of a UTF-8 character did not merely fail to save:
    // it killed the process. Measured on the real takt4.exe with a settings file carrying
    // one, before the fix: dead 1.1 s after launch with FAST_FAIL_FATAL_APP_EXIT and nothing
    // on screen to say why. `Value::ofText` cuts on a character boundary now; this is the
    // path that would abort if it stopped.
    //
    // The assertion is almost beside the point — if the cut came back malformed this test
    // would not fail, it would take the binary with it, which Catch2 reports as a crash.
    Rig rig;
    Rule::Config config;
    config.id = "clip";
    config.address = "/deck/{label}";
    takt4::trigger::Generator::Config segment;
    segment.kind = GeneratorKind::Fixed;
    // 46 ASCII bytes then U+00E9, so the 47-byte cut falls inside the accented character.
    segment.fixed = takt4::trigger::Value::ofText(std::string(46, 'a') + "\xC3\xA9");
    config.segments.push_back(segment);
    config.value.kind = GeneratorKind::Fixed;
    config.value.fixed = takt4::trigger::Value::ofInt(1);

    RulesController editor(rig.runner, {config});
    editor.tick();
    const auto slots = editor.window().get_slots();
    REQUIRE(slots->row_count() == 2);
    CHECK(std::string(slots->row_data(0)->fixed) == std::string(46, 'a'));

    SECTION("and a value that fits keeps every byte of itself") {
        // The other direction: nothing is given up that did not have to be.
        Rule::Config keeps = config;
        keeps.segments.front().fixed = takt4::trigger::Value::ofText("drop \xE2\x82\xAC");
        RulesController other(rig.runner, {keeps});
        other.tick();
        CHECK(std::string(other.window().get_slots()->row_data(0)->fixed) == "drop \xE2\x82\xAC");
    }
}
