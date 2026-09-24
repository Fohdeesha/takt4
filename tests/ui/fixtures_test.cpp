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
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/rule.hpp"
#include "ui/fixtures_controller.hpp"
#include "ui/rules_controller.hpp"

#include <catch2/catch_test_macros.hpp>
#include <slint-platform.h>

#include <algorithm>
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
    window.invoke_name_edited("wash L");
    window.invoke_group_edited("washes");
    window.invoke_address_changed(1);
    CHECK(patch.fixtures()[0].name == "wash L");
    CHECK(patch.fixtures()[0].group == "washes");

    // 3. A second one, which lands *after* the first rather than on top of it. That is the
    //    arithmetic an operator would otherwise do by hand for every fixture in a row.
    window.invoke_added();
    REQUIRE(patch.fixtures().size() == 2);
    CHECK(patch.fixtures()[1].address == 4); // the par occupies 1-3
    window.invoke_name_edited("wash R");
    window.invoke_group_edited("washes");

    // 4. And a moving head, which is a different shape — picked from the mode list rather
    //    than assembled a channel at a time.
    window.invoke_added();
    window.invoke_name_edited("head 1");
    window.invoke_group_edited("heads");
    window.invoke_address_changed(11);
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
        window.invoke_channel_parked_changed(4, 200);
        CHECK(patch.fixtures()[2].parked[4] == 200);
        window.invoke_channel_removed(11);
        CHECK(patch.fixtures()[2].channels.size() == 11);
        // The parked levels stay the same length as the map, or a fixture patched later
        // would take its levels from the wrong channels.
        CHECK(patch.fixtures()[2].parked.size() == 11);
    }

    SECTION("a universe that is not one is refused rather than written half-typed") {
        window.invoke_universe_edited("4");
        CHECK(patch.fixtures()[2].universe == 4);
        window.invoke_universe_edited("not a universe");
        CHECK(patch.fixtures()[2].universe == 4); // unchanged, not zeroed on the way past
        window.invoke_universe_edited("1:2:3");
        CHECK(patch.fixtures()[2].universe == 0x123);
    }

    SECTION("a duplicate lands after the fixture it came from, with a name of its own") {
        window.invoke_picked(0);
        window.invoke_duplicated();
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
        window.invoke_name_edited("front wash");
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

TEST_CASE("a name typed for one fixture stays with it when another is clicked", "[ui][dmx]") {
    // The audit's H10, with the gestures an operator makes. The name box was bound one way, and
    // a one-way binding dies on the first keystroke: after typing a name and clicking another
    // fixture in the list, the box went on showing the first one's text, and the focus-out that
    // Slint runs a loop late renamed the *second* fixture to it.
    Rig rig;
    FixturesController patch(rig.runner, {takt4::dmx::fixtureFromMode("left", 1, 0, 1),
                                          takt4::dmx::fixtureFromMode("right", 1, 0, 10)});
    patch.show();
    auto& window = patch.window().window();
    window.dispatch_scale_factor_change_event(1.0f);
    window.dispatch_resize_event(slint::LogicalSize({1100.0f, 800.0f}));
    window.dispatch_window_active_changed_event(true);
    REQUIRE(patch.selected() == 0);

    // The name box: the first spot in the editor pane where a typed letter lands in the name.
    bool inBox = false;
    for (float y = 20.0f; y < 200.0f && !inBox; y += 6.0f) {
        for (float x = 340.0f; x < 700.0f && !inBox; x += 40.0f) {
            clickAt(window, x, y);
            typeText(window, "Q");
            inBox = std::string(patch.window().get_name()).find('Q') != std::string::npos;
        }
    }
    INFO("no click landed in the name box");
    REQUIRE(inBox);
    typeText(window, "X");
    const std::string typed(patch.window().get_name());
    REQUIRE(typed.find("QX") != std::string::npos);

    // The second fixture in the list, found by clicking down the list column.
    bool switched = false;
    for (float y = 30.0f; y < 400.0f && !switched; y += 6.0f) {
        clickAt(window, 120.0f, y);
        slint::platform::update_timers_and_animations();
        switched = patch.selected() == 1;
    }
    REQUIRE(switched);
    // Whatever Slint runs a loop late has run.
    slint::platform::update_timers_and_animations();

    CHECK(patch.fixtures()[0].name == typed);   // the name went to the fixture it was typed for
    CHECK(patch.fixtures()[1].name == "right"); // and not to the one clicked
    CHECK(std::string(patch.window().get_name()) == "right"); // and the box shows the new one
}

namespace {

void pressKey(slint::Window& window, const slint::SharedString& key) {
    window.dispatch_key_press_event(key);
    window.dispatch_key_release_event(key);
}

/// Opens the dropdown under (x, y), moves it one entry down and closes it: what an operator
/// does with the arrow keys, and a gesture that starts from whatever the dropdown is *showing*.
void stepDropdown(slint::Window& window, float x, float y) {
    clickAt(window, x, y);
    slint::platform::update_timers_and_animations();
    pressKey(window, slint::SharedString(u8"")); // Key.DownArrow
    pressKey(window, slint::SharedString(u8"\u001b")); // Key.Escape
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
    reset();
    clickAt(window, kDoesColumn, comboY);
    for (int redraw = 0; redraw < 10; ++redraw) {
        settle();
    }
    clickAt(window, kDoesColumn, entryY);
    settle();
    INFO("dropdown at " << kDoesColumn << ", " << comboY << "; entry at " << entryY);
    CHECK(patch.fixtures()[0].channels[0] == picked);
}

TEST_CASE("the list's ADD, COPY and DELETE buttons do what they say when clicked", "[ui][dmx]") {
    // The audit's T1: the patching test above said "by clicking" and called the controller.
    // These are the three buttons under the list, pressed with the pointer.
    //
    // Every click is judged by what it did to the patch, with the selection put back on the
    // moving head before the next one, so the three cannot be mistaken for each other or for a
    // miss: ADD puts a three-channel par at the end, COPY puts a second head straight after the
    // first, and DELETE takes the head away.
    Rig rig;
    const auto headMode = static_cast<std::size_t>(modeIndexOf("moving head 16-bit (12ch)") - 1);
    FixturesController patch(rig.runner, {takt4::dmx::fixtureFromMode("head", headMode, 0, 1)});
    REQUIRE(patch.fixtures()[0].channels.size() == 12);
    const std::string headId = patch.fixtures()[0].id;
    REQUIRE_FALSE(headId.empty());
    patch.show();
    auto& window = patch.window().window();
    window.dispatch_scale_factor_change_event(1.0f);
    window.dispatch_resize_event(slint::LogicalSize({1100.0f, 800.0f}));
    window.dispatch_window_active_changed_event(true);
    patch.tick();
    slint::platform::update_timers_and_animations();

    const auto headAt = [&patch, &headId] {
        const std::vector<Fixture>& now = patch.fixtures();
        for (std::size_t i = 0; i < now.size(); ++i) {
            if (now[i].id == headId) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    bool added = false;
    bool copied = false;
    bool deleted = false;
    std::string misread;
    // The buttons are the bottom row of the list column; swept from the bottom up, so the row
    // is met before anything the clicks have added to the list above it.
    for (float y = 790.0f; y > 400.0f && !deleted; y -= 6.0f) {
        for (float x = 16.0f; x < 330.0f && !deleted; x += 12.0f) {
            const int head = headAt();
            patch.pick(head);
            patch.tick();
            slint::platform::update_timers_and_animations();
            const std::vector<Fixture> before = patch.fixtures();

            clickAt(window, x, y);
            patch.tick();
            slint::platform::update_timers_and_animations();

            const std::vector<Fixture>& now = patch.fixtures();
            if (now.size() == before.size() + 1) {
                // The new one is the fixture whose id was not there before.
                std::size_t fresh = now.size();
                for (std::size_t i = 0; i < now.size() && fresh == now.size(); ++i) {
                    const bool known = std::any_of(before.begin(), before.end(), [&](const Fixture& f) {
                        return f.id == now[i].id;
                    });
                    if (!known) {
                        fresh = i;
                    }
                }
                const bool headStayed = headAt() == head;
                if (headStayed && fresh + 1 == now.size() && now[fresh].channels.size() == 3) {
                    added = true;
                } else if (headStayed && fresh == static_cast<std::size_t>(head) + 1 &&
                           now[fresh].channels.size() == 12) {
                    copied = true;
                } else {
                    misread = "a click added a fixture that was neither a par nor a copy";
                }
            } else if (now.size() + 1 == before.size()) {
                if (headAt() < 0) {
                    deleted = true;
                } else {
                    misread = "a click removed a fixture other than the selected one";
                }
            }
        }
    }
    INFO(misread);
    CHECK(misread.empty());
    CHECK(added);
    CHECK(copied);
    CHECK(deleted);
}
