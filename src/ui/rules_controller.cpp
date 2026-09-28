// `RulesController`: the rule list and the edits to a rule. What is drawn from them is in
// `rules_controller_publish.cpp`, the edits to a rule's slots in `rules_controller_slots.cpp`,
// the words in `rule_text` and the ready-made rules in `rule_presets`.

#include "ui/rules_controller.hpp"

#include "core/features/intensity.hpp"
#include "core/io/utf8.hpp"
#include "core/trigger/generator.hpp"
#include "ui/model_rows.hpp"
#include "ui/native_window.hpp"
#include "ui/rule_presets.hpp"
#include "ui/rule_text.hpp"
#include "ui/rules_editor_support.hpp"
#include "ui/window_state.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace takt4::ui {

using namespace rule_presets;
using namespace rule_text;
using namespace rules_detail;
using trigger::Generator;
using trigger::GeneratorKind;
using trigger::Rule;

RulesController::RulesController(output::OutputRunner& runner,
                                 std::vector<trigger::Rule::Config> rules)
    : runner_(runner), rules_(std::move(rules)), window_(RulesWindow::create()),
      listModel_(std::make_shared<slint::VectorModel<RuleRow>>()),
      logModel_(std::make_shared<slint::VectorModel<slint::SharedString>>()) {
    window_->set_rules(listModel_);
    window_->set_output_choices(choiceRows_.model());
    window_->set_fixture_choices(fixtureRows_.model());
    window_->set_slots(slotRows_.model());
    window_->set_palette(paletteRows_.model());
    window_->set_follow_ups(followRows_.model());
    window_->set_log(logModel_);

    // The dropdowns, filled from the enums themselves so a kind added to `core/trigger`
    // appears here without anyone remembering to add it.
    auto triggers = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::Trigger which : trigger::kTriggers) {
        triggers->push_back(shared(std::string(trigger::labelOf(which))));
    }
    window_->set_trigger_names(triggers);

    auto sends = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::Message::Kind kind : trigger::kMessageKinds) {
        sends->push_back(shared(std::string(trigger::labelOf(kind))));
    }
    window_->set_send_kinds(sends);

    auto kinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const GeneratorKind kind : trigger::kGeneratorKinds) {
        kinds->push_back(shared(std::string(trigger::labelOf(kind))));
    }
    window_->set_generator_kinds(kinds);
    // A color's chip offers only what can make a color (`trigger::handsBackValues`).
    auto colorKinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const GeneratorKind kind : trigger::kColorGeneratorKinds) {
        colorKinds->push_back(shared(std::string(trigger::labelOf(kind))));
    }
    window_->set_color_generator_kinds(colorKinds);

    auto sources = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::LiveSource source : trigger::kLiveSources) {
        sources->push_back(shared(std::string(trigger::labelOf(source))));
    }
    window_->set_live_sources(sources);

    auto shapes = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::RampShape shape : trigger::kRampShapes) {
        shapes->push_back(shared(std::string(trigger::labelOf(shape))));
    }
    window_->set_ramp_shapes(shapes);

    auto hosts = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const HostPreset& preset : kHostPresets) {
        hosts->push_back(slint::SharedString(preset.label));
    }
    window_->set_host_presets(hosts);

    auto rigs = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const char* label : kRigPresets) {
        rigs->push_back(slint::SharedString(label));
    }
    window_->set_rig_presets(rigs);

    auto units = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::DelayUnit unit : trigger::kDelayUnits) {
        units->push_back(shared(std::string(trigger::labelOf(unit))));
    }
    window_->set_follow_up_units(units);

    // The lighting dropdowns, filled from `core/dmx`'s own tables for the same reason the
    // others are: an effect added there appears here without anyone remembering to add it.
    auto effects = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::EffectKind kind : dmx::kEffectKinds) {
        effects->push_back(shared(std::string(dmx::labelOf(kind))));
    }
    window_->set_effect_kinds(effects);

    auto roles = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::Role role : dmx::kAimableRoles) {
        roles->push_back(shared(std::string(dmx::labelOf(role))));
    }
    window_->set_effect_roles(roles);

    auto curves = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::Curve curve : dmx::kCurves) {
        curves->push_back(shared(std::string(dmx::labelOf(curve))));
    }
    window_->set_effect_curves(curves);

    auto paths = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::PathShape shape : dmx::kPathShapes) {
        paths->push_back(shared(std::string(dmx::labelOf(shape))));
    }
    window_->set_effect_shapes(paths);

    auto colorModes = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::ColorMode mode : trigger::kColorModes) {
        colorModes->push_back(shared(std::string(trigger::labelOf(mode))));
    }
    window_->set_color_modes(colorModes);

    // **Every action commits what was being typed before it runs** (the audit of 2026-09-25,
    // M18) — see `typing_`. A box's own commit is not wrapped: its setter clears its own record
    // (`typed`), and while it has the keyboard no other box can have one. What the box keystrokes
    // are is said once, here, rather than at the top of forty setters, where the forty-first
    // would have been the one that forgot.
    const auto finishing = [this](auto action) {
        return [this, action](auto... args) {
            commitTyping();
            action(args...);
        };
    };
    // And a box's own commit is dropped when it only repeats what an action already committed
    // for it — see `echo_`. By then the action may have put another rule, or another row, under
    // the box: + ADD shows the new rule's "every" in the box that was typed 7 into, and the late
    // commit of that 7 went to the new rule.
    const auto number = [](int n) { return slint::SharedString(std::to_string(n)); };

    window_->on_rule_picked(finishing([this](int index) { pick(index); }));
    window_->on_rule_picked_with(finishing(
        [this](int index, bool control, bool shift) { pickWith(index, control, shift); }));
    window_->on_rule_added(finishing([this] { add(); }));
    window_->on_rule_removed(finishing([this] { remove(); }));
    window_->on_rule_duplicated(finishing([this] { duplicate(); }));
    // Through `DeleteGuard`, which drops the second click of a double-click on ×: the row
    // below moves up under the pointer and would take it (the audit of 2026-09-25, L10).
    window_->on_rule_removed_at(finishing([this](int index) {
        if (ruleMarks_.press(index)) {
            removeAt(index);
        }
    }));
    window_->on_rule_duplicated_at(finishing([this](int index) { duplicateAt(index); }));
    window_->on_rig_added(finishing([this](int index) { addRig(index); }));
    window_->on_rule_enabled_changed(finishing([this](bool on) { setEnabled(on); }));
    window_->on_rule_muted_changed(finishing([this](bool on) { setMuted(on); }));
    window_->on_rule_rate_changed(finishing([this](float factor) { nudgeRate(factor); }));
    // The name commits on every keystroke, so there is never anything of its own to carry.
    window_->on_rule_renamed([this](const slint::SharedString& n) { rename(std::string(n)); });
    window_->on_rule_tested(finishing([this] { test(); }));
    window_->on_panic_clicked(finishing([this] { panic(); }));
    window_->on_panic_released(finishing([this] { releasePanic(); }));

    window_->on_trigger_picked(finishing([this](int index) { pickTrigger(index); }));
    window_->on_every_changed([this, number](int every) {
        if (!echoes(TypedIn::Rule, kEveryBox, number(every))) {
            setEvery(every);
        }
    });
    window_->on_pulses_changed([this, number](int pulses) {
        if (!echoes(TypedIn::Rule, kPulsesBox, number(pulses))) {
            setPulses(pulses);
        }
    });
    window_->on_output_chosen(
        finishing([this](const slint::SharedString& name, bool chosen) {
            setOutputChosen(std::string(name), chosen);
        }));
    window_->on_all_outputs_chosen(finishing([this] { chooseAllOutputs(); }));
    window_->on_fixture_chosen(
        finishing([this](const slint::SharedString& name, bool chosen) {
            setFixtureChosen(std::string(name), chosen);
        }));
    window_->on_no_fixtures_chosen(finishing([this] { chooseNoFixtures(); }));
    window_->on_effect_picked(finishing([this](int index) { pickEffect(index); }));
    window_->on_effect_role_picked(finishing([this](int index) { pickRole(index); }));
    window_->on_effect_curve_picked(finishing([this](int index) { pickCurve(index); }));
    window_->on_effect_shape_picked(finishing([this](int index) { pickPathShape(index); }));
    window_->on_effect_duration_edited([this](const slint::SharedString& text) {
        if (!echoes(TypedIn::Rule, kDurationBox, text)) {
            setDuration(std::string(text));
        }
    });
    window_->on_effect_unit_picked(finishing([this](int unit) { pickDurationUnit(unit); }));
    window_->on_effect_base_changed([this, number](int level) {
        if (!echoes(TypedIn::Rule, kBaseBox, number(level))) {
            setBase(level);
        }
    });
    window_->on_effect_cycles_edited([this](const slint::SharedString& text) {
        if (!echoes(TypedIn::Rule, kCyclesBox, text)) {
            setCycles(std::string(text));
        }
    });
    window_->on_effect_duty_changed(finishing([this](float percent) { setDuty(percent); }));
    window_->on_effect_hue_edited([this](const slint::SharedString& text) {
        if (!echoes(TypedIn::Rule, kHueBox, text)) {
            setHueRange(std::string(text));
        }
    });
    window_->on_effect_size_changed(finishing([this](float percent) { setSize(percent); }));
    window_->on_color_mode_picked(finishing([this](int index) { pickColorMode(index); }));
    window_->on_palette_added(finishing([this] { addPaletteColor(); }));
    window_->on_palette_removed(finishing([this](int index) { removePaletteColor(index); }));
    window_->on_palette_color_changed(
        finishing([this](int index, float hue, float sat, float val) {
            setPaletteColor(index, hue, sat, val);
        }));
    window_->on_palette_hex_edited([this](int index, const slint::SharedString& text) {
        if (!echoes(TypedIn::Palette, 0, text)) {
            setPaletteHex(index, std::string(text));
        }
    });
    window_->on_palette_typed([this](int index, const slint::SharedString& t) {
        noteTyping(TypedIn::Palette, index, 0, std::string(t));
    });
    window_->on_picker_open_changed([this](bool open) { setPickerOpen(open); });

    // Widened explicitly, here and on `probability` below: Slint hands a slider's value over
    // as a float and both settings are held as double. GCC and Clang refuse the implicit
    // promotion under -Wdouble-promotion -Werror, and MSVC accepts it silently — which is
    // why these two were still here after the same fix went in for the engine.
    window_->on_min_confidence_changed(
        finishing([this](float v) { setMinConfidence(static_cast<double>(v)); }));
    window_->on_intensity_changed(
        finishing([this](int which, bool on) { setIntensity(which, on); }));
    window_->on_bpm_range_edited([this](const slint::SharedString& t) {
        if (!echoes(TypedIn::Rule, kBpmRangeBox, t)) {
            setBpmRange(std::string(t));
        }
    });
    window_->on_probability_changed(
        finishing([this](float v) { setProbability(static_cast<double>(v)); }));
    window_->on_cooldown_changed([this](const slint::SharedString& t) {
        if (!echoes(TypedIn::Rule, kCooldownBox, t)) {
            setCooldown(std::string(t));
        }
    });

    window_->on_send_picked(finishing([this](int index) { pickSend(index); }));
    window_->on_address_edited([this](const slint::SharedString& a) {
        if (!echoes(TypedIn::Rule, kAddressBox, a)) {
            setAddress(std::string(a));
        }
    });
    window_->on_channel_changed([this, number](int channel) {
        if (!echoes(TypedIn::Rule, kChannelBox, number(channel))) {
            setChannel(channel);
        }
    });
    window_->on_host_preset_picked(finishing([this](int index) { pickHostPreset(index); }));
    window_->on_send_value_changed(finishing([this](bool on) { setSendValue(on); }));
    window_->on_rule_typed([this](int field, const slint::SharedString& t) {
        noteTyping(TypedIn::Rule, 0, field, std::string(t));
    });

    window_->on_follow_up_added(finishing([this] { addFollowUp(); }));
    window_->on_follow_up_removed(finishing([this](int index) {
        if (followMarks_.press(index)) { // a double-click on × is one deletion (L10)
            removeFollowUp(index);
        }
    }));
    window_->on_follow_kind_picked(
        finishing([this](int index, int choice) { pickFollowKind(index, choice); }));
    // A × on a row above moves the box that had the keyboard onto the next follow-up before its
    // late commit arrives, which is the other thing the echo is for.
    window_->on_follow_number_changed([this, number](int index, int n) {
        if (!echoes(TypedIn::FollowUp, 2, number(n))) {
            setFollowNumber(index, n);
        }
    });
    window_->on_follow_value_edited([this](int index, const slint::SharedString& t) {
        if (!echoes(TypedIn::FollowUp, 0, t)) {
            setFollowValue(index, std::string(t));
        }
    });
    window_->on_follow_delay_edited([this](int index, const slint::SharedString& t) {
        if (!echoes(TypedIn::FollowUp, 1, t)) {
            setFollowDelay(index, std::string(t));
        }
    });
    window_->on_follow_unit_picked(
        finishing([this](int index, int unit) { pickFollowUnit(index, unit); }));

    window_->on_slot_kind_picked(finishing([this](int slot, int kind) { pickSlotKind(slot, kind); }));
    window_->on_slot_pool_changed(finishing([this](int slot, bool list) { setSlotPool(slot, list); }));
    window_->on_slot_range_edited([this](int slot, const slint::SharedString& t) {
        if (!echoes(TypedIn::Slot, 1, t)) {
            setSlotRange(slot, std::string(t));
        }
    });
    window_->on_slot_values_edited([this](int slot, const slint::SharedString& t) {
        if (!echoes(TypedIn::Slot, 2, t)) {
            setSlotValues(slot, std::string(t));
        }
    });
    window_->on_slot_typed([this](int slot, int field, const slint::SharedString& t) {
        noteTyping(TypedIn::Slot, slot, field, std::string(t));
    });
    window_->on_follow_typed([this](int index, int field, const slint::SharedString& t) {
        noteTyping(TypedIn::FollowUp, index, field, std::string(t));
    });
    window_->on_slot_normalise_edited([this](int slot, const slint::SharedString& t) {
        if (!echoes(TypedIn::Slot, 4, t)) {
            setSlotNormalise(slot, std::string(t));
        }
    });
    window_->on_slot_weights_edited([this](int slot, const slint::SharedString& t) {
        if (!echoes(TypedIn::Slot, 3, t)) {
            setSlotWeights(slot, std::string(t));
        }
    });
    window_->on_slot_no_repeat_changed([this, number](int slot, int n) {
        if (!echoes(TypedIn::Slot, 5, number(n))) {
            setSlotNoRepeat(slot, n);
        }
    });
    window_->on_slot_color_changed(
        finishing([this](int slot, float hue, float saturation, float bright) {
            setSlotColor(slot, hue, saturation, bright);
        }));
    window_->on_slot_fixed_edited([this](int slot, const slint::SharedString& t) {
        if (!echoes(TypedIn::Slot, 0, t)) {
            setSlotFixed(slot, std::string(t));
        }
    });
    window_->on_slot_live_picked(
        finishing([this](int slot, int source) { pickSlotLive(slot, source); }));
    window_->on_slot_shape_picked(
        finishing([this](int slot, int shape) { pickSlotShape(slot, shape); }));
    window_->on_slot_ramp_bars_changed([this, number](int slot, int bars) {
        if (!echoes(TypedIn::Slot, 6, number(bars))) {
            setSlotRampBars(slot, bars);
        }
    });
    window_->on_slot_ramp_float_changed(
        finishing([this](int slot, bool asFloat) { setSlotRampFloat(slot, asFloat); }));

    window_->on_log_cleared(finishing([this] { clearLog(); }));

    // The size this opens at, set here rather than in the markup: Slint takes a window's
    // initial size from what its content asks for, not from the Window's own
    // `preferred-width` — so this window opened at its minimum, 900 x 560, whatever the
    // markup preferred. See `kRulesWindowWidth`. Before the first `show()`, which is what
    // makes the backend leave it alone; a later drag is the operator's and is kept.
    // No larger than the screen has room for, though: see `fitToScreen` (the audit's M26).
    const LogicalExtent opening = fitToScreen({kRulesWindowWidth, kRulesWindowHeight});
    window_->window().set_size(slint::LogicalSize({opening.width, opening.height}));

    // The window's own close box goes through `hide` like CLOSE does, so what was being typed is
    // committed and `visible_` is told (the audit of 2026-09-25, M16).
    window_->window().on_close_requested([this] {
        hide();
        return slint::CloseRequestResponse::HideWindow;
    });

    resettle(0);
    publishAll();
}

void RulesController::show() {
    window_->show();
    visible_ = true;
    // **And brought forward**, which `show()` does not do for a window that is already up: it
    // leaves it exactly where it was in the Z order, so pressing TRIGGERS with the editor
    // behind the main window looked like a button that did nothing. Reported from a rig.
    //
    // By a substring of the title, which is how `bringWindowToFront` finds a window at all —
    // "triggers" appears in this window's title and in no other of ours. A no-op away from
    // Windows and under the testing backend, where there is no platform window to raise.
    (void)bringWindowToFront("triggers");
}

void RulesController::hide() {
    commitTyping();
    window_->hide();
    visible_ = false;
    // A picker goes with the window, and nothing in the markup will say so — a popup reports
    // that it closed, not that the window under it went away. Left set, it would hold every
    // repeater rebuild for the rest of the session (see `pickerOpen_`), which is the "editing
    // one changes them all" bug coming back by the side door. Nothing is on screen and nothing
    // has a pointer grab, so clearing it here is safe in a way that clearing it anywhere else
    // would not be.
    pickerOpen_ = false;
}

const trigger::Rule::Config* RulesController::findRule(std::string_view id) const noexcept {
    for (const Rule::Config& rule : rules_) {
        if (rule.id == id) {
            return &rule;
        }
    }
    return nullptr;
}

std::uint64_t RulesController::freshSeed(const std::vector<Rule::Config>& pending) const {
    // 977 apart, as seeds always were: a rule's generators take the seeds just above its own
    // (`matchSegmentsToAddress`), so neighbours need room between them.
    constexpr std::uint64_t kStride = 977;
    std::uint64_t seed = 1;
    for (const auto* set : {&rules_, &pending}) {
        for (const Rule::Config& rule : *set) {
            seed = std::max(seed, rule.seed + kStride);
        }
    }
    return seed;
}

trigger::Rule::Config* RulesController::current() noexcept {
    if (selected_ < 0 || static_cast<std::size_t>(selected_) >= rules_.size()) {
        return nullptr;
    }
    return &rules_[static_cast<std::size_t>(selected_)];
}

const trigger::Rule::Config* RulesController::current() const noexcept {
    return const_cast<RulesController*>(this)->current();
}

void RulesController::noteTyping(TypedIn where, int index, int field, std::string text) {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    typing_ = Typing{rule->id, where, index, field, std::move(text)};
    echo_.reset();
}

void RulesController::rowRemoved(TypedIn where, int index) noexcept {
    // What was being typed into a row follows the row: gone with the one removed, or up one
    // with a row below it. Left at its old index, the next `commitTyping` wrote it into the row
    // that took that place, or into nothing (the 2026-09-25 audit's L13). The window commits
    // before a × runs; any other caller does not.
    const Rule::Config* rule = current();
    if (!typing_ || typing_->where != where || rule == nullptr || typing_->ruleId != rule->id) {
        return;
    }
    if (typing_->index == index) {
        typing_.reset();
    } else if (typing_->index > index) {
        --typing_->index;
    }
}

void RulesController::typed(TypedIn where, int index, int field) noexcept {
    if (typing_ && typing_->where == where && typing_->index == index && typing_->field == field) {
        typing_.reset();
    }
}

bool RulesController::typingInto(RuleBox box) const noexcept {
    const Rule::Config* rule = current();
    return typing_ && rule != nullptr && typing_->ruleId == rule->id &&
           typing_->where == TypedIn::Rule && typing_->field == box;
}

bool RulesController::echoes(TypedIn where, int field, const slint::SharedString& text) noexcept {
    // Not by row: the row is exactly what may have moved. By the kind of box and what it holds,
    // which only a keystroke can change — and a keystroke clears the echo.
    if (!echo_ || echo_->where != where || echo_->field != field ||
        std::string_view(text) != echo_->text) {
        return false;
    }
    echo_.reset();
    return true;
}

void RulesController::commitTyping() {
    if (!typing_) {
        return;
    }
    const Typing pending = *typing_;
    typing_.reset();
    const Rule::Config* rule = current();
    if (rule == nullptr || rule->id != pending.ruleId) {
        return; // the rule it was typed for is not the one showing; nothing to put it in
    }
    echo_ = pending;
    const std::optional<int> number = typedNumber(pending.text);
    switch (pending.where) {
    case TypedIn::FollowUp:
        if (pending.field == 0) {
            setFollowValue(pending.index, pending.text);
        } else if (pending.field == 1) {
            setFollowDelay(pending.index, pending.text);
        } else if (number) {
            setFollowNumber(pending.index, *number);
        }
        return;
    case TypedIn::Palette:
        setPaletteHex(pending.index, pending.text);
        return;
    case TypedIn::Rule:
        switch (pending.field) {
        case kAddressBox:
            setAddress(pending.text);
            break;
        case kBpmRangeBox:
            setBpmRange(pending.text);
            break;
        case kCooldownBox:
            setCooldown(pending.text);
            break;
        case kDurationBox:
            setDuration(pending.text);
            break;
        case kCyclesBox:
            setCycles(pending.text);
            break;
        case kHueBox:
            setHueRange(pending.text);
            break;
        case kEveryBox:
            if (number) {
                setEvery(*number);
            }
            break;
        case kPulsesBox:
            if (number) {
                setPulses(*number);
            }
            break;
        case kChannelBox:
            if (number) {
                setChannel(*number);
            }
            break;
        case kBaseBox:
            if (number) {
                setBase(*number);
            }
            break;
        default:
            break;
        }
        return;
    case TypedIn::Slot:
        break;
    }
    switch (pending.field) {
    case 0:
        setSlotFixed(pending.index, pending.text);
        break;
    case 1:
        setSlotRange(pending.index, pending.text);
        break;
    case 2:
        setSlotValues(pending.index, pending.text);
        break;
    case 3:
        setSlotWeights(pending.index, pending.text);
        break;
    case 4:
        setSlotNormalise(pending.index, pending.text);
        break;
    case 5:
        if (number) {
            setSlotNoRepeat(pending.index, *number);
        }
        break;
    case 6:
        if (number) {
            setSlotRampBars(pending.index, *number);
        }
        break;
    default:
        break;
    }
}

void RulesController::choseSlot(int slot) noexcept {
    Rule::Config* rule = current();
    if (rule == nullptr || slot < 0) {
        return;
    }
    const std::vector<trigger::Slot> layout = trigger::slotLayout(*rule);
    const auto index = static_cast<std::size_t>(slot);
    if (index < layout.size() && layout[index].role == trigger::SlotRole::Number) {
        rule->numberChosen = true;
    }
}

Generator::Config* RulesController::slotConfig(int slot) noexcept {
    // Through `trigger::slotLayout`, which `publishSlots` builds the rows from and which is
    // tested against the order a fire records: the index of a row is the index of the slot,
    // for every kind and every effect, with no second walk of the conditions to keep in step.
    Rule::Config* rule = current();
    if (rule == nullptr || slot < 0) {
        return nullptr;
    }
    const std::vector<trigger::Slot> layout = trigger::slotLayout(*rule);
    const auto index = static_cast<std::size_t>(slot);
    return index < layout.size() ? &trigger::slotGenerator(*rule, layout[index]) : nullptr;
}

void RulesController::commit() {
    // Whatever the status line was saying was about the *last* edit — "a range is two
    // numbers", most often — and this one worked. Cleared here rather than by a timer so
    // the message stays up for exactly as long as it is the last thing that happened; the
    // few callers that raise a status of their own do it after committing.
    setStatus({}, false);
    runner_.post(output::OutputCommand::rules(rules_));
    if (changed_) {
        changed_(rules_);
    }
    publishList();
    publishSlots();
    // With the slots, because these rows describe the same rule and read from the same fields
    // — a release's summary names the channel, so changing the channel has to move it.
    publishFollowUps();
}

void RulesController::matchSegmentsToAddress(Rule::Config& rule) {
    // §5.8's validity is mostly this count, so an edit that adds a `{}` should hand the
    // operator a chip rather than a rule that refuses to fire until they work out why.
    // Extra generators are kept until the address stops needing them, and a shortened
    // address drops from the end — which is where a deleted placeholder came from.
    const std::size_t wanted = trigger::countPlaceholders(rule.address);
    while (rule.segments.size() < wanted) {
        Generator::Config segment;
        // Shuffle over 1-8 is §5.8's own default and the useful thing for a clip grid.
        segment.seed = rule.seed + rule.segments.size() + 1;
        rule.segments.push_back(segment);
    }
    if (rule.segments.size() > wanted) {
        rule.segments.resize(wanted);
    }
}

void RulesController::setRules(std::vector<trigger::Rule::Config> rules) {
    // Nothing half-typed is carried into a set it was not typed for.
    typing_.reset();
    // A whole new set, so the counts and last values start again. A preset may well reuse
    // the ids of the set it replaces — `add` numbers them "rule1", "rule2" — and a card
    // inheriting a number from a different rule that happened to share its id is a readout
    // that is quietly wrong, which is worse than one that reads zero.
    firesSeen_.clear();
    slotsSeen_.clear();
    // And the mutes and rates last seen on the running rules, which the loaded set starts without
    // (the audit of 2026-09-25, M11): kept, they showed a rule muted that the new set had not.
    mutedSeen_.clear();
    rateSeen_.clear();
    rules_ = std::move(rules);
    resettle(selected_ < 0 ? 0 : selected_);
    publishAll();
}

std::vector<int> RulesController::chosen() const {
    std::vector<int> rows;
    for (std::size_t i = 0; i < chosen_.size(); ++i) {
        if (chosen_[i]) {
            rows.push_back(static_cast<int>(i));
        }
    }
    return rows;
}

void RulesController::resettle(int wanted) {
    chosen_.assign(rules_.size(), false);
    if (rules_.empty()) {
        selected_ = -1;
        anchor_ = -1;
        return;
    }
    selected_ = std::clamp(wanted, 0, static_cast<int>(rules_.size()) - 1);
    chosen_[static_cast<std::size_t>(selected_)] = true;
    anchor_ = selected_;
}

void RulesController::pick(int index) {
    pickWith(index, false, false);
}

void RulesController::pickWith(int index, bool control, bool shift) {
    if (index < 0 || static_cast<std::size_t>(index) >= rules_.size()) {
        return;
    }
    // What is half-typed in a box goes to the rule it was typed into, before that stops being
    // the rule on screen — see `typing_`.
    commitTyping();
    chosen_.resize(rules_.size(), false);
    if (control) {
        // In or out, one row at a time. Never out of the last one: a selection of nothing has
        // no editor to show and no rule for the marks to act on, so a control-click that
        // would empty it simply leaves that row selected.
        chosen_[static_cast<std::size_t>(index)] = !chosen_[static_cast<std::size_t>(index)];
        if (chosen().empty()) {
            chosen_[static_cast<std::size_t>(index)] = true;
        }
        anchor_ = index;
    } else if (shift && anchor_ >= 0 && static_cast<std::size_t>(anchor_) < rules_.size()) {
        // The run from the anchor to here, replacing whatever was chosen. The anchor stays
        // put, so shift-clicking again grows or shrinks the same run.
        const int low = std::min(anchor_, index);
        const int high = std::max(anchor_, index);
        chosen_.assign(rules_.size(), false);
        for (int i = low; i <= high; ++i) {
            chosen_[static_cast<std::size_t>(i)] = true;
        }
    } else {
        chosen_.assign(rules_.size(), false);
        chosen_[static_cast<std::size_t>(index)] = true;
        anchor_ = index;
    }
    // **The editor always shows a row that is in the selection**, and this used to be a plain
    // `selected_ = index`. Control-clicking a chosen row takes it *out* of the selection, so
    // that assignment left the card on screen showing a rule the marks would not act on: press
    // × on any lit row and six other rules go while the one being read stays. Found by the
    // soak test below rather than by anybody using it, which is the only reason it is written
    // down here and not in a bug report.
    //
    // The nearest row that is still chosen, because a selection is a run more often than not
    // and the eye is where the click was.
    if (chosen_[static_cast<std::size_t>(index)]) {
        selected_ = index;
    } else {
        const int last = static_cast<int>(rules_.size()) - 1;
        selected_ = index;
        for (int away = 1; away <= last; ++away) {
            if (index - away >= 0 && chosen_[static_cast<std::size_t>(index - away)]) {
                selected_ = index - away;
                break;
            }
            if (index + away <= last && chosen_[static_cast<std::size_t>(index + away)]) {
                selected_ = index + away;
                break;
            }
        }
    }
    // A new selection is a new last-fired line: the old one belonged to another rule and
    // leaving it up would credit this one with what that one sent.
    lastFired_.clear();
    lastFiredAt_ = -1.0;
    publishSelected();
    publishSlots();
    publishList(); // the chosen flags, which are what the rows draw themselves from
    publishFiring();
    window_->set_selected(selected_);
    // Said out loud, because a selection of six looks like a selection of six only if you
    // know to count the highlighted rows — and because what × does next depends on it.
    const std::size_t count = chosen().size();
    setStatus(count > 1 ? std::to_string(count) +
                              " triggers selected — the marks on any of "
                              "them act on all " +
                              std::to_string(count)
                        : std::string{},
              false);
}

void RulesController::add() {
    commitTyping();
    Rule::Config rule;
    // A fresh id that is legal as an OSC address segment (§5.7 addresses a rule by it) and
    // that nothing else has. Numbered rather than named, because a name is the operator's
    // to write and an id is only ever machine-facing.
    for (int n = static_cast<int>(rules_.size()) + 1;; ++n) {
        const std::string candidate = "rule" + std::to_string(n);
        const auto clash = [&candidate](const Rule::Config& other) {
            return other.id == candidate;
        };
        if (std::none_of(rules_.begin(), rules_.end(), clash)) {
            rule.id = candidate;
            break;
        }
    }
    // A name, rather than the blank the list showed as "(unnamed)". It is the operator's to
    // change and most of them will, but a list of "(unnamed)" rows is a list that cannot be
    // read, and "Trigger #5" at least says which one this is. Numbered past anything already
    // called that, so deleting from the middle and adding again does not make two.
    for (int n = static_cast<int>(rules_.size()) + 1;; ++n) {
        const std::string candidate = "Trigger #" + std::to_string(n);
        const auto clash = [&candidate](const Rule::Config& other) {
            return other.name == candidate;
        };
        if (std::none_of(rules_.begin(), rules_.end(), clash)) {
            rule.name = candidate;
            break;
        }
    }
    // Distinct seeds, so two rules in a preset do not fire the same clip as each other —
    // `Generator::Config::seed`'s whole reason. See `freshSeed`.
    rule.seed = freshSeed();
    rule.value.kind = GeneratorKind::Fixed;
    rule.value.fixed = trigger::Value::ofInt(1);
    // **Switched on.** This was off, on the reasoning that a half-built rule must not fire
    // into somebody's rig while they are still typing — and the reasoning was sound and the
    // result was wrong. A rule with no address is invalid, so it cannot fire whatever this
    // says (`Rule::problem`); what "off" actually bought was every new rule and every preset
    // arriving inert, behind an unlabelled tick box, with nothing on screen saying why
    // nothing happened. Reported from a rig: "it's not obvious that they're entirely
    // disabled". The box is labelled now, and a rule an operator asked for is armed.
    rule.enabled = true;
    rules_.push_back(rule);
    resettle(static_cast<int>(rules_.size()) - 1);
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
}

void RulesController::remove() {
    commitTyping();
    const std::vector<int> going = chosen();
    if (going.empty()) {
        return;
    }
    // From the back, so the indices ahead of each erase are still the ones just measured.
    for (auto row = going.rbegin(); row != going.rend(); ++row) {
        const auto at = static_cast<std::size_t>(*row);
        if (at >= rules_.size()) {
            continue;
        }
        // Forgotten with the rule. `add` recycles ids — delete "rule1" and the next rule
        // added is called "rule1" again — so a count left behind here would be handed to a
        // rule that has never fired.
        firesSeen_.erase(rules_[at].id);
        slotsSeen_.erase(rules_[at].id);
        rules_.erase(rules_.begin() + static_cast<std::ptrdiff_t>(at));
    }
    // The row that moved up into the first deleted one's place, which is where the eye is.
    resettle(going.front());
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
}

void RulesController::duplicate() {
    commitTyping();
    const std::vector<int> sources = chosen();
    if (sources.empty()) {
        return;
    }
    // Every copy goes after the last of the originals, in the originals' own order — six
    // rules duplicated read as six more in the same order, not as six pairs.
    std::vector<Rule::Config> copies;
    copies.reserve(sources.size());
    for (const int row : sources) {
        const auto at = static_cast<std::size_t>(row);
        if (at >= rules_.size()) {
            continue;
        }
        const Rule::Config& source = rules_[at];
        Rule::Config copy = source;
        copy.id += "-copy";
        const auto taken = [this, &copies](const std::string& id) {
            return std::any_of(rules_.begin(), rules_.end(),
                               [&id](const Rule::Config& o) { return o.id == id; }) ||
                   std::any_of(copies.begin(), copies.end(),
                               [&id](const Rule::Config& o) { return o.id == id; });
        };
        for (int n = 2; taken(copy.id); ++n) {
            copy.id = source.id + "-copy" + std::to_string(n);
        }
        if (!copy.name.empty()) {
            copy.name += " copy";
        }
        // A different stream, or the copy would fire exactly what the original fires — which
        // is never what duplicating a rule is for. From `freshSeed`, past every copy made so
        // far too; `source.seed + 977` was the next rule's seed as often as not (L9).
        copy.seed = freshSeed(copies);
        copies.push_back(std::move(copy));
    }
    const int after = sources.back() + 1;
    rules_.insert(rules_.begin() + after, copies.begin(), copies.end());
    // The copies are what the operator is now looking at, so they are what is selected.
    chosen_.assign(rules_.size(), false);
    for (std::size_t i = 0; i < copies.size(); ++i) {
        chosen_[static_cast<std::size_t>(after) + i] = true;
    }
    selected_ = after;
    anchor_ = after;
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
}

void RulesController::removeAt(int index) {
    commitTyping();
    if (index < 0 || static_cast<std::size_t>(index) >= chosen_.size() ||
        !chosen_[static_cast<std::size_t>(index)]) {
        pick(index);
    }
    remove();
}

void RulesController::duplicateAt(int index) {
    commitTyping();
    if (index < 0 || static_cast<std::size_t>(index) >= chosen_.size() ||
        !chosen_[static_cast<std::size_t>(index)]) {
        pick(index);
    }
    duplicate();
}

void RulesController::addRig(int index) {
    if (index <= 0) {
        return; // the picker's own label
    }
    commitTyping();
    const std::vector<Rule::Config> added = rigPresetRules(static_cast<std::size_t>(index));
    if (added.empty()) {
        return;
    }
    // Appended rather than replacing: an operator adding a second rig to a set they have
    // already built has not asked to lose the first.
    for (Rule::Config rule : added) {
        // An id already in use would make §5.7's `/ctl/rule/<id>/enable` ambiguous, and
        // `TriggerEngine::find` takes the first. Numbered up rather than refused: adding the
        // same rig twice is a reasonable thing to do with three more layers in mind.
        std::uint64_t copies = 0;
        std::string id = rule.id;
        while (findRule(id) != nullptr) {
            ++copies;
            id = rule.id + "-" + std::to_string(copies + 1);
        }
        if (copies > 0) {
            rule.id = std::move(id);
        }
        // A different stream too, or a second copy of the rig fires exactly what the first
        // does — or a rule of the operator's own that happened to have this seed does.
        if (std::any_of(rules_.begin(), rules_.end(),
                        [&rule](const Rule::Config& held) { return held.seed == rule.seed; })) {
            rule.seed = freshSeed();
        }
        rules_.push_back(std::move(rule));
    }
    // The whole rig that was just added, selected — so it can be re-routed or deleted again
    // in one gesture, which is the other half of a preset being a starting point.
    chosen_.assign(rules_.size(), false);
    const int first = static_cast<int>(rules_.size()) - static_cast<int>(added.size());
    for (std::size_t i = static_cast<std::size_t>(first); i < rules_.size(); ++i) {
        chosen_[i] = true;
    }
    selected_ = first;
    anchor_ = first;
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
}

void RulesController::rename(const std::string& name) {
    if (Rule::Config* rule = current()) {
        rule->name = name;
        commit();
    }
}

void RulesController::setEnabled(bool on) {
    if (Rule::Config* rule = current()) {
        rule->enabled = on;
        commit();
        // **And to the running rule itself**, as MUTE is (the audit of 2026-09-25, M10). A set
        // posted with the switch unchanged keeps the live switch (`Rule::carryFrom`), and after a
        // control surface had switched this rule the editor's copy said what the surface did — so
        // unticking it posted the value the running rule was already configured with, the rule
        // went on firing, and the box said it was off.
        runner_.post(output::OutputCommand::ruleEnabled(rule->id, on));
    }
}

void RulesController::setMuted(bool on) {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    // Straight to the output thread, and **not** through `commit()`: this is not part of the
    // rule's configuration and must not be saved. `commit` would also republish the whole rule
    // set, which resets every live gesture on every other rule — the thing this is trying to
    // set on one of them.
    mutedSeen_[rule->id] = on;
    runner_.post(output::OutputCommand::ruleMuted(rule->id, on));
    publishSelected();
    publishList();
}

void RulesController::nudgeRate(double factor) {
    const Rule::Config* rule = current();
    if (rule == nullptr || !(factor > 0.0)) {
        return;
    }
    // The same clamp the rule itself applies, mirrored so the readout cannot claim a rate the
    // output thread refused. `Rule::setRate` is the authority; this has to agree with it.
    const double current = rateSeen_.count(rule->id) != 0 ? rateSeen_[rule->id] : 1.0;
    rateSeen_[rule->id] = std::clamp(current * factor, 0.0625, 64.0);
    runner_.post(output::OutputCommand::ruleRate(rule->id, factor, /*relative=*/true));
    publishSelected();
}

void RulesController::test() {
    if (const Rule::Config* rule = current()) {
        runner_.post(output::OutputCommand::testRule(rule->id));
    }
}

void RulesController::panic() {
    // Engage, whatever the state: a press on PANIC is never a request to let go. It was a
    // toggle, and the second click of a double-click undid the first.
    runner_.panic(true);
    // **Shown from what was asked for, not read back.** `panic` *posts*: the output thread
    // applies it about a millisecond later, so reading the flag here returns the state the
    // press was leaving, and the button lit the wrong way for a frame. On a PANIC button that
    // is the worst possible place for a flicker. `tick` puts it back in step if the change
    // somehow did not take.
    window_->set_panicked(true);
}

void RulesController::releasePanic() {
    runner_.panic(false);
    window_->set_panicked(false);
}

void RulesController::pickTrigger(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= trigger::kTriggers.size()) {
        return;
    }
    rule->trigger = trigger::kTriggers[static_cast<std::size_t>(index)];
    commit();
    publishSelected();
}

void RulesController::setEvery(int every) {
    typed(TypedIn::Rule, 0, kEveryBox);
    if (Rule::Config* rule = current()) {
        rule->every = static_cast<std::uint32_t>(std::max(1, every));
        commit();
        // Back into the box: a `NumberBox` never writes its own value, so it shows what this
        // says it is — the std `SpinBox` it replaced set itself and so needed nothing here.
        window_->set_every(static_cast<int>(rule->every));
    }
}

void RulesController::setTargets(std::vector<output::OutputTarget> targets) {
    // **Only when they have actually changed.** The main window tells us this from
    // `publishOutputs`, which runs on its redraw timer — thirty times a second while the
    // tracker does. `publishSelected` writes every field of the selected rule, including the
    // name, the address and the routing, so republishing unconditionally meant those fields
    // were reset thirty times a second and a rule could not be typed into while anything was
    // playing, which is the only time anybody edits one.
    if (targets == targets_) {
        return;
    }
    targets_ = std::move(targets);
    publishSelected(); // the "reaches ..." line beside the routing field
}

void RulesController::setPulses(int pulses) {
    typed(TypedIn::Rule, 0, kPulsesBox);
    if (Rule::Config* rule = current()) {
        rule->pulses = static_cast<std::uint32_t>(std::max(0, pulses));
        commit();
        publishSelected(); // the pattern the two numbers name
    }
}

void RulesController::setOutputs(const std::string& text) {
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    rule->outputs.clear();
    for (const std::string_view part : split(text, ",;")) {
        rule->outputs.emplace_back(part);
    }
    // Names are what a person types; the rule keeps the ids of the outputs they name.
    output::routeByIds(rule->outputs, targets_);
    commit();
    publishSelected();
}

void RulesController::setOutputChosen(const std::string& id, bool chosen) {
    Rule::Config* rule = current();
    if (rule == nullptr || id.empty()) {
        return;
    }
    const auto at = std::find(rule->outputs.begin(), rule->outputs.end(), id);
    if (chosen) {
        if (at == rule->outputs.end()) {
            rule->outputs.push_back(id);
        }
    } else if (at != rule->outputs.end()) {
        rule->outputs.erase(at);
    }
    // Nothing to say about an empty list: it already means every output, which is what the
    // "every output" line then shows as ticked. `resolveOutputs` and this cannot disagree
    // because there is only one representation of "everywhere".
    commit();
    publishSelected();
}

void RulesController::chooseAllOutputs() {
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    rule->outputs.clear();
    commit();
    publishSelected();
}

void RulesController::setPatch(std::vector<dmx::Fixture> patch) {
    // Only when they have actually changed, for the reason `setTargets` gives at length: this
    // is told from the main window's redraw timer, and republishing the selected rule thirty
    // times a second would make it impossible to type into.
    if (patch == patch_) {
        return;
    }
    patch_ = std::move(patch);
    publishSelected();
}

void RulesController::setFixtureChosen(const std::string& key, bool chosen) {
    Rule::Config* rule = current();
    if (rule == nullptr || key.empty()) {
        return;
    }
    std::vector<std::string>& aims = rule->dmx.fixtures;
    const auto at = std::find(aims.begin(), aims.end(), key);
    if (chosen) {
        if (at == aims.end()) {
            aims.push_back(key);
        }
    } else if (at != aims.end()) {
        aims.erase(at);
    }
    // Un-ticking the last one leaves the rule reaching **nothing**, which is the opposite of
    // what un-ticking the last output does. `Rule::validate` reports it as a problem, so the
    // card says so rather than the rig doing something nobody asked for.
    commit();
    publishSelected();
}

void RulesController::chooseNoFixtures() {
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    rule->dmx.fixtures.clear();
    commit();
    publishSelected();
}

void RulesController::setMinConfidence(double value) {
    if (Rule::Config* rule = current()) {
        rule->conditions.minConfidence = std::clamp(value, 0.0, 1.0);
        commit();
    }
}

void RulesController::setIntensity(int which, bool allowed) {
    Rule::Config* rule = current();
    if (rule == nullptr || which < 0 || which > 2) {
        return;
    }
    rule->conditions.intensities[static_cast<std::size_t>(which)] = allowed;
    commit();
}

void RulesController::setBpmRange(const std::string& text) {
    typed(TypedIn::Rule, 0, kBpmRangeBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    const std::string_view view(text);
    // "any" and an empty field both mean the same thing, and both are what an operator
    // types to undo a range they no longer want.
    if (trim(view).empty() || trim(view) == "any") {
        rule->conditions.minBpm = 0.0;
        rule->conditions.maxBpm = 1000.0;
        commit();
        publishSelected();
        return;
    }
    // A dash after a digit, so "-30 - 40" is not read as three numbers. Anything separating
    // two numbers is accepted, which is what "70-140", "70 to 140" and "70 140" all are.
    const std::vector<std::string_view> parts = split(view, "-–to ");
    if (parts.size() != 2) {
        setStatus("A BPM range is two numbers, like 120 - 140.", true);
        return;
    }
    const std::optional<double> low = readNumber(parts[0]);
    const std::optional<double> high = readNumber(parts[1]);
    if (!low || !high) {
        setStatus("A BPM range is two numbers, like 120 - 140.", true);
        return;
    }
    rule->conditions.minBpm = std::min(*low, *high);
    rule->conditions.maxBpm = std::max(*low, *high);
    commit();
    publishSelected();
}

void RulesController::setProbability(double value) {
    if (Rule::Config* rule = current()) {
        rule->conditions.probability = std::clamp(value, 0.0, 1.0);
        commit();
    }
}

void RulesController::setCooldown(const std::string& text) {
    typed(TypedIn::Rule, 0, kCooldownBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    const std::string_view trimmed = trim(text);
    // An empty box is no cooldown, which is what an operator clears it to mean. Anything else
    // that is not a number is said rather than silently read as zero — `text.to-float()` used
    // to do that in the markup, so a typo turned a two-second cooldown off without a word.
    if (trimmed.empty()) {
        rule->conditions.cooldownSeconds = 0.0;
    } else {
        const std::optional<double> number = readNumber(trimmed);
        if (!number) {
            setStatus("A cooldown is a number of milliseconds, like 250.", true);
            return;
        }
        rule->conditions.cooldownSeconds = std::max(0.0, *number) / 1000.0;
    }
    commit();
    publishSelected();
}

void RulesController::pickSend(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= trigger::kMessageKinds.size()) {
        return;
    }
    const trigger::Message::Kind was = rule->sendKind;
    rule->sendKind = trigger::kMessageKinds[static_cast<std::size_t>(index)];
    // **A number nobody chose does not go out** (the audit's C7, and the operator's call: the
    // rule stays armed and waits). A rule switched to a note or a controller used to fire at
    // once with whatever the number generator held — a shuffle over 1 to 8 by default, so CC 7
    // at one on channel 1, to every MIDI output, on some bar: a synth's volume gone. A number
    // carries over only where it means the same thing (`trigger::sameNumber`).
    if (trigger::sendsNumber(rule->sendKind) && !trigger::sameNumber(was, rule->sendKind)) {
        const std::uint64_t seed = rule->number.seed;
        rule->number = Generator::Config{};
        rule->number.kind = GeneratorKind::Fixed;
        rule->number.seed = seed;
        rule->numberChosen = false;
    }
    // And a velocity that is still another kind's default becomes this kind's. A new rule's
    // value is OSC's `1`, which as a velocity is a note too quiet to hear; pitch bend is 14-bit
    // with its rest at 8192, where 100 is a bend nearly all the way down.
    if (trigger::sendsValue(rule->sendKind) && rule->value.kind == GeneratorKind::Fixed) {
        const trigger::Value rest = trigger::Value::ofInt(
            rule->sendKind == trigger::Message::Kind::MidiPitchBend ? kBendRest : kVelocity);
        for (const std::int32_t untouched : {1, kVelocity, kBendRest}) {
            if (rule->value.fixed == trigger::Value::ofInt(untouched)) {
                rule->value.fixed = rest;
                break;
            }
        }
    }
    // A follow-up left on the far side of the OSC/MIDI divide would be **silently dropped** at
    // fire time (`trigger::followUpFits`) — a clip pressed and never released, which is the
    // worst failure this editor has. Turned into releases instead, which is what an operator
    // switching a rule's send kind means by the rows they already had: let go of what was
    // pressed. An explicit number goes with the kind that carried it; there is nowhere to put
    // one on the other side.
    int stranded = 0;
    for (trigger::FollowUp& owed : rule->followUps) {
        if (owed.kind && !trigger::followUpFits(*owed.kind, rule->sendKind)) {
            owed.kind.reset();
            ++stranded;
        }
    }
    commit();
    publishSelected();
    if (stranded > 0) {
        setStatus(
            std::to_string(stranded) +
                (stranded == 1 ? " follow-up became a release" : " follow-ups became releases") +
                ", because what they sent cannot follow a " +
                std::string(trigger::labelOf(rule->sendKind)) + ".",
            false);
    }
}

void RulesController::setAddress(const std::string& address) {
    typed(TypedIn::Rule, 0, kAddressBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    rule->address = address;
    matchSegmentsToAddress(*rule);
    commit();
    publishSelected();
}

void RulesController::setChannel(int channel) {
    typed(TypedIn::Rule, 0, kChannelBox);
    if (Rule::Config* rule = current()) {
        rule->channel = std::clamp(channel, 1, 16);
        commit();
        window_->set_channel(rule->channel); // see `setEvery`
    }
}

// --- the lighting half ------------------------------------------------------------------

void RulesController::pickEffect(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= dmx::kEffectKinds.size()) {
        return;
    }
    const dmx::EffectKind effect = dmx::kEffectKinds[static_cast<std::size_t>(index)];
    if (rule->dmx.effect == effect) {
        return;
    }
    rule->dmx.effect = effect;
    // A curve that suits the effect, but only where the operator has not chosen one — which
    // cannot be told apart from the default here, so this only moves the two that are
    // *wrong* rather than merely unfashionable. A movement wants to ease at both ends so the
    // head does not jerk; a fade wants to ease out because the eye's response to light is
    // not linear either. Neither is a preference: linear movement visibly snaps.
    if (dmx::takesMovement(effect) && rule->dmx.curve == dmx::Curve::EaseOut) {
        rule->dmx.curve = dmx::Curve::EaseInOut;
    }
    commit();
    publishSelected();
}

void RulesController::pickRole(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= dmx::kAimableRoles.size()) {
        return;
    }
    rule->dmx.role = dmx::kAimableRoles[static_cast<std::size_t>(index)];
    commit();
    publishSelected();
}

void RulesController::pickCurve(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 || static_cast<std::size_t>(index) >= dmx::kCurves.size()) {
        return;
    }
    rule->dmx.curve = dmx::kCurves[static_cast<std::size_t>(index)];
    commit();
}

void RulesController::pickPathShape(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= dmx::kPathShapes.size()) {
        return;
    }
    rule->dmx.shape = dmx::kPathShapes[static_cast<std::size_t>(index)];
    commit();
}

void RulesController::setDuration(const std::string& text) {
    typed(TypedIn::Rule, 0, kDurationBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    const std::optional<double> number = readNumber(trim(text));
    if (!number || *number < 0.0) {
        setStatus("A duration is a number, and not a negative one.", true);
        publishSelected();
        return;
    }
    // Into whichever field the row's unit names, so switching units does not rewrite the
    // number the operator is not currently looking at — the same reasoning
    // `trigger::FollowUp::delayBeats` gives for holding two.
    if (rule->dmx.unit == trigger::DelayUnit::Milliseconds) {
        rule->dmx.durationSeconds = *number / 1000.0;
    } else {
        rule->dmx.durationBeats = *number;
    }
    commit();
    publishSelected();
}

void RulesController::pickDurationUnit(int unit) {
    Rule::Config* rule = current();
    if (rule == nullptr || unit < 0 ||
        static_cast<std::size_t>(unit) >= trigger::kDelayUnits.size()) {
        return;
    }
    rule->dmx.unit = trigger::kDelayUnits[static_cast<std::size_t>(unit)];
    commit();
    publishSelected();
}

void RulesController::setBase(int level) {
    typed(TypedIn::Rule, 0, kBaseBox);
    if (Rule::Config* rule = current()) {
        rule->dmx.base = std::clamp(level, 0, 255);
        commit();
        window_->set_effect_base(rule->dmx.base); // see `setEvery`
    }
}

void RulesController::setCycles(const std::string& text) {
    typed(TypedIn::Rule, 0, kCyclesBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    const std::optional<double> number = readNumber(trim(text));
    if (!number || !(*number > 0.0)) {
        setStatus("A number of cycles is a number above zero.", true);
        publishSelected();
        return;
    }
    rule->dmx.cycles = std::min(*number, 1024.0);
    commit();
    publishSelected();
}

void RulesController::setDuty(float percent) {
    if (Rule::Config* rule = current()) {
        rule->dmx.duty = std::clamp(static_cast<double>(percent) / 100.0, 0.0, 1.0);
        commit();
    }
}

void RulesController::setHueRange(const std::string& text) {
    typed(TypedIn::Rule, 0, kHueBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    const std::vector<std::string_view> parts = split(trim(text), "-–to ");
    if (parts.size() != 2) {
        setStatus("A hue sweep is two angles in degrees, like 0 - 360.", true);
        publishSelected();
        return;
    }
    const std::optional<double> low = readNumber(parts[0]);
    const std::optional<double> high = readNumber(parts[1]);
    if (!low || !high) {
        setStatus("A hue sweep is two angles in degrees, like 0 - 360.", true);
        publishSelected();
        return;
    }
    // **Not sorted**, unlike a BPM range. A sweep from 360 to 0 goes round the wheel the other
    // way, and a sweep from 0 to 720 goes round twice — both are instructions somebody means,
    // and putting them in order would silently turn them into something else.
    rule->dmx.hueFrom = std::clamp(*low, -3600.0, 3600.0);
    rule->dmx.hueTo = std::clamp(*high, -3600.0, 3600.0);
    commit();
    publishSelected();
}

void RulesController::setSize(float percent) {
    if (Rule::Config* rule = current()) {
        rule->dmx.size = std::clamp(static_cast<double>(percent) / 100.0, 0.0, 1.0);
        commit();
    }
}

void RulesController::pickHostPreset(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 || static_cast<std::size_t>(index) >= kHostPresets.size()) {
        return;
    }
    if (index == 0) {
        // **Nothing, when it already is custom.** A dropdown reports a pick of the entry it is
        // already showing — the mouse wheel over a focused one does it a notch at a time — and
        // this box reads "custom" for every address of the operator's own, so a scroll past it
        // wiped the address and every follow-up (the audit of 2026-09-25, M19).
        if (presetOf(rule->address) == 0) {
            return;
        }
        // §5.6's *"blank custom option"*, blanked. The address goes, its chips go with it —
        // `matchSegmentsToAddress` drops a generator when the placeholder it filled does —
        // and so does the press-then-release, which is a thing about Resolume's `connect`
        // and not about OSC. What is left is a rule that sends a value to an address the
        // operator is about to type, which is what the entry has always claimed to be.
        rule->address.clear();
        matchSegmentsToAddress(*rule);
        rule->followUps.clear();
        commit();
        publishSelected();
        setStatus("Cleared the address. Type one, or pick a host to start from.", false);
        return;
    }
    const HostPreset& preset = kHostPresets[static_cast<std::size_t>(index)];
    rule->sendKind = trigger::Message::Kind::Osc;
    rule->address = preset.address;
    matchSegmentsToAddress(*rule);
    // §5.6's Resolume line, whole: "int 1 (press) then 0 (release)". §7.4 is emphatic that
    // a rule sending only the 1 leaves the clip held, so a preset that filled the address
    // and left the release off would have shipped the trap rather than the answer.
    if (trigger::countPlaceholders(rule->address) > 0 ||
        rule->address.find("connect") != std::string::npos) {
        rule->value.kind = GeneratorKind::Fixed;
        rule->value.fixed = trigger::Value::ofInt(1);
        // Replaced rather than added to: picking a host preset is choosing that host's whole
        // gesture, and appending a second release to one an operator already had would send
        // the 0 twice.
        rule->followUps.assign(1, releaseAfterMs(0, 50));
    }
    commit();
    publishSelected();
}

void RulesController::setSendValue(bool on) {
    if (Rule::Config* rule = current()) {
        rule->sendValue = on;
        commit();
    }
}

trigger::FollowUp* RulesController::followConfig(int index) noexcept {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 || static_cast<std::size_t>(index) >= rule->followUps.size()) {
        return nullptr;
    }
    return &rule->followUps[static_cast<std::size_t>(index)];
}

void RulesController::addFollowUp() {
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    if (rule->followUps.size() >= trigger::kMaxFollowUps) {
        setStatus("A trigger can send " + std::to_string(trigger::kMaxFollowUps) +
                      " follow-ups at most.",
                  true);
        return;
    }
    trigger::FollowUp entry;
    // **A release, one beat later.** What an operator adding a row nearly always means is
    // "let go of what was just pressed" — a note off for a note, a 0 for Resolume's connect —
    // and the length they mean it for is musical rather than a number of milliseconds. A
    // first row of "release after 1 beat" is the laser rig's whole ask, ready to test.
    entry.unit = trigger::DelayUnit::Beats;
    entry.delayBeats = 1.0;
    // Except on a pitch bend, where letting go is the **centre** and not the bottom. Zero is
    // hard left on a 14-bit bend; 8192 is the wheel released, which is what a release means.
    if (rule->sendKind == trigger::Message::Kind::MidiPitchBend) {
        entry.value = trigger::Value::ofInt(8192);
    }
    rule->followUps.push_back(entry);
    commit();
    publishSelected();
}

void RulesController::removeFollowUp(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 || static_cast<std::size_t>(index) >= rule->followUps.size()) {
        return;
    }
    rule->followUps.erase(rule->followUps.begin() + index);
    rowRemoved(TypedIn::FollowUp, index);
    commit();
    publishSelected();
}

void RulesController::pickFollowKind(int index, int choice) {
    trigger::FollowUp* entry = followConfig(index);
    if (entry == nullptr || choice < 0 || static_cast<std::size_t>(choice) > followKinds_.size()) {
        return;
    }
    // Zero is "release" — no kind at all, which is what makes the follow-up *be* the message
    // that fired. Everything past it indexes the kinds this rule is allowed to reach.
    entry->kind = choice == 0 ? std::optional<trigger::Message::Kind>{}
                              : followKinds_[static_cast<std::size_t>(choice) - 1];
    commit();
    publishSelected();
}

void RulesController::setFollowNumber(int index, int number) {
    typed(TypedIn::FollowUp, index, 2);
    if (trigger::FollowUp* entry = followConfig(index)) {
        entry->number = std::clamp(number, 0, 127);
        commit();
        publishSelected();
    }
}

void RulesController::setFollowValue(int index, const std::string& text) {
    typed(TypedIn::FollowUp, index, 0);
    if (trigger::FollowUp* entry = followConfig(index)) {
        entry->value = parseValue(text);
        commit();
        publishSelected();
    }
}

void RulesController::setFollowDelay(int index, const std::string& text) {
    typed(TypedIn::FollowUp, index, 1);
    trigger::FollowUp* entry = followConfig(index);
    if (entry == nullptr) {
        return;
    }
    const std::optional<double> number = readNumber(trim(text));
    if (!number) {
        setStatus("A delay is a number, like 1 or 50.", true);
        return;
    }
    // Into whichever of the two numbers the row is showing, leaving the other alone: they are
    // different magnitudes of the same idea, and a single field would turn "50" into "0.077"
    // the moment the unit changed. See `trigger::FollowUp::delayBeats`.
    if (entry->unit == trigger::DelayUnit::Milliseconds) {
        entry->delaySeconds = std::max(0.0, *number) / 1000.0;
    } else {
        entry->delayBeats = std::max(0.0, *number);
    }
    commit();
    publishSelected();
}

void RulesController::pickFollowUnit(int index, int unit) {
    trigger::FollowUp* entry = followConfig(index);
    if (entry == nullptr || unit < 0 ||
        static_cast<std::size_t>(unit) >= trigger::kDelayUnits.size()) {
        return;
    }
    entry->unit = trigger::kDelayUnits[static_cast<std::size_t>(unit)];
    commit();
    publishSelected();
}

void RulesController::adoptLive(const std::vector<output::OutputRunner::LiveRule>& live) {
    bool enabledMoved = false;
    for (const output::OutputRunner::LiveRule& one : live) {
        const auto rule = std::find_if(rules_.begin(), rules_.end(),
                                       [&one](const Rule::Config& config) { return config.id == one.id; });
        if (rule == rules_.end()) {
            continue;
        }
        if (rule->enabled != one.enabled) {
            rule->enabled = one.enabled;
            enabledMoved = true;
        }
        mutedSeen_[one.id] = one.muted;
        rateSeen_[one.id] = one.rate;
    }
    if (enabledMoved && changed_) {
        changed_(rules_);
    }
    publishList();
    if (const Rule::Config* rule = current()) {
        window_->set_rule_enabled(rule->enabled);
        const auto muted = mutedSeen_.find(rule->id);
        window_->set_rule_muted(muted != mutedSeen_.end() && muted->second);
        const auto rate = rateSeen_.find(rule->id);
        window_->set_rule_rate(shared(describeRate(rate == rateSeen_.end() ? 1.0 : rate->second)));
    }
}

void RulesController::clearLog() {
    log_.clear();
    logModel_->clear();
}

void RulesController::tick() {
    // First, and outside every widget callback: a publisher found a row it could not honestly
    // update in place, and this is where the repeater is built again. See `rowsDirty_`.
    if (rowsDirty_) {
        rebuildRows();
    }
    // What a control surface changed behind this editor's back. See `adoptLive`. Only once the
    // output thread has got to the last set posted — this editor's own last edit, most often:
    // before then the switches it reports are the set before, and adopting them undid the edit.
    // The version is left unseen meanwhile, so they are read the moment they are current.
    if (const std::uint64_t version = runner_.liveRulesVersion();
        version != liveSeen_ && runner_.liveRulesCurrent()) {
        liveSeen_ = version;
        adoptLive(runner_.liveRules());
    }
    // Drained whether or not the window is up: the buffer is bounded and dropping the
    // oldest, so a log that is never drained would quietly lose the start of a set. It is
    // also the cheapest possible call when nothing has fired.
    const std::vector<output::OutputRunner::Fired> fired = runner_.takeFired();
    if (!fired.empty()) {
        const Rule::Config* rule = current();
        for (const output::OutputRunner::Fired& entry : fired) {
            // A muted rule still runs and still "fires" — that is what keeps it in phase — but
            // nothing left, and the log said it had (the audit's M11).
            const std::string message =
                entry.muted ? entry.message + "  (muted, not sent)" : entry.message;
            // **By the name the list shows**, not the id — "rule1-copy2" is a word this window
            // shows nowhere else, so a log line could not be matched to a rule (the audit of
            // 2026-09-25, L38). Named as the rule is called when it fired; the id stays the
            // key, and only one no longer in the set is said to be gone.
            const auto named =
                std::find_if(rules_.begin(), rules_.end(),
                             [&entry](const Rule::Config& one) { return one.id == entry.ruleId; });
            const std::string who = named == rules_.end() ? "(a rule since deleted)"
                                    : named->name.empty() ? "(unnamed)"
                                                          : named->name;
            log_.push_back(spellNumber(entry.when) + "s  " + who + "  " + message);
            lastFiredAnywhere_ = message;
            if (!entry.followUp) {
                // The press, not the release: see `OutputRunner::Fired::followUp`.
                ++firesSeen_[entry.ruleId];
                slotsSeen_[entry.ruleId] = entry.slots;
            }
            if (rule != nullptr && entry.ruleId == rule->id) {
                lastFired_ = message;
                lastFiredAt_ = entry.when;
            }
        }
        if (log_.size() > kLogLines) {
            log_.erase(log_.begin(),
                       log_.begin() + static_cast<std::ptrdiff_t>(log_.size() - kLogLines));
        }
        std::vector<slint::SharedString> lines;
        lines.reserve(log_.size());
        // Newest first: the thing that just happened is what an operator is looking for, and
        // a list that scrolls itself is a list they have to chase.
        for (auto line = log_.rbegin(); line != log_.rend(); ++line) {
            lines.push_back(shared(*line));
        }
        writeRows(*logModel_, lines);
        publishList();
        publishSlots();
    }

    publishFiring();
    window_->set_panicked(runner_.panicked());
}

void RulesController::setStatus(const std::string& text, bool error) {
    window_->set_status(shared(text));
    window_->set_status_is_error(error);
}

} // namespace takt4::ui
