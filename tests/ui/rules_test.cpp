#include "core/dmx/color.hpp"
#include "core/dmx/effect.hpp"
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

#include "support/loopback_receiver.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <slint-platform.h>

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
using takt4::tests::ModelWatch;
using takt4::testing::LoopbackReceiver;
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
        // really sends, to a socket that really receives. Nothing about this is a mock. (It
        // used to read the runner's own log of what it fired and call that a socket — the
        // audit's T1.)
        LoopbackReceiver receiver;
        takt4::output::OutputTarget resolume;
        resolume.id = "o-0000cafe";
        resolume.name = "resolume";
        resolume.host = "127.0.0.1";
        resolume.port = receiver.port();
        rig.runner.post(takt4::output::OutputCommand::outputs({resolume}));
        REQUIRE(rig.runner.transports().osc().targetCount() == 1);

        for (const Rule::Config& rule : editor.rules()) {
            rig.runner.post(takt4::output::OutputCommand::testRule(rule.id));
        }

        std::set<std::string> layers;
        std::size_t seen = 0;
        for (std::string datagram = receiver.receive(); !datagram.empty();
             datagram = receiver.receive()) {
            ++seen;
            const std::size_t at = datagram.find("/composition/layers/");
            if (at != std::string::npos) {
                layers.insert(datagram.substr(at + 20, 1));
            }
        }
        INFO(seen << " datagrams arrived");
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
    targets[0].id = "o-0000000d";
    targets[0].name = "deck";
    targets[0].port = 7000;
    targets[1].id = "o-0000000e";
    targets[1].name = "wall";
    targets[1].port = 7001;
    editor.setTargets(targets);

    editor.add();
    editor.setAddress("/fire");

    // Empty is everywhere, and says which everywhere is — an operator who has just added a
    // second output otherwise has no way to know the rule now reaches it too.
    CHECK(editor.rules()[0].outputs.empty());
    CHECK(std::string(editor.window().get_outputs_available()) == "every output: deck, wall");

    // Named by what they are called, held by what they are: the rule keeps the ids.
    editor.setOutputs("deck, wall");
    CHECK(editor.rules()[0].outputs == std::vector<std::string>{"o-0000000d", "o-0000000e"});
    CHECK(std::string(editor.window().get_outputs_available()) == "reaches 2 outputs");

    SECTION("renaming an output leaves the rule routed to it") {
        // The operator's report of 2026-09-23: renaming an OSC target broke every rule routed
        // to it, and each had to be routed again by hand.
        rig.runner.post(takt4::output::OutputCommand::outputs(targets));
        REQUIRE(rig.runner.triggers().rule(0).outputMask() == 0b11);
        targets[0].name = "media server";
        editor.setTargets(targets);
        rig.runner.post(takt4::output::OutputCommand::outputs(targets));
        CHECK(editor.rules()[0].outputs == std::vector<std::string>{"o-0000000d", "o-0000000e"});
        CHECK(std::string(editor.window().get_outputs_available()) == "reaches 2 outputs");
        CHECK(rig.runner.triggers().rule(0).outputMask() == 0b11);
    }

    SECTION("a name this rig does not have is kept, and said") {
        // A preset written where there was a "lights" output should still say "lights", so
        // that plugging it back in restores the routing rather than needing it retyped.
        editor.setOutputs("lights");
        CHECK(editor.rules()[0].outputs == std::vector<std::string>{"lights"});
        CHECK(std::string(editor.window().get_outputs_available()) ==
              "1 output it was routed to is gone");
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
    targets[0].id = "o-0000000d";
    targets[0].name = "deck";
    targets[1].id = "o-0000000e";
    targets[1].name = "wall";
    editor.setTargets(targets);
    const std::string deck = targets[0].id;
    const std::string wall = targets[1].id;

    editor.add();
    editor.setAddress("/fire");

    const auto choices = [&editor] { return editor.window().get_output_choices(); };

    // A rule that names nothing reaches every output, and that is the state the list opens
    // in: the "every output" line ticked, and the outputs themselves not.
    CHECK(editor.window().get_outputs_all());
    CHECK(std::string(editor.window().get_outputs_summary()) == "every output");
    REQUIRE(choices()->row_count() == 2);
    CHECK(std::string(choices()->row_data(0)->name) == "deck");
    // What a tick sends back is the output's id, not its name.
    CHECK(std::string(choices()->row_data(0)->key) == deck);
    CHECK_FALSE(choices()->row_data(0)->chosen);
    CHECK_FALSE(choices()->row_data(1)->chosen);

    SECTION("ticking one routes the rule to it alone") {
        editor.setOutputChosen(deck, true);
        CHECK(editor.rules()[0].outputs == std::vector<std::string>{deck});
        CHECK_FALSE(editor.window().get_outputs_all());
        CHECK(std::string(editor.window().get_outputs_summary()) == "deck");
        CHECK(choices()->row_data(0)->chosen);
        CHECK_FALSE(choices()->row_data(1)->chosen);

        // And more than one, which is the whole reason this is not a ComboBox.
        editor.setOutputChosen(wall, true);
        CHECK(editor.rules()[0].outputs == std::vector<std::string>{deck, wall});
        CHECK(std::string(editor.window().get_outputs_summary()) == "deck, wall");
    }

    SECTION("a renamed output stays ticked, under its new name") {
        editor.setOutputChosen(deck, true);
        targets[0].name = "media server";
        editor.setTargets(targets);
        CHECK(editor.rules()[0].outputs == std::vector<std::string>{deck});
        CHECK(std::string(editor.window().get_outputs_summary()) == "media server");
        CHECK(std::string(choices()->row_data(0)->name) == "media server");
        CHECK(choices()->row_data(0)->chosen);
        CHECK_FALSE(choices()->row_data(0)->missing);
    }

    SECTION("ticking the same one twice does not name it twice") {
        editor.setOutputChosen(deck, true);
        editor.setOutputChosen(deck, true);
        CHECK(editor.rules()[0].outputs == std::vector<std::string>{deck});
    }

    SECTION("unticking the last one is every output again") {
        // An empty list *is* everywhere (`output::resolveOutputs`), so there is no third
        // state to get wrong: the two cannot disagree because there is only one of them.
        editor.setOutputChosen(deck, true);
        editor.setOutputChosen(deck, false);
        CHECK(editor.rules()[0].outputs.empty());
        CHECK(editor.window().get_outputs_all());
        CHECK(std::string(editor.window().get_outputs_summary()) == "every output");
    }

    SECTION("and every output is one click from anywhere") {
        editor.setOutputChosen(wall, true);
        editor.chooseAllOutputs();
        CHECK(editor.rules()[0].outputs.empty());
        CHECK(editor.window().get_outputs_all());
    }

    SECTION("an output this rig has not got is listed, marked, and can only be taken off") {
        // A preset written where there was a "lights" output. It cannot be typed back — the
        // list is the rig's — so it has to be visible while it is still named.
        editor.setOutputs("lights");
        REQUIRE(choices()->row_count() == 3);
        CHECK(std::string(choices()->row_data(2)->name) == "an output that is gone");
        CHECK(std::string(choices()->row_data(2)->key) == "lights");
        CHECK(choices()->row_data(2)->chosen);
        CHECK(choices()->row_data(2)->missing);
        CHECK_FALSE(choices()->row_data(0)->missing);

        editor.setOutputChosen("lights", false);
        CHECK(editor.rules()[0].outputs.empty());
        CHECK(choices()->row_count() == 2);
    }

    SECTION("an output added to the rig appears in the list") {
        targets.emplace_back();
        targets[2].id = "o-0000000f";
        targets[2].name = "haze";
        editor.setTargets(targets);
        REQUIRE(choices()->row_count() == 3);
        CHECK(std::string(choices()->row_data(2)->name) == "haze");
    }

    SECTION("and the list follows the selected rule") {
        editor.setOutputChosen(wall, true);
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

TEST_CASE("the host preset picker says what the rule is, and custom empties it", "[ui][trigger]") {
    // Reported from a rig on 2026-09-06: pick the Resolume preset, change the picker back to
    // "custom", and the Resolume address, its two chips and its press-then-release are all
    // still there under a box that says "custom". Both halves of that were wrong — the box
    // never followed the rule, and "custom" did nothing.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();

    editor.pickHostPreset(1); // Resolume 7 — clip
    REQUIRE(editor.rules().front().address == "/composition/layers/{layer}/clips/{clip}/connect");
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
        CHECK(editor.rules().front().address == "/composition/layers/{layer}/clips/{clip}/select");
        CHECK(editor.rules().front().segments.size() == 2);
    }

    SECTION("and selecting another rule brings that rule's answer, not this one's") {
        editor.add();
        CHECK(editor.window().get_host_preset_index() == 0);
        editor.pick(0);
        CHECK(editor.window().get_host_preset_index() == 1);
    }

    SECTION("custom picked again over an address of the operator's own changes nothing") {
        // The audit of 2026-09-25, M19. A dropdown reports a pick of the entry it already shows
        // — the mouse wheel over a focused one does it a notch at a time — and this one reads
        // "custom" for any address that is not a preset's, so scrolling past it wiped the
        // address and every follow-up.
        editor.setAddress("/my/own/{n}");
        editor.addFollowUp();
        REQUIRE(editor.window().get_host_preset_index() == 0);
        const std::size_t follows = editor.rules().front().followUps.size();
        editor.pickHostPreset(0);
        CHECK(editor.rules().front().address == "/my/own/{n}");
        CHECK(editor.rules().front().segments.size() == 1);
        CHECK(editor.rules().front().followUps.size() == follows);
        CHECK_FALSE(editor.window().get_status_is_error());
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

    // A second press is not a release — it used to be, so a double-click undid the halt
    // (the audit's H18). RELEASE is its own button.
    editor.panic();
    CHECK(rig.runner.panicked());
    editor.releasePanic();
    CHECK_FALSE(rig.runner.panicked());
    CHECK_FALSE(editor.window().get_panicked());
}

TEST_CASE("the editor's PANIC holds through a double click and answers Escape", "[ui][trigger]") {
    // The same fixes as the main window's (the audit's H18), driven with real clicks and keys:
    // PANIC only engages, RELEASE appears *above* it so the second click of a double-click
    // still lands on PANIC, Escape is PANIC from anywhere in this window — and Escape in a text
    // box only leaves the box.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    const std::string before = editor.rules().front().name;
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
    const auto press = [&window](const std::string& key) {
        window.window().dispatch_key_press_event(slint::SharedString(key));
        window.window().dispatch_key_release_event(slint::SharedString(key));
    };
    const std::string escape(1, '\x1b');

    // PANIC: the bottom of the rule list, found by climbing from the window's bottom edge.
    constexpr float kX = 60.0f;
    float panicY = -1.0f;
    for (float y = takt4::ui::kRulesWindowHeight - 14.0f;
         y > takt4::ui::kRulesWindowHeight - 140.0f && panicY < 0.0f; y -= 4.0f) {
        click(kX, y);
        if (rig.runner.panicked()) {
            panicY = y;
        }
    }
    REQUIRE(panicY > 0.0f);
    click(kX, panicY); // the second click of the double-click
    CHECK(rig.runner.panicked());

    bool released = false;
    for (float y = panicY - 20.0f; y > panicY - 120.0f && !released; y -= 4.0f) {
        click(kX, y);
        released = !rig.runner.panicked();
    }
    CHECK(released);

    press(escape);
    CHECK(rig.runner.panicked());
    editor.releasePanic();

    // The rule's name box in the title bar, found by typing into candidates until the name
    // changes. Only the hit is asserted on: it is the round in which Escape was inside a box.
    bool found = false;
    bool panickedInBox = true;
    for (float y = 12.0f; y < 58.0f && !found; y += 4.0f) {
        for (float x = 320.0f; x < 760.0f && !found; x += 20.0f) {
            click(x, y);
            press("q");
            press(escape);
            if (editor.rules().front().name != before) {
                found = true;
                panickedInBox = rig.runner.panicked();
            }
            if (rig.runner.panicked()) {
                editor.releasePanic();
            }
        }
    }
    INFO("no click landed in the rule's name box");
    REQUIRE(found);
    CHECK_FALSE(panickedInBox);
    CHECK(editor.rules().front().name.find('q') != std::string::npos);
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
    // **That row and no other** (the audit's M16): throwing the whole list away took the box
    // the operator had just clicked into along with it.
    //
    // **And only for a widget that sets itself** (the audit of 2026-09-25, M17). The boxes are
    // `LiveField`s and `NumberBox`es now, which never go deaf, so what they hold is told to them
    // in place; a row rebuilt for one of them took the box beside it down too. What is left is
    // the std dropdowns and tick boxes, which assign their own values when they are used.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/deck/{clip}");
    editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Shuffle));
    editor.setSlotRange(0, "1 - 4");
    editor.tick();

    const auto slots = editor.window().get_slots();
    REQUIRE(slots->row_count() == 2);
    const auto watch = std::make_shared<ModelWatch>();
    slots->attach_peer(watch);

    // The dropdown: a pick the controller then answers with a different kind of row entirely.
    editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Random));
    CHECK(watch->resets == 0); // not from inside the callback that caused it
    CHECK(watch->removed == 0);
    editor.tick();
    // That one row as a new element, and the list itself never reset.
    CHECK(watch->removed == 1);
    CHECK(watch->added == 1);
    CHECK(watch->resets == 0);

    SECTION("a text box is told without being rebuilt") {
        // A range typed the wrong way round, which the rule holds the right way round.
        editor.setSlotRange(0, "8 - 1");
        CHECK(slots->row_data(0)->low == 1);
        CHECK(slots->row_data(0)->high == 8);
        editor.tick();
        CHECK(watch->removed == 1); // the dropdown's rebuild above, and no other
        CHECK(watch->resets == 0);
    }

    SECTION("a number box is told without being rebuilt") {
        // A guard wider than the pool can satisfy is clamped (§5.8).
        editor.setSlotRange(0, "1 - 8");
        editor.setSlotNoRepeat(0, 40);
        CHECK(slots->row_data(0)->no_repeat == 7); // one less than the eight distinct values
        editor.tick();
        CHECK(watch->removed == 1);
        CHECK(watch->resets == 0);
    }

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
    wall.id = "o-000000aa";
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
    // The sweep passes over the SEND kind dropdown on its way down the pane, and a stray pick
    // there turns the rule into a MIDI or a lighting one — which since 2026-09-16 changes what
    // the routing list is allowed to offer, and on a lighting rule takes the row away
    // altogether. So the kind is put back before each candidate and a candidate that moved it
    // is not believed. See `targetTakes`: a rule is only offered the outputs it can reach.
    const auto oscKind = static_cast<int>(std::find(takt4::trigger::kMessageKinds.begin(),
                                                    takt4::trigger::kMessageKinds.end(),
                                                    takt4::trigger::Message::Kind::Osc) -
                                          takt4::trigger::kMessageKinds.begin());
    const auto stillOsc = [&editor] {
        return editor.rules().front().sendKind == takt4::trigger::Message::Kind::Osc;
    };
    float openX = -1.0f;
    float openY = -1.0f;
    float everyOutputY = -1.0f;
    for (float y = 250.0f; y < 760.0f && everyOutputY < 0.0f; y += 4.0f) {
        for (float x = 150.0f; x < 420.0f && everyOutputY < 0.0f; x += 10.0f) {
            editor.pickSend(oscKind);
            editor.setOutputs("wall"); // something for "every output" to clear
            click(700.0f, 40.0f);      // close whatever is open; outside every popup
            click(x, y);
            for (float row = y + 20.0f; row < y + 80.0f && everyOutputY < 0.0f; row += 3.0f) {
                click(x + 30.0f, row);
                if (editor.rules().front().outputs.empty() && stillOsc()) {
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
        if (!editor.rules().front().outputs.empty() && stillOsc()) {
            tickY = row;
        }
    }
    INFO("no tick below the \"every output\" row routed the rule");
    REQUIRE(tickY >= 0.0f);
    REQUIRE(editor.rules().front().outputs == std::vector<std::string>{wall.id});

    // And now the question. Rule 1 names no output, so the box has to come up clear — one
    // click on it, in the same place, routes this rule rather than un-ticking the last.
    editor.pick(1);
    editor.tick(); // the rebuild is deferred by a round, as every other one is
    REQUIRE(editor.rules().back().outputs.empty());
    click(tickX, tickY);
    INFO("tick at " << tickX << "," << tickY);
    CHECK(editor.rules().back().outputs == std::vector<std::string>{wall.id});

    // And the first rule is untouched by any of it.
    CHECK(editor.rules().front().outputs == std::vector<std::string>{wall.id});
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

TEST_CASE("clicking a button finishes what was being typed", "[ui][trigger]") {
    // **The other half of "the operator has finished".**
    //
    // Every box in this window commits when it loses the keyboard focus, which is what makes
    // clicking away save the edit rather than lose it. Nothing in either window was willing
    // to *take* the focus, so a click on a heading, on the panel behind a row, or on a button
    // moved it nowhere: the box stayed lit and what was typed in it stayed uncommitted.
    // Reported from a rig on 2026-09-16 — "when I click outside of a box, that box stays
    // selected as an entry box ... clicking save, etc other ui elements doesnt exit out of
    // that box being in edit mode either" — and answered by `FocusSink` in theme.slint.
    //
    // Driven by real pointer events, because what is under test is the markup: the controller
    // was right all along and a test that called the callback would pass either way.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    auto& window = editor.window();
    window.show();
    window.window().dispatch_scale_factor_change_event(1.0f);
    window.window().dispatch_resize_event(slint::LogicalSize({900.0f, 520.0f}));
    window.window().dispatch_window_active_changed_event(true);
    REQUIRE(std::string(window.get_cooldown_ms()) == "0");

    // The cooldown box, found rather than written down — a coordinate in this file rots the
    // first time a row moves, and a test that then clicked on nothing would pass for ever.
    const auto click = [&window](float x, float y) {
        const slint::LogicalPosition at({x, y});
        window.window().dispatch_pointer_move_event(at);
        window.window().dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
        window.window().dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
    };
    const auto type = [&window](const char* key) {
        window.window().dispatch_key_press_event(slint::SharedString(key));
        window.window().dispatch_key_release_event(slint::SharedString(key));
    };

    float boxX = -1.0f;
    float boxY = -1.0f;
    for (float y = 250.0f; y < 345.0f && boxX < 0.0f; y += 2.0f) {
        for (float x = 540.0f; x < 660.0f && boxX < 0.0f; x += 20.0f) {
            click(x, y);
            type("5");
            if (std::string(window.get_cooldown_ms()) != "0") {
                boxX = x;
                boxY = y;
            }
        }
    }
    INFO("the cooldown box never took a keystroke — the sweep found no text field");
    REQUIRE(boxX >= 0.0f);
    // Typed, not committed: that is the contract, and it is what makes the rest of this test
    // mean something.
    REQUIRE(editor.rules().front().conditions.cooldownSeconds == Approx(0.0));

    SECTION("a click on the panel behind the rows commits it") {
        // **A click that touches nothing else**, which is the whole claim: the commit has to
        // come from the background taking the focus and not from having landed on a slider,
        // a tick box or another text field. So the sweep below only accepts a point after
        // checking that every other setting on the rule is where it was — a click on the
        // probability slider would also commit the box, and would prove nothing.
        const takt4::trigger::Conditions before = editor.rules().front().conditions;
        const auto inert = [&editor, &before] {
            const takt4::trigger::Conditions& now = editor.rules().front().conditions;
            return now.minConfidence == Approx(before.minConfidence) &&
                   now.probability == Approx(before.probability) &&
                   now.minBpm == Approx(before.minBpm) && now.maxBpm == Approx(before.maxBpm) &&
                   now.intensities == before.intensities;
        };
        // The event-log pane on the right, below its lines: no rule control is there at all,
        // so nothing in this region can commit the box except the background itself.
        const slint::SharedString rate = window.get_rule_rate();
        bool committed = false;
        for (float y = 220.0f; y < 400.0f && !committed; y += 10.0f) {
            for (float x = 760.0f; x < 890.0f && !committed; x += 20.0f) {
                click(x, y);
                committed = editor.rules().front().conditions.cooldownSeconds > 0.0 && inert() &&
                            window.get_rule_rate() == rate;
            }
        }
        INFO("cooldown box held \"" << std::string(window.get_cooldown_ms()) << "\"");
        CHECK(committed);
    }

    SECTION("and so does pressing a button") {
        // PANIC, bottom left of the rule list — a `Press`, whose own touch area would
        // otherwise swallow the click before the background ever saw it.
        //
        // **Found first, then pressed once.** This used to accept any click in the region that
        // committed the box, which a click on any other button, or on the background, does as
        // well — it never checked that PANIC was hit (the audit's T1). So PANIC is found by
        // what it does, released, the box typed into again, and PANIC pressed at that spot:
        // then the commit can only have come from the button.
        float panicX = -1.0f;
        float panicY = -1.0f;
        for (float y = 470.0f; y < 515.0f && panicX < 0.0f; y += 4.0f) {
            for (float x = 40.0f; x < 190.0f && panicX < 0.0f; x += 20.0f) {
                click(x, y);
                slint::platform::update_timers_and_animations();
                if (rig.runner.panicked()) {
                    panicX = x;
                    panicY = y;
                }
            }
        }
        {
            INFO("no click in the rule list's bottom row panicked the runner");
            REQUIRE(panicX >= 0.0f);
        }
        editor.releasePanic();
        REQUIRE_FALSE(rig.runner.panicked());

        click(boxX, boxY);
        type("7");
        slint::platform::update_timers_and_animations();
        const double committed = editor.rules().front().conditions.cooldownSeconds;
        const std::string typed(window.get_cooldown_ms());
        REQUIRE(typed.find('7') != std::string::npos);
        // Typed, not committed, as before.
        REQUIRE(std::to_string(static_cast<int>(committed * 1000.0 + 0.5)) != typed);

        click(panicX, panicY);
        slint::platform::update_timers_and_animations();
        CHECK(rig.runner.panicked()); // it was PANIC that was pressed
        INFO("cooldown box held \"" << std::string(window.get_cooldown_ms()) << "\"");
        CHECK(editor.rules().front().conditions.cooldownSeconds * 1000.0 ==
              Approx(std::stod(typed)).margin(0.5));
        editor.releasePanic();
    }
}

TEST_CASE("a rule is only offered the outputs its kind can reach", "[ui][trigger]") {
    // **A choice that cannot do anything is worse than no choice**, because ticking it looks
    // like routing and is silence. `RuleSink` already sends MIDI only to MIDI ports and OSC
    // only to OSC targets, and nothing routes to an Art-Net node by name at all — a lighting
    // rule picks its *fixtures* and the fixture says which universe it is in. The editor knew
    // none of that and offered all three lists to all three kinds. Reported from a rig on
    // 2026-09-16: "why is my rdm10 artnet destination showing up as an option for midi and
    // OSC output!?"
    Rig rig;
    RulesController editor(rig.runner, {});
    takt4::output::OutputTarget wall;
    wall.id = "o-000000aa";
    wall.name = "wall";
    wall.host = "127.0.0.1";
    wall.port = 7000;
    takt4::output::OutputTarget desk;
    desk.id = "o-000000bb";
    desk.name = "desk";
    desk.kind = takt4::output::OutputTarget::Kind::Midi;
    desk.device = "Some MIDI Out";
    takt4::output::OutputTarget node;
    node.id = "o-000000cc";
    node.name = "RDM10";
    node.kind = takt4::output::OutputTarget::Kind::ArtNet;
    node.host = "192.168.1.33";
    node.port = 6454;
    editor.setTargets({wall, desk, node});
    editor.add();
    editor.tick();

    const auto names = [&editor] {
        std::vector<std::string> found;
        const auto rows = editor.window().get_output_choices();
        for (std::size_t i = 0; i < rows->row_count(); ++i) {
            found.push_back(std::string(rows->row_data(i)->name));
        }
        return found;
    };

    // A fresh rule sends OSC.
    CHECK(names() == std::vector<std::string>{"wall"});
    CHECK(std::string(editor.window().get_outputs_available()).find("RDM10") == std::string::npos);

    SECTION("a MIDI rule is offered the MIDI port and nothing else") {
        const auto midi =
            std::find(takt4::trigger::kMessageKinds.begin(), takt4::trigger::kMessageKinds.end(),
                      takt4::trigger::Message::Kind::MidiNote) -
            takt4::trigger::kMessageKinds.begin();
        editor.pickSend(static_cast<int>(midi));
        editor.tick();
        CHECK(names() == std::vector<std::string>{"desk"});
        const std::string line(editor.window().get_outputs_available());
        INFO(line);
        CHECK(line.find("RDM10") == std::string::npos);
        CHECK(line.find("desk") != std::string::npos);
    }

    SECTION("an output the rule holds but its kind cannot reach is shown as reaching nothing") {
        editor.setOutputChosen(wall.id, true);
        const auto midi =
            std::find(takt4::trigger::kMessageKinds.begin(), takt4::trigger::kMessageKinds.end(),
                      takt4::trigger::Message::Kind::MidiCc) -
            takt4::trigger::kMessageKinds.begin();
        editor.pickSend(static_cast<int>(midi));
        editor.tick();
        const auto rows = editor.window().get_output_choices();
        bool sawWall = false;
        for (std::size_t i = 0; i < rows->row_count(); ++i) {
            if (std::string(rows->row_data(i)->name) == "wall") {
                sawWall = true;
                // Kept so it can be un-ticked, and marked so it is not mistaken for routing.
                CHECK(rows->row_data(i)->missing);
            }
        }
        CHECK(sawWall);
    }
}

TEST_CASE("a color generator starts as a color rather than as the integers 1 to 8",
          "[ui][trigger][dmx]") {
    // `Generator::Config`'s default is §5.8's — shuffle over 1 to 8 — which is right for a
    // clip index and nonsense for a color: every draw is a number `parseColor` cannot read,
    // so every fire fell back to white while the editor showed a box asking which eight
    // colors 1 through 8 were. Reported from a rig on 2026-09-16: "I define a range 1-9
    // which maps to what!? it just stays the same #ffff color code".
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    const auto dmx =
        std::find(takt4::trigger::kMessageKinds.begin(), takt4::trigger::kMessageKinds.end(),
                  takt4::trigger::Message::Kind::Dmx) -
        takt4::trigger::kMessageKinds.begin();
    editor.pickSend(static_cast<int>(dmx));
    const auto colorEffect =
        std::find(takt4::dmx::kEffectKinds.begin(), takt4::dmx::kEffectKinds.end(),
                  takt4::dmx::EffectKind::Color) -
        takt4::dmx::kEffectKinds.begin();
    editor.pickEffect(static_cast<int>(colorEffect));
    editor.tick();

    const takt4::trigger::Generator::Config& color = editor.rules().front().dmx.color;
    CHECK(color.kind == GeneratorKind::Fixed);
    CHECK(takt4::dmx::parseColor(color.fixed.text()).has_value());

    SECTION("and switching it to shuffle seeds a palette rather than a number range") {
        const auto shuffle =
            std::find(takt4::trigger::kGeneratorKinds.begin(),
                      takt4::trigger::kGeneratorKinds.end(), GeneratorKind::Shuffle) -
            takt4::trigger::kGeneratorKinds.begin();
        editor.pickSlotKind(0, static_cast<int>(shuffle));
        editor.tick();
        const takt4::trigger::Generator::Config& seeded = editor.rules().front().dmx.color;
        CHECK(seeded.pool == Pool::List);
        CHECK(seeded.values.size() >= 4);
        for (const takt4::trigger::Value& value : seeded.values) {
            INFO("palette entry " << value.text());
            CHECK(takt4::dmx::parseColor(value.text()).has_value());
        }
        // And the editor shows them as swatches rather than as a comma-separated line.
        CHECK(editor.window().get_palette_shown());
        CHECK(editor.window().get_palette()->row_count() == seeded.values.size());
    }

    SECTION("the mix mode gives one generator per component, over the whole byte") {
        editor.pickColorMode(1); // mix red, green, blue
        editor.tick();
        const takt4::trigger::DmxSend& send = editor.rules().front().dmx;
        CHECK(send.colorMode == takt4::trigger::ColorMode::Mix);
        CHECK(send.red.low == 0);
        CHECK(send.red.high == 255);
        // Three chips, one per component, in the order `Rule::buildPayload` draws them.
        const auto slots = editor.window().get_slots();
        REQUIRE(slots->row_count() == 3);
        CHECK(std::string(slots->row_data(0)->label) == "red");
        CHECK(std::string(slots->row_data(1)->label) == "green");
        CHECK(std::string(slots->row_data(2)->label) == "blue");
        // And the palette is not offered, because in this mode there is not one.
        CHECK_FALSE(editor.window().get_palette_shown());
    }
}

TEST_CASE("dragging a color slider does not tear its own picker down", "[ui][trigger][dmx]") {
    // **The crash.** Reported on 2026-09-16: *"the program completely crashed while I was
    // using the color picker. I moved the hue slider and it completely crashed."*
    //
    // The picker is a `PopupWindow` inside the palette repeater, so the swatch it belongs to
    // is one of the items that repeater owns. `setPaletteColor` republished the palette, and
    // `publishPalette` asked `rowsNeedRebuild` whether any surviving row had moved — with a
    // comparator that compared *every* field, including the swatch and the hue the slider had
    // just changed. So every pixel of the drag said "this row moved, build the repeater
    // again", and the next redraw did: `rebuildRows` clears `paletteModel_`, the repeater
    // throws its items away, and the item being thrown away is the one holding the open popup
    // and the slider under the operator's finger.
    //
    // What this counts is the reset, which is the cause; the gesture test below is the effect.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    const auto dmx =
        std::find(takt4::trigger::kMessageKinds.begin(), takt4::trigger::kMessageKinds.end(),
                  takt4::trigger::Message::Kind::Dmx) -
        takt4::trigger::kMessageKinds.begin();
    editor.pickSend(static_cast<int>(dmx));
    const auto colorEffect =
        std::find(takt4::dmx::kEffectKinds.begin(), takt4::dmx::kEffectKinds.end(),
                  takt4::dmx::EffectKind::Color) -
        takt4::dmx::kEffectKinds.begin();
    editor.pickEffect(static_cast<int>(colorEffect));
    editor.addPaletteColor(); // a fixed color becomes a palette, which is what has swatches
    editor.tick();

    const auto palette = editor.window().get_palette();
    REQUIRE(palette->row_count() > 1);
    // Settle first: building the rule rebuilt the repeater, as it is entitled to.
    editor.tick();
    const auto watch = std::make_shared<ModelWatch>();
    palette->attach_peer(watch);

    // One call per pixel of movement — which is what `changed(v)` on the hue slider is — with
    // the redraw timer running between them, as it does in the app.
    for (int hue = 0; hue < 90; ++hue) {
        editor.setPaletteColor(0, static_cast<float>(hue), 100.0f, 100.0f);
        editor.tick();
    }

    // **Not one reset.** A reset here is the popup being destroyed under the pointer that is
    // dragging it. Measured before the fix: 90 of them, one per pixel.
    INFO("palette model resets during the drag: " << watch->resets);
    CHECK(watch->resets == 0);
    CHECK(watch->removed == 0);
    // ...and the drag still did its job: the row follows the slider.
    CHECK(watch->changes > 0);
    CHECK(editor.rules().front().dmx.color.values.front().text() ==
          takt4::dmx::formatColor(takt4::dmx::fromHsv(89.0, 1.0, 1.0)));

    SECTION("and the same is true of the fixed color's picker") {
        // The other picker on the same row, which has the same popup in the same repeater —
        // `slotModel_` rather than `paletteModel_`, and `setSlotColor` rather than
        // `setPaletteColor`, but the same drag and the same teardown.
        editor.setRules({});
        editor.add();
        editor.pickSend(static_cast<int>(dmx));
        editor.pickEffect(static_cast<int>(colorEffect));
        editor.tick();
        editor.tick();
        const auto slots = editor.window().get_slots();
        REQUIRE(slots->row_count() == 1);
        const auto slotWatch = std::make_shared<ModelWatch>();
        slots->attach_peer(slotWatch);
        for (int hue = 0; hue < 90; ++hue) {
            editor.setSlotColor(0, static_cast<float>(hue), 100.0f, 100.0f);
            editor.tick();
        }
        INFO("slot model resets during the drag: " << slotWatch->resets);
        CHECK(slotWatch->resets == 0);
    }
}

TEST_CASE("the hue slider can be dragged the way it was when it crashed", "[ui][trigger][dmx]") {
    // **Driven by real pointer events**, because the thing that broke is a popup being
    // destroyed while the pointer has a grab on something inside it, and neither half of that
    // exists unless the gesture is real. The test above counts the cause; this one makes the
    // gesture. Reverted, it does not fail — it takes the process down, which is the report.
    Rig rig;
    RulesController editor(rig.runner, {});
    auto& window = editor.window();
    window.show();
    window.window().dispatch_scale_factor_change_event(1.0f);
    // Tall enough that the picker, which hangs below its swatch, has somewhere to hang.
    window.window().dispatch_resize_event(slint::LogicalSize({900.0f, 1000.0f}));
    window.window().dispatch_window_active_changed_event(true);

    const auto dmx =
        std::find(takt4::trigger::kMessageKinds.begin(), takt4::trigger::kMessageKinds.end(),
                  takt4::trigger::Message::Kind::Dmx) -
        takt4::trigger::kMessageKinds.begin();
    const auto colorEffect =
        std::find(takt4::dmx::kEffectKinds.begin(), takt4::dmx::kEffectKinds.end(),
                  takt4::dmx::EffectKind::Color) -
        takt4::dmx::kEffectKinds.begin();
    // A rule with a palette, built from nothing. Called again before every candidate below,
    // because a press that misses the picker lands on whatever is underneath it, and a stray
    // drag on the confidence slider or a dropdown would leave the next candidate testing a
    // different rule.
    const auto buildRule = [&] {
        editor.setRules({});
        editor.add();
        editor.pickSend(static_cast<int>(dmx));
        editor.pickEffect(static_cast<int>(colorEffect));
        editor.addPaletteColor();
        editor.tick();
        editor.tick();
    };
    buildRule();
    REQUIRE(window.get_palette_shown());
    REQUIRE(window.get_palette()->row_count() > 1);

    const auto firstColor = [&] {
        return std::string(editor.rules().front().dmx.color.values.front().text());
    };
    const std::string before = firstColor();

    // **Found rather than written down**, for the reason the other sweeps in this file give: a
    // coordinate in this file rots the first time a row moves, and a test that then clicked on
    // nothing would go on passing for ever.
    //
    // A swatch is 40px wide and 30px tall, so a 16px by 8px step cannot step over one, and the
    // picker itself says when one has been hit — `PopupWindow::is-open` reaches the controller,
    // which is the wiring the rebuild guard runs on.
    const auto click = [&window](float x, float y) {
        const slint::LogicalPosition at({x, y});
        window.window().dispatch_pointer_move_event(at);
        window.window().dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
        window.window().dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
    };
    float swatchX = 0.0f;
    float swatchY = 0.0f;
    bool opened = false;
    for (float y = 300.0f; y < 900.0f && !opened; y += 8.0f) {
        for (float x = 240.0f; x < 620.0f && !opened; x += 16.0f) {
            click(x, y);
            if (editor.pickerOpen()) {
                opened = true;
                swatchX = x;
                swatchY = y;
                break;
            }
            // Whatever that click did land on, undone before the next candidate.
            buildRule();
        }
    }
    INFO("no click anywhere in the editor pane opened a color picker");
    REQUIRE(opened);
    INFO("the swatch was hit at " << swatchX << ", " << swatchY);

    // And now a slider inside it, found the same way. **Where the picker opens cannot be
    // computed from the markup**: it is written to hang below its swatch, and Slint puts it
    // wherever it fits — measured, from a swatch at the bottom of the pane, entirely *above*
    // it. So this hunts rather than assuming, and it hunts with a plain click, because a press
    // on a slider's track sets the value there and then (see Slint's `SliderBase`) — a click
    // that moves the color has landed on one.
    //
    // The swatch is clicked again whenever the picker has closed: a click that misses the
    // popup is a click outside it, which closes it, and every later probe would be pressing on
    // nothing.
    float sliderX = -1.0f;
    float sliderY = -1.0f;
    for (float y = std::max(0.0f, swatchY - 360.0f); y < swatchY + 300.0f && sliderX < 0.0f;
         y += 8.0f) {
        for (float x = std::max(0.0f, swatchX - 300.0f); x < swatchX + 300.0f && sliderX < 0.0f;
             x += 8.0f) {
            if (!editor.pickerOpen()) {
                click(swatchX, swatchY);
            }
            if (!editor.pickerOpen()) {
                continue;
            }
            click(x, y);
            if (firstColor() != before) {
                sliderX = x;
                sliderY = y;
            }
        }
    }
    INFO("the picker opened but no click inside it moved the first palette color");
    REQUIRE(sliderX >= 0.0f);
    INFO("a slider is at " << sliderX << ", " << sliderY);

    // **The gesture that crashed.** Press on the slider and walk it sideways, letting the
    // redraw timer run between the moves exactly as it does in the app: the move publishes,
    // the publish asked for the repeater to be built again, and the tick built it — destroying
    // the popup this pointer is holding and the slider inside it.
    buildRule();
    click(swatchX, swatchY);
    REQUIRE(editor.pickerOpen());
    const slint::LogicalPosition grab({sliderX, sliderY});
    window.window().dispatch_pointer_move_event(grab);
    window.window().dispatch_pointer_press_event(grab, slint::PointerEventButton::Left);
    std::set<std::string> seen;
    for (float at = sliderX; at < sliderX + 120.0f; at += 4.0f) {
        window.window().dispatch_pointer_move_event(slint::LogicalPosition({at, sliderY}));
        editor.tick();
        seen.insert(firstColor());
    }
    const slint::LogicalPosition let({sliderX + 120.0f, sliderY});
    window.window().dispatch_pointer_move_event(let);
    window.window().dispatch_pointer_release_event(let, slint::PointerEventButton::Left);
    editor.tick();

    // **The slider kept its grab for the whole drag.** This is the assertion that fails
    // without the fix even on a run that does not take the process down with it: once the
    // repeater has been rebuilt, the element the pointer is holding is gone, so the rest of
    // the movement reaches nothing and the color stops following the hand.
    INFO("colors seen during a thirty-step drag: " << seen.size());
    CHECK(seen.size() > 10);
    // ...and the picker is still up, the palette still a palette, the rule still a color rule.
    CHECK(editor.pickerOpen());
    CHECK(takt4::dmx::parseColor(firstColor()).has_value());
    CHECK(window.get_palette_shown());
    CHECK(window.get_palette()->row_count() == editor.rules().front().dmx.color.values.size());
    CHECK(editor.rules().front().dmx.effect == takt4::dmx::EffectKind::Color);
}

TEST_CASE("a new rule switched to MIDI waits for its number and says so", "[ui][trigger]") {
    // The audit's C7, the way an operator meets it: press +, pick "MIDI CC" to start building,
    // and the rule used to fire CC 1 to 8 at a value of one on every bar, to every MIDI output.
    // The operator's call is that it stays armed and waits for a number, in words.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    const auto cc = std::find(takt4::trigger::kMessageKinds.begin(),
                              takt4::trigger::kMessageKinds.end(),
                              takt4::trigger::Message::Kind::MidiCc) -
                    takt4::trigger::kMessageKinds.begin();
    editor.pickSend(static_cast<int>(cc));
    editor.tick();

    REQUIRE(rig.runner.triggers().ruleCount() == 1);
    CHECK(editor.rules().front().enabled); // armed
    CHECK_FALSE(rig.runner.triggers().rule(0).valid());
    CHECK(std::string(editor.window().get_rule_problem()) == "choose a controller number");
    // The list row says it too, not only with a color.
    CHECK(std::string(editor.window().get_rules()->row_data(0)->problem) ==
          "choose a controller number");

    // The number box is empty and asks for one; the value is a velocity that can be heard.
    const auto slots = editor.window().get_slots();
    REQUIRE(slots->row_count() == 2);
    CHECK(std::string(slots->row_data(0)->fixed).empty());
    CHECK(slots->row_data(0)->wanting);
    CHECK(std::string(slots->row_data(1)->fixed) == "100");

    // TEST, the one gesture that fires past the trigger and the conditions, reaches nothing.
    editor.test();
    CHECK(rig.runner.takeFired().empty());

    // Text is not a number, and does not arm it.
    editor.setSlotFixed(0, "volume");
    CHECK_FALSE(rig.runner.triggers().rule(0).valid());

    // A number does.
    editor.setSlotFixed(0, "7");
    CHECK(rig.runner.triggers().rule(0).valid());
    CHECK(std::string(editor.window().get_rule_problem()).empty());
    editor.test();
    const std::vector<OutputRunner::Fired> fired = rig.runner.takeFired();
    REQUIRE(fired.size() == 1);
    CHECK(fired.front().message == "cc 7 ch 1 = 100");

    SECTION("a note number chosen for a note on carries to a note off") {
        editor.pickSend(1); // note on, `kMessageKinds` order: a controller is not a note
        CHECK_FALSE(rig.runner.triggers().rule(0).valid());
        editor.setSlotFixed(0, "60");
        editor.pickSend(2); // note off
        CHECK(editor.rules().front().numberChosen);
        CHECK(rig.runner.triggers().rule(0).valid());
    }
    SECTION("but not to a program change, where 7 is a different instruction") {
        const auto program = std::find(takt4::trigger::kMessageKinds.begin(),
                                       takt4::trigger::kMessageKinds.end(),
                                       takt4::trigger::Message::Kind::MidiProgramChange) -
                             takt4::trigger::kMessageKinds.begin();
        editor.pickSend(static_cast<int>(program));
        CHECK_FALSE(rig.runner.triggers().rule(0).valid());
        CHECK(std::string(editor.window().get_rule_problem()) == "choose a program number");
    }
}

TEST_CASE("a muted rule's fire is logged as not sent", "[ui][trigger]") {
    // The audit's M11. A muted rule runs — its generators advance and its count moves, which is
    // what keeps it in phase — and nothing leaves. The log said it had.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/go");
    editor.pickTrigger(indexOf(Trigger::Manual));
    editor.setMuted(true);

    rig.runner.post(takt4::output::OutputCommand::manual());
    editor.tick();

    const auto log = editor.window().get_log();
    REQUIRE(log->row_count() == 1);
    const std::string line(*log->row_data(0));
    INFO(line);
    CHECK(line.find("/go") != std::string::npos);
    CHECK(line.find("muted, not sent") != std::string::npos);
    CHECK(std::string(editor.window().get_last_fired()).find("muted, not sent") !=
          std::string::npos);
}

TEST_CASE("a switch flipped from a control surface shows in the editor and survives its edits",
          "[ui][trigger]") {
    // The audit's H7. A Stream Deck's /ctl/rule/<id>/enable 0 went to the live rule and nowhere
    // else: the editor went on showing the rule switched on, and the next edit it made to any
    // rule — a keystroke of a rename — posted the set whole and switched it back on.
    Rig rig;
    std::vector<Rule::Config> saved;
    RulesController editor(rig.runner, {});
    editor.setRulesChanged([&saved](const std::vector<Rule::Config>& rules) { saved = rules; });
    editor.add();
    editor.setAddress("/go");
    const std::string id = editor.rules().front().id;
    REQUIRE(rig.runner.triggers().rule(0).enabled());

    // From outside the editor, the way `control::OscControl` reaches it.
    rig.runner.post(takt4::output::OutputCommand::ruleEnabled(id, false));
    rig.runner.post(takt4::output::OutputCommand::ruleMuted(id, true));
    editor.tick();

    CHECK_FALSE(editor.rules().front().enabled);
    CHECK_FALSE(editor.window().get_rule_enabled());
    CHECK(editor.window().get_rule_muted());
    CHECK_FALSE(editor.window().get_rules()->row_data(0)->enabled);
    // Enabled is configuration and is saved; the owner is told.
    REQUIRE_FALSE(saved.empty());
    CHECK_FALSE(saved.front().enabled);

    // And an edit afterwards leaves the rule where the surface put it.
    editor.rename("renamed after");
    CHECK_FALSE(rig.runner.triggers().rule(0).enabled());
    CHECK(rig.runner.triggers().rule(0).muted());

    // While ticking the box in the editor still switches it back on.
    editor.setEnabled(true);
    CHECK(rig.runner.triggers().rule(0).enabled());
}

TEST_CASE("the editor's switch still works on a rule a control surface switched",
          "[ui][trigger]") {
    // The audit of 2026-09-25, M10. The test above passes because it renames the rule before
    // ticking the box. Without that: a surface switches the rule, the editor takes the new state
    // in without posting it, and the operator's click posts the set with the switch at what the
    // *running* rule is already configured to — so `carryFrom` keeps the surface's live switch,
    // and the rule goes on firing behind an unticked box (or stays silent behind a ticked one).
    const bool surfaceTurnsOn = GENERATE(true, false);
    INFO((surfaceTurnsOn ? "the surface switched it on" : "the surface switched it off"));
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/strobe");
    const std::string id = editor.rules().front().id;
    editor.setEnabled(!surfaceTurnsOn);
    editor.tick();
    REQUIRE(rig.runner.triggers().rule(0).enabled() == !surfaceTurnsOn);

    rig.runner.post(takt4::output::OutputCommand::ruleEnabled(id, surfaceTurnsOn));
    editor.tick();
    REQUIRE(editor.window().get_rule_enabled() == surfaceTurnsOn);

    // The operator puts it back from the editor.
    editor.window().invoke_rule_enabled_changed(!surfaceTurnsOn);
    editor.tick();
    CHECK(rig.runner.triggers().rule(0).enabled() == !surfaceTurnsOn);
    CHECK(editor.rules().front().enabled == !surfaceTurnsOn);
    CHECK(editor.window().get_rule_enabled() == !surfaceTurnsOn);
}

TEST_CASE("a weighted generator is given its values and weights", "[ui][trigger]") {
    // The audit's M10: "weighted" was offered in the kind list with no way to say what to
    // weight, so the generator had nothing to draw from and sent zero on every fire.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/w");
    const int slot = 0; // the value, on an address with no placeholders
    const auto weighted = std::find(takt4::trigger::kGeneratorKinds.begin(),
                                    takt4::trigger::kGeneratorKinds.end(), GeneratorKind::Weighted) -
                          takt4::trigger::kGeneratorKinds.begin();
    editor.pickSlotKind(slot, static_cast<int>(weighted));
    editor.setSlotWeights(slot, "7:3, 12:1");

    const auto& choices = editor.rules().front().value.choices;
    REQUIRE(choices.size() == 2);
    CHECK(choices[0].value.asInt() == 7);
    CHECK(choices[0].weight == Approx(3.0));
    CHECK(choices[1].value.asInt() == 12);
    CHECK(choices[1].weight == Approx(1.0));
    const auto slots = editor.window().get_slots();
    REQUIRE(slots->row_count() == 1);
    CHECK(slots->row_data(0)->is_weighted);
    CHECK(std::string(slots->row_data(0)->weights) == "7:3, 12:1");

    // What it sends is those values, the first three times as often as the second.
    int sevens = 0;
    int twelves = 0;
    int other = 0;
    for (int i = 0; i < 400; ++i) {
        editor.test();
    }
    for (const OutputRunner::Fired& fired : rig.runner.takeFired()) {
        if (fired.message == "/w 7") {
            ++sevens;
        } else if (fired.message == "/w 12") {
            ++twelves;
        } else {
            ++other;
        }
    }
    INFO(sevens << " sevens, " << twelves << " twelves, " << other << " other");
    CHECK(other == 0);
    CHECK(sevens + twelves == 400);
    CHECK(sevens > 2 * twelves);
    CHECK(sevens < 4 * twelves);

    SECTION("a weight that is not a number is refused, and the list kept") {
        editor.setSlotWeights(slot, "7:lots");
        CHECK(editor.window().get_status_is_error());
        CHECK(editor.rules().front().value.choices.size() == 2);
    }
}

TEST_CASE("a normalised tempo is given the host's range", "[ui][trigger]") {
    // The audit's M10: "BPM normalised" maps the tempo onto 0 to 1 across a host's range —
    // Resolume's is 20 to 500 — and the range had no control, so no other host could use it.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/tempo");
    const auto live = std::find(takt4::trigger::kGeneratorKinds.begin(),
                                takt4::trigger::kGeneratorKinds.end(), GeneratorKind::Live) -
                      takt4::trigger::kGeneratorKinds.begin();
    const auto normalised =
        std::find(takt4::trigger::kLiveSources.begin(), takt4::trigger::kLiveSources.end(),
                  takt4::trigger::LiveSource::BpmNormalised) -
        takt4::trigger::kLiveSources.begin();
    editor.pickSlotKind(0, static_cast<int>(live));
    editor.pickSlotLive(0, static_cast<int>(normalised));
    const auto slots = editor.window().get_slots();
    REQUIRE(slots->row_count() == 1);
    CHECK(slots->row_data(0)->is_normalised);
    CHECK(std::string(slots->row_data(0)->normalise) == "20 - 500");

    editor.setSlotNormalise(0, "60 - 200");
    CHECK(editor.rules().front().value.normaliseLow == Approx(60.0));
    CHECK(editor.rules().front().value.normaliseHigh == Approx(200.0));
    CHECK(std::string(editor.window().get_slots()->row_data(0)->normalise) == "60 - 200");

    SECTION("upside down is refused, and the range kept") {
        editor.setSlotNormalise(0, "200 - 60");
        CHECK(editor.window().get_status_is_error());
        CHECK(editor.rules().front().value.normaliseLow == Approx(60.0));
    }
}

TEST_CASE("a lighting rule is not offered a follow-up that is the release again",
          "[ui][trigger][dmx]") {
    // The audit's M10: a lighting rule's follow-up list offered "release" and "DMX", and a DMX
    // follow-up with no effect of its own is the release — two entries doing one thing.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    const auto dmx = std::find(takt4::trigger::kMessageKinds.begin(),
                               takt4::trigger::kMessageKinds.end(),
                               takt4::trigger::Message::Kind::Dmx) -
                     takt4::trigger::kMessageKinds.begin();
    editor.pickSend(static_cast<int>(dmx));
    editor.addFollowUp();
    CHECK(editor.window().get_follow_kinds()->row_count() == 1);

    SECTION("while a note still has its real second gestures") {
        editor.pickSend(1); // note on
        CHECK(editor.window().get_follow_kinds()->row_count() > 1);
    }
}

TEST_CASE("a lighting rule's release says what it does to the light", "[ui][trigger][dmx]") {
    // The words came from the OSC branch: "release (same address)" and "the same address" on a
    // rule that has no address. And a move's release is skipped by `Rule::followUpsFor`, so on
    // a pan or tilt effect the row sends nothing and has to say so rather than offer a level
    // that does nothing. The engine's half is in dmx_rule_test.cpp ("a release with no effect
    // of its own dims what fired", "a release of a movement effect is skipped").
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    const auto indexIn = [](const auto& list, auto value) {
        return static_cast<int>(std::find(list.begin(), list.end(), value) - list.begin());
    };
    editor.pickSend(indexIn(takt4::trigger::kMessageKinds, takt4::trigger::Message::Kind::Dmx));
    editor.addFollowUp();
    const auto row = [&editor] {
        const auto rows = editor.window().get_follow_ups();
        REQUIRE(rows->row_count() == 1);
        return *rows->row_data(0);
    };

    CHECK(std::string(*editor.window().get_follow_kinds()->row_data(0)) ==
          "release (same fixtures)");
    // The default effect: a level on the dimmer.
    CHECK(std::string(row().summary) == "dimmer to this level, same fixtures");
    CHECK(row().takes_value);
    CHECK(std::string(row().value_label) == "level");

    SECTION("a color comes back at the release's level") {
        editor.pickEffect(indexIn(takt4::dmx::kEffectKinds, takt4::dmx::EffectKind::Color));
        CHECK(std::string(row().summary) == "the same color at this level, same fixtures");
        CHECK(row().takes_value);
    }

    SECTION("a flash on another channel releases that channel") {
        editor.pickEffect(indexIn(takt4::dmx::kEffectKinds, takt4::dmx::EffectKind::Flash));
        editor.pickRole(indexIn(takt4::dmx::kAimableRoles, takt4::dmx::Role::Red));
        CHECK(std::string(row().summary) == "red to this level, same fixtures");
    }

    SECTION("a move has no release, and the row says it sends nothing") {
        for (const auto move : {takt4::dmx::EffectKind::Position, takt4::dmx::EffectKind::Path,
                                takt4::dmx::EffectKind::Home}) {
            editor.pickEffect(indexIn(takt4::dmx::kEffectKinds, move));
            INFO("effect " << takt4::dmx::labelOf(move));
            const std::string summary(row().summary);
            CHECK(summary.find("sends nothing") != std::string::npos);
            CHECK_FALSE(row().takes_value);
        }
    }
}

TEST_CASE("the rate buttons are greyed out on a trigger with no count", "[ui][trigger]") {
    // The audit's M10. ÷2 and ×2 multiply how many beats or bars a rule counts; a downbeat, a
    // lock change or a manual press has no count, so pressing them changed a number nothing
    // read. Driven by real clicks: what is under test is the button's `enabled`.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/go");
    editor.pickTrigger(indexOf(Trigger::Bar));
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

    // The ÷2 button, found by what pressing it does on a counted trigger.
    float foundX = -1.0f;
    float foundY = -1.0f;
    for (float y = 100.0f; y < 260.0f && foundX < 0.0f; y += 6.0f) {
        for (float x = 560.0f; x < 1000.0f && foundX < 0.0f; x += 8.0f) {
            click(x, y);
            if (std::string(window.get_rule_rate()) == "2× faster" &&
                editor.rules().front().trigger == Trigger::Bar) {
                foundX = x;
                foundY = y;
            }
        }
    }
    INFO("no click landed on the halve button");
    REQUIRE(foundX >= 0.0f);
    editor.nudgeRate(2.0); // back to as written
    REQUIRE(std::string(window.get_rule_rate()).empty());

    editor.pickTrigger(indexOf(Trigger::Downbeat));
    editor.tick();
    click(foundX, foundY);
    INFO("halve at " << foundX << "," << foundY);
    CHECK(std::string(window.get_rule_rate()).empty());
}

namespace {

/// The rule editor, shown at its own size on the headless platform, with the two gestures the
/// M16 tests are made of.
struct Shown {
    /// At the window's own size, or taller — the THEN SEND rows are below the fold at 872, and a
    /// scrolled pane would put a click somewhere a person would not.
    explicit Shown(RulesController& editor, float height = takt4::ui::kRulesWindowHeight)
        : window(editor.window()) {
        window.show();
        window.window().dispatch_scale_factor_change_event(1.0f);
        window.window().dispatch_resize_event(
            slint::LogicalSize({takt4::ui::kRulesWindowWidth, height}));
        window.window().dispatch_window_active_changed_event(true);
    }
    /// Whatever Slint runs a loop late — a `changed has-focus`, a `changed` on a popup's
    /// `is-open` — run now, so a gesture's consequences are in before the next is made.
    static void settle() { slint::platform::update_timers_and_animations(); }
    void click(float x, float y) const {
        const slint::LogicalPosition at({x, y});
        window.window().dispatch_pointer_move_event(at);
        window.window().dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
        window.window().dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
        settle();
    }
    void type(const std::string& text) const {
        for (const char c : text) {
            const slint::SharedString key(std::string(1, c));
            window.window().dispatch_key_press_event(key);
            window.window().dispatch_key_release_event(key);
        }
        settle();
    }
    /// Enter, which commits the box that has the keyboard. Not Tab: from a generator chip's
    /// box on the headless platform a Tab moves nothing and commits nothing, and a probe that
    /// relied on it was credited to the next click instead — one step off every time.
    void enter() const { type("\n"); }
    void escape() const { type("\x1b"); }
    /// Everything in the box that has the keyboard gone — End, then Backspace until it is empty
    /// — so what is typed next is all it holds, wherever the click left the caret.
    void clearBox() const {
        key(u8""); // Key.End
        type(std::string(40, '\b'));
    }
    /// A key by its Slint name — `Key.UpArrow` is U+F700, `Key.DownArrow` U+F701.
    void key(const slint::SharedString& text) const {
        window.window().dispatch_key_press_event(text);
        window.window().dispatch_key_release_event(text);
        settle();
    }
    /// A notch of the mouse wheel over a point, the way a pane scrolled past a box delivers it.
    void wheel(float x, float y, float dy) const {
        const slint::LogicalPosition at({x, y});
        window.window().dispatch_pointer_move_event(at);
        window.window().dispatch_pointer_scroll_event(at, 0.0f, dy);
        settle();
    }
    RulesWindow& window;
};

/// Where a click lands in a box: the first point of a sweep at which `probe` (a click there,
/// then whatever the box is asked to do) says it hit. `reset` puts back whatever a miss did.
struct Spot {
    float x = -1.0f;
    float y = -1.0f;
    [[nodiscard]] bool found() const { return x >= 0.0f; }
};

template <typename Probe, typename Reset>
Spot sweep(float x0, float x1, float dx, float y0, float y1, float dy, Probe probe, Reset reset) {
    for (float y = y0; y < y1; y += dy) {
        for (float x = x0; x < x1; x += dx) {
            const bool hit = probe(x, y);
            reset();
            if (hit) {
                return {x, y};
            }
        }
    }
    return {};
}

} // namespace

TEST_CASE("clicking from one chip's box into the next keeps the second one", "[ui][trigger]") {
    // The audit's M16. Leaving the first box commits it, and a box that was typed into has to
    // come back as a new element — so the next redraw rebuilds the chips. If that rebuild tore
    // down the box the operator had just clicked into, what they typed next went nowhere.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/composition/layers/{layer}/clips/{clip}/connect");
    editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Fixed));
    editor.setSlotFixed(0, "1");
    editor.pickSlotKind(1, static_cast<int>(GeneratorKind::Fixed));
    editor.setSlotFixed(1, "1");
    editor.tick();
    const Shown shown(editor);
    editor.tick();

    // The {layer} box: the first spot where a digit and Enter change the layer.
    float layerX = -1.0f;
    float layerY = -1.0f;
    for (float y = 480.0f; y < 740.0f && layerX < 0.0f; y += 4.0f) {
        for (float x = 430.0f; x < 820.0f && layerX < 0.0f; x += 40.0f) {
            shown.click(x, y);
            shown.type("9");
            shown.enter();
            editor.tick();
            if (editor.rules().front().segments[0].fixed.asInt() != 1) {
                layerX = x;
                layerY = y;
            }
        }
    }
    {
        INFO("no click landed in the layer box");
        REQUIRE(layerX >= 0.0f);
    }
    editor.setSlotFixed(0, "1");
    editor.tick();

    // The {clip} box below it, found the same way.
    float clipX = -1.0f;
    float clipY = -1.0f;
    for (float y = layerY + 20.0f; y < layerY + 140.0f && clipX < 0.0f; y += 4.0f) {
        for (float x = 430.0f; x < 820.0f && clipX < 0.0f; x += 40.0f) {
            shown.click(x, y);
            shown.type("9");
            shown.enter();
            editor.tick();
            if (editor.rules().front().segments[1].fixed.asInt() != 1) {
                clipX = x;
                clipY = y;
            }
        }
    }
    {
        INFO("no click landed in the clip box");
        REQUIRE(clipX >= 0.0f);
    }
    // Both back to one: a probe that missed every box typed into whichever box still had the
    // keyboard, which is the layer's.
    editor.setSlotFixed(0, "1");
    editor.setSlotFixed(1, "1");
    editor.tick();
    REQUIRE(editor.rules().front().segments[0].fixed.asInt() == 1);

    // The gesture: a digit in the layer box, a click into the clip box, a redraw, and then a
    // digit and Enter there.
    shown.click(layerX, layerY);
    shown.type("4");
    shown.click(clipX, clipY);
    editor.tick();
    editor.tick();
    shown.type("7");
    shown.enter();
    editor.tick();
    INFO("layer at " << layerX << "," << layerY << "; clip at " << clipX << "," << clipY);
    CHECK(editor.rules().front().segments[0].fixed.asInt() == 14); // the layer was committed
    CHECK(editor.rules().front().segments[1].fixed.asInt() == 17); // and the clip box kept
}

TEST_CASE("a box typed into and then another rule clicked keeps the edit on its own rule",
          "[ui][trigger]") {
    // The audit's M16. Clicking a rule in the list does not take the keyboard off a box on its
    // own, so a box typed into kept its text across the switch — and committed it, when it did
    // lose the focus, into whichever rule was showing by then.
    Rig rig;
    RulesController editor(rig.runner, {});
    for (int i = 0; i < 2; ++i) {
        editor.add();
        editor.setAddress("/layer/{n}");
        editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Fixed));
        editor.setSlotFixed(0, "1");
    }
    editor.pick(0);
    editor.tick();
    const Shown shown(editor);
    editor.tick();

    float boxX = -1.0f;
    float boxY = -1.0f;
    for (float y = 400.0f; y < 740.0f && boxX < 0.0f; y += 4.0f) {
        for (float x = 430.0f; x < 820.0f && boxX < 0.0f; x += 40.0f) {
            shown.click(x, y);
            shown.type("9");
            shown.enter();
            editor.tick();
            if (editor.rules()[0].segments[0].fixed.asInt() != 1) {
                boxX = x;
                boxY = y;
            }
        }
    }
    {
        INFO("no click landed in the chip's box");
        REQUIRE(boxX >= 0.0f);
    }
    editor.setSlotFixed(0, "1");
    editor.tick();

    // The second rule in the list, found by clicking down its column.
    shown.click(boxX, boxY);
    shown.type("5");
    bool switched = false;
    for (float y = 60.0f; y < 300.0f && !switched; y += 4.0f) {
        shown.click(60.0f, y);
        switched = editor.selected() == 1;
    }
    REQUIRE(switched);
    editor.tick();
    editor.tick();
    // And away from everything, which is where a focus still in a box would finally commit.
    shown.click(700.0f, 40.0f);
    editor.tick();

    CHECK(editor.rules()[0].segments[0].fixed.asInt() != 1); // the edit went to its own rule
    CHECK(editor.rules()[1].segments[0].fixed.asInt() == 1); // and not to the one clicked
}

TEST_CASE("removing a swatch from its own picker lets the chips rebuild again",
          "[ui][trigger][dmx]") {
    // The audit's M16. REMOVE is inside the picker and closes it before taking the swatch
    // away — and the swatch's own "the picker is open" watcher goes with the swatch before it
    // can say the picker closed. Left believing a picker was open, the editor held every chip
    // rebuild for the rest of the session: the "editing one changes them all" bug again.
    //
    // **The first swatch and the last** (the audit of 2026-09-25, M20). The fix above held for
    // every swatch but the last: removing the first leaves the element that showed it standing
    // (it shows the next color now) and its watcher reports the close; removing the last takes
    // the last element away at once, watcher and all.
    const bool last = GENERATE(false, true);
    INFO((last ? "the last swatch" : "the first swatch"));
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    const auto dmx = std::find(takt4::trigger::kMessageKinds.begin(),
                               takt4::trigger::kMessageKinds.end(),
                               takt4::trigger::Message::Kind::Dmx) -
                     takt4::trigger::kMessageKinds.begin();
    editor.pickSend(static_cast<int>(dmx));
    const auto color = std::find(takt4::dmx::kEffectKinds.begin(), takt4::dmx::kEffectKinds.end(),
                                 takt4::dmx::EffectKind::Color) -
                       takt4::dmx::kEffectKinds.begin();
    editor.pickEffect(static_cast<int>(color));
    editor.addPaletteColor();
    editor.addPaletteColor();
    editor.tick();
    const Shown shown(editor);
    editor.tick();
    const auto swatches = editor.window().get_palette();
    REQUIRE(swatches->row_count() >= 2);
    const std::size_t before = swatches->row_count();

    // A swatch: the first click that opens a picker.
    float swatchX = -1.0f;
    float swatchY = -1.0f;
    for (float y = 300.0f; y < 740.0f && swatchX < 0.0f; y += 4.0f) {
        for (float x = 380.0f; x < 800.0f && swatchX < 0.0f; x += 6.0f) {
            shown.click(x, y);
            if (editor.pickerOpen()) {
                swatchX = x;
                swatchY = y;
            } else {
                shown.click(1050.0f, 740.0f); // close anything a miss opened
            }
        }
    }
    {
        INFO("no click opened a swatch's picker");
        REQUIRE(swatchX >= 0.0f);
    }
    // That is whichever swatch the sweep met first — the second of three, as it happens, which is
    // the one this test removed until 2026-09-26. Where the row starts is found by walking left to
    // the last point that still opens a picker; swatches are 40 px wide with 8 between, so the
    // one wanted is aimed at from there.
    float rowLeft = swatchX;
    for (float x = swatchX; x > std::max(240.0f, swatchX - 200.0f); x -= 3.0f) {
        shown.click(x, swatchY);
        if (editor.pickerOpen()) {
            rowLeft = x;
        }
        shown.click(1050.0f, 740.0f); // close it
    }
    swatchX = rowLeft + 20.0f + (last ? 48.0f * static_cast<float>(before - 1) : 0.0f);
    shown.click(swatchX, swatchY);
    {
        INFO("the row starts at " << rowLeft << "; aimed at " << swatchX << ", " << swatchY);
        REQUIRE(editor.pickerOpen());
    }
    shown.click(1050.0f, 740.0f);
    REQUIRE(swatches->row_count() == before);
    std::vector<takt4::trigger::Value> kept = editor.rules().front().dmx.color.values;
    REQUIRE(kept.size() == before);
    kept.erase(last ? kept.end() - 1 : kept.begin());

    // Its REMOVE: hunted inside the popup, which is wherever it fitted — so the picker is
    // opened again before every probe, since a probe that misses it closes it.
    bool removed = false;
    for (float y = swatchY - 360.0f; y < swatchY + 360.0f && !removed; y += 6.0f) {
        for (float x = swatchX - 40.0f; x < swatchX + 300.0f && !removed; x += 12.0f) {
            if (!editor.pickerOpen()) {
                shown.click(swatchX, swatchY);
                if (!editor.pickerOpen()) {
                    continue;
                }
            }
            shown.click(x, y);
            removed = swatches->row_count() < before;
        }
    }
    {
        INFO("no click inside the picker removed the swatch");
        REQUIRE(removed);
    }
    // The one that was meant — or the test is about some other swatch.
    CHECK(editor.rules().front().dmx.color.values == kept);
    editor.tick();
    editor.tick();
    CHECK_FALSE(editor.pickerOpen());
}

TEST_CASE("a slot's dropdown survives the redraws while it is open, and its pick lands",
          "[ui][trigger]") {
    // The audit's T3: of all the popups inside repeaters, only the two color pickers had a
    // test. A repeated row rebuilt while its dropdown is open takes the popup away under the
    // pointer — the color picker's crash in another shape. So the slot row's generator
    // dropdown is opened with the pointer, the editor redraws ten times while it is open, and
    // then an entry in its list is clicked, which only lands if the popup is still there.
    Rig rig;
    RulesController editor(rig.runner, {});
    auto& window = editor.window();
    window.show();
    window.window().dispatch_scale_factor_change_event(1.0f);
    window.window().dispatch_resize_event(
        slint::LogicalSize({takt4::ui::kRulesWindowWidth, takt4::ui::kRulesWindowHeight}));
    window.window().dispatch_window_active_changed_event(true);

    const auto settle = [] { slint::platform::update_timers_and_animations(); };
    const auto click = [&window, &settle](float x, float y) {
        const slint::LogicalPosition at({x, y});
        window.window().dispatch_pointer_move_event(at);
        window.window().dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
        window.window().dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
        settle();
    };
    const auto key = [&window](const slint::SharedString& text) {
        window.window().dispatch_key_press_event(text);
        window.window().dispatch_key_release_event(text);
    };
    // A rule with one slot, built afresh before every probe: a probe that misses lands on
    // whatever is underneath it.
    const auto build = [&] {
        editor.setRules({});
        editor.add();
        editor.setAddress("/fire/{what}");
        editor.tick();
        settle();
    };
    const auto kind = [&editor] { return editor.rules().front().segments.front().kind; };
    build();
    REQUIRE(editor.rules().front().segments.size() == 1);
    const GeneratorKind initial = kind();

    // The dropdown, found by what one step of it does: a click on it, one arrow key, and the
    // slot is on the next kind.
    float comboX = -1.0f;
    float comboY = -1.0f;
    for (float y = 440.0f; y < 720.0f && comboX < 0.0f; y += 8.0f) {
        for (float x = 300.0f; x < 620.0f && comboX < 0.0f; x += 16.0f) {
            click(x, y);
            key(u8"\uF701"); // Key.DownArrow
            key(u8"\u001b"); // Key.Escape
            settle();
            if (kind() != initial) {
                comboX = x;
                comboY = y;
            }
            build();
        }
    }
    {
        INFO("no click in the slot rows reached the generator dropdown");
        REQUIRE(comboX >= 0.0f);
    }

    // An entry in the list, found with nothing redrawing, and the kind it picks noted. The
    // popup opens wherever Slint finds room, so it is hunted above and below — and hunted
    // without the redraws, because a probe that misses lands on whatever is underneath, and
    // "the kind changed" alone is not "the popup was clicked".
    float entryY = -1.0f;
    GeneratorKind picked = initial;
    for (float dy = -320.0f; dy <= 320.0f && entryY < 0.0f; dy += 6.0f) {
        if (dy > -14.0f && dy < 14.0f) {
            continue; // the dropdown itself
        }
        build();
        click(comboX, comboY);
        click(comboX, comboY + dy);
        if (kind() != initial) {
            entryY = comboY + dy;
            picked = kind();
        }
    }
    {
        INFO("no click opened the dropdown and landed on its list");
        REQUIRE(entryY >= 0.0f);
    }

    // Then the gesture: opened, left open through ten redraws, and that same entry clicked.
    build();
    click(comboX, comboY);
    for (int redraw = 0; redraw < 10; ++redraw) {
        editor.tick();
        settle();
    }
    click(comboX, entryY);
    INFO("dropdown at " << comboX << ", " << comboY << "; entry at " << entryY);
    CHECK(kind() == picked);
}

namespace {

/// TEST, found by what it does: the first click near the top right after which the rule has
/// fired. The rule has to be one that can.
Spot findTest(RulesController& editor, const Shown& shown) {
    return sweep(
        860.0f, 1010.0f, 10.0f, 8.0f, 58.0f, 6.0f,
        [&](float x, float y) {
            shown.click(x, y);
            editor.tick();
            return !std::string(editor.window().get_last_fired()).empty();
        },
        [] {});
}

} // namespace

TEST_CASE("a number box answers Escape by putting itself back, and panics nothing",
          "[ui][trigger]") {
    // The audit of 2026-09-25, H2. The number boxes were std `SpinBox`es, whose text input takes
    // no control key — so Escape, pressed to back out of "every", went on up to the window, where
    // it is PANIC, and every rule halted with the lights frozen. Now Escape leaves the box
    // holding what it held, and PANIC is one more Escape away, as it is from a text box.
    //
    // And the rest of what the `SpinBox` got wrong, on the same box: Enter sets what was typed
    // and the box then says so (a `NumberBox` never writes its own value, so the controller has
    // to), TEST commits a number still being typed (M18), and the mouse wheel over it does
    // nothing (M19).
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.setAddress("/fire");
    editor.pickTrigger(indexOf(Trigger::Bar));
    editor.setEvery(4);
    editor.tick();
    const Shown shown(editor);
    editor.tick();
    const auto every = [&editor] { return editor.rules().front().every; };

    // The box: the first spot where a cleared box, a digit and Enter set the count.
    const Spot box = sweep(
        470.0f, 700.0f, 10.0f, 96.0f, 160.0f, 4.0f,
        [&](float x, float y) {
            shown.click(x, y);
            shown.clearBox();
            shown.type("7");
            shown.enter();
            return every() == 7;
        },
        [&] {
            editor.pickTrigger(indexOf(Trigger::Bar));
            editor.setEvery(4);
            if (rig.runner.panicked()) {
                editor.releasePanic();
            }
        });
    INFO("every at " << box.x << ", " << box.y);
    REQUIRE(box.found());
    REQUIRE(every() == 4);
    REQUIRE(editor.window().get_every() == 4);

    SECTION("Enter sets it, and the box says so") {
        shown.click(box.x, box.y);
        shown.clearBox();
        shown.type("12");
        shown.enter();
        CHECK(every() == 12);
        CHECK(editor.window().get_every() == 12);
    }

    SECTION("Escape puts it back and halts nothing, and the next Escape is PANIC") {
        shown.click(box.x, box.y);
        shown.clearBox();
        shown.type("9");
        shown.escape();
        CHECK_FALSE(rig.runner.panicked());
        CHECK(every() == 4);
        // What the box holds now, committed by a click in it and Enter: the 4, not the 9.
        shown.click(box.x, box.y);
        shown.enter();
        CHECK(every() == 4);
        // And the keyboard is back with the window, where Escape is still PANIC.
        shown.escape();
        CHECK(rig.runner.panicked());
    }

    SECTION("the mouse wheel over it changes nothing, with the keyboard in it or not") {
        shown.wheel(box.x, box.y, 40.0f);
        shown.wheel(box.x, box.y, 40.0f);
        CHECK(every() == 4);
        shown.click(box.x, box.y);
        shown.wheel(box.x, box.y, -40.0f);
        shown.wheel(box.x, box.y, -40.0f);
        shown.enter();
        CHECK(every() == 4);
    }
}

TEST_CASE("TEST sends on the MIDI channel still being typed", "[ui][trigger]") {
    // The audit of 2026-09-25, M18, for a number box: a channel typed and not entered, then
    // TEST. The box commits when it lets go of the keyboard — a turn of the event loop after the
    // click on TEST, which had fired the rule on the old channel by then. The channel is in
    // what goes out, so the log says which one TEST used.
    Rig rig;
    RulesController editor(rig.runner, {});
    const auto build = [&] {
        editor.setRules({});
        editor.add();
        editor.pickSend(1); // MIDI note
        editor.setChannel(3);
        editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Fixed));
        editor.setSlotFixed(0, "60");
        editor.tick();
        Shown::settle();
    };
    build();
    const Shown shown(editor);
    build();
    const auto channel = [&editor] { return editor.rules().front().channel; };
    const Spot box = sweep(
        380.0f, 900.0f, 12.0f, 360.0f, 520.0f, 4.0f,
        [&](float x, float y) {
            shown.click(x, y);
            shown.clearBox();
            shown.type("9");
            shown.enter();
            return channel() == 9;
        },
        [&] {
            if (channel() != 3 || editor.rules().front().sendKind !=
                                      takt4::trigger::Message::Kind::MidiNote) {
                build();
            }
        });
    INFO("channel box at " << box.x << ", " << box.y);
    REQUIRE(box.found());
    const Spot test = findTest(editor, shown);
    REQUIRE(test.found());
    REQUIRE(std::string(editor.window().get_last_fired()).find(" ch 3") != std::string::npos);

    shown.click(box.x, box.y);
    shown.clearBox();
    shown.type("5"); // no Enter
    shown.click(test.x, test.y);
    editor.tick();
    const std::string fired(editor.window().get_last_fired());
    INFO("fired: " << fired);
    CHECK(fired.find(" ch 5") != std::string::npos);
    CHECK(channel() == 5);
    CHECK(editor.window().get_channel() == 5);
}

namespace {

/// A MIDI note rule with one follow-up that has a number of its own — an explicit note off —
/// built afresh, so a probe that missed leaves nothing behind.
void buildNoteOff(RulesController& editor) {
    editor.setRules({});
    editor.add();
    editor.pickSend(1); // MIDI note
    editor.setChannel(3);
    editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Fixed));
    editor.setSlotFixed(0, "60");
    editor.addFollowUp();
    editor.pickFollowKind(0, 2); // "MIDI note off", past "release"
    editor.tick();
    Shown::settle();
}

/// The three boxes of that follow-up's row, found by typing a 9 into each and pressing Enter.
struct FollowBoxes {
    Spot number;
    Spot value;
    Spot delay;
};

FollowBoxes findFollowBoxes(RulesController& editor, const Shown& shown) {
    const auto owed = [&editor] { return editor.rules().front().followUps.front(); };
    const auto nine = [&](float x, float y) {
        shown.click(x, y);
        shown.clearBox();
        shown.type("9");
        shown.enter();
        editor.tick();
    };
    const auto rebuild = [&] { buildNoteOff(editor); };
    FollowBoxes boxes;
    // The number first, down the column it sits in: past the kind dropdown and its label.
    boxes.number = sweep(
        460.0f, 540.0f, 20.0f, 560.0f, 1460.0f, 6.0f,
        [&](float x, float y) {
            nine(x, y);
            return owed().number == 9;
        },
        rebuild);
    if (!boxes.number.found()) {
        return boxes;
    }
    // The other two along the same row.
    const float row = boxes.number.y;
    boxes.value = sweep(
        boxes.number.x + 60.0f, 900.0f, 8.0f, row, row + 1.0f, 1.0f,
        [&](float x, float y) {
            nine(x, y);
            return owed().value.asInt() == 9;
        },
        rebuild);
    boxes.delay = sweep(
        boxes.value.found() ? boxes.value.x + 40.0f : boxes.number.x + 60.0f, 1000.0f, 8.0f, row,
        row + 1.0f, 1.0f,
        [&](float x, float y) {
            nine(x, y);
            return owed().delayBeats == Approx(9.0);
        },
        rebuild);
    return boxes;
}

} // namespace

TEST_CASE("a follow-up's number takes every digit typed into it", "[ui][trigger]") {
    // The audit of 2026-09-25, M17. The number was a `SpinBox`, which sent every keystroke:
    // typing 60 sent the 6, the 6 changed the row, the next redraw built the row again — and the
    // 0 went into a box that no longer existed. The rule sent note off 6. So the gesture is the
    // operator's, with a redraw between each key the way a running show puts one there.
    Rig rig;
    RulesController editor(rig.runner, {});
    buildNoteOff(editor);
    const Shown shown(editor, 1500.0f);
    buildNoteOff(editor);
    REQUIRE(editor.rules().front().followUps.front().kind.has_value());
    const FollowBoxes boxes = findFollowBoxes(editor, shown);
    INFO("number at " << boxes.number.x << ", " << boxes.number.y);
    REQUIRE(boxes.number.found());
    const auto owed = [&editor] { return editor.rules().front().followUps.front(); };

    SECTION("typed, with redraws between the keys") {
        const auto watch = std::make_shared<ModelWatch>();
        editor.window().get_follow_ups()->attach_peer(watch);
        shown.click(boxes.number.x, boxes.number.y);
        shown.clearBox();
        shown.type("6");
        editor.tick();
        editor.tick();
        shown.type("0");
        editor.tick();
        shown.enter();
        editor.tick();
        CHECK(owed().number == 60);
        CHECK(watch->removed == 0); // the row was never built again under the keys
    }

    SECTION("stepped with the arrow keys, with a redraw between the steps") {
        shown.click(boxes.number.x, boxes.number.y);
        shown.clearBox();
        shown.type("10");
        shown.enter();
        REQUIRE(owed().number == 10);
        shown.click(boxes.number.x, boxes.number.y);
        shown.key(u8""); // Key.UpArrow
        editor.tick();
        editor.tick();
        shown.key(u8"");
        editor.tick();
        shown.enter();
        CHECK(owed().number == 12);
    }
}

TEST_CASE("a follow-up's value committed by clicking into its delay keeps the delay box",
          "[ui][trigger]") {
    // The audit of 2026-09-25, M17's last case. Leaving the value box commits it; the commit
    // changed the row; and the row was built again at the next redraw — with the delay box the
    // operator had just moved into, so what they typed there went nowhere. The boxes are
    // `LiveField`s now, told in place, and the row is left standing.
    Rig rig;
    RulesController editor(rig.runner, {});
    buildNoteOff(editor);
    const Shown shown(editor, 1500.0f);
    buildNoteOff(editor);
    const FollowBoxes boxes = findFollowBoxes(editor, shown);
    INFO("value at " << boxes.value.x << ", " << boxes.value.y << "; delay at " << boxes.delay.x
                     << ", " << boxes.delay.y);
    REQUIRE(boxes.value.found());
    REQUIRE(boxes.delay.found());
    const auto owed = [&editor] { return editor.rules().front().followUps.front(); };

    shown.click(boxes.value.x, boxes.value.y);
    shown.clearBox();
    shown.type("100");
    shown.click(boxes.delay.x, boxes.delay.y);
    editor.tick();
    editor.tick();
    shown.clearBox();
    shown.type("3");
    shown.enter();
    editor.tick();
    CHECK(owed().value.asInt() == 100);
    CHECK(owed().delayBeats == Approx(3.0));
}

TEST_CASE("a follow-up's value typed and then the row above removed stays with its own row",
          "[ui][trigger]") {
    // The audit of 2026-09-25, M15's shape in the rule editor. The rows are written in place, so
    // × on a row above hands the element below it the next follow-up — and the box typed into
    // commits by its index when it lets go of the keyboard, a turn of the event loop after the
    // click. The × commits what was typed first; the box's own late commit is then only an
    // echo of it (`RulesController::echo_`), and is dropped rather than sent to the next row.
    Rig rig;
    RulesController editor(rig.runner, {});
    // **The second and third hold the same value**, and that is the case that needs the echo.
    // Where the row that moves up holds something else, the box is told its new value before
    // its own late commit runs, and commits that. Where it holds the same, the box is told
    // nothing and still holds what was typed (measured: with distinct values this test passed
    // with the echo taken out).
    const auto build = [&] {
        buildNoteOff(editor);
        editor.addFollowUp();
        editor.addFollowUp();
        editor.setFollowValue(0, "10");
        editor.setFollowValue(1, "11");
        editor.setFollowValue(2, "11");
        editor.tick();
        Shown::settle();
    };
    build();
    const Shown shown(editor, 1500.0f);
    build();
    const auto value = [&editor](std::size_t i) {
        return editor.rules().front().followUps[i].value.asInt();
    };

    // The second row's value box and the first row's ×, found by what they do. The second row
    // is a release, with no number box, so its value sits further left than the first row's.
    // Steps a quarter of the box's width and height: every probe builds the rule again, and at
    // eight by four this sweep alone took over two minutes under ASan.
    const Spot box = sweep(
        440.0f, 900.0f, 16.0f, 560.0f, 1460.0f, 8.0f,
        [&](float x, float y) {
            shown.click(x, y);
            shown.clearBox();
            shown.type("9");
            shown.enter();
            editor.tick();
            return editor.rules().front().followUps.size() == 3 && value(1) == 9;
        },
        build);
    INFO("second row's value at " << box.x << ", " << box.y);
    REQUIRE(box.found());
    const Spot cross = sweep(
        800.0f, 1000.0f, 6.0f, box.y - 90.0f, box.y - 10.0f, 3.0f,
        [&](float x, float y) {
            shown.click(x, y);
            editor.tick();
            return editor.rules().front().followUps.size() == 2 && value(0) == 11;
        },
        build);
    INFO("first row's x at " << cross.x << ", " << cross.y);
    REQUIRE(cross.found());

    shown.click(box.x, box.y);
    shown.clearBox();
    shown.type("77"); // no Enter
    const slint::LogicalPosition at({cross.x, cross.y});
    editor.window().window().dispatch_pointer_move_event(at);
    editor.window().window().dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
    editor.window().window().dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
    // What the event loop does next, in the order it does it: the box's late commit, then a
    // redraw.
    Shown::settle();
    editor.tick();
    Shown::settle();
    REQUIRE(editor.rules().front().followUps.size() == 2);
    CHECK(value(0) == 77); // the row it was typed on
    CHECK(value(1) == 11); // and not the one that moved up under the box

    // And the × took the keyboard, as a button does: it used to take nothing, so the box
    // stayed lit over the row that moved up, still holding the 77, and what was typed next went
    // into that row.
    shown.type("5");
    shown.enter();
    editor.tick();
    CHECK(value(0) == 77);
    CHECK(value(1) == 11);
}

TEST_CASE("a chip's box typed into still shows what the controller sets afterwards",
          "[ui][trigger]") {
    // Why the chip rows stopped being rebuilt for their text (the audit of 2026-09-25, M17): the
    // boxes are `LiveField`s, which take what the row says whenever they are not being typed in
    // — where a `Field` bound one way went deaf at the first keystroke and showed that forever.
    // Seen the way the operator would meet it: a click in the box and Enter commit whatever it
    // is showing.
    Rig rig;
    RulesController editor(rig.runner, {});
    const auto build = [&] {
        editor.setRules({});
        editor.add();
        editor.setAddress("/deck/{clip}");
        editor.pickSlotKind(0, static_cast<int>(GeneratorKind::Shuffle));
        editor.setSlotRange(0, "1 - 8");
        editor.tick();
        Shown::settle();
    };
    build();
    const Shown shown(editor);
    build();
    const auto range = [&editor] {
        const auto& chip = editor.rules().front().segments.front();
        return std::make_pair(chip.low, chip.high);
    };

    const Spot box = sweep(
        430.0f, 820.0f, 30.0f, 440.0f, 760.0f, 4.0f,
        [&](float x, float y) {
            shown.click(x, y);
            shown.clearBox();
            shown.type("2 - 5");
            shown.enter();
            editor.tick();
            return range() == std::make_pair(2, 5);
        },
        [&] {
            if (range() != std::make_pair(1, 8) || editor.rules().front().address != "/deck/{clip}") {
                build();
            }
        });
    INFO("range box at " << box.x << ", " << box.y);
    REQUIRE(box.found());

    // Typed into, the way the operator does, and entered.
    shown.click(box.x, box.y);
    shown.clearBox();
    shown.type("2 - 6");
    shown.enter();
    editor.tick();
    REQUIRE(range() == std::make_pair(2, 6));
    // Then changed by the controller rather than by the box — as another rule's values, a kind
    // switched and back, or a preset would.
    editor.setSlotRange(0, "3 - 4");
    editor.tick();
    Shown::settle();
    // What the box shows, committed.
    shown.click(box.x, box.y);
    shown.enter();
    editor.tick();
    CHECK(range() == std::make_pair(3, 4));
}

TEST_CASE("+ ADD after typing into every leaves the new rule's count alone", "[ui][trigger]") {
    // The echo the other way: a number typed into "every" and not entered, then + ADD. The action
    // commits the 7 to the rule it was typed for, and the new rule is put in the box — whose late
    // commit, of the 7 still in it, went to the new rule.
    //
    // **The first rule starts at the count a new one gets.** Then the box's value comes back to
    // where it was by the end of the click, the box is told of no change, and it still holds the
    // 7 when it lets go — the case the echo is for. From any other count the box is told the new
    // rule's, and this passed with the echo taken out (measured).
    const std::uint32_t fresh = Rule::Config{}.every; // what + ADD gives a new rule
    Rig rig;
    RulesController editor(rig.runner, {});
    const auto build = [&] {
        editor.setRules({});
        editor.add();
        editor.pickTrigger(indexOf(Trigger::Bar));
        editor.setEvery(static_cast<int>(fresh));
        editor.tick();
        Shown::settle();
    };
    build();
    const Shown shown(editor);
    build();
    const Spot box = sweep(
        470.0f, 700.0f, 10.0f, 96.0f, 160.0f, 4.0f,
        [&](float x, float y) {
            shown.click(x, y);
            shown.clearBox();
            shown.type("7");
            shown.enter();
            return editor.rules().front().every == 7;
        },
        [&] {
            if (editor.rules().size() != 1 || editor.rules().front().every != fresh ||
                editor.rules().front().trigger != Trigger::Bar) {
                build();
            }
        });
    const Spot plus = sweep(
        150.0f, 210.0f, 6.0f, 8.0f, 44.0f, 4.0f,
        [&](float x, float y) {
            shown.click(x, y);
            return editor.rules().size() == 2;
        },
        [&] {
            if (editor.rules().size() != 1) {
                build();
            }
        });
    INFO("every at " << box.x << ", " << box.y << "; + at " << plus.x << ", " << plus.y);
    REQUIRE(box.found());
    REQUIRE(plus.found());
    build();
    REQUIRE(fresh != 7);

    shown.click(box.x, box.y);
    shown.clearBox();
    shown.type("7"); // no Enter
    const slint::LogicalPosition at({plus.x, plus.y});
    editor.window().window().dispatch_pointer_move_event(at);
    editor.window().window().dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
    editor.window().window().dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
    Shown::settle();
    editor.tick();
    Shown::settle();
    REQUIRE(editor.rules().size() == 2);
    CHECK(editor.rules()[0].every == 7);
    CHECK(editor.rules()[1].every == fresh);
}

TEST_CASE("TEST fires the address being typed, not the one before it", "[ui][trigger]") {
    // The audit of 2026-09-25, M18. A box commits when it loses the keyboard, a turn of the event
    // loop after the click that took it — so TEST posted the rule as it was, and a new rule sent
    // nothing at all. Every action now commits what is being typed before it runs.
    Rig rig;
    RulesController editor(rig.runner, {});
    const auto build = [&] {
        editor.setRules({});
        editor.add();
        editor.setAddress("/before");
        editor.tick();
        Shown::settle();
    };
    build();
    const Shown shown(editor);
    build();
    const Spot test = findTest(editor, shown);
    REQUIRE(test.found());

    const Spot address = sweep(
        300.0f, 900.0f, 100.0f, 360.0f, 640.0f, 4.0f,
        [&](float x, float y) {
            shown.click(x, y);
            shown.clearBox();
            shown.type("/probe");
            shown.enter();
            return editor.rules().front().address == "/probe";
        },
        [&] {
            if (editor.rules().front().address != "/before") {
                build();
            }
        });
    INFO("address at " << address.x << ", " << address.y);
    REQUIRE(address.found());
    build();

    shown.click(address.x, address.y);
    shown.clearBox();
    shown.type("/after");
    shown.click(test.x, test.y); // no Enter: the click is what finishes the edit
    editor.tick();
    const std::string fired(editor.window().get_last_fired());
    INFO("fired: " << fired);
    CHECK(fired.find("/after") != std::string::npos);
    CHECK(editor.rules().front().address == "/after");

    SECTION("and a republish from outside leaves what is being typed alone") {
        // The main window's outputs changing republishes the rule — and the address box is bound
        // both ways, so that wrote the stored address over the keystrokes.
        shown.click(address.x, address.y);
        shown.clearBox();
        shown.type("/typing");
        takt4::output::OutputTarget wall;
        wall.id = "o-000000aa";
        wall.name = "wall";
        wall.host = "127.0.0.1";
        wall.port = 7000;
        editor.setTargets({wall});
        editor.tick();
        shown.enter();
        CHECK(editor.rules().front().address == "/typing");
    }
}

TEST_CASE("+ ADD keeps what was typed into the rule it was typed for", "[ui][trigger]") {
    // The audit of 2026-09-25, M18: + ADD, MUTE, a follow-up's ×, a click on another rule — each
    // republished the rule, and the BPM box, bound both ways, took the stored range back over the
    // keystrokes. The box's late commit then sent that stored range, and the edit was gone.
    Rig rig;
    RulesController editor(rig.runner, {});
    const auto build = [&] {
        editor.setRules({});
        editor.add();
        editor.setBpmRange("any");
        editor.tick();
        Shown::settle();
    };
    build();
    const Shown shown(editor);
    build();
    const auto bpm = [&editor] {
        const auto& conditions = editor.rules().front().conditions;
        return std::make_pair(conditions.minBpm, conditions.maxBpm);
    };
    const auto anyBpm = bpm();

    const Spot box = sweep(
        330.0f, 520.0f, 20.0f, 250.0f, 400.0f, 4.0f,
        [&](float x, float y) {
            shown.click(x, y);
            shown.clearBox();
            shown.type("90 - 95");
            shown.enter();
            return bpm() == std::make_pair(90.0, 95.0);
        },
        [&] {
            if (bpm() != anyBpm || editor.rules().size() != 1) {
                build();
            }
        });
    INFO("BPM box at " << box.x << ", " << box.y);
    REQUIRE(box.found());
    const Spot plus = sweep(
        150.0f, 210.0f, 6.0f, 8.0f, 44.0f, 4.0f,
        [&](float x, float y) {
            shown.click(x, y);
            return editor.rules().size() == 2;
        },
        [&] {
            if (editor.rules().size() != 1) {
                build();
            }
        });
    INFO("+ at " << plus.x << ", " << plus.y);
    REQUIRE(plus.found());

    shown.click(box.x, box.y);
    shown.clearBox();
    shown.type("100 - 110");
    shown.click(plus.x, plus.y);
    editor.tick();
    REQUIRE(editor.rules().size() == 2);
    CHECK(bpm() == std::make_pair(100.0, 110.0)); // the first rule: the one it was typed into
    const auto& added = editor.rules().back().conditions;
    CHECK(std::make_pair(added.minBpm, added.maxBpm) == anyBpm); // and not the new one
}
