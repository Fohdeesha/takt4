// The lighting half of §5.9's editing.
//
// The engine tests say what a fade does; these say that a fade can be *built* — that patching
// a moving head and aiming a rule at it is a sequence of window callbacks, and that what they
// produce is the configuration the engine was tested against. That pairing is the whole point
// of a controller test: neither half on its own says the app works.
//
// **A callback is not a click.** The two long tests below invoke the window's own callbacks,
// which proves the wiring from the markup's callback to the controller and nothing about the
// markup itself — they used to be called "by clicking" and called the controller's methods
// directly (the audit's T1). What is proved by real pointer and key events is at the bottom
// of the file: the list's buttons, the name box, a channel's dropdown.

#include "core/dmx/dmx_engine.hpp"
#include "core/dmx/fixture.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/fixtures/fixture_library.hpp"
#include "core/fixtures/fixture_profile.hpp"
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/rule.hpp"
#include "ui/delete_guard.hpp"
#include "ui/fixtures_controller.hpp"
#include "ui/keys.hpp"
#include "ui/model_watch.hpp"
#include "ui/nothing_real.hpp"
#include "ui/shot.hpp"
#include "ui/rules_controller.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <slint-platform.h>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using takt4::dmx::Fixture;
using takt4::dmx::Role;
using takt4::engine::BeatEngine;
using takt4::output::OutputRunner;
using takt4::output::Transports;
using takt4::tests::ModelWatch;
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

/// A text box's edit as the window sends it: the keystrokes, then the edit finished. A finished
/// edit with nothing typed is dropped — it is only a box letting go of what it was shown, which may
/// be another fixture's by then (`FixturesController::typed_`).
void enterName(FixturesWindow& window, const char* text) {
    window.invoke_name_typed(text);
    window.invoke_name_edited(text);
}
void enterGroup(FixturesWindow& window, const char* text) {
    window.invoke_group_typed(text);
    window.invoke_group_edited(text);
}
void enterUniverse(FixturesWindow& window, const char* text) {
    window.invoke_universe_typed(text);
    window.invoke_universe_edited(text);
}
/// And a number box's.
void enterAddress(FixturesWindow& window, int address) {
    window.invoke_address_typed(address);
    window.invoke_address_changed(address);
}
void enterParked(FixturesWindow& window, int index, int level) {
    window.invoke_channel_parked_typed(index, level);
    window.invoke_channel_parked_changed(index, level);
}
void enterTestLevel(FixturesWindow& window, int level) {
    window.invoke_test_level_typed(level);
    window.invoke_test_level_changed(level);
}

} // namespace

TEST_CASE("picking custom as a fixture's mode says how a custom map is made", "[ui][dmx]") {
    // The audit of 2026-09-25, L39. "custom" is what an edited channel map shows as, and
    // picking it did nothing — the dropdown went on saying "custom" over a fixture that was
    // still an RGB par. Picked as the dropdown picks it: its index written through the
    // two-way binding, then the callback.
    Rig rig;
    FixturesController patch(rig.runner, {});
    auto& window = patch.window();
    window.invoke_added(); // an RGB par
    const int rgb = window.get_mode_index();
    REQUIRE(rgb > 0);
    const std::vector<Role> before = patch.fixtures()[0].channels;

    window.set_mode_index(0);
    window.invoke_mode_picked(0);
    CHECK(window.get_mode_index() == rgb);             // it still says what the map is
    CHECK(patch.fixtures()[0].channels == before);     // and nothing was changed
    const std::string said(window.get_status());
    INFO(said);
    CHECK(said.find("Edit the channels below") != std::string::npos);
    CHECK_FALSE(window.get_status_error());
    // And the entry says what it is where it is offered.
    CHECK(std::string(*window.get_modes()->row_data(0)).find("the channels below") !=
          std::string::npos);
}

TEST_CASE("a rig is patched through the window's callbacks", "[ui][dmx]") {
    Rig rig;
    FixturesController patch(rig.runner, {});
    REQUIRE(patch.fixtures().empty());
    auto& window = patch.window();

    // 1. A fixture, which arrives usable rather than blank — an RGB par at universe 0
    //    channel 1, which is what a rig with nothing on it should get.
    window.invoke_added();
    REQUIRE(patch.fixtures().size() == 1);
    CHECK(patch.selected() == 0);
    CHECK(takt4::dmx::problemWith(patch.fixtures()[0]).empty());

    // 2. Named, grouped, and told where it is.
    enterName(window, "wash L");
    enterGroup(window, "washes");
    enterAddress(window, 1);
    CHECK(patch.fixtures()[0].name == "wash L");
    CHECK(patch.fixtures()[0].group == "washes");

    // 3. A second one, which lands *after* the first rather than on top of it. That is the
    //    arithmetic an operator would otherwise do by hand for every fixture in a row.
    window.invoke_added();
    REQUIRE(patch.fixtures().size() == 2);
    CHECK(patch.fixtures()[1].address == 4); // the par occupies 1-3
    enterName(window, "wash R");
    enterGroup(window, "washes");

    // 4. And a moving head, which is a different shape — picked from the mode list rather
    //    than assembled a channel at a time.
    window.invoke_added();
    enterName(window, "head 1");
    enterGroup(window, "heads");
    enterAddress(window, 11);
    window.invoke_mode_picked(modeIndexOf("moving head 16-bit (12ch)"));

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
        window.invoke_mode_picked(modeIndexOf("RGB (3ch)"));
        CHECK(patch.fixtures()[2].name == "head 1");
        CHECK(patch.fixtures()[2].group == "heads");
        CHECK(patch.fixtures()[2].address == 11);
        CHECK(patch.fixtures()[2].channels.size() == 3);
    }

    SECTION("the movement window is set from the stage, in percentages") {
        window.invoke_pan_range_changed(20, 80);
        window.invoke_tilt_range_changed(70, 45); // dragged past each other, an ordinary gesture
        CHECK(patch.fixtures()[2].panMin == 0.2);
        CHECK(patch.fixtures()[2].panMax == 0.8);
        // Sorted rather than refused: the two sliders are independent.
        CHECK(patch.fixtures()[2].tiltMin == 0.45);
        CHECK(patch.fixtures()[2].tiltMax == 0.7);
    }

    SECTION("a channel map can be edited one channel at a time") {
        window.invoke_channel_role_picked(4, roleIndexOf(Role::Unused)); // the speed channel
        CHECK(patch.fixtures()[2].channels[4] == Role::Unused);
        enterParked(window, 4, 200);
        CHECK(patch.fixtures()[2].parked[4] == 200);
        window.invoke_channel_removed(11);
        CHECK(patch.fixtures()[2].channels.size() == 11);
        // The parked levels stay the same length as the map, or a fixture patched later
        // would take its levels from the wrong channels.
        CHECK(patch.fixtures()[2].parked.size() == 11);
    }

    SECTION("a universe that is not one is refused rather than written half-typed") {
        enterUniverse(window, "4");
        CHECK(patch.fixtures()[2].universe == 4);
        enterUniverse(window, "not a universe");
        CHECK(patch.fixtures()[2].universe == 4); // unchanged, not zeroed on the way past
        enterUniverse(window, "1:2:3");
        CHECK(patch.fixtures()[2].universe == 0x123);
    }

    SECTION("a duplicate lands after the fixture it came from, with a name of its own") {
        window.invoke_picked(0);
        window.invoke_duplicated_at(0);
        REQUIRE(patch.fixtures().size() == 4);
        CHECK(patch.fixtures()[1].name != "wash L");
        CHECK(patch.fixtures()[1].address == 4); // straight after the par at 1-3
        CHECK(patch.fixtures()[1].group == "washes");
        // A fixture of its own: a rule aimed at the original does not reach the copy.
        CHECK_FALSE(patch.fixtures()[1].id.empty());
        CHECK(patch.fixtures()[1].id != patch.fixtures()[0].id);
    }

    SECTION("renaming a fixture leaves the rules aimed at it reaching it") {
        // The audit's M28, and the operator's report of 2026-09-23 about outputs: a rule aimed
        // at a fixture by name stopped reaching it the moment it was renamed.
        const std::string id = patch.fixtures()[0].id;
        REQUIRE_FALSE(id.empty());
        window.invoke_picked(0);
        enterName(window, "front wash");
        CHECK(patch.fixtures()[0].id == id);
        CHECK(takt4::dmx::resolveFixtures(patch.fixtures(), {id}) == 0b001);
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

TEST_CASE("a lighting rule is built through the window's callbacks, against a patch",
          "[ui][dmx]") {
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
    auto& rules = editor.window();

    // 1. A rule, on every fourth bar.
    rules.invoke_rule_added();
    rules.invoke_rule_renamed("Heads move on the phrase");
    rules.invoke_trigger_picked(1); // bars
    rules.invoke_every_changed(4);

    // 2. Sending DMX, which is the last entry of the send list.
    const int dmxSend = static_cast<int>(takt4::trigger::kMessageKinds.size()) - 1;
    REQUIRE(takt4::trigger::kMessageKinds[static_cast<std::size_t>(dmxSend)] ==
            takt4::trigger::Message::Kind::Dmx);
    rules.invoke_send_picked(dmxSend);

    // 3. Aimed at a *group*, which is one tick rather than one per head.
    rules.invoke_fixture_chosen("heads", true);

    // 4. A random position over a bar.
    rules.invoke_effect_picked(effectIndexOf(takt4::dmx::EffectKind::Position));
    rules.invoke_effect_unit_picked(2); // bars
    rules.invoke_effect_duration_edited("1");
    // Pan drawn at random across the window the patch allows; tilt held.
    rules.invoke_slot_kind_picked(0, 1); // random — `kGeneratorKinds[1]`
    REQUIRE(takt4::trigger::kGeneratorKinds[1] == takt4::trigger::GeneratorKind::Random);
    rules.invoke_slot_range_edited(0, "0 - 100");
    rules.invoke_slot_kind_picked(1, 4); // fixed
    rules.invoke_slot_fixed_edited(1, "40");

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
        rules.invoke_no_fixtures_chosen();
        const Rule rule(editor.rules().front());
        CHECK_FALSE(rule.valid());
        CHECK(rule.problem().find("fixture") != std::string::npos);
    }

    SECTION("the slots follow the effect, because each effect has its own generators") {
        // A position has pan and tilt; a fade has a level. Switching between them must not
        // leave a pan box bound to the level it used to be — which is what the rebuild in
        // `publishSlots` is for, and what this checks from the outside.
        rules.invoke_effect_picked(effectIndexOf(takt4::dmx::EffectKind::Level));
        rules.invoke_effect_role_picked(0); // dimmer
        rules.invoke_slot_fixed_edited(0, "200");
        CHECK(editor.rules().front().dmx.level.fixed.asInt() == 200);
        // The pan generator the operator set earlier is untouched, so switching back brings
        // it with them rather than making them type it again.
        CHECK(editor.rules().front().dmx.pan.kind == takt4::trigger::GeneratorKind::Random);
    }

    SECTION("a color palette is a list on the color slot, and nothing more") {
        rules.invoke_effect_picked(effectIndexOf(takt4::dmx::EffectKind::Color));
        rules.invoke_slot_kind_picked(0, 0); // shuffle — the default, and the first of the kinds
        rules.invoke_slot_values_edited(0, "#ff2040, #20ff80, #2040ff");
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
        rules.invoke_effect_picked(effectIndexOf(takt4::dmx::EffectKind::Level));
        rules.invoke_follow_up_added();
        REQUIRE(editor.rules().front().followUps.size() == 1);
        rules.invoke_follow_unit_picked(0, 2); // bars
        rules.invoke_follow_delay_edited(0, "2");
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
    // The patch editor's per-channel TEST, all the way through: the button's callback, the
    // controller, the command queue, the engine, the universe buffer. (It said "the button" and
    // called the controller, which skips the wiring a click goes through — the audit of
    // 2026-09-25, T13.) Asked for on 2026-09-16 — "next to the
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
    enterTestLevel(patch.window(), 180);
    patch.window().invoke_channel_tested(2); // the third channel of the map, which is DMX 72
    CHECK(engine.levels(5)[71] == 180);
    // And only that one.
    CHECK(engine.levels(5)[69] == 0);
    CHECK(engine.levels(5)[70] == 0);

    SECTION("and it lets go on its own") {
        // The hold is `FixturesController::kTestSeconds` long on the runner's clock, so this
        // asks the engine directly rather than waiting three seconds of wall clock — at a time
        // on that clock past the hold's end. It used to tick at 4 s flat, which is past the end
        // only for a TEST pressed in the runner's first second: under AddressSanitizer a slow
        // set-up pressed it later, and the hold was still on (measured, 2026-10-04).
        engine.tick(rig.runner.elapsed() + FixturesController::kTestSeconds + 1.0);
        CHECK(engine.levels(5)[71] == 0);
    }

    SECTION("a channel index that is not on the fixture does nothing") {
        patch.testChannel(9);
        CHECK(engine.levels(5)[71] == 180); // the one held before is still the only one
    }
}

TEST_CASE("an imported fixture's channel names follow its channels through every edit",
          "[ui][dmx][import]") {
    // Nothing in the window imports yet; the fixtures an import makes are made here by the
    // library itself and handed to the editor, whose own edits must keep each channel's name
    // beside its channel and the link to the definition honest.
    using takt4::fixtures::FixtureProfile;
    using takt4::fixtures::ProfileChannel;
    using takt4::fixtures::ProfileMode;
    FixtureProfile profile;
    profile.format = "gdtf";
    profile.key = "KEY";
    profile.model = "Bar";
    ProfileMode single;
    single.name = "A";
    single.parts = {{ProfileChannel{Role::Dimmer, "dimmer", 0, {}},
                     ProfileChannel{Role::Red, "red · Cell 1", 0, {}},
                     ProfileChannel{Role::Green, "green · Cell 1", 0, {}},
                     ProfileChannel{Role::Blue, "blue · Cell 1", 0, {}}}};
    ProfileMode pair;
    pair.name = "Pair";
    pair.parts = {{ProfileChannel{Role::Dimmer, "dimmer", 0, {}}},
                  {ProfileChannel{Role::Red, "red · Cell 2", 0, {}}}};
    profile.modes = {single, pair};
    std::vector<FixtureProfile> library;
    takt4::fixtures::addProfile(library, profile);
    std::vector<Fixture> fixtures =
        takt4::fixtures::makeFixtures(library[0], "A", 1, 0, 1, {}).fixtures;
    const auto two = takt4::fixtures::makeFixtures(library[0], "Pair", 1, 0, 20, fixtures).fixtures;
    fixtures.insert(fixtures.end(), two.begin(), two.end());
    REQUIRE(fixtures.size() == 3);

    Rig rig;
    FixturesController patch(rig.runner, fixtures);
    FixturesWindow& window = patch.window();
    window.invoke_picked(0);

    SECTION("a channel removed takes its name with it") {
        window.invoke_channel_removed(1);
        CHECK(patch.fixtures()[0].labels ==
              std::vector<std::string>{"dimmer", "green · Cell 1", "blue · Cell 1"});
        CHECK(patch.fixtures()[0].parked.size() == 3);
    }
    SECTION("a channel added has no name, and the others keep theirs") {
        window.invoke_channel_added();
        CHECK(patch.fixtures()[0].labels == std::vector<std::string>{"dimmer", "red · Cell 1",
                                                                     "green · Cell 1",
                                                                     "blue · Cell 1", ""});
        // Still from the definition, and now an edit of its mode.
        CHECK(patch.fixtures()[0].profile.linked());
        CHECK(takt4::fixtures::isEdited(patch.fixtures()[0], library[0]));
    }
    SECTION("a built-in shape is not the definition's mode") {
        window.invoke_mode_picked(modeIndexOf("RGB (3ch)"));
        CHECK(patch.fixtures()[0].labels.empty());
        CHECK_FALSE(patch.fixtures()[0].profile.linked());
    }
    SECTION("a copy of a fixture stays linked; a copy of one of a pair leaves the pair") {
        window.invoke_duplicated_at(0);
        REQUIRE(patch.fixtures().size() == 4);
        CHECK(patch.fixtures()[1].profile == patch.fixtures()[0].profile);
        CHECK(patch.fixtures()[1].labels == patch.fixtures()[0].labels);
        window.invoke_duplicated_at(3); // "Bar 1 · part 2"
        REQUIRE(patch.fixtures().size() == 5);
        CHECK_FALSE(patch.fixtures()[4].profile.linked());
        CHECK(patch.fixtures()[4].labels == std::vector<std::string>{"red · Cell 2"});
        // The pair itself is untouched: still two, sharing their id.
        CHECK(patch.fixtures()[2].profile.pair == patch.fixtures()[3].profile.pair);
        CHECK_FALSE(patch.fixtures()[3].profile.pair.empty());
    }
}

TEST_CASE("a fixture with more than one head says which head each pan and tilt moves",
          "[ui][dmx][heads]") {
    // What a rule's "heads" ticks mean on this fixture, counted as the engine counts them: head n
    // is the nth pan and the nth tilt, each with its nth fine, and a pan the heads share is the
    // first's (`dmx::headsOf`). A fixture with one head, or none, has no such column.
    Fixture twin;
    twin.id = "f-twin";
    twin.name = "twin";
    twin.address = 1;
    twin.channels = {Role::Dimmer, Role::Pan,     Role::PanFine, Role::Tilt,    Role::TiltFine,
                     Role::Pan,    Role::PanFine, Role::Tilt,    Role::TiltFine};
    twin.parked.assign(twin.channels.size(), 0);
    Fixture yoke;
    yoke.id = "f-yoke";
    yoke.name = "yoke";
    yoke.address = 20;
    yoke.channels = {Role::Pan, Role::Tilt, Role::Tilt, Role::Dimmer};
    yoke.parked.assign(yoke.channels.size(), 0);
    Fixture head = takt4::dmx::fixtureFromMode("head", 6, 0, 40);
    head.id = "f-head";

    Rig rig;
    FixturesController patch(rig.runner, {twin, yoke, head});
    FixturesWindow& window = patch.window();
    const auto heads = [&window] {
        std::vector<int> out;
        const auto rows = window.get_channels();
        for (std::size_t i = 0; i < rows->row_count(); ++i) {
            out.push_back(rows->row_data(i)->head);
        }
        return out;
    };

    window.invoke_picked(0);
    CHECK(window.get_has_heads());
    CHECK(heads() == std::vector<int>{0, 1, 1, 1, 1, 2, 2, 2, 2});

    window.invoke_picked(1);
    CHECK(window.get_has_heads());
    CHECK(heads() == std::vector<int>{1, 1, 2, 0}); // the one pan is the first head's

    window.invoke_picked(2);
    CHECK_FALSE(window.get_has_heads());
    CHECK(heads() == std::vector<int>(head.channels.size(), 0));

    SECTION("a pan made something else by hand is no head's, and the count follows") {
        window.invoke_picked(0);
        const auto dimmer =
            std::find(takt4::dmx::kRoles.begin(), takt4::dmx::kRoles.end(), Role::Dimmer) -
            takt4::dmx::kRoles.begin();
        window.invoke_channel_role_picked(1, static_cast<int>(dimmer));
        // The second pan is the first now: head 1's pan is on channel 6, and the tilts are as
        // they were.
        CHECK(heads() == std::vector<int>{0, 0, 1, 1, 1, 1, 2, 2, 2});
        CHECK(window.get_has_heads()); // two tilts: still two heads
    }
}

TEST_CASE("TEST and IDENTIFY say PANIC is engaged instead of lighting anything",
          "[ui][dmx]") {
    // The audit's M17: both went to the lights past the rules, and so past a panic. The runner
    // drops them now; this is the window saying why, rather than a button that did nothing.
    Rig rig;
    FixturesController patch(rig.runner, {});
    patch.add();
    patch.setUniverse("5");
    patch.setAddress(70);
    patch.pickMode(modeIndexOf("RGB (3ch)"));
    takt4::dmx::DmxEngine& engine = rig.runner.transports().dmx();
    rig.runner.post(takt4::output::OutputCommand::panic(true));

    patch.setTestLevel(180);
    patch.testChannel(2);
    CHECK(engine.levels(5)[71] == 0);
    CHECK(std::string(patch.window().get_status()).find("PANIC") != std::string::npos);
    CHECK(patch.window().get_status_error());

    patch.identify();
    CHECK(engine.running() == 0);
    CHECK(engine.levels(5)[69] == 0);
    CHECK(std::string(patch.window().get_status()).find("PANIC") != std::string::npos);
}

TEST_CASE("IDENTIFY puts a lit fixture back as it found it", "[ui][dmx]") {
    // The audit's L1: IDENTIFY flashed every emitter from full down to 0, which is where it
    // found a dark fixture only. A lit one went out; an RGB par, whose colour is kept apart from
    // its brightness, lost its colour, so the next dimmer flash came up white.
    using takt4::dmx::EffectKind;
    using takt4::dmx::Payload;
    Rig rig;
    FixturesController patch(rig.runner, {});
    patch.add();
    patch.setUniverse("5");
    patch.setAddress(70); // red, green, blue on DMX 70-72
    takt4::dmx::DmxEngine& engine = rig.runner.transports().dmx();
    const auto at = [&engine](int channel) {
        return int{engine.levels(5)[static_cast<std::size_t>(channel - 1)]};
    };
    const auto settle = [&](double seconds) {
        const double start = rig.runner.elapsed();
        engine.tick(start + seconds);
        return start;
    };

    SECTION("an RGB par lit red") {
        patch.pickMode(modeIndexOf("RGB (3ch)"));
        Payload red;
        red.kind = EffectKind::Color;
        red.color = {255, 0, 0};
        rig.runner.post(takt4::output::OutputCommand::effect(1, red));
        settle(0.0);
        REQUIRE(at(70) == 255);
        REQUIRE(at(71) == 0);

        patch.identify();
        const double start = settle(0.05);
        CHECK(at(71) > 200); // it does flash: green and blue come up
        CHECK(at(72) > 200);
        engine.tick(start + FixturesController::kIdentifySeconds + 1.0);
        CHECK(at(70) == 255); // and it is red again, not dark
        CHECK(at(71) == 0);
        CHECK(at(72) == 0);

        // Its colour survived too: a flash on the dimmer it has not got is a red flash.
        Payload flash;
        flash.kind = EffectKind::Flash;
        flash.role = takt4::dmx::Role::Dimmer;
        flash.durationSeconds = 1.0f;
        rig.runner.post(takt4::output::OutputCommand::effect(1, flash));
        settle(0.0);
        CHECK(at(70) == 255);
        CHECK(at(71) == 0);
        CHECK(at(72) == 0);
    }

    SECTION("a fixture with a dimmer, at half") {
        patch.pickMode(modeIndexOf("dimmer + RGB (4ch)")); // dimmer on 70, colour on 71-73
        Payload half;
        half.kind = EffectKind::Level;
        half.role = takt4::dmx::Role::Dimmer;
        half.level = 128;
        rig.runner.post(takt4::output::OutputCommand::effect(1, half));
        Payload blue;
        blue.kind = EffectKind::Color;
        blue.color = {0, 0, 255};
        rig.runner.post(takt4::output::OutputCommand::effect(1, blue));
        settle(0.0);
        REQUIRE(at(70) == 128);
        REQUIRE(at(73) == 255);

        patch.identify();
        const double start = settle(0.05);
        CHECK(at(70) > 200);
        engine.tick(start + FixturesController::kIdentifySeconds + 1.0);
        CHECK(at(70) == 128);
        CHECK(at(71) == 0);
        CHECK(at(72) == 0);
        CHECK(at(73) == 255);
    }
}

namespace {

void clickAt(slint::Window& window, float x, float y) {
    const slint::LogicalPosition at({x, y});
    window.dispatch_pointer_move_event(at);
    window.dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
    window.dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
}

void typeText(slint::Window& window, const std::string& text) {
    for (const char c : text) {
        const slint::SharedString key(std::string(1, c));
        window.dispatch_key_press_event(key);
        window.dispatch_key_release_event(key);
    }
}

} // namespace

TEST_CASE("the patch says when two fixtures share channels, and what no rule can reach",
          "[ui][dmx]") {
    // The audit's M21: overlapping fixtures were never reported, and fixtures past the 64th
    // were offered to rules that could not reach them.
    Rig rig;
    FixturesController patch(rig.runner, {});
    patch.add();
    patch.rename("left");
    patch.pickMode(modeIndexOf("RGB (3ch)")); // 1-3
    patch.add();
    patch.rename("right");
    patch.pickMode(modeIndexOf("RGB (3ch)")); // placed after it
    REQUIRE(patch.fixtures().size() == 2);
    CHECK(std::string(patch.window().get_summary()).find("share channel") == std::string::npos);

    patch.setAddress(3); // "right" onto 3-5: its red is the left one's blue
    const std::string summary(patch.window().get_summary());
    INFO(summary);
    CHECK(summary.find("left and right share channel 3 of universe 0") != std::string::npos);

    SECTION("past the last a rule can reach") {
        // 512 since 2026-09-28 — it was 64, one machine word, and the operator asked whether it
        // had to be. Started with that many rather than clicked up to it.
        const std::size_t limit = takt4::dmx::kMaxRoutableFixtures;
        REQUIRE(limit == 512);
        std::vector<takt4::dmx::Fixture> many;
        for (std::size_t i = 0; i < limit; ++i) {
            many.push_back(takt4::dmx::fixtureFromMode("par " + std::to_string(i + 1), 1,
                                                       static_cast<takt4::dmx::PortAddress>(i / 170),
                                                       static_cast<std::uint16_t>(1 + 3 * (i % 170))));
        }
        takt4::dmx::ensureFixtureIds(many);
        FixturesController full(rig.runner, many);
        REQUIRE(full.fixtures().size() == limit);
        CHECK_FALSE(full.window().get_status_error());
        full.add(); // the 513th
        CHECK(full.window().get_status_error());
        CHECK(std::string(full.window().get_status()).find("first 512") != std::string::npos);

        // IDENTIFY cannot aim at it — said, rather than a button that does nothing — and can at
        // the 512th, which the old limit could not.
        full.pick(static_cast<int>(limit));
        full.identify();
        CHECK(std::string(full.window().get_status()).find("identify reaches the first 512") !=
              std::string::npos);
        full.pick(static_cast<int>(limit) - 1);
        full.identify();
        CHECK(std::string(full.window().get_status()).find("identify reaches") == std::string::npos);

        // And a rule is offered the 512 it can reach, not the 513th.
        RulesController editor(rig.runner, {});
        editor.setPatch(full.fixtures());
        editor.add();
        const auto dmx = std::find(takt4::trigger::kMessageKinds.begin(),
                                   takt4::trigger::kMessageKinds.end(),
                                   takt4::trigger::Message::Kind::Dmx) -
                         takt4::trigger::kMessageKinds.begin();
        editor.pickSend(static_cast<int>(dmx));
        CHECK(editor.window().get_fixture_choices()->row_count() == limit);
    }
}

namespace {

void pressKey(slint::Window& window, const slint::SharedString& key) {
    window.dispatch_key_press_event(key);
    window.dispatch_key_release_event(key);
}

/// Clicks the dropdown under (x, y), shuts the list it opened with Escape, and steps it one entry
/// down with the arrow on the closed box — what an operator does with the arrow keys, and a gesture
/// that starts from whatever the dropdown is *showing*. Not Down and Enter in the open list: the list
/// can open over the pointer, whose hover lights the entry under it, and Down then moves from that.
void stepDropdown(slint::Window& window, float x, float y) {
    clickAt(window, x, y);
    slint::platform::update_timers_and_animations();
    pressKey(window, slint::SharedString("\x1b")); // Key.Escape: the list shut, the box focused
    slint::platform::update_timers_and_animations();
    pressKey(window, slint::SharedString(u8"")); // Key.DownArrow
    slint::platform::update_timers_and_animations();
}

} // namespace

TEST_CASE("a channel's role follows a new mode after it was picked by hand", "[ui][dmx]") {
    // The channel rows are updated in place while a fixture keeps the same number of channels,
    // and each row's dropdown is bound to its role one way — so once the operator has picked a
    // role from it, that binding is gone and the dropdown shows its own pick from then on. A
    // mode picked afterwards with the same channel count changed the map underneath and left
    // the dropdown showing the old role; the next arrow press stepped from *that*.
    Rig rig;
    FixturesController patch(rig.runner, {takt4::dmx::fixtureFromMode("par", 2, 0, 1)}); // RGBW
    patch.show();
    auto& window = patch.window().window();
    window.dispatch_scale_factor_change_event(1.0f);
    window.dispatch_resize_event(slint::LogicalSize({1100.0f, 800.0f}));
    window.dispatch_window_active_changed_event(true);
    patch.tick();
    slint::platform::update_timers_and_animations();
    REQUIRE(patch.fixtures()[0].channels.size() == 4);
    REQUIRE(patch.fixtures()[0].channels[0] == Role::Red);

    // The first channel's dropdown, found by trying the "does" column from below the fixture's
    // own boxes down: the first spot where one step moves the first channel's role.
    constexpr float kDoesColumn = 440.0f;
    float found = -1.0f;
    for (float y = 250.0f; y < 500.0f && found < 0.0f; y += 6.0f) {
        stepDropdown(window, kDoesColumn, y);
        if (patch.fixtures()[0].channels[0] != Role::Red) {
            found = y;
        }
    }
    {
        INFO("no click landed on the first channel's dropdown");
        REQUIRE(found >= 0.0f);
    }
    REQUIRE(patch.fixtures()[0].channels[0] == takt4::dmx::kRoles[static_cast<std::size_t>(roleIndexOf(Role::Red)) + 1]);

    // A mode with the same number of channels and a different first one.
    patch.window().invoke_mode_picked(modeIndexOf("dimmer + RGB (4ch)"));
    patch.tick();
    slint::platform::update_timers_and_animations();
    REQUIRE(patch.fixtures()[0].channels.size() == 4);
    REQUIRE(patch.fixtures()[0].channels[0] == Role::Dimmer);

    // One more step from what the dropdown now shows: from dimmer, not from the pick before.
    stepDropdown(window, kDoesColumn, found);
    CHECK(patch.fixtures()[0].channels[0] == takt4::dmx::kRoles[static_cast<std::size_t>(roleIndexOf(Role::Dimmer)) + 1]);
}

TEST_CASE("a channel's dropdown survives the redraws while it is open, and its pick lands",
          "[ui][dmx]") {
    // The audit's T3: popups inside repeaters were tested only for the two color pickers. The
    // channel rows are a repeater the controller rebuilds when a fixture's shape changes, and
    // a row rebuilt with its dropdown open takes the popup away under the pointer. Opened with
    // the pointer, left open through ten redraws, then an entry clicked — which only lands if
    // the popup is still there.
    Rig rig;
    FixturesController patch(rig.runner, {takt4::dmx::fixtureFromMode("par", 2, 0, 1)}); // RGBW
    patch.show();
    auto& window = patch.window().window();
    window.dispatch_scale_factor_change_event(1.0f);
    window.dispatch_resize_event(slint::LogicalSize({1100.0f, 800.0f}));
    window.dispatch_window_active_changed_event(true);
    const auto settle = [&patch] {
        patch.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto reset = [&patch, &settle] {
        patch.window().invoke_channel_role_picked(0, roleIndexOf(Role::Red));
        settle();
    };
    settle();
    REQUIRE(patch.fixtures()[0].channels[0] == Role::Red);

    // The first channel's dropdown, found as the test above finds it.
    constexpr float kDoesColumn = 440.0f;
    float comboY = -1.0f;
    for (float y = 250.0f; y < 500.0f && comboY < 0.0f; y += 6.0f) {
        stepDropdown(window, kDoesColumn, y);
        if (patch.fixtures()[0].channels[0] != Role::Red) {
            comboY = y;
        }
        reset();
    }
    {
        INFO("no click landed on the first channel's dropdown");
        REQUIRE(comboY >= 0.0f);
    }

    // **An entry in the list, found with nothing redrawing**, and the role it picks noted.
    // Found this way and not during the redraws, because a probe that misses the popup lands
    // on whatever is under it — and more than one control in this window can move a channel's
    // role, so "the role changed" alone is not "the popup was clicked".
    float entryY = -1.0f;
    Role picked = Role::Red;
    for (float dy = -300.0f; dy <= 300.0f && entryY < 0.0f; dy += 6.0f) {
        if (dy > -14.0f && dy < 14.0f) {
            continue; // the dropdown itself
        }
        reset();
        clickAt(window, kDoesColumn, comboY);
        slint::platform::update_timers_and_animations();
        clickAt(window, kDoesColumn, comboY + dy);
        settle();
        if (patch.fixtures()[0].channels[0] != Role::Red) {
            entryY = comboY + dy;
            picked = patch.fixtures()[0].channels[0];
        }
    }
    {
        INFO("no click opened the dropdown and landed on its list");
        REQUIRE(entryY >= 0.0f);
    }

    // Then the gesture: opened, left open through ten redraws, and that same entry clicked.
    //
    // **With the channel's level moving through them**, as it does whenever the rig is playing:
    // the row whose list is open carries what takt4 is sending on that channel, and the runner
    // mirrors it sixty times a second. Ten redraws with nothing moving could not have seen a
    // rebuild (the audit of 2026-09-25, T13); a live level taken for a change that needs a new
    // element would take the list away on the first frame, and pass that test. The runner is
    // running for this, as it is in the app, and each level is a channel test from elsewhere.
    reset();
    rig.runner.start();
    const auto channels = patch.window().get_channels();
    const auto watch = std::make_shared<ModelWatch>();
    channels->attach_peer(watch);
    std::set<int> levels;
    clickAt(window, kDoesColumn, comboY);
    for (int redraw = 0; redraw < 10; ++redraw) {
        rig.runner.post(takt4::output::OutputCommand::channelTest(
            0, 1, static_cast<std::uint8_t>(20 + 20 * redraw), 5.0));
        std::this_thread::sleep_for(std::chrono::milliseconds(40)); // two of the mirror's frames
        settle();
        levels.insert(channels->row_data(0)->live);
    }
    clickAt(window, kDoesColumn, entryY);
    settle();
    rig.runner.stop();
    INFO("dropdown at " << kDoesColumn << ", " << comboY << "; entry at " << entryY);
    CHECK(levels.size() > 1); // the row really was changing under the list
    CHECK(watch->changes > 0);
    CHECK(patch.fixtures()[0].channels[0] == picked);
}

namespace {

/// The patch editor on the headless platform at a size that shows a twelve-channel map, with
/// the gestures the tests below are made of. Every gesture lets what Slint runs a loop late run.
struct Patching {
    explicit Patching(FixturesController& controller, float width = 1100.0f, float height = 800.0f)
        : patch(controller), window(controller.window().window()) {
        patch.show();
        window.dispatch_scale_factor_change_event(1.0f);
        window.dispatch_resize_event(slint::LogicalSize({width, height}));
        window.dispatch_window_active_changed_event(true);
        settle();
    }
    void settle() const {
        patch.tick();
        slint::platform::update_timers_and_animations();
    }
    void click(float x, float y) const {
        clickAt(window, x, y);
        settle();
    }
    void type(const std::string& text) const {
        typeText(window, text);
        settle();
    }
    void enter() const { type("\n"); }
    /// Everything in the box with the keyboard gone, wherever the click left the caret.
    void clearBox() const {
        takt4::tests::endOfText(window);
        type(std::string(40, '\b'));
    }
    FixturesController& patch;
    slint::Window& window;
};

/// A spot in a sweep where `probe` hit, or none. `reset` runs after every probe.
template <typename Probe, typename Reset>
std::pair<float, float> sweepFor(float x0, float x1, float dx, float y0, float y1, float dy,
                                 Probe probe, Reset reset) {
    for (float y = y0; y < y1; y += dy) {
        for (float x = x0; x < x1; x += dx) {
            const bool hit = probe(x, y);
            reset();
            if (hit) {
                return {x, y};
            }
        }
    }
    return {-1.0f, -1.0f};
}

} // namespace

TEST_CASE("a fixture's address is patched once, when it is entered", "[ui][dmx]") {
    // The audit of 2026-09-25, M22. The address was a `SpinBox`, which sent every keystroke:
    // typing 101 patched the fixture at 1, then 10, then 101 — and a re-patch parks every channel
    // the fixture lands on and drops every one it leaves, so a strobe parked at full, passing
    // over a par's channel on the way, flashed it.
    Rig rig;
    FixturesController patch(rig.runner, {takt4::dmx::fixtureFromMode("par", 1, 0, 1)});
    const Patching shown(patch);
    const auto address = [&patch] { return static_cast<int>(patch.fixtures()[0].address); };

    const auto box = sweepFor(
        340.0f, 800.0f, 20.0f, 40.0f, 200.0f, 4.0f,
        [&](float x, float y) {
            shown.click(x, y);
            shown.clearBox();
            shown.type("9");
            shown.enter();
            return address() == 9;
        },
        [&] {
            patch.setAddress(1);
            patch.rename("par");
            patch.setGroup("");
            patch.setUniverse("0");
        });
    INFO("address box at " << box.first << ", " << box.second);
    REQUIRE(box.first >= 0.0f);
    REQUIRE(address() == 1);

    shown.click(box.first, box.second);
    shown.clearBox();
    shown.type("1");
    CHECK(address() == 1);
    shown.type("0");
    CHECK(address() == 1); // not 10
    shown.type("1");
    CHECK(address() == 1);
    shown.enter();
    CHECK(address() == 101);
    CHECK(patch.window().get_address() == 101);
}

TEST_CASE("a channel's dropdown shows the next fixture's role after a delete or an import",
          "[ui][dmx]") {
    // The audit of 2026-09-25, M21. Deleting the selected fixture puts the next one at the same
    // index, and in a rig of identical pars at the same channel count too — and a std dropdown
    // whose role was picked by hand was bound to nothing any more, so it went on showing the
    // deleted fixture's pick over the next one's map. The same for an IMPORT of a patch of the
    // same shape. The rows are written in place since the redesign of 2026-09-30, and the
    // dropdown never sets itself, so it shows the model's role. Judged the way the operator would
    // meet it: one arrow step from what the dropdown shows.
    const bool importing = GENERATE(false, true);
    INFO((importing ? "after an import" : "after deleting the selected fixture"));
    Rig rig;
    const std::vector<Fixture> pars = {takt4::dmx::fixtureFromMode("par 1", 2, 0, 1),
                                       takt4::dmx::fixtureFromMode("par 2", 2, 0, 5),
                                       takt4::dmx::fixtureFromMode("par 3", 2, 0, 9)};
    FixturesController patch(rig.runner, pars);
    // The show as saved, ids and all: an import of the same file brings the same ids back, and
    // that is the case the id alone cannot tell from "the fixture showing" (measured: an import
    // of fixtures given new ids passed with the rebuild on import taken out).
    const std::vector<Fixture> saved = patch.fixtures();
    REQUIRE_FALSE(saved[1].id.empty());
    const Patching shown(patch);
    patch.pick(1);
    shown.settle();
    REQUIRE(patch.fixtures()[1].channels[0] == Role::Red);

    // The first channel's dropdown, found as the tests above find it.
    constexpr float kDoesColumn = 440.0f;
    float found = -1.0f;
    for (float y = 250.0f; y < 500.0f && found < 0.0f; y += 6.0f) {
        stepDropdown(shown.window, kDoesColumn, y);
        shown.settle();
        if (patch.fixtures()[1].channels[0] != Role::Red) {
            found = y;
        }
    }
    {
        INFO("no click landed on the first channel's dropdown");
        REQUIRE(found >= 0.0f);
    }
    const Role stepped = takt4::dmx::kRoles[static_cast<std::size_t>(roleIndexOf(Role::Red)) + 1];
    REQUIRE(patch.fixtures()[1].channels[0] == stepped);

    if (importing) {
        patch.setFixtures(saved); // the same show again, its par 2 still red
        REQUIRE(patch.fixtures()[1].id == saved[1].id);
        patch.pick(1);
    } else {
        patch.window().invoke_removed_at(1); // par 2 goes; par 3 takes its place in the list
        REQUIRE(patch.fixtures().size() == 2);
        REQUIRE(patch.selected() == 1);
        REQUIRE(patch.fixtures()[1].name == "par 3");
    }
    shown.settle();
    shown.settle();
    REQUIRE(patch.fixtures()[1].channels[0] == Role::Red);

    stepDropdown(shown.window, kDoesColumn, found);
    shown.settle();
    CHECK(patch.fixtures()[1].channels[0] == stepped); // one step from red, not from the pick
}

// --- the redesign of 2026-09-30: every control, clicked, typed into, wheeled over -------------
//
// The operator asked for the rebuilt patch editor to be tested "adversarially to find whats broken
// and hidden". Each of these drives real pointer and key events into the window, on the rig the
// approved mockup holds, and judges by what reached the patch.

namespace {

/// The approved mockup's rig: two washes, two heads — the second left out of the show — and a
/// blinder that runs off the end of its universe.
std::vector<Fixture> rigOfFive() {
    std::vector<Fixture> rig = {
        takt4::dmx::fixtureFromMode("wash L", 3, 0, 1),     // dimmer + RGB, 1-4
        takt4::dmx::fixtureFromMode("wash R", 4, 0, 5),     // LED par, 5-10
        takt4::dmx::fixtureFromMode("head 1", 6, 0, 11),    // 16-bit head, 11-22
        takt4::dmx::fixtureFromMode("head 2", 6, 0, 23),    // 23-34
        takt4::dmx::fixtureFromMode("blinder", 6, 0, 505)}; // runs off the end
    rig[0].group = "washes";
    rig[1].group = "washes";
    rig[2].group = "heads";
    rig[3].group = "heads";
    rig[3].enabled = false;
    rig[2].panMin = 0.1;
    rig[2].panMax = 0.9;
    rig[2].tiltMin = 0.2;
    rig[2].tiltMax = 0.7;
    takt4::dmx::ensureFixtureIds(rig);
    return rig;
}

/// Where the patch editor's controls are at 1000 x 1180 — the width it opens at, and tall enough
/// to show a twelve-channel head's map and C under it with nothing scrolled — with head 1 picked.
/// Measured off its render; every test REQUIREs what a click at one of these did, so a moved
/// control fails loudly rather than quietly testing something else.
namespace at {
constexpr float kWidth = 1000.0f;
constexpr float kHeight = 1180.0f;
/// The page's margin: nothing there but the window's own background.
constexpr float kNothingX = 4.0f;
constexpr float kNothingY = 4.0f;
// The list: row i's name line, its dot and its marks, and the + in its heading.
constexpr float rowY(int i) { return 101.0f + 61.0f * static_cast<float>(i); }
constexpr float kRowX = 120.0f;
constexpr float kDotX = 35.0f;
constexpr float kCopyX = 219.0f;
constexpr float kKillX = 243.0f;
constexpr float kAddX = 246.0f;
constexpr float kAddY = 34.0f;
// The top row.
constexpr float kTopY = 37.0f;
constexpr float kTickX = 306.0f;
constexpr float kNameX = 559.0f;
constexpr float kGroupX = 794.0f;
constexpr float kIdentifyX = 931.0f;
// The sheets' fold arrows, at the far right of their headings.
constexpr float kFoldX = 960.0f;
constexpr float kWhereY = 120.0f;
constexpr float kChannelsY = 264.0f;
// A.
constexpr float kWhereRowY = 159.0f;
constexpr float kUniverseX = 460.0f;
constexpr float kAddressX = 649.0f;
constexpr float kModeX = 544.0f;
constexpr float kModeY = 201.0f;
// B.
constexpr float kAddChannelX = 905.0f;
constexpr float kTestLevelX = 460.0f;
constexpr float kTestLevelY = 303.0f;
constexpr float bandY(int i) { return 376.0f + 50.0f * static_cast<float>(i); }
constexpr float kRoleX = 450.0f;
constexpr float kParkedX = 586.0f;
constexpr float kTestX = 901.0f;
constexpr float kKillChannelX = 952.0f;
// C: the pan and tilt rows.
constexpr float kPanY = 1061.0f;
constexpr float kTiltY = 1103.0f;
} // namespace at

const Fixture* byId(const std::vector<Fixture>& patch, const std::string& id) {
    for (const Fixture& fixture : patch) {
        if (fixture.id == id) {
            return &fixture;
        }
    }
    return nullptr;
}

/// A bare patch window holding what a controller publishes for the five-fixture rig with head 1
/// picked, and every callback it fires written down — so a click sweep can be held to what each
/// control is for without a controller acting on it and moving everything under the next click.
struct Recorder {
    Rig rig;
    FixturesController source{rig.runner, rigOfFive()};
    slint::ComponentHandle<FixturesWindow> window = FixturesWindow::create();
    std::vector<std::string> fired;

    Recorder() {
        source.pick(2);
        auto& from = source.window();
        auto& to = *window;
        to.set_fixtures(from.get_fixtures());
        to.set_selected(from.get_selected());
        to.set_name(from.get_name());
        to.set_group(from.get_group());
        to.set_universe(from.get_universe());
        to.set_address(from.get_address());
        to.set_enabled(from.get_enabled());
        to.set_roles(from.get_roles());
        to.set_modes(from.get_modes());
        to.set_mode_index(from.get_mode_index());
        to.set_channels(from.get_channels());
        to.set_test_level(from.get_test_level());
        to.set_test_seconds(from.get_test_seconds());
        to.set_moves(from.get_moves());
        to.set_summary(from.get_summary());
        resetMoves();
        const auto note = [this](std::string what) { fired.push_back(std::move(what)); };
        const auto n = [](int i) { return " " + std::to_string(i); };
        to.on_picked([=](int i) { note("picked" + n(i)); });
        to.on_added([=] { note("added"); });
        to.on_import_clicked([=] { note("import"); });
        to.on_duplicated_at([=](int i) { note("copy" + n(i)); });
        to.on_removed_at([=](int i) { note("kill" + n(i)); });
        to.on_enabled_changed([=](int i, bool) { note("dot" + n(i)); });
        to.on_name_edited([=](const slint::SharedString&) { note("name-edited"); });
        to.on_name_typed([=](const slint::SharedString&) { note("name-typed"); });
        to.on_group_edited([=](const slint::SharedString&) { note("group-edited"); });
        to.on_group_typed([=](const slint::SharedString&) { note("group-typed"); });
        to.on_universe_edited([=](const slint::SharedString&) { note("universe-edited"); });
        to.on_universe_typed([=](const slint::SharedString&) { note("universe-typed"); });
        to.on_address_changed([=](int) { note("address"); });
        to.on_address_typed([=](int) { note("address-typed"); });
        to.on_fixture_enabled_changed([=](bool) { note("in-the-show"); });
        to.on_mode_picked([=](int) { note("mode"); });
        to.on_channel_added([=] { note("add-channel"); });
        to.on_channel_removed([=](int i) { note("kill-channel" + n(i)); });
        to.on_channel_role_picked([=](int i, int) { note("role" + n(i)); });
        to.on_channel_parked_changed([=](int i, int) { note("parked" + n(i)); });
        to.on_channel_parked_typed([=](int i, int) { note("parked-typed" + n(i)); });
        to.on_channel_tested([=](int i) { note("test" + n(i)); });
        to.on_test_level_changed([=](int) { note("test-level"); });
        to.on_test_level_typed([=](int) { note("test-level-typed"); });
        to.on_pan_range_changed([=](float, float) { note("pan"); });
        to.on_tilt_range_changed([=](float, float) { note("tilt"); });
        to.on_identify([=] { note("identify"); });
        to.on_fold_clicked([=](int i) { note("fold" + n(i)); });

        to.show();
        handle().dispatch_scale_factor_change_event(1.0f);
        handle().dispatch_resize_event(slint::LogicalSize({at::kWidth, at::kHeight}));
        handle().dispatch_window_active_changed_event(true);
        slint::platform::update_timers_and_animations();
    }
    slint::Window& handle() { return window->window(); }
    /// The sliders write the window's own pan and tilt as they are dragged (the window follows
    /// the hand and the controller hears of it once let go), so a probe that hit one is undone.
    void resetMoves() {
        window->set_pan_min(10.0f);
        window->set_pan_max(90.0f);
        window->set_tilt_min(20.0f);
        window->set_tilt_max(70.0f);
    }
};

} // namespace

TEST_CASE("every spot in the patch editor fires what is under it, once, and nothing else",
          "[ui][dmx]") {
    // A click every 10 px over the whole window, each followed by a click on the page's margin that
    // lets go of whatever the first one took — a box's late commit, an open list — and what the
    // first click fired is written down against where it was. Held to: one thing per click at
    // most; only the things a click is for (a click never picks from a list, types or edits);
    // every control reached somewhere; and each row's or band's own controls only on that row.
    Recorder recorder;
    const takt4::tests::NothingReal nothingReal;
    std::map<std::string, std::vector<std::pair<float, float>>> where;
    std::vector<std::string> doubles;
    for (float y = 6.0f; y < at::kHeight; y += 10.0f) {
        for (float x = 6.0f; x < at::kWidth; x += 10.0f) {
            recorder.fired.clear();
            clickAt(recorder.handle(), x, y);
            slint::platform::update_timers_and_animations();
            const std::vector<std::string> fired = recorder.fired;
            if (fired.size() > 1) {
                std::string all;
                for (const std::string& one : fired) {
                    all += one + "; ";
                }
                doubles.push_back(std::to_string(static_cast<int>(x)) + "," +
                                  std::to_string(static_cast<int>(y)) + ": " + all);
            }
            for (const std::string& one : fired) {
                where[one].emplace_back(x, y);
            }
            clickAt(recorder.handle(), at::kNothingX, at::kNothingY);
            slint::platform::update_timers_and_animations();
            slint::platform::update_timers_and_animations();
            recorder.resetMoves();
        }
    }
    {
        std::string seen;
        for (const auto& [what, spots] : where) {
            seen += what + " (" + std::to_string(spots.size()) + "), ";
        }
        INFO("fired: " << seen);
        std::string twice;
        for (const std::string& one : doubles) {
            twice += one + "\n";
        }
        INFO("clicks that fired more than one thing:\n" << twice);
        CHECK(doubles.empty());

        // What a click is for, and nothing else: a box is typed into, not clicked into a change,
        // and a list is picked from by a second click, which this sweep never makes.
        std::set<std::string> expected = {"added",  "in-the-show", "identify", "fold 0", "fold 1",
                                          "fold 2", "add-channel", "pan",      "tilt",   "import"};
        for (int i = 0; i < 5; ++i) {
            for (const char* what : {"picked ", "dot ", "copy ", "kill "}) {
                expected.insert(what + std::to_string(i));
            }
        }
        for (int i = 0; i < 12; ++i) {
            expected.insert("test " + std::to_string(i));
            expected.insert("kill-channel " + std::to_string(i));
        }
        for (const auto& [what, spots] : where) {
            INFO(what << " at " << spots.front().first << ", " << spots.front().second);
            CHECK(expected.count(what) == 1);
        }
        for (const std::string& what : expected) {
            INFO(what << " was reached by no click");
            CHECK(where.count(what) == 1);
        }
    }
    // Each row's and band's controls on that row and no other.
    for (int i = 0; i < 5; ++i) {
        for (const char* what : {"picked ", "dot ", "copy ", "kill "}) {
            const std::string key = what + std::to_string(i);
            for (const auto& [x, y] : where[key]) {
                INFO(key << " at " << x << ", " << y);
                CHECK(y >= at::rowY(i) - 22.0f);
                CHECK(y <= at::rowY(i) + 38.0f);
                CHECK(x < 270.0f); // in the list
            }
        }
    }
    for (int i = 0; i < 12; ++i) {
        for (const char* what : {"test ", "kill-channel "}) {
            const std::string key = what + std::to_string(i);
            for (const auto& [x, y] : where[key]) {
                INFO(key << " at " << x << ", " << y);
                CHECK(std::abs(y - at::bandY(i)) <= 22.0f);
            }
        }
    }
    nothingReal.check();
}

namespace {

/// What a box is, where it is, what is typed into it, and whether the patch got it.
struct TypedBox {
    const char* name;
    float x;
    float y;
    const char* text;
    /// Whether `fixture` — the one it was typed for — holds what was typed, given where its
    /// channels went (`firstChannelGone`: channel 0 was deleted by the next click).
    std::function<bool(const Fixture&, const FixturesController&, bool firstChannelGone)> holds;
    /// Whether another fixture holds it — which it must not.
    std::function<bool(const Fixture& other, const Fixture& before)> leaked;
};

std::vector<TypedBox> typedBoxes() {
    return {
        {"the name", at::kNameX, at::kTopY, "QQ",
         [](const Fixture& f, const FixturesController&, bool) { return f.name == "QQ"; },
         [](const Fixture& o, const Fixture& b) { return o.name != b.name; }},
        {"the group", at::kGroupX, at::kTopY, "GG",
         [](const Fixture& f, const FixturesController&, bool) { return f.group == "GG"; },
         [](const Fixture& o, const Fixture& b) { return o.group != b.group; }},
        {"the universe", at::kUniverseX, at::kWhereRowY, "7",
         [](const Fixture& f, const FixturesController&, bool) { return f.universe == 7; },
         [](const Fixture& o, const Fixture& b) { return o.universe != b.universe; }},
        {"the start address", at::kAddressX, at::kWhereRowY, "77",
         [](const Fixture& f, const FixturesController&, bool) { return f.address == 77; },
         [](const Fixture& o, const Fixture& b) { return o.address != b.address; }},
        {"a channel's parked level", at::kParkedX, at::bandY(1), "99",
         [](const Fixture& f, const FixturesController&, bool gone) {
             const std::size_t at = gone ? 0 : 1;
             return f.parked.size() > at && f.parked[at] == 99;
         },
         [](const Fixture& o, const Fixture& b) { return o.parked != b.parked; }},
        {"the TEST level", at::kTestLevelX, at::kTestLevelY, "44",
         [](const Fixture&, const FixturesController& p, bool) { return p.testLevel() == 44; },
         [](const Fixture&, const Fixture&) { return false; }},
    };
}

} // namespace

TEST_CASE("what is typed in the patch editor goes to the fixture it was typed for, whatever is "
          "clicked next",
          "[ui][dmx]") {
    // The audit of 2026-09-25's H10 and M18 for every box against every kind of next click: a
    // box commits a turn of the event loop after the click that takes its keyboard, by when that
    // click may have put another fixture in the editor, deleted one, added a channel or folded the
    // box's sheet away. Typed and not entered; then the click; then the patch is read. The typed
    // value is on the fixture it was typed for and on no other — a copy of it aside, which is
    // made from it after the edit.
    Rig rig;
    const std::vector<Fixture> original = rigOfFive();
    const std::string head = original[2].id;
    FixturesController patch(rig.runner, original);
    const Patching shown(patch, at::kWidth, at::kHeight);
    const takt4::tests::NothingReal nothingReal;
    const auto escape = [&shown] { shown.type("\x1b"); };

    struct Next {
        const char* name;
        std::function<void()> go;
        /// That the click did what it is for — or the case tests nothing.
        std::function<bool()> happened;
        bool firstChannelGone = false;
    };
    // A × is pressed past the double-click time since the last press on it: a quicker one is taken
    // for the second click of a double-click and ignored (`DeleteGuard`), and the cases run a few
    // milliseconds apart.
    const auto pastDoubleClick = [] {
        std::this_thread::sleep_for(takt4::ui::DeleteGuard::interval() +
                                    std::chrono::milliseconds(60));
    };
    const auto headNow = [&]() -> const Fixture& { return *byId(patch.fixtures(), head); };
    const auto said = [&](const char* what) {
        return std::string(patch.window().get_status()).find(what) != std::string::npos;
    };
    const std::vector<Next> nexts = {
        {"Enter", [&] { shown.enter(); }, [&] { return patch.selected() == 2; }},
        {"the page's margin", [&] { shown.click(at::kNothingX, at::kNothingY); },
         [&] { return patch.selected() == 2; }},
        {"another fixture's row", [&] { shown.click(at::kRowX, at::rowY(0)); },
         [&] { return patch.selected() == 0; }},
        {"another fixture's dot", [&] { shown.click(at::kDotX, at::rowY(0)); },
         [&] { return !patch.fixtures()[0].enabled; }},
        {"the list's +", [&] { shown.click(at::kAddX, at::kAddY); },
         [&] { return patch.fixtures().size() == original.size() + 1; }},
        {"this fixture's copy mark", [&] { shown.click(at::kCopyX, at::rowY(2)); },
         [&] { return patch.fixtures().size() == original.size() + 1 && patch.selected() == 3; }},
        {"another fixture's x",
         [&] {
             pastDoubleClick();
             shown.click(at::kKillX, at::rowY(0));
         },
         [&] { return byId(patch.fixtures(), original[0].id) == nullptr; }},
        {"in the show", [&] { shown.click(at::kTickX, at::kTopY); },
         [&] { return !headNow().enabled; }},
        {"identify", [&] { shown.click(at::kIdentifyX, at::kTopY); },
         [&] { return said("Identifying"); }},
        {"A's fold arrow", [&] { shown.click(at::kFoldX, at::kWhereY); },
         [&] { return patch.window().get_where_folded(); }},
        {"B's fold arrow", [&] { shown.click(at::kFoldX, at::kChannelsY); },
         [&] { return patch.window().get_channels_folded(); }},
        {"B's + add", [&] { shown.click(at::kAddChannelX, at::kChannelsY); },
         [&] { return headNow().channels.size() == 13; }},
        {"a channel's test", [&] { shown.click(at::kTestX, at::bandY(3)); },
         [&] { return said("Channel ") && said(" for 3 s"); }},
        {"the first channel's x",
         [&] {
             pastDoubleClick();
             shown.click(at::kKillChannelX, at::bandY(0));
         },
         [&] { return headNow().channels.size() == 11; }, true},
        {"the mode list, opened and shut",
         [&] {
             shown.click(at::kModeX, at::kModeY);
             escape();
         },
         [&] { return headNow().channels == original[2].channels; }},
        {"a channel's dropdown, opened and shut",
         [&] {
             shown.click(at::kRoleX, at::bandY(5));
             escape();
         },
         [&] { return headNow().channels == original[2].channels; }},
    };

    const auto reset = [&] {
        shown.click(at::kNothingX, at::kNothingY);
        patch.setFixtures(original);
        patch.applyLayout(takt4::settings::MachineSettings{});
        patch.setTestLevel(255);
        patch.pick(2);
        shown.settle();
        shown.settle();
    };

    for (const TypedBox& box : typedBoxes()) {
        for (const Next& next : nexts) {
            INFO(box.name << ", typed, then " << next.name);
            reset();
            shown.click(box.x, box.y);
            shown.clearBox();
            shown.type(box.text);
            // Nothing yet: what is typed is kept until it is finished with.
            {
                const Fixture* now = byId(patch.fixtures(), head);
                REQUIRE(now != nullptr);
                CHECK_FALSE(box.holds(*now, patch, false));
            }
            next.go();
            shown.settle();
            shown.settle();
            REQUIRE(byId(patch.fixtures(), head) != nullptr);
            CHECK(next.happened());

            const Fixture* typedFor = byId(patch.fixtures(), head);
            REQUIRE(typedFor != nullptr);
            CHECK(box.holds(*typedFor, patch, next.firstChannelGone));
            for (const Fixture& other : patch.fixtures()) {
                const Fixture* before = byId(original, other.id);
                if (other.id == head || before == nullptr) {
                    continue; // the one it was for, and a fixture the next click made
                }
                INFO("on " << other.name);
                CHECK_FALSE(box.leaked(other, *before));
            }
        }
    }
    nothingReal.check();
}

TEST_CASE("a box clicked into and left without typing changes nothing on the fixture clicked next",
          "[ui][dmx]") {
    // Nothing typed at all, every box, and another fixture clicked. Held because it could go wrong
    // two ways: a box let go of commits what it shows, and the controller acts on whatever is in
    // the editor by then. Measured 2026-10-01: it holds even without the patch editor's draft rule
    // — Slint runs the box's new value before its lost focus, so it commits wash L's own — and
    // since then a box commits nothing it was not changed from (weltformat's `Entry`). This keeps
    // it that way.
    Rig rig;
    const std::vector<Fixture> original = rigOfFive();
    FixturesController patch(rig.runner, original);
    const Patching shown(patch, at::kWidth, at::kHeight);
    for (const TypedBox& box : typedBoxes()) {
        INFO(box.name);
        shown.click(at::kNothingX, at::kNothingY);
        patch.setFixtures(original);
        patch.pick(2);
        shown.settle();
        shown.click(box.x, box.y); // the keyboard, and nothing typed
        shown.click(at::kRowX, at::rowY(0));
        shown.settle();
        shown.settle();
        REQUIRE(patch.selected() == 0);
        for (std::size_t i = 0; i < original.size(); ++i) {
            const Fixture& now = patch.fixtures()[i];
            INFO("fixture " << now.name);
            CHECK(now.name == original[i].name);
            CHECK(now.group == original[i].group);
            CHECK(now.universe == original[i].universe);
            CHECK(now.address == original[i].address);
            CHECK(now.parked == original[i].parked);
        }
        // And what the boxes show is wash L's.
        CHECK(std::string(patch.window().get_name()) == "wash L");
        CHECK(patch.window().get_address() == 1);
    }
}

TEST_CASE("deleting the fixture being typed for, or the last one, keeps the next one as it was",
          "[ui][dmx]") {
    // A name typed for head 1, and head 1's own × clicked: the typed name goes with head 1 and not
    // onto head 2, which takes its place in the list and in the editor. Then every fixture deleted
    // the same way, each with something typed, down to an empty patch — which says what to do —
    // and + gives a fixture whose boxes hold its own name, not the last one's typing.
    Rig rig;
    const std::vector<Fixture> original = rigOfFive();
    FixturesController patch(rig.runner, original);
    const Patching shown(patch, at::kWidth, at::kHeight);
    patch.pick(2);
    shown.settle();
    shown.click(at::kNameX, at::kTopY);
    shown.clearBox();
    shown.type("doomed");
    shown.click(at::kKillX, at::rowY(2));
    shown.settle();
    shown.settle();
    REQUIRE(patch.fixtures().size() == 4);
    CHECK(byId(patch.fixtures(), original[2].id) == nullptr);
    CHECK(patch.fixtures()[2].name == "head 2");
    CHECK(patch.selected() == 2);
    CHECK(std::string(patch.window().get_name()) == "head 2");
    for (const Fixture& fixture : patch.fixtures()) {
        CHECK(fixture.name != "doomed");
    }

    // Down to nothing, typing into whatever is showing before each ×.
    while (!patch.fixtures().empty()) {
        const int at = patch.selected();
        REQUIRE(at >= 0);
        shown.click(at::kNameX, at::kTopY);
        shown.type("x");
        // Past the DeleteGuard's window for a double-click, which a quick second × on the row that
        // moved up is not meant to reach. The system's window, not a guess at it: 450 ms passed on
        // the rig (410) and failed on every CI runner (500).
        std::this_thread::sleep_for(takt4::ui::DeleteGuard::interval() +
                                    std::chrono::milliseconds(60));
        const std::size_t before = patch.fixtures().size();
        shown.click(at::kKillX, at::rowY(at));
        shown.settle();
        REQUIRE(patch.fixtures().size() + 1 == before);
    }
    CHECK(patch.selected() == -1);
    CHECK(patch.window().get_selected() == -1);
    CHECK(std::string(patch.window().get_summary()) == "Nothing patched yet.");

    shown.click(at::kAddX, at::kAddY);
    shown.settle();
    REQUIRE(patch.fixtures().size() == 1);
    CHECK(patch.fixtures()[0].name == "fixture 1");
    CHECK(std::string(patch.window().get_name()) == "fixture 1");
    // And its name box takes typing for it.
    shown.click(at::kNameX, at::kTopY);
    shown.clearBox();
    shown.type("par");
    shown.enter();
    CHECK(patch.fixtures()[0].name == "par");
}

TEST_CASE("the wheel over every box and dropdown in the patch editor changes nothing",
          "[ui][dmx]") {
    // The audit's H2 and M19: a std SpinBox and ComboBox changed under the wheel as a pane scrolled
    // past them, which is how a parked level moved on the rig. Wheeled over every box and dropdown
    // in both directions, focused and not.
    Rig rig;
    const std::vector<Fixture> original = rigOfFive();
    FixturesController patch(rig.runner, original);
    const Patching shown(patch, at::kWidth, at::kHeight);
    patch.pick(2);
    shown.settle();
    const std::vector<std::pair<float, float>> spots = {
        {at::kNameX, at::kTopY},           {at::kGroupX, at::kTopY},
        {at::kUniverseX, at::kWhereRowY},  {at::kAddressX, at::kWhereRowY},
        {at::kModeX, at::kModeY},          {at::kTestLevelX, at::kTestLevelY},
        {at::kRoleX, at::bandY(0)},        {at::kParkedX, at::bandY(0)},
        {at::kRoleX, at::bandY(4)},        {at::kParkedX, at::bandY(6)}};
    for (const bool focused : {false, true}) {
        for (const auto& [x, y] : spots) {
            INFO((focused ? "focused, " : "") << "at " << x << ", " << y);
            if (focused) {
                shown.click(x, y);
                shown.type("\x1b"); // a list opened by the click, shut; a box left as it was
            }
            for (const float dy : {120.0f, -120.0f, 360.0f, -360.0f}) {
                shown.window.dispatch_pointer_scroll_event(slint::LogicalPosition({x, y}), 0.0f, dy);
                shown.settle();
            }
            shown.click(at::kNothingX, at::kNothingY);
            const Fixture& now = patch.fixtures()[2];
            CHECK(now.channels == original[2].channels);
            CHECK(now.parked == original[2].parked);
            CHECK(now.address == original[2].address);
            CHECK(now.universe == original[2].universe);
            CHECK(now.name == original[2].name);
            CHECK(patch.testLevel() == 255);
            CHECK(patch.selected() == 2);
        }
    }
}

TEST_CASE("Escape in a patch editor box puts a number back and finishes a name, and nothing more",
          "[ui][dmx]") {
    Rig rig;
    const std::vector<Fixture> original = rigOfFive();
    FixturesController patch(rig.runner, original);
    const Patching shown(patch, at::kWidth, at::kHeight);
    patch.pick(2);
    shown.settle();

    // A number: what was typed thrown away, and not committed by the click after either.
    shown.click(at::kAddressX, at::kWhereRowY);
    shown.clearBox();
    shown.type("300");
    shown.type("\x1b");
    shown.click(at::kRowX, at::rowY(0));
    shown.settle();
    CHECK(patch.fixtures()[2].address == 11);
    CHECK(patch.fixtures()[0].address == 1);
    patch.pick(2);
    shown.settle();
    shown.click(at::kParkedX, at::bandY(2));
    shown.clearBox();
    shown.type("17");
    shown.type("\x1b");
    shown.click(at::kNothingX, at::kNothingY);
    CHECK(patch.fixtures()[2].parked == original[2].parked);

    // A name: Escape finishes it, as a click away does.
    shown.click(at::kNameX, at::kTopY);
    shown.clearBox();
    shown.type("spot");
    shown.type("\x1b");
    shown.settle();
    CHECK(patch.fixtures()[2].name == "spot");
    CHECK(patch.selected() == 2);
    CHECK(patch.fixtures().size() == original.size());
}

TEST_CASE("pan and tilt follow the hand, and are patched once, when let go", "[ui][dmx]") {
    // H8: each pixel of a movement-limit drag re-patched the rig. The window follows the hand, and
    // the patch hears of it once — on the release, or on each arrow key's step.
    Rig rig;
    FixturesController patch(rig.runner, rigOfFive());
    int patched = 0;
    patch.setPatchChanged([&patched](const std::vector<Fixture>&) { ++patched; });
    const Patching shown(patch, at::kWidth, at::kHeight);
    patch.pick(2);
    shown.settle();
    REQUIRE(patch.window().get_moves());

    // The pan slider's low handle, at 10 %: found along its row as the spot where a press moves
    // nothing — a press off a handle jumps that end there — and a drag from it does. And the press
    // that moved nothing is no patch: a re-patch for nothing ends a running TEST.
    float handle = -1.0f;
    int pressesThatPatched = 0;
    for (float x = 400.0f; x < 560.0f && handle < 0.0f; x += 2.0f) {
        const slint::LogicalPosition atPoint({x, at::kPanY});
        shown.window.dispatch_pointer_move_event(atPoint);
        shown.window.dispatch_pointer_press_event(atPoint, slint::PointerEventButton::Left);
        shown.settle();
        const bool still = patch.window().get_pan_min() == 10.0f;
        if (still) {
            shown.window.dispatch_pointer_release_event(atPoint, slint::PointerEventButton::Left);
            shown.settle();
            pressesThatPatched += patched;
            const slint::LogicalPosition dragged({x + 20.0f, at::kPanY});
            shown.window.dispatch_pointer_move_event(atPoint);
            shown.window.dispatch_pointer_press_event(atPoint, slint::PointerEventButton::Left);
            shown.window.dispatch_pointer_move_event(dragged);
            shown.settle();
            if (patch.window().get_pan_min() > 10.0f) {
                handle = x; // a press here moved nothing, and a drag from here does
            }
            shown.window.dispatch_pointer_release_event(dragged, slint::PointerEventButton::Left);
        } else {
            shown.window.dispatch_pointer_release_event(atPoint, slint::PointerEventButton::Left);
        }
        shown.settle();
        // Put back for the next probe.
        patch.setPanRange(10.0f, 90.0f);
        shown.settle();
        patched = 0;
    }
    INFO("pan's low handle at " << handle);
    REQUIRE(handle > 0.0f);
    CHECK(pressesThatPatched == 0);

    patched = 0;
    const slint::LogicalPosition start({handle, at::kPanY});
    shown.window.dispatch_pointer_move_event(start);
    shown.window.dispatch_pointer_press_event(start, slint::PointerEventButton::Left);
    shown.settle();
    float last = patch.window().get_pan_min();
    int followed = 0;
    for (int step = 1; step <= 30; ++step) {
        shown.window.dispatch_pointer_move_event(
            slint::LogicalPosition({handle + 3.0f * static_cast<float>(step), at::kPanY}));
        shown.settle();
        if (patch.window().get_pan_min() != last) {
            ++followed;
            last = patch.window().get_pan_min();
        }
    }
    CHECK(followed > 10);    // the handle went with the hand
    CHECK(patched == 0);     // and nothing was patched on the way
    CHECK(patch.fixtures()[2].panMin == 0.1);
    shown.window.dispatch_pointer_release_event(
        slint::LogicalPosition({handle + 90.0f, at::kPanY}), slint::PointerEventButton::Left);
    shown.settle();
    CHECK(patched == 1);
    CHECK(patch.fixtures()[2].panMin == Catch::Approx(last / 100.0f).margin(1e-6));
    CHECK(patch.fixtures()[2].panMin > 0.1);
    CHECK(patch.fixtures()[2].panMax == 0.9); // the other end left where it was

    // And from the keyboard: each arrow a step, and a patch.
    patched = 0;
    const double before = patch.fixtures()[2].panMin;
    pressKey(shown.window, slint::SharedString(u8"")); // Key.RightArrow
    shown.settle();
    CHECK(patched == 1);
    CHECK(patch.fixtures()[2].panMin > before);
}

TEST_CASE("a right-click puts pan and tilt back to the whole of their travel, once each",
          "[ui][dmx]") {
    // The operator's ask of 2026-10-05: a right-click on any slider sets it back to its default,
    // and a fixture's own is the whole of its travel, 0 to 100 % (`dmx::Fixture`). Along each
    // row, right-clicked until something changes: that one slider is put back, both ends, and
    // the patch hears of it once.
    Rig rig;
    FixturesController patch(rig.runner, rigOfFive());
    int patched = 0;
    patch.setPatchChanged([&patched](const std::vector<Fixture>&) { ++patched; });
    const Patching shown(patch, at::kWidth, at::kHeight);
    patch.pick(2);
    shown.settle();
    REQUIRE(patch.window().get_moves());
    const Fixture fresh;
    const auto now = [&patch] { return patch.fixtures()[2]; };
    REQUIRE(now().panMin == 0.1);
    REQUIRE(now().tiltMax == 0.7);
    const auto asSet = [&now] {
        const Fixture fixture = now();
        return fixture.panMin == 0.1 && fixture.panMax == 0.9 && fixture.tiltMin == 0.2 &&
               fixture.tiltMax == 0.7;
    };

    const auto rightClick = [&shown](float x, float y) {
        const slint::LogicalPosition point({x, y});
        shown.window.dispatch_pointer_move_event(point);
        shown.window.dispatch_pointer_press_event(point, slint::PointerEventButton::Right);
        shown.window.dispatch_pointer_release_event(point, slint::PointerEventButton::Right);
        shown.settle();
    };
    for (const bool pan : {true, false}) {
        INFO((pan ? "pan" : "tilt"));
        patched = 0;
        float hit = -1.0f;
        for (float x = 200.0f; x < at::kWidth - 4.0f && hit < 0.0f; x += 4.0f) {
            rightClick(x, pan ? at::kPanY : at::kTiltY);
            if (!asSet()) {
                hit = x;
            }
        }
        INFO("hit at " << hit);
        REQUIRE(hit > 0.0f);
        if (pan) {
            CHECK(now().panMin == fresh.panMin);
            CHECK(now().panMax == fresh.panMax);
            CHECK(now().tiltMin == 0.2); // the other left alone
            CHECK(now().tiltMax == 0.7);
            CHECK(patch.window().get_pan_min() == 0.0f);
            CHECK(patch.window().get_pan_max() == 100.0f);
        } else {
            CHECK(now().tiltMin == fresh.tiltMin);
            CHECK(now().tiltMax == fresh.tiltMax);
            CHECK(now().panMin == 0.1);
            CHECK(now().panMax == 0.9);
            CHECK(patch.window().get_tilt_min() == 0.0f);
            CHECK(patch.window().get_tilt_max() == 100.0f);
        }
        CHECK(patched == 1);
        // Put back as it was, for the other row.
        patch.window().invoke_pan_range_changed(10.0f, 90.0f);
        patch.window().invoke_tilt_range_changed(20.0f, 70.0f);
        shown.settle();
        REQUIRE(asSet());
    }
}

TEST_CASE("the patch editor's sheets fold from their arrows, say what they hold, and are kept",
          "[ui][dmx]") {
    Rig rig;
    FixturesController patch(rig.runner, rigOfFive());
    const Patching shown(patch, at::kWidth, at::kHeight);
    patch.pick(2);
    shown.settle();
    shown.click(at::kFoldX, at::kWhereY);
    CHECK(patch.window().get_where_folded());
    // A folded, B's heading is 94 px higher (A open is 134 tall, folded 40).
    shown.click(at::kFoldX, at::kChannelsY - 94.0f);
    CHECK(patch.window().get_channels_folded());
    // And C's, under two folded sheets: 50 px below B's heading.
    shown.click(at::kFoldX, at::kChannelsY - 94.0f + 50.0f);
    CHECK(patch.window().get_moves_folded());
    takt4::settings::MachineSettings machine;
    patch.layoutInto(machine);
    CHECK(machine.patchSectionsFolded == std::array<bool, 3>{true, true, true});

    // A second editor opened from those settings opens folded.
    FixturesController again(rig.runner, rigOfFive());
    again.applyLayout(machine);
    CHECK(again.window().get_where_folded());
    CHECK(again.window().get_channels_folded());
    CHECK(again.window().get_moves_folded());

    // Opened again by the same arrows, and the boxes in them still take typing.
    shown.click(at::kFoldX, at::kWhereY);
    CHECK_FALSE(patch.window().get_where_folded());
    patch.applyLayout(takt4::settings::MachineSettings{});
    shown.settle();
    shown.click(at::kAddressX, at::kWhereRowY);
    shown.clearBox();
    shown.type("40");
    shown.enter();
    CHECK(patch.fixtures()[2].address == 40);
}

TEST_CASE("the patch editor lays out the same at any height, and fits its narrowest", "[ui][dmx]") {
    // In a tall window a conditional row takes a share of whatever height is spare. Every sheet
    // here that comes and goes is clipped rather than conditional, which this measures: the sheets'
    // edges down the editor's left margin are where they are at 800 and at 1.6 times that. And
    // "Narrow": at the window's least width every row of the editor fits its pane.
    Rig rig;
    FixturesController patch(rig.runner, rigOfFive());
    patch.pick(2);
    const auto edges = [&patch](int height) {
        const takt4::tests::Shot shot = takt4::tests::render(patch.window(), 1000, height);
        // x = 285 is inside the editor's sheets, left of everything drawn on them.
        std::vector<int> found;
        bool sheet = false;
        for (int y = 0; y < 790; ++y) {
            const slint::Rgb8Pixel p = shot.at(285, y);
            const bool now = !(p.r == 0x0e && p.g == 0x0e && p.b == 0x0e);
            if (now != sheet) {
                found.push_back(y);
                sheet = now;
            }
        }
        return found;
    };
    const std::vector<int> at800 = edges(800);
    const std::vector<int> at1280 = edges(1280);
    INFO("edges at 800: " << at800.size() << ", at 1280: " << at1280.size());
    REQUIRE(at800.size() >= 4); // the top row's sheet and A's, at least
    CHECK(at800 == at1280);

    // With nothing patched, and with a fixture that cannot move (no C), the same.
    patch.pick(1);
    CHECK(edges(800) == edges(1280));

    // The narrowest: the editor's column of sheets gets at least what its rows need, the list
    // having given way first.
    takt4::tests::render(patch.window(), 880, 520);
    slint::platform::update_timers_and_animations();
    takt4::tests::render(patch.window(), 880, 520);
    patch.pick(2);
    takt4::tests::render(patch.window(), 880, 520);
    CHECK(patch.window().get_body_width() >= patch.window().get_body_least_width());
    CHECK(patch.window().get_list_width() >= 200.0f);

    // And a fixture with both of the channels' optional columns, its names and which head: at 880
    // the head column pushed the test buttons and the × off the pane (the render of 2026-10-05).
    Fixture twin;
    twin.name = "twin yoke";
    twin.address = 40;
    twin.channels = {Role::Dimmer, Role::Pan,     Role::PanFine, Role::Tilt,    Role::TiltFine,
                     Role::Pan,    Role::PanFine, Role::Tilt,    Role::TiltFine};
    twin.labels = {"Dimmer",         "Pan Upper",       "Pan Upper fine",
                   "Tilt Upper",     "Tilt Upper fine", "Pan Lower",
                   "Pan Lower fine", "Tilt Lower",      "Tilt Lower fine"};
    twin.parked.assign(twin.channels.size(), 0);
    FixturesController named(rig.runner, {twin});
    named.pick(0);
    REQUIRE(named.window().get_has_labels());
    REQUIRE(named.window().get_has_heads());
    takt4::tests::render(named.window(), 880, 520);
    slint::platform::update_timers_and_animations();
    takt4::tests::render(named.window(), 880, 520);
    INFO("the rows ask for " << named.window().get_body_least_width() << " px and have "
                             << named.window().get_body_width());
    CHECK(named.window().get_body_width() >= named.window().get_body_least_width());
    CHECK(named.window().get_list_width() >= 200.0f);
}

TEST_CASE("every spot in the About box fires what is under it, once", "[ui]") {
    auto about = AboutWindow::create();
    std::vector<std::string> fired;
    about->on_licence_opened([&] { fired.emplace_back("licence"); });
    about->on_notices_opened([&] { fired.emplace_back("notices"); });
    about->on_closed([&] { fired.emplace_back("close"); });
    about->set_version(slint::SharedString("1.0.0"));
    about->set_opened(slint::SharedString("Opened C:\\somewhere\\takt4-LICENSE.txt"));
    about->show();
    auto& window = about->window();
    window.dispatch_scale_factor_change_event(1.0f);
    window.dispatch_resize_event(slint::LogicalSize({580.0f, 640.0f}));
    window.dispatch_window_active_changed_event(true);
    std::map<std::string, int> hits;
    int doubles = 0;
    for (float y = 4.0f; y < 640.0f; y += 6.0f) {
        for (float x = 4.0f; x < 580.0f; x += 6.0f) {
            fired.clear();
            clickAt(window, x, y);
            slint::platform::update_timers_and_animations();
            doubles += fired.size() > 1 ? 1 : 0;
            for (const std::string& one : fired) {
                ++hits[one];
            }
        }
    }
    CHECK(doubles == 0);
    CHECK(hits.size() == 3);
    CHECK(hits["licence"] > 0);
    CHECK(hits["notices"] > 0);
    CHECK(hits["close"] > 0);
    // And the whole of it fits: CLOSE is drawn, inside the window, at the size it opens at.
    const takt4::tests::Shot shot = takt4::tests::render(*about, 580, 640);
    bool closeDrawn = false;
    for (int x = 400; x < 576 && !closeDrawn; ++x) {
        const slint::Rgb8Pixel p = shot.at(x, 612);
        closeDrawn = p.r == 0xef && p.g == 0xee && p.b == 0xe9; // the primary button's face
    }
    CHECK(closeDrawn);
}

TEST_CASE("test pressed on one channel after another reaches each of them", "[ui][dmx]") {
    // What an operator does finding which channel is which: TEST down the column, a click on each.
    // The editor's message line under the top row came and went, and the first TEST's "Channel 11
    // at 255 for 3 s." pushed every row 22 px down under the pointer — the next click landed in the
    // gap between two rows and tested nothing (found testing the build, 2026-09-30). The line is
    // always there now. Each click judged by what the window then says it held.
    Rig rig;
    FixturesController patch(rig.runner, rigOfFive());
    const Patching shown(patch, at::kWidth, at::kHeight);
    patch.pick(2);
    shown.settle();
    for (int i = 0; i < 12; ++i) {
        INFO("channel row " << i);
        shown.click(at::kTestX, at::bandY(i));
        const std::string said(patch.window().get_status());
        CHECK(said.find("Channel " + std::to_string(11 + i) + " at 255") != std::string::npos);
    }
    // And the line's going again does not move them back: a fixture picked clears it.
    patch.pick(1);
    shown.settle();
    CHECK(std::string(patch.window().get_status()).empty());
    patch.pick(2);
    shown.settle();
    shown.click(at::kTestX, at::bandY(4));
    CHECK(std::string(patch.window().get_status()).find("Channel 15 at") != std::string::npos);
}

TEST_CASE("nothing the patch editor does builds its rows again", "[ui][dmx]") {
    // Never destroy the element somebody is holding: a model reset throws every row's
    // elements away and builds new ones — a dropdown's list and a box being typed in with them.
    // The rows are written in place since the redesign; this counts resets on both repeaters
    // through every kind of edit, a fixture switch and an import of a patch of the same shape.
    Rig rig;
    const std::vector<Fixture> original = rigOfFive();
    FixturesController patch(rig.runner, original);
    patch.show();
    const auto channels = std::make_shared<ModelWatch>();
    const auto list = std::make_shared<ModelWatch>();
    patch.window().get_channels()->attach_peer(channels);
    patch.window().get_fixtures()->attach_peer(list);
    auto& window = patch.window();
    patch.pick(2);
    enterParked(window, 1, 50);
    window.invoke_channel_role_picked(3, roleIndexOf(Role::Dimmer));
    window.invoke_channel_added();
    window.invoke_channel_removed(0);
    window.invoke_mode_picked(modeIndexOf("RGB (3ch)"));
    window.invoke_mode_picked(modeIndexOf("moving head 16-bit (12ch)"));
    patch.pick(0);
    patch.pick(3);
    window.invoke_enabled_changed(3, true);
    enterName(window, "head two");
    window.invoke_added();
    window.invoke_duplicated_at(1);
    window.invoke_removed_at(0);
    patch.setFixtures(original);
    patch.pick(4);
    patch.tick();
    CHECK(channels->resets == 0);
    CHECK(list->resets == 0);
    CHECK(channels->changes + channels->added + channels->removed > 0); // it was watching
    CHECK(list->changes + list->added + list->removed > 0);
}

TEST_CASE("the rule editor shows the id a control surface names the rule by", "[ui][trigger]") {
    // `/takt4/ctl/rule/<id>/mute` and the rest name a rule by its id, which the window never showed
    // — the README sent the operator to settings.json for it (the operator, 2026-09-30: "yes you
    // should show rule IDs"). Beside the name, following the rule picked, and gone with none.
    Rig rig;
    RulesController editor(rig.runner, {});
    editor.add();
    editor.add();
    REQUIRE(editor.rules().size() == 2);
    REQUIRE(editor.rules()[0].id != editor.rules()[1].id);
    editor.pick(0);
    CHECK(std::string(editor.window().get_rule_id()) == editor.rules()[0].id);
    editor.pick(1);
    CHECK(std::string(editor.window().get_rule_id()) == editor.rules()[1].id);
    // A rename does not touch it: nothing points at a rule by a name anybody can change.
    editor.rename("renamed");
    CHECK(std::string(editor.window().get_rule_id()) == editor.rules()[1].id);
    editor.setRules({});
    CHECK(std::string(editor.window().get_rule_id()).empty());
}

// --- found reviewing the redesign, 2026-10-01 ---------------------------------------------------

TEST_CASE("nothing that leaves the patch as it was ends a channel's TEST", "[ui][dmx]") {
    // Every re-patch lets go of a TEST hold, and pan, tilt and a parked level re-patched even when
    // they had not changed: a press on a pan handle that moved nothing, or an arrow at the end of
    // its travel, cut a running TEST short.
    Rig rig;
    FixturesController patch(rig.runner, rigOfFive());
    int patched = 0;
    patch.setPatchChanged([&patched](const std::vector<Fixture>&) { ++patched; });
    auto& window = patch.window();
    patch.pick(2);
    takt4::dmx::DmxEngine& engine = rig.runner.transports().dmx();
    // The patch to the output thread, as an edit would post it: the controller does not post the
    // patch it was made with (the owner has it already).
    rig.runner.post(takt4::output::OutputCommand::patch(patch.fixtures()));
    REQUIRE(engine.levels(0).size() >= 16);
    patch.setTestLevel(200);
    patch.testChannel(5); // the dimmer, DMX 16
    REQUIRE(engine.levels(0)[15] == 200);

    window.invoke_pan_range_changed(10.0f, 90.0f);
    window.invoke_tilt_range_changed(20.0f, 70.0f);
    enterParked(window, 1, static_cast<int>(patch.fixtures()[2].parked[1]));
    CHECK(patched == 0);
    CHECK(engine.levels(0)[15] == 200);

    // And a real change does re-patch, and lets go of it — which is why nothing else may.
    window.invoke_pan_range_changed(15.0f, 90.0f);
    CHECK(patched == 1);
    CHECK(engine.levels(0)[15] != 200);
}

TEST_CASE("a click on the pan track with a name being typed moves the handle and keeps the name",
          "[ui][dmx]") {
    // The press takes the keyboard, the name box commits — a turn of the event loop later, between
    // the press and the release — and the controller republished the fixture's stored pan over the
    // handle the press had just jumped, so the release sent the old value back.
    Rig rig;
    FixturesController patch(rig.runner, rigOfFive());
    const Patching shown(patch, at::kWidth, at::kHeight);
    patch.pick(2);
    shown.settle();

    // The low handle at 10 %, found as the pan test finds it.
    float handle = -1.0f;
    for (float x = 400.0f; x < 560.0f && handle < 0.0f; x += 2.0f) {
        const slint::LogicalPosition atPoint({x, at::kPanY});
        shown.window.dispatch_pointer_move_event(atPoint);
        shown.window.dispatch_pointer_press_event(atPoint, slint::PointerEventButton::Left);
        shown.settle();
        if (patch.window().get_pan_min() == 10.0f) {
            const slint::LogicalPosition dragged({x + 20.0f, at::kPanY});
            shown.window.dispatch_pointer_move_event(dragged);
            shown.settle();
            if (patch.window().get_pan_min() > 10.0f) {
                handle = x;
            }
            shown.window.dispatch_pointer_release_event(dragged, slint::PointerEventButton::Left);
        } else {
            shown.window.dispatch_pointer_release_event(atPoint, slint::PointerEventButton::Left);
        }
        shown.settle();
        patch.setPanRange(10.0f, 90.0f);
        shown.settle();
    }
    REQUIRE(handle > 0.0f);

    shown.click(at::kNameX, at::kTopY);
    shown.clearBox();
    shown.type("spot left");
    // Left of the low handle, on the track: the low end jumps there.
    const slint::LogicalPosition left({handle - 14.0f, at::kPanY});
    shown.window.dispatch_pointer_move_event(left);
    shown.window.dispatch_pointer_press_event(left, slint::PointerEventButton::Left);
    shown.settle();
    shown.settle();
    shown.window.dispatch_pointer_release_event(left, slint::PointerEventButton::Left);
    shown.settle();
    CHECK(patch.fixtures()[2].name == "spot left");
    CHECK(patch.fixtures()[2].panMin < 0.1);
    CHECK(patch.fixtures()[2].panMax == 0.9);
    CHECK(patch.window().get_pan_min() == Catch::Approx(patch.fixtures()[2].panMin * 100.0));
}

TEST_CASE("+ add on the channels adds exactly when the universe has room, and says when not",
          "[ui][dmx]") {
    // It was offered, or not, by a rule that disagreed with the controller at the end of the
    // universe: a fixture at 512 with no channels could not take channel 512, and a start address
    // typed and not entered left it offered for a click that then added nothing, silently.
    Rig rig;
    Fixture end = takt4::dmx::fixtureFromMode("end", 0, 0, 512); // a dimmer at 512
    end.channels.clear();
    end.parked.clear();
    std::vector<Fixture> six = rigOfFive();
    six.push_back(end);
    takt4::dmx::ensureFixtureIds(six);
    FixturesController patch(rig.runner, six);
    const Patching shown(patch, at::kWidth, at::kHeight);
    patch.pick(5);
    shown.settle();
    REQUIRE(patch.fixtures()[5].channels.empty());
    shown.click(at::kAddChannelX, at::kChannelsY);
    CHECK(patch.fixtures()[5].channels.size() == 1); // channel 512
    shown.click(at::kAddChannelX, at::kChannelsY);
    CHECK(patch.fixtures()[5].channels.size() == 1); // switched off now: 513 is not a channel

    patch.pick(2); // the 16-bit head at 11
    shown.settle();
    shown.click(at::kAddressX, at::kWhereRowY);
    shown.clearBox();
    shown.type("505"); // 505 to 516: past the end
    shown.click(at::kAddChannelX, at::kChannelsY);
    CHECK(patch.fixtures()[2].address == 505);
    CHECK(patch.fixtures()[2].channels.size() == 12);
    const std::string said(patch.window().get_status());
    INFO(said);
    CHECK(said.find("past the end of the universe") != std::string::npos);
    CHECK(patch.window().get_status_error());
}

TEST_CASE("the patch editor's message goes with the fixture it was about", "[ui][dmx]") {
    // "Identifying head 1..." stayed over head 2 after head 1's x, and "Channel 18 at 255 for 3 s."
    // over a fixture just added.
    Rig rig;
    FixturesController patch(rig.runner, rigOfFive());
    auto& window = patch.window();
    patch.pick(2);
    window.invoke_identify();
    REQUIRE(std::string(window.get_status()).find("Identifying head 1") != std::string::npos);
    window.invoke_removed_at(2);
    CHECK(std::string(window.get_status()).empty());
    window.invoke_channel_tested(0);
    REQUIRE_FALSE(std::string(window.get_status()).empty());
    window.invoke_added();
    CHECK(std::string(window.get_status()).empty());
    window.invoke_channel_tested(0);
    REQUIRE_FALSE(std::string(window.get_status()).empty());
    window.invoke_duplicated_at(0);
    CHECK(std::string(window.get_status()).empty());
    // A fixture taken away from under another one leaves what was said about the one showing.
    int washR = -1;
    for (std::size_t i = 0; i < patch.fixtures().size(); ++i) {
        if (patch.fixtures()[i].name == "wash R") {
            washR = static_cast<int>(i);
        }
    }
    REQUIRE(washR >= 0);
    patch.pick(washR);
    window.invoke_identify();
    std::this_thread::sleep_for(takt4::ui::DeleteGuard::interval() + std::chrono::milliseconds(60));
    window.invoke_removed_at(washR == 0 ? 1 : 0);
    CHECK(std::string(window.get_status()).find("Identifying wash R") != std::string::npos);
}

TEST_CASE("a double-click on a fixture's copy mark makes one copy", "[ui][dmx]") {
    Rig rig;
    FixturesController patch(rig.runner, rigOfFive());
    auto& window = patch.window();
    window.invoke_duplicated_at(1);
    window.invoke_duplicated_at(1); // the second click of the double-click
    CHECK(patch.fixtures().size() == 6);
    std::this_thread::sleep_for(takt4::ui::DeleteGuard::interval() + std::chrono::milliseconds(60));
    window.invoke_duplicated_at(1);
    CHECK(patch.fixtures().size() == 7);
}

TEST_CASE("the mode list lets go of the keyboard once it has picked", "[ui][dmx]") {
    // Left with the keyboard, the next Up or Down picked the neighbouring mode — which replaces the
    // channel map and the parked levels, a custom map gone with no undo.
    Rig rig;
    FixturesController patch(rig.runner, rigOfFive());
    const Patching shown(patch, at::kWidth, at::kHeight);
    patch.pick(1); // the LED par
    shown.settle();
    const std::size_t par = patch.fixtures()[1].channels.size();
    REQUIRE(par == 6);
    shown.click(at::kModeX, at::kModeY); // opened
    shown.type("\x1b");                  // shut, the box focused
    pressKey(shown.window, slint::SharedString(u8"\uF701")); // Down: the next mode, picked
    shown.settle();
    const std::vector<Role> picked = patch.fixtures()[1].channels;
    REQUIRE(picked.size() != par); // the 8-bit head
    pressKey(shown.window, slint::SharedString(u8"\uF701")); // and a second: nothing now
    shown.settle();
    CHECK(patch.fixtures()[1].channels == picked);
}

TEST_CASE("the About box fits its least size and the notices' path stays on its sheet", "[ui]") {
    // A guard, not a bug found: a review said a long path would run off the sheet, and measured on
    // 2026-10-01 it did not — Slint breaks a word too long for its line, word-wrapped or not (this
    // passed with word-wrap too). What did need fixing: the redrawn box's least height was taller
    // than a small laptop's screen at 125 %, so the sheets scroll now and CLOSE stays put.
    auto about = AboutWindow::create();
    about->set_version(slint::SharedString("1.0.0"));
    about->set_opened(slint::SharedString(
        // A user name with nothing a line may break at: everything up to "takt4-" is one word,
        // 72 characters, wider than the sheet at its least width.
        "Opened C:\\Users\\JonathanSandsWorkstationAccount\\AppData\\Local\\Temp\\takt4\\takt4-"
        "THIRD-PARTY-NOTICES.txt"));
    // At its least width: the page's own colour just past the sheets' right edge (x 510) — a sheet
    // pushed wider by an unbreakable line, or the line itself, would be drawn there.
    const takt4::tests::Shot narrow = takt4::tests::render(*about, 520, 640);
    int pastSheet = 0;
    for (int y = 0; y < 600; ++y) {
        for (int x = 512; x < 519; ++x) {
            const slint::Rgb8Pixel p = narrow.at(x, y);
            pastSheet += (p.r == 0x0e && p.g == 0x0e && p.b == 0x0e) ? 0 : 1;
        }
    }
    CHECK(pastSheet == 0);
    // At its least height: CLOSE still drawn, at the bottom, inside the window.
    const takt4::tests::Shot shortest = takt4::tests::render(*about, 520, 320);
    bool closeDrawn = false;
    for (int x = 380; x < 516 && !closeDrawn; ++x) {
        const slint::Rgb8Pixel p = shortest.at(x, 320 - 26);
        closeDrawn = p.r == 0xef && p.g == 0xee && p.b == 0xe9;
    }
    CHECK(closeDrawn);
}

TEST_CASE("TEST and IDENTIFY on a fixture left out of the show say so and light nothing",
          "[ui][dmx]") {
    // A fixture left out is sent nothing, and both said they were lighting it all the same: a
    // TEST that read "channel 3 at 180" over a dark lamp, and an IDENTIFY that flashed nothing.
    Rig rig;
    FixturesController patch(rig.runner, {});
    patch.add();
    patch.rename("Bedroom RGB");
    patch.setUniverse("5");
    patch.setAddress(70);
    patch.pickMode(modeIndexOf("RGB (3ch)"));
    takt4::dmx::DmxEngine& engine = rig.runner.transports().dmx();
    patch.window().invoke_fixture_enabled_changed(false); // the "in the show" tick
    REQUIRE_FALSE(patch.fixtures().front().enabled);
    const auto said = [&patch] { return std::string(patch.window().get_status()); };

    enterTestLevel(patch.window(), 180);
    patch.window().invoke_channel_tested(2);
    CHECK(engine.levels(5)[71] == 0);
    CHECK(said().find("Bedroom RGB is left out of the show") != std::string::npos);
    CHECK(said().find("to test it") != std::string::npos);
    CHECK(patch.window().get_status_error());

    patch.window().invoke_identify();
    CHECK(engine.running() == 0);
    CHECK(said().find("to identify it") != std::string::npos);

    // In the show again, TEST lights it.
    patch.window().invoke_fixture_enabled_changed(true);
    patch.window().invoke_channel_tested(2);
    CHECK(engine.levels(5)[71] == 180);
    CHECK(said().find("left out") == std::string::npos);
}
