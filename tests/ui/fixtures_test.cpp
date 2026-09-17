// The lighting half of §5.9's editing, driven the way an operator drives it.
//
// The engine tests say what a fade does; these say that a fade can be *built* — that patching
// a moving head and aiming a rule at it is a sequence of clicks, and that what the clicks
// produce is the configuration the engine was tested against. That pairing is the whole point
// of a controller test: neither half on its own says the app works.

#include "core/dmx/dmx_engine.hpp"
#include "core/dmx/fixture.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/rule.hpp"
#include "ui/fixtures_controller.hpp"
#include "ui/rules_controller.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using takt4::dmx::Fixture;
using takt4::dmx::Role;
using takt4::engine::BeatEngine;
using takt4::output::OutputRunner;
using takt4::output::Transports;
using takt4::trigger::Rule;
using takt4::ui::FixturesController;
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

/// An engine and a **stopped** output runner, which is everything either controller needs.
/// Stopped is the point: the runner applies commands on the calling thread while it is not
/// running, so a test can read back what the output thread was told.
struct Rig {
    std::unique_ptr<BeatEngine> engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner{*engine, Transports::Config{}};
};

/// Where a built-in mode sits in the window's dropdown, which puts "custom" at 0.
int modeIndexOf(std::string_view name) {
    const auto modes = takt4::dmx::builtinModes();
    for (std::size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].name == name) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

int roleIndexOf(Role role) {
    for (std::size_t i = 0; i < takt4::dmx::kRoles.size(); ++i) {
        if (takt4::dmx::kRoles[i] == role) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

int effectIndexOf(takt4::dmx::EffectKind kind) {
    for (std::size_t i = 0; i < takt4::dmx::kEffectKinds.size(); ++i) {
        if (takt4::dmx::kEffectKinds[i] == kind) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

} // namespace

TEST_CASE("a rig is patched by clicking", "[ui][dmx]") {
    Rig rig;
    FixturesController patch(rig.runner, {});
    REQUIRE(patch.fixtures().empty());

    // 1. A fixture, which arrives usable rather than blank — an RGB par at universe 0
    //    channel 1, which is what a rig with nothing on it should get.
    patch.add();
    REQUIRE(patch.fixtures().size() == 1);
    CHECK(patch.selected() == 0);
    CHECK(takt4::dmx::problemWith(patch.fixtures()[0]).empty());

    // 2. Named, grouped, and told where it is.
    patch.rename("wash L");
    patch.setGroup("washes");
    patch.setAddress(1);
    CHECK(patch.fixtures()[0].name == "wash L");
    CHECK(patch.fixtures()[0].group == "washes");

    // 3. A second one, which lands *after* the first rather than on top of it. That is the
    //    arithmetic an operator would otherwise do by hand for every fixture in a row.
    patch.add();
    REQUIRE(patch.fixtures().size() == 2);
    CHECK(patch.fixtures()[1].address == 4); // the par occupies 1-3
    patch.rename("wash R");
    patch.setGroup("washes");

    // 4. And a moving head, which is a different shape — picked from the mode list rather
    //    than assembled a channel at a time.
    patch.add();
    patch.rename("head 1");
    patch.setGroup("heads");
    patch.setAddress(11);
    patch.pickMode(modeIndexOf("moving head 16-bit (12ch)"));

    const Fixture& head = patch.fixtures()[2];
    CHECK(head.channels.size() == 12);
    CHECK(takt4::dmx::channelOf(head, Role::Pan) == 11);
    CHECK(takt4::dmx::channelOf(head, Role::PanFine) == 12);
    CHECK(takt4::dmx::channelOf(head, Role::Dimmer) == 16);
    // The parked levels came with the mode, which is what makes the head respond at all: a
    // shutter left at zero emits nothing however hard a rule drives the dimmer.
    CHECK(head.parked[takt4::dmx::channelOf(head, Role::Strobe) - head.address] == 255);

    SECTION("picking a mode keeps everything the operator decided") {
        // "This fixture is shaped like that", not "start again".
        patch.pickMode(modeIndexOf("RGB (3ch)"));
        CHECK(patch.fixtures()[2].name == "head 1");
        CHECK(patch.fixtures()[2].group == "heads");
        CHECK(patch.fixtures()[2].address == 11);
        CHECK(patch.fixtures()[2].channels.size() == 3);
    }

    SECTION("the movement window is set from the stage, in percentages") {
        patch.setPanRange(20, 80);
        patch.setTiltRange(70, 45); // dragged past each other, which is an ordinary gesture
        CHECK(patch.fixtures()[2].panMin == 0.2);
        CHECK(patch.fixtures()[2].panMax == 0.8);
        // Sorted rather than refused: the two sliders are independent.
        CHECK(patch.fixtures()[2].tiltMin == 0.45);
        CHECK(patch.fixtures()[2].tiltMax == 0.7);
    }

    SECTION("a channel map can be edited one channel at a time") {
        patch.pickChannelRole(4, roleIndexOf(Role::Unused)); // the head's speed channel
        CHECK(patch.fixtures()[2].channels[4] == Role::Unused);
        patch.setChannelParked(4, 200);
        CHECK(patch.fixtures()[2].parked[4] == 200);
        patch.removeChannel(11);
        CHECK(patch.fixtures()[2].channels.size() == 11);
        // The parked levels stay the same length as the map, or a fixture patched later
        // would take its levels from the wrong channels.
        CHECK(patch.fixtures()[2].parked.size() == 11);
    }

    SECTION("a universe that is not one is refused rather than written half-typed") {
        patch.setUniverse("4");
        CHECK(patch.fixtures()[2].universe == 4);
        patch.setUniverse("not a universe");
        CHECK(patch.fixtures()[2].universe == 4); // unchanged, not zeroed on the way past
        patch.setUniverse("1:2:3");
        CHECK(patch.fixtures()[2].universe == 0x123);
    }

    SECTION("a duplicate lands after the fixture it came from, with a name of its own") {
        patch.pick(0);
        patch.duplicate();
        REQUIRE(patch.fixtures().size() == 4);
        CHECK(patch.fixtures()[1].name != "wash L");
        CHECK(patch.fixtures()[1].address == 4); // straight after the par at 1-3
        CHECK(patch.fixtures()[1].group == "washes");
    }

    SECTION("the patch reaches the output thread, not just this class") {
        // The runner is stopped, so the command was applied on this thread and the engine
        // behind it really holds the patch — which is what a rule's fixture mask resolves
        // against and what builds the universe buffers.
        const std::vector<takt4::dmx::Fixture>& live = rig.runner.transports().patch();
        REQUIRE(live.size() == 3);
        CHECK(live[2].name == "head 1");
        CHECK(rig.runner.transports().dmx().universes().size() == 1);
        CHECK_FALSE(rig.runner.transports().dmx().levels(0).empty());
    }
}

TEST_CASE("a lighting rule is built by clicking, against a patch", "[ui][dmx]") {
    Rig rig;
    FixturesController patch(rig.runner, {});
    patch.add();
    patch.rename("head 1");
    patch.setGroup("heads");
    patch.pickMode(modeIndexOf("moving head 16-bit (12ch)"));
    patch.setPanRange(25, 75);
    patch.add();
    patch.rename("head 2");
    patch.setGroup("heads");
    patch.pickMode(modeIndexOf("moving head 16-bit (12ch)"));

    RulesController editor(rig.runner, {});
    editor.setPatch(patch.fixtures());

    // 1. A rule, on every fourth bar.
    editor.add();
    editor.rename("Heads move on the phrase");
    editor.pickTrigger(1); // bars
    editor.setEvery(4);

    // 2. Sending DMX, which is the last entry of the send list.
    const int dmxSend = static_cast<int>(takt4::trigger::kMessageKinds.size()) - 1;
    REQUIRE(takt4::trigger::kMessageKinds[static_cast<std::size_t>(dmxSend)] ==
            takt4::trigger::Message::Kind::Dmx);
    editor.pickSend(dmxSend);

    // 3. Aimed at a *group*, which is one tick rather than one per head.
    editor.setFixtureChosen("heads", true);

    // 4. A random position over a bar.
    editor.pickEffect(effectIndexOf(takt4::dmx::EffectKind::Position));
    editor.pickDurationUnit(2); // bars
    editor.setDuration("1");
    // Pan drawn at random across the window the patch allows; tilt held.
    editor.pickSlotKind(0, 1); // random — `kGeneratorKinds[1]`
    REQUIRE(takt4::trigger::kGeneratorKinds[1] == takt4::trigger::GeneratorKind::Random);
    editor.setSlotRange(0, "0 - 100");
    editor.pickSlotKind(1, 4); // fixed
    editor.setSlotFixed(1, "40");

    const Rule::Config& built = editor.rules().front();
    CHECK(built.sendKind == takt4::trigger::Message::Kind::Dmx);
    CHECK(built.dmx.fixtures == std::vector<std::string>{"heads"});
    CHECK(built.dmx.effect == takt4::dmx::EffectKind::Position);
    CHECK(built.dmx.unit == takt4::trigger::DelayUnit::Bars);
    CHECK(built.dmx.durationBeats == 1.0);
    CHECK(built.dmx.pan.kind == takt4::trigger::GeneratorKind::Random);
    CHECK(built.dmx.pan.low == 0);
    CHECK(built.dmx.pan.high == 100);
    CHECK(built.dmx.tilt.fixed.asInt() == 40);
    // And it is a rule that can fire, which is the thing a list of fields does not say.
    CHECK(Rule(built).valid());

    SECTION("the rule's fixture names are resolved against the patch by the runner") {
        // Names become bits where the patch is known, which is neither the rule's business
        // nor the editor's. Two heads, so the mask is both.
        REQUIRE(rig.runner.triggers().ruleCount() == 1);
        CHECK(rig.runner.triggers().rule(0).fixtureMask() == 0b11);
    }

    SECTION("a rule that names nothing says so rather than reaching everything") {
        editor.chooseNoFixtures();
        const Rule rule(editor.rules().front());
        CHECK_FALSE(rule.valid());
        CHECK(rule.problem().find("fixture") != std::string::npos);
    }

    SECTION("the slots follow the effect, because each effect has its own generators") {
        // A position has pan and tilt; a fade has a level. Switching between them must not
        // leave a pan box bound to the level it used to be — which is what the rebuild in
        // `publishSlots` is for, and what this checks from the outside.
        editor.pickEffect(effectIndexOf(takt4::dmx::EffectKind::Level));
        editor.pickRole(0); // dimmer
        editor.setSlotFixed(0, "200");
        CHECK(editor.rules().front().dmx.level.fixed.asInt() == 200);
        // The pan generator the operator set earlier is untouched, so switching back brings
        // it with them rather than making them type it again.
        CHECK(editor.rules().front().dmx.pan.kind == takt4::trigger::GeneratorKind::Random);
    }

    SECTION("a color palette is a list on the color slot, and nothing more") {
        editor.pickEffect(effectIndexOf(takt4::dmx::EffectKind::Color));
        editor.pickSlotKind(0, 0); // shuffle — the default, and the first of the kinds
        editor.setSlotValues(0, "#ff2040, #20ff80, #2040ff");
        const takt4::trigger::Generator::Config& color = editor.rules().front().dmx.color;
        CHECK(color.kind == takt4::trigger::GeneratorKind::Shuffle);
        CHECK(color.pool == takt4::trigger::Pool::List);
        REQUIRE(color.values.size() == 3);
        CHECK(color.values[1].text() == "#20ff80");
    }

    SECTION("a fade out is a follow-up with a duration of its own") {
        // The operator's own first example: fade up on the downbeat, fade down two bars
        // later. The *delay* is when it starts and the *duration* is how long it runs, and
        // they are different numbers on the same row.
        editor.pickEffect(effectIndexOf(takt4::dmx::EffectKind::Level));
        editor.addFollowUp();
        REQUIRE(editor.rules().front().followUps.size() == 1);
        editor.pickFollowUnit(0, 2); // bars
        editor.setFollowDelay(0, "2");
        CHECK(editor.rules().front().followUps[0].delayBeats == 2.0);
        CHECK(editor.rules().front().followUps[0].unit == takt4::trigger::DelayUnit::Bars);
    }
}

TEST_CASE("mute and rate are live, and are not saved with the rule", "[ui][dmx]") {
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    const std::string id = editor.rules().front().id;

    editor.setMuted(true);
    REQUIRE(rig.runner.triggers().ruleCount() == 1);
    CHECK(rig.runner.triggers().rule(0).muted());
    // **Not in the configuration.** A preset that loaded silent would look exactly like a
    // preset that did not load, so this state lives on the live rule and nowhere else.
    CHECK(editor.rules().front().enabled); // untouched — mute is not disable

    editor.nudgeRate(2.0);
    CHECK(rig.runner.triggers().rule(0).rate() == 2.0);
    editor.nudgeRate(2.0);
    CHECK(rig.runner.triggers().rule(0).rate() == 4.0); // relative, so it compounds
    editor.nudgeRate(0.5);
    CHECK(rig.runner.triggers().rule(0).rate() == 2.0);

    SECTION("an edit to the rule does not reset them") {
        // `commit()` posts the whole rule set, which rebuilds every live rule — so an edit to
        // the *name* would silently unmute a rule if the states were not re-applied. That is
        // the trap this guards.
        editor.rename("renamed mid-set");
        CHECK(editor.rules().front().name == "renamed mid-set");
        CHECK(rig.runner.triggers().rule(0).muted());
        CHECK(rig.runner.triggers().rule(0).rate() == 2.0);
    }
    (void)id;
}

TEST_CASE("TEST drives one channel and puts it back", "[ui][dmx]") {
    // The patch editor's per-channel TEST, all the way through: the button, the controller, the
    // command queue, the engine, the universe buffer. Asked for on 2026-09-16 — "next to the
    // channel assignments, there should also be a test button that will temporarily send a test
    // value of your choice" — and it is the question IDENTIFY cannot answer: IDENTIFY flashes
    // the whole fixture and says which lamp this is, not whether channel 72 is really its blue.
    Rig rig;
    FixturesController patch(rig.runner, {});
    patch.add();
    patch.rename("Bedroom RGB");
    patch.setUniverse("5");
    patch.setAddress(70);
    patch.pickMode(modeIndexOf("RGB (3ch)"));
    REQUIRE(patch.fixtures().size() == 1);
    REQUIRE(patch.fixtures().front().channels.size() == 3);

    // `Transports::dmx()` hands back a non-const engine from a const `Transports`, which is
    // what lets this test advance the lighting clock without a thread running.
    takt4::dmx::DmxEngine& engine = rig.runner.transports().dmx();
    REQUIRE(!engine.levels(5).empty());
    REQUIRE(engine.levels(5)[71] == 0); // channel 72: the blue one

    // Not full, so the check cannot pass on a value something else would have written.
    patch.setTestLevel(180);
    patch.testChannel(2); // the third channel of the map, which is DMX 72
    CHECK(engine.levels(5)[71] == 180);
    // And only that one.
    CHECK(engine.levels(5)[69] == 0);
    CHECK(engine.levels(5)[70] == 0);

    SECTION("and it lets go on its own") {
        // The hold is `FixturesController::kTestSeconds` long on the runner's clock, so this
        // asks the engine directly rather than waiting three seconds of wall clock.
        engine.tick(FixturesController::kTestSeconds + 1.0);
        CHECK(engine.levels(5)[71] == 0);
    }

    SECTION("a channel index that is not on the fixture does nothing") {
        patch.testChannel(9);
        CHECK(engine.levels(5)[71] == 180); // the one held before is still the only one
    }
}
