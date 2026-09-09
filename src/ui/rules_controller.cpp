#include "ui/rules_controller.hpp"

#include "core/features/intensity.hpp"
#include "core/trigger/generator.hpp"
#include "ui/model_rows.hpp"
#include "ui/native_window.hpp"
#include "ui/window_state.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace takt4::ui {
namespace {

using trigger::Generator;
using trigger::GeneratorKind;
using trigger::Rule;

slint::SharedString shared(const std::string& text) {
    return slint::SharedString(text);
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    return text;
}

/// Everything between separators, trimmed, empties dropped. What turns "3, 7, 1, 12" into a
/// sequence and " 70 - 140 " into a range, which is the same job twice.
std::vector<std::string_view> split(std::string_view text, std::string_view separators) {
    std::vector<std::string_view> parts;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t next = text.find_first_of(separators, at);
        const std::string_view part = trim(
            text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (next == std::string_view::npos) {
            break;
        }
        at = next + 1;
    }
    return parts;
}

std::optional<double> readNumber(std::string_view text) noexcept {
    double value = 0.0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

/// One entry of a list an operator typed.
///
/// **Type is inferred from how it is written**, which is the same rule the preset file uses
/// and the reason a list can hold more than clip numbers: `7` is an int, `0.5` a float,
/// `intro` text. Nothing here refuses — an entry that is not a number is a name, and names
/// are exactly what an address segment often is.
trigger::Value parseValue(std::string_view text) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) {
        return trigger::Value{};
    }
    if (trimmed == "true") {
        return trigger::Value::ofBool(true);
    }
    if (trimmed == "false") {
        return trigger::Value::ofBool(false);
    }
    if (const std::optional<double> number = readNumber(trimmed)) {
        // A whole number typed without a point is an int, because a clip index has to come
        // back as one. `3.0` is a float, which is what writing the point asked for.
        if (trimmed.find('.') == std::string_view::npos &&
            trimmed.find('e') == std::string_view::npos && *number >= -2147483648.0 &&
            *number <= 2147483647.0) {
            return trigger::Value::ofInt(static_cast<std::int32_t>(*number));
        }
        return trigger::Value::ofFloat(static_cast<float>(*number));
    }
    return trigger::Value::ofText(trimmed);
}

/// The inverse, as the field shows it back: "3, 7, 1, 12".
std::string spellValues(const std::vector<trigger::Value>& values) {
    std::string text;
    for (const trigger::Value& value : values) {
        if (!text.empty()) {
            text += ", ";
        }
        value.appendTo(text);
    }
    return text;
}

std::string spellValue(const trigger::Value& value) {
    std::string text;
    value.appendTo(text);
    return text;
}

/// A number with no trailing zeroes, for a field an operator will edit rather than read.
std::string spellNumber(double value) {
    if (value == std::floor(value) && std::abs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    std::string text = std::to_string(value);
    while (text.size() > 1 && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

/// A comma-separated list, as the routing field shows it back.
std::string join(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        if (!text.empty()) {
            text += ", ";
        }
        text += name;
    }
    return text;
}

/// What a rule's routing currently reaches, beside the field the names were typed into.
///
/// Three different things worth saying, and they are not interchangeable. A rule that names
/// nothing goes everywhere and should say which "everywhere" is, or an operator who has just
/// added a second output has no way to know the rule now hits it too. A name that matches
/// nothing is the one real mistake here — and is *kept* rather than corrected, because a
/// preset from another rig should still say what it meant (`Rule::Config::outputs`), so
/// saying so is the only way it gets noticed.
std::string describeRouting(const std::vector<std::string>& names,
                            const std::vector<output::OutputTarget>& targets) {
    if (targets.empty()) {
        return "no outputs yet";
    }
    if (names.empty()) {
        std::string all = "every output: ";
        for (std::size_t i = 0; i < targets.size(); ++i) {
            all += i == 0 ? "" : ", ";
            all += targets[i].name;
        }
        return all;
    }
    std::string missing;
    for (const std::string& name : names) {
        if (output::findTarget(targets, name) == nullptr) {
            missing += missing.empty() ? "" : ", ";
            missing += name;
        }
    }
    if (missing.empty()) {
        return "reaches " + std::to_string(names.size()) +
               (names.size() == 1 ? " output" : " outputs");
    }
    return "no output called " + missing;
}

/// A Euclidean pattern as something a person can hear before it plays: "x..x..x.".
///
/// Two numbers are not a rhythm anybody can read, and this is the cheapest way to make them
/// one — it is the same `euclidHit` the engine fires on, so the picture cannot disagree with
/// the playing.
std::string spellEuclid(const Rule::Config& rule) {
    if (!trigger::takesPulses(rule.trigger)) {
        return {};
    }
    const std::uint32_t steps = std::max<std::uint32_t>(1, rule.every);
    if (steps > 64) {
        return std::to_string(rule.pulses) + " in " + std::to_string(steps);
    }
    std::string pattern;
    pattern.reserve(steps);
    for (std::uint32_t step = 0; step < steps; ++step) {
        pattern += trigger::euclidHit(step, rule.pulses, steps) ? 'x' : '.';
    }
    return pattern;
}

/// §5.6's host presets: *"Ship presets for Resolume 7, TouchDesigner, MadMapper, QLC+ and a
/// blank custom option. The preset is a starting point the user can edit — never a hardcoded
/// code path."*
///
/// So each one is nothing but an address template and the generators that fill it — the same
/// data an operator could have typed, arrived at by clicking. Applying one leaves every part
/// of it editable, and nothing downstream ever asks which preset a rule came from.
struct HostPreset {
    const char* label;
    /// The address, with a `{}` wherever the host expects a number that varies. Each one
    /// becomes a chip on §5.9's editor, starting on §5.8's default generator — Shuffle over
    /// 1-8 — which the operator then says what they actually meant.
    const char* address;
};

/// Only Resolume's addresses are *verified* — HANDOFF §A.4 checked them against Resolume's
/// own documentation, and §5.6 quotes them. The rest are the shapes those hosts use, and are
/// starting points in the strongest sense: a TouchDesigner address is whatever the operator
/// named their CHOP, so a preset can only offer the convention.
///
/// The first entry is §5.6's *"blank custom option"*, and it means what it says: picking it
/// empties the address and takes the host's press-then-release off with it. It used to change
/// nothing, on the reasoning that an operator who had typed an address and then brushed the
/// picker had not asked to lose it — but the picker did not show which preset the rule *was*
/// either, so going back to "custom" after a Resolume preset left the Resolume address, its
/// two chips and its release sitting under a box that said "custom". Reported from a rig.
/// The picker now follows the address (`presetOf`), so "custom" is a state as well as a
/// choice, and choosing it is choosing an empty one.
const std::array<HostPreset, 5> kHostPresets{{
    {"custom", ""},
    {"Resolume 7 — clip", "/composition/layers/{layer}/clips/{clip}/connect"},
    {"Resolume 7 — resync", "/composition/tempocontroller/resync"},
    {"TouchDesigner", "/takt4/{name}"},
    {"MadMapper — cue", "/medias/{cue}/play"},
}};

/// Which preset an address *is*, or 0 for none of them — which is what "custom" means.
///
/// By the address alone: it is the whole of what a preset writes that cannot also have been
/// typed, and an operator who has edited one character of it is no longer on that preset,
/// which is exactly what the picker should then say.
int presetOf(std::string_view address) {
    for (std::size_t i = 1; i < kHostPresets.size(); ++i) {
        if (address == kHostPresets[i].address) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// --- rig presets -------------------------------------------------------------------------
//
// A host preset fills in one rule's address. A *rig* preset builds several rules at once,
// which is a different thing and the one an operator actually starts from: "random clips on
// three layers" is three rules, and nobody wants to build the same rule three times and
// remember to give each a different seed.
//
// Still §5.6's rule about presets — "a starting point the user can edit, never a hardcoded
// code path". Every one of these produces ordinary `Rule::Config`s that the editor then edits
// like any other, and nothing downstream ever asks which preset a rule came from.

/// One clip-launching rule for one Resolume layer.
Rule::Config resolumeLayer(int layer, std::uint32_t everyBars, std::uint64_t seed) {
    Rule::Config rule;
    rule.id = "layer" + std::to_string(layer);
    rule.name = "Layer " + std::to_string(layer) + " — random clip";
    rule.enabled = true; // armed, like every rule the editor makes — see `add`
    rule.trigger = trigger::Trigger::Bar;
    rule.every = everyBars;
    rule.address = "/composition/layers/{layer}/clips/{clip}/connect";

    Generator::Config which;
    which.kind = GeneratorKind::Fixed;
    which.fixed = trigger::Value::ofInt(layer);

    Generator::Config clip;
    clip.kind = GeneratorKind::Shuffle; // §5.8's default, and why: repeats read as bugs
    clip.low = 1;
    clip.high = 8;
    clip.noRepeatWithin = 2;
    rule.segments = {which, clip};

    rule.value.kind = GeneratorKind::Fixed;
    rule.value.fixed = trigger::Value::ofInt(1);
    // §7.4: connect is a mouse click. Without the release the clip stays held.
    rule.followUp = true;
    rule.followUpValue = trigger::Value::ofInt(0);
    rule.followUpDelaySeconds = 0.05;
    rule.seed = seed;
    return rule;
}

/// The rig presets, in the order the picker offers them.
std::vector<Rule::Config> rigPresetRules(std::size_t index) {
    switch (index) {
    case 1: {
        // The ask this was built for: random clips on three Resolume layers at once. Each
        // layer gets its own rule so each can be switched off, re-timed or re-routed on its
        // own — and its own seed, or all three would fire the same clip as each other, which
        // is `Generator::Config::seed`'s whole reason.
        //
        // Staggered periods rather than three identical ones: 4, 8 and 16 bars means the
        // three layers change at different times and the combination keeps moving. Three
        // layers all changing on the same downbeat is one event, not three.
        return {resolumeLayer(1, 4, 101), resolumeLayer(2, 8, 202), resolumeLayer(3, 16, 303)};
    }
    case 2: {
        // Resolume's own tempo, kept in step with the tracker. §5.6 and §A.4 verified both
        // addresses: the tempo is "float, normalised 0-1 across 20-500 BPM", which is exactly
        // what the `Live` generator's `BpmNormalised` source exists for.
        Rule::Config tempo;
        tempo.id = "tempo";
        tempo.name = "Resolume tempo follows takt4";
        tempo.enabled = true;
        tempo.trigger = trigger::Trigger::TempoChange;
        tempo.address = "/composition/tempocontroller/tempo";
        tempo.value.kind = GeneratorKind::Live;
        tempo.value.source = trigger::LiveSource::BpmNormalised;
        tempo.value.normaliseLow = 20.0;
        tempo.value.normaliseHigh = 500.0;
        tempo.seed = 404;

        Rule::Config resync;
        resync.id = "resync";
        resync.name = "Resync Resolume when the lock returns";
        resync.enabled = true;
        resync.trigger = trigger::Trigger::LockChange;
        resync.address = "/composition/tempocontroller/resync";
        resync.value.kind = GeneratorKind::Fixed;
        resync.value.fixed = trigger::Value::ofInt(1);
        resync.conditions.minConfidence = 0.5; // not while it is still hunting
        resync.seed = 505;
        return {tempo, resync};
    }
    case 3: {
        // Something that *moves* rather than jumping: a dashboard parameter breathing over
        // four bars, locked to the downbeat. This is what `Ramp` is for, and the one rule
        // here that fires on every beat — a ramp is only as smooth as it is sampled.
        Rule::Config breathe;
        breathe.id = "breathe";
        breathe.name = "Dashboard breathes over 4 bars";
        breathe.enabled = true;
        breathe.trigger = trigger::Trigger::Beat;
        breathe.address = "/composition/dashboard/link1";
        breathe.value.kind = GeneratorKind::Ramp;
        breathe.value.shape = trigger::RampShape::Sine;
        breathe.value.rampBars = 4;
        breathe.value.rampFloat = true;
        breathe.value.low = 0;
        breathe.value.high = 1;
        breathe.seed = 606;
        return {breathe};
    }
    case 4: {
        // A Euclidean pattern out to MIDI: three hits over eight beats, the tresillo, on a
        // note a lighting desk or a sampler can learn. The rhythm nothing else here can make.
        Rule::Config stabs;
        stabs.id = "stabs";
        stabs.name = "Euclidean stabs — 3 in 8";
        stabs.enabled = true;
        stabs.trigger = trigger::Trigger::Euclid;
        stabs.every = 8;
        stabs.pulses = 3;
        stabs.sendKind = trigger::Message::Kind::MidiNote;
        stabs.channel = 10; // where a drum map lives on most hardware
        stabs.number.kind = GeneratorKind::Shuffle;
        stabs.number.low = 36;
        stabs.number.high = 43;
        stabs.value.kind = GeneratorKind::Fixed;
        stabs.value.fixed = trigger::Value::ofInt(110);
        stabs.followUp = true;
        stabs.followUpValue = trigger::Value::ofInt(0); // velocity 0 is a note-off
        stabs.followUpDelaySeconds = 0.08;
        stabs.seed = 707;
        return {stabs};
    }
    default:
        return {};
    }
}

/// The labels the preset menu shows, in `rigPresetRules`' order — which counts from **one**,
/// zero being "no preset". The window's list holds these four and adds the one back on
/// (`rig-added(i + 1)`); it used to hold a fifth "add a preset..." entry at the front, which
/// is what a dropdown needs to have something to sit on and what a menu does not.
constexpr std::array<const char*, 4> kRigPresets{
    "Resolume: clips on 3 layers", "Resolume: tempo and resync", "Resolume: breathing dashboard",
    "MIDI: euclidean stabs"};

} // namespace

RulesController::RulesController(output::OutputRunner& runner,
                                 std::vector<trigger::Rule::Config> rules)
    : runner_(runner), rules_(std::move(rules)), window_(RulesWindow::create()),
      listModel_(std::make_shared<slint::VectorModel<RuleRow>>()),
      choiceModel_(std::make_shared<slint::VectorModel<OutputChoice>>()),
      slotModel_(std::make_shared<slint::VectorModel<SlotRow>>()),
      logModel_(std::make_shared<slint::VectorModel<slint::SharedString>>()) {
    window_->set_rules(listModel_);
    window_->set_output_choices(choiceModel_);
    window_->set_slots(slotModel_);
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

    window_->on_rule_picked([this](int index) { pick(index); });
    window_->on_rule_picked_with(
        [this](int index, bool control, bool shift) { pickWith(index, control, shift); });
    window_->on_rule_added([this] { add(); });
    window_->on_rule_removed([this] { remove(); });
    window_->on_rule_duplicated([this] { duplicate(); });
    window_->on_rule_removed_at([this](int index) { removeAt(index); });
    window_->on_rule_duplicated_at([this](int index) { duplicateAt(index); });
    window_->on_rig_added([this](int index) { addRig(index); });
    window_->on_rule_enabled_changed([this](bool on) { setEnabled(on); });
    window_->on_rule_renamed([this](const slint::SharedString& n) { rename(std::string(n)); });
    window_->on_rule_tested([this] { test(); });
    window_->on_panic_clicked([this] { panic(); });

    window_->on_trigger_picked([this](int index) { pickTrigger(index); });
    window_->on_every_changed([this](int every) { setEvery(every); });
    window_->on_pulses_changed([this](int pulses) { setPulses(pulses); });
    window_->on_output_chosen([this](const slint::SharedString& name, bool chosen) {
        setOutputChosen(std::string(name), chosen);
    });
    window_->on_all_outputs_chosen([this] { chooseAllOutputs(); });

    window_->on_min_confidence_changed([this](float v) { setMinConfidence(v); });
    window_->on_intensity_changed([this](int which, bool on) { setIntensity(which, on); });
    window_->on_bpm_range_edited(
        [this](const slint::SharedString& t) { setBpmRange(std::string(t)); });
    window_->on_probability_changed([this](float v) { setProbability(v); });
    window_->on_cooldown_changed([this](float ms) { setCooldownMs(ms); });

    window_->on_send_picked([this](int index) { pickSend(index); });
    window_->on_address_edited(
        [this](const slint::SharedString& a) { setAddress(std::string(a)); });
    window_->on_channel_changed([this](int channel) { setChannel(channel); });
    window_->on_host_preset_picked([this](int index) { pickHostPreset(index); });
    window_->on_send_value_changed([this](bool on) { setSendValue(on); });
    window_->on_follow_up_changed([this](bool on) { setFollowUp(on); });
    window_->on_follow_up_value_edited(
        [this](const slint::SharedString& t) { setFollowUpValue(std::string(t)); });
    window_->on_follow_up_delay_changed([this](float ms) { setFollowUpMs(ms); });
    window_->on_follow_up_beats_changed([this](float beats) { setFollowUpBeats(beats); });
    window_->on_follow_up_unit_picked([this](int unit) { pickFollowUpUnit(unit); });

    window_->on_slot_kind_picked([this](int slot, int kind) { pickSlotKind(slot, kind); });
    window_->on_slot_pool_changed([this](int slot, bool list) { setSlotPool(slot, list); });
    window_->on_slot_range_edited(
        [this](int slot, const slint::SharedString& t) { setSlotRange(slot, std::string(t)); });
    window_->on_slot_values_edited(
        [this](int slot, const slint::SharedString& t) { setSlotValues(slot, std::string(t)); });
    window_->on_slot_no_repeat_changed([this](int slot, int n) { setSlotNoRepeat(slot, n); });
    window_->on_slot_fixed_edited(
        [this](int slot, const slint::SharedString& t) { setSlotFixed(slot, std::string(t)); });
    window_->on_slot_live_picked([this](int slot, int source) { pickSlotLive(slot, source); });
    window_->on_slot_shape_picked([this](int slot, int shape) { pickSlotShape(slot, shape); });
    window_->on_slot_ramp_bars_changed([this](int slot, int bars) { setSlotRampBars(slot, bars); });
    window_->on_slot_ramp_float_changed(
        [this](int slot, bool asFloat) { setSlotRampFloat(slot, asFloat); });

    window_->on_log_cleared([this] { clearLog(); });

    // The size this opens at, set here rather than in the markup: Slint takes a window's
    // initial size from what its content asks for, not from the Window's own
    // `preferred-width` — so this window opened at its minimum, 900 x 560, whatever the
    // markup preferred. See `kRulesWindowWidth`. Before the first `show()`, which is what
    // makes the backend leave it alone; a later drag is the operator's and is kept.
    window_->window().set_size(
        slint::LogicalSize({kRulesWindowWidth, kRulesWindowHeight}));

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
    window_->hide();
    visible_ = false;
}

const trigger::Rule::Config* RulesController::findRule(std::string_view id) const noexcept {
    for (const Rule::Config& rule : rules_) {
        if (rule.id == id) {
            return &rule;
        }
    }
    return nullptr;
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

Generator::Config* RulesController::slotConfig(int slot) noexcept {
    Rule::Config* rule = current();
    if (rule == nullptr || slot < 0) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(slot);
    if (rule->sendKind == trigger::Message::Kind::Osc) {
        if (index < rule->segments.size()) {
            return &rule->segments[index];
        }
        return index == rule->segments.size() ? &rule->value : nullptr;
    }
    // MIDI: the note or controller number first, then the velocity or value.
    return index == 0 ? &rule->number : index == 1 ? &rule->value : nullptr;
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
    // A whole new set, so the counts and last values start again. A preset may well reuse
    // the ids of the set it replaces — `add` numbers them "rule1", "rule2" — and a card
    // inheriting a number from a different rule that happened to share its id is a readout
    // that is quietly wrong, which is worse than one that reads zero.
    firesSeen_.clear();
    slotsSeen_.clear();
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
    selected_ = index;
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
    setStatus(count > 1 ? std::to_string(count) + " triggers selected — the marks on any of "
                                                  "them act on all " +
                              std::to_string(count)
                        : std::string{},
              false);
}

void RulesController::add() {
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
    // `Generator::Config::seed`'s whole reason.
    rule.seed = static_cast<std::uint64_t>(rules_.size()) * 977 + 1;
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
        // is never what duplicating a rule is for.
        copy.seed = source.seed + 977;
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
    if (index < 0 || static_cast<std::size_t>(index) >= chosen_.size() ||
        !chosen_[static_cast<std::size_t>(index)]) {
        pick(index);
    }
    remove();
}

void RulesController::duplicateAt(int index) {
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
        std::string id = rule.id;
        for (int n = 2; findRule(id) != nullptr; ++n) {
            id = rule.id + "-" + std::to_string(n);
        }
        if (id != rule.id) {
            rule.seed += 977; // a different stream too, or the copy fires what the first does
            rule.id = id;
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
    }
}

void RulesController::test() {
    if (const Rule::Config* rule = current()) {
        runner_.post(output::OutputCommand::testRule(rule->id));
    }
}

void RulesController::panic() {
    runner_.panic(!runner_.panicked());
    window_->set_panicked(runner_.panicked());
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
    if (Rule::Config* rule = current()) {
        rule->every = static_cast<std::uint32_t>(std::max(1, every));
        commit();
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
    commit();
    publishSelected();
}

void RulesController::setOutputChosen(const std::string& name, bool chosen) {
    Rule::Config* rule = current();
    if (rule == nullptr || name.empty()) {
        return;
    }
    const auto at = std::find(rule->outputs.begin(), rule->outputs.end(), name);
    if (chosen) {
        if (at == rule->outputs.end()) {
            rule->outputs.push_back(name);
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

void RulesController::pickSlotShape(int slot, int shape) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr || shape < 0 ||
        static_cast<std::size_t>(shape) >= trigger::kRampShapes.size()) {
        return;
    }
    config->shape = trigger::kRampShapes[static_cast<std::size_t>(shape)];
    commit();
}

void RulesController::setSlotRampBars(int slot, int bars) {
    if (Generator::Config* config = slotConfig(slot)) {
        config->rampBars = static_cast<std::uint32_t>(std::max(1, bars));
        commit();
    }
}

void RulesController::setSlotRampFloat(int slot, bool asFloat) {
    if (Generator::Config* config = slotConfig(slot)) {
        config->rampFloat = asFloat;
        commit();
    }
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

void RulesController::setCooldownMs(double milliseconds) {
    if (Rule::Config* rule = current()) {
        rule->conditions.cooldownSeconds = std::max(0.0, milliseconds) / 1000.0;
        commit();
        publishSelected();
    }
}

void RulesController::pickSend(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= trigger::kMessageKinds.size()) {
        return;
    }
    rule->sendKind = trigger::kMessageKinds[static_cast<std::size_t>(index)];
    commit();
    publishSelected();
}

void RulesController::setAddress(const std::string& address) {
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
    if (Rule::Config* rule = current()) {
        rule->channel = std::clamp(channel, 1, 16);
        commit();
    }
}

void RulesController::pickHostPreset(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 || static_cast<std::size_t>(index) >= kHostPresets.size()) {
        return;
    }
    if (index == 0) {
        // §5.6's *"blank custom option"*, blanked. The address goes, its chips go with it —
        // `matchSegmentsToAddress` drops a generator when the placeholder it filled does —
        // and so does the press-then-release, which is a thing about Resolume's `connect`
        // and not about OSC. What is left is a rule that sends a value to an address the
        // operator is about to type, which is what the entry has always claimed to be.
        rule->address.clear();
        matchSegmentsToAddress(*rule);
        rule->followUp = false;
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
        rule->followUp = true;
        rule->followUpValue = trigger::Value::ofInt(0);
        rule->followUpDelaySeconds = 0.05;
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

void RulesController::setFollowUp(bool on) {
    if (Rule::Config* rule = current()) {
        rule->followUp = on;
        commit();
    }
}

void RulesController::setFollowUpValue(const std::string& text) {
    if (Rule::Config* rule = current()) {
        rule->followUpValue = parseValue(text);
        commit();
        publishSelected();
    }
}

void RulesController::setFollowUpMs(double milliseconds) {
    if (Rule::Config* rule = current()) {
        rule->followUpDelaySeconds = std::max(0.0, milliseconds) / 1000.0;
        commit();
        publishSelected();
    }
}

void RulesController::setFollowUpBeats(double beats) {
    if (Rule::Config* rule = current()) {
        rule->followUpDelayBeats = std::max(0.0, beats);
        commit();
        publishSelected();
    }
}

void RulesController::pickFollowUpUnit(int unit) {
    Rule::Config* rule = current();
    if (rule == nullptr || unit < 0 ||
        static_cast<std::size_t>(unit) >= trigger::kDelayUnits.size()) {
        return;
    }
    rule->followUpUnit = trigger::kDelayUnits[static_cast<std::size_t>(unit)];
    commit();
    publishSelected();
}

void RulesController::pickSlotKind(int slot, int kind) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr || kind < 0 ||
        static_cast<std::size_t>(kind) >= trigger::kGeneratorKinds.size()) {
        return;
    }
    config->kind = trigger::kGeneratorKinds[static_cast<std::size_t>(kind)];
    commit();
}

void RulesController::setSlotPool(int slot, bool list) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    config->pool = list ? trigger::Pool::List : trigger::Pool::Range;
    // A list switched on with nothing in it is what an operator sees the moment they tick
    // the box, and an empty list sends zero. Seeding it from the range they already had is
    // what makes the switch show them their own values rather than a blank.
    if (list && config->values.empty()) {
        const std::int64_t span = static_cast<std::int64_t>(config->high) - config->low + 1;
        if (span > 0 && span <= 64) {
            for (std::int32_t value = config->low; value <= config->high; ++value) {
                config->values.push_back(trigger::Value::ofInt(value));
            }
        }
    }
    commit();
}

void RulesController::setSlotRange(int slot, const std::string& text) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    const std::vector<std::string_view> parts = split(text, "-– ");
    if (parts.size() != 2) {
        setStatus("A range is two numbers, like 1 - 12.", true);
        return;
    }
    const std::optional<double> low = readNumber(parts[0]);
    const std::optional<double> high = readNumber(parts[1]);
    if (!low || !high) {
        setStatus("A range is two numbers, like 1 - 12.", true);
        return;
    }
    config->low = static_cast<std::int32_t>(*low);
    config->high = static_cast<std::int32_t>(*high);
    commit();
}

void RulesController::setSlotValues(int slot, const std::string& text) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    config->values.clear();
    for (const std::string_view part : split(text, ",;")) {
        config->values.push_back(parseValue(part));
    }
    // Typing a list is saying the values come from a list, so the switch follows rather than
    // making the operator tick a box to make what they typed take effect.
    config->pool = trigger::Pool::List;
    commit();
}

void RulesController::setSlotNoRepeat(int slot, int within) {
    if (Generator::Config* config = slotConfig(slot)) {
        config->noRepeatWithin = static_cast<std::size_t>(std::max(0, within));
        commit();
    }
}

void RulesController::setSlotFixed(int slot, const std::string& text) {
    if (Generator::Config* config = slotConfig(slot)) {
        config->fixed = parseValue(text);
        commit();
    }
}

void RulesController::pickSlotLive(int slot, int source) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr || source < 0 ||
        static_cast<std::size_t>(source) >= trigger::kLiveSources.size()) {
        return;
    }
    config->source = trigger::kLiveSources[static_cast<std::size_t>(source)];
    commit();
}

void RulesController::clearLog() {
    log_.clear();
    logModel_->clear();
}

void RulesController::tick() {
    // Drained whether or not the window is up: the buffer is bounded and dropping the
    // oldest, so a log that is never drained would quietly lose the start of a set. It is
    // also the cheapest possible call when nothing has fired.
    const std::vector<output::OutputRunner::Fired> fired = runner_.takeFired();
    if (!fired.empty()) {
        const Rule::Config* rule = current();
        for (const output::OutputRunner::Fired& entry : fired) {
            log_.push_back(spellNumber(entry.when) + "s  " + entry.ruleId + "  " + entry.message);
            lastFiredAnywhere_ = entry.message;
            if (!entry.followUp) {
                // The press, not the release: see `OutputRunner::Fired::followUp`.
                ++firesSeen_[entry.ruleId];
                slotsSeen_[entry.ruleId] = entry.slots;
            }
            if (rule != nullptr && entry.ruleId == rule->id) {
                lastFired_ = entry.message;
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

void RulesController::publishAll() {
    publishList();
    publishSelected();
    publishSlots();
    publishFiring();
    window_->set_selected(selected_);
}

std::uint64_t RulesController::firesOf(std::string_view ruleId) const {
    const auto at = firesSeen_.find(std::string(ruleId));
    return at == firesSeen_.end() ? 0 : at->second;
}

void RulesController::publishList() {
    std::vector<RuleRow> rows;
    rows.reserve(rules_.size());
    for (const Rule::Config& config : rules_) {
        // Built rather than read from the runner: `OutputRunner::triggers()` is the output
        // thread's and is safe to read only while it is stopped. A `Rule` costs a few
        // generators to construct and this runs on a list of a handful, once per edit.
        const Rule rule(config);
        RuleRow row{};
        row.name = shared(config.name);
        row.enabled = config.enabled;
        row.problem = shared(rule.problem());
        // By id, so a rule deleted from the middle does not hand its count to the one that
        // moves up into its place. See `firesSeen_`.
        row.fires = static_cast<int>(firesOf(config.id));
        row.chosen = rows.size() < chosen_.size() && chosen_[rows.size()];
        rows.push_back(std::move(row));
    }
    // In place: this runs on the redraw timer whenever a rule fires, and the fire counts
    // move on every beat. See `writeRows`.
    writeRows(*listModel_, rows);
}

void RulesController::publishSelected() {
    const Rule::Config* rule = current();
    window_->set_selected(selected_);
    if (rule == nullptr) {
        window_->set_rule_name(slint::SharedString(""));
        window_->set_rule_enabled(false);
        window_->set_address(slint::SharedString(""));
        return;
    }
    window_->set_rule_name(shared(rule->name));
    window_->set_rule_enabled(rule->enabled);

    const auto triggerIndex =
        std::find(trigger::kTriggers.begin(), trigger::kTriggers.end(), rule->trigger) -
        trigger::kTriggers.begin();
    window_->set_trigger_index(static_cast<int>(triggerIndex));
    window_->set_trigger_takes_every(trigger::takesEvery(rule->trigger));
    window_->set_every(static_cast<int>(rule->every));
    window_->set_trigger_takes_pulses(trigger::takesPulses(rule->trigger));
    window_->set_pulses(static_cast<int>(rule->pulses));
    window_->set_euclid_pattern(shared(spellEuclid(*rule)));

    // §5.6's rule subset, and what it currently reaches. Both are needed: the first is what
    // the rule names and the second is whether this rig has it, which is the one question a
    // preset from another rig raises.
    publishOutputChoices();
    window_->set_outputs_available(shared(describeRouting(rule->outputs, targets_)));

    window_->set_min_confidence(static_cast<float>(rule->conditions.minConfidence));
    window_->set_allow_calm(rule->conditions.allows(features::Intensity::Calm));
    window_->set_allow_normal(rule->conditions.allows(features::Intensity::Normal));
    window_->set_allow_intense(rule->conditions.allows(features::Intensity::Intense));
    window_->set_min_bpm(static_cast<float>(rule->conditions.minBpm));
    window_->set_max_bpm(static_cast<float>(rule->conditions.maxBpm));
    window_->set_probability(static_cast<float>(rule->conditions.probability));
    window_->set_cooldown_ms(static_cast<float>(rule->conditions.cooldownSeconds * 1000.0));

    const auto sendIndex =
        std::find(trigger::kMessageKinds.begin(), trigger::kMessageKinds.end(), rule->sendKind) -
        trigger::kMessageKinds.begin();
    window_->set_send_index(static_cast<int>(sendIndex));
    window_->set_sends_osc(rule->sendKind == trigger::Message::Kind::Osc);
    window_->set_address(shared(rule->address));
    // Which preset this address is, so the picker describes the rule in front of it rather
    // than the last thing anybody clicked in it.
    window_->set_host_preset_index(presetOf(rule->address));
    window_->set_channel(rule->channel);
    window_->set_send_value(rule->sendValue);
    window_->set_follow_up(rule->followUp);
    window_->set_follow_up_value(shared(spellValue(rule->followUpValue)));
    window_->set_follow_up_ms(static_cast<float>(rule->followUpDelaySeconds * 1000.0));
    window_->set_follow_up_beats(static_cast<float>(rule->followUpDelayBeats));
    const auto unitIndex =
        std::find(trigger::kDelayUnits.begin(), trigger::kDelayUnits.end(), rule->followUpUnit) -
        trigger::kDelayUnits.begin();
    window_->set_follow_up_unit(static_cast<int>(unitIndex));
}

void RulesController::publishOutputChoices() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        choiceModel_->clear();
        window_->set_outputs_all(false);
        window_->set_outputs_summary(slint::SharedString(""));
        return;
    }

    const auto names = rule->outputs;
    const auto named = [&names](const std::string& name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };

    std::vector<OutputChoice> rows;
    rows.reserve(targets_.size() + names.size());
    for (const output::OutputTarget& target : targets_) {
        OutputChoice row{};
        row.name = shared(target.name);
        row.chosen = named(target.name);
        row.missing = false;
        rows.push_back(std::move(row));
    }
    // Names this rule carries that the rig has no target for — a preset written elsewhere.
    // Listed rather than dropped, for `Rule::Config::outputs`' own reason: plugging that
    // output back in should restore the routing, so the name has to survive not being here.
    for (const std::string& name : names) {
        if (output::findTarget(targets_, name) == nullptr) {
            OutputChoice row{};
            row.name = shared(name);
            row.chosen = true;
            row.missing = true;
            rows.push_back(std::move(row));
        }
    }
    writeRows(*choiceModel_, rows);

    window_->set_outputs_all(names.empty());
    window_->set_outputs_summary(shared(names.empty() ? "every output" : join(names)));
}

void RulesController::publishSlots() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        slotModel_->clear();
        return;
    }

    // What this rule's generators produced last time it fired, in the order the chips are
    // built below — which is the order `Rule::lastSlots` promises. Empty until it has fired.
    const auto seen = slotsSeen_.find(rule->id);
    const std::vector<trigger::Value>* const produced =
        seen == slotsSeen_.end() ? nullptr : &seen->second;

    std::vector<SlotRow> rows;
    const auto push = [&rows, produced](const std::string& label, const Generator::Config& raw) {
        // What the generator **accepted**, not what was typed. `Generator::config()` gives
        // back the clamped configuration, so §5.8's clamp-never-refuse policy shows on
        // screen instead of being silent: a range typed backwards comes back the right way
        // round, and a no-repeat wider than the pool comes back cut to what can be
        // satisfied. An operator who cannot see the clamp has no way to know it happened.
        const Generator::Config config = Generator(raw).config();
        SlotRow row{};
        row.label = shared(label);
        const auto kindIndex = std::find(trigger::kGeneratorKinds.begin(),
                                         trigger::kGeneratorKinds.end(), config.kind) -
                               trigger::kGeneratorKinds.begin();
        row.kind_index = static_cast<int>(kindIndex);
        row.takes_pool = trigger::takesPool(config.kind);
        row.is_list = config.pool == trigger::Pool::List;
        row.low = config.low;
        row.high = config.high;
        row.values = shared(spellValues(config.values));
        row.no_repeat = static_cast<int>(config.noRepeatWithin);
        row.fixed = shared(spellValue(config.fixed));
        row.is_fixed = config.kind == GeneratorKind::Fixed;
        row.is_live = config.kind == GeneratorKind::Live;
        const auto sourceIndex =
            std::find(trigger::kLiveSources.begin(), trigger::kLiveSources.end(), config.source) -
            trigger::kLiveSources.begin();
        row.live_index = static_cast<int>(sourceIndex);
        row.is_ramp = config.kind == GeneratorKind::Ramp;
        const auto shapeIndex =
            std::find(trigger::kRampShapes.begin(), trigger::kRampShapes.end(), config.shape) -
            trigger::kRampShapes.begin();
        row.shape_index = static_cast<int>(shapeIndex);
        row.ramp_bars = static_cast<int>(config.rampBars);
        row.ramp_float = config.rampFloat;
        // §5.9's "what this slot last produced", paired by position: this chip is being
        // pushed at index `rows.size()`, which is the slot `Rule::lastSlots` put there.
        // Blank rather than stale when the rule has not fired since it was last edited —
        // adding a placeholder shifts every chip after it, and a number under the wrong box
        // is worse than no number.
        row.last = produced != nullptr && rows.size() < produced->size()
                       ? shared(spellValue((*produced)[rows.size()]))
                       : slint::SharedString("");
        rows.push_back(std::move(row));
    };

    if (rule->sendKind == trigger::Message::Kind::Osc) {
        // Named by the placeholder they fill, so a chip and the address read together —
        // §5.9's "template segments render as editable chips".
        const std::vector<std::string> names = [&rule] {
            std::vector<std::string> found;
            std::size_t at = 0;
            while ((at = rule->address.find('{', at)) != std::string::npos) {
                const std::size_t close = rule->address.find('}', at);
                if (close == std::string::npos) {
                    break;
                }
                found.push_back(rule->address.substr(at, close - at + 1));
                at = close + 1;
            }
            return found;
        }();
        for (std::size_t i = 0; i < rule->segments.size(); ++i) {
            push(i < names.size() ? names[i] : "{?}", rule->segments[i]);
        }
    } else {
        push(rule->sendKind == trigger::Message::Kind::MidiNote ? "note" : "cc", rule->number);
    }
    if (rule->sendValue) {
        push("value", rule->value);
    }
    // In place, and this is the one that mattered: these rows are the generator chips, they
    // are full of text boxes, and `tick` republishes them every time a rule fires. See
    // `writeRows`. Nothing here moves between fires, so the common case writes no rows at
    // all and the boxes are left entirely alone.
    writeRows(*slotModel_, rows);
}

void RulesController::publishFiring() {
    window_->set_last_fired(shared(lastFired_));
    if (lastFiredAt_ < 0.0) {
        window_->set_last_fired_ago(slint::SharedString(""));
        return;
    }
    const double ago =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count() -
        lastFiredAt_;
    // Seconds, coarsely. A message that landed a moment ago and one that landed a minute ago
    // are different facts; a tenth of a second between them is not.
    window_->set_last_fired_ago(
        shared(ago < 1.0 ? "just now" : spellNumber(std::floor(ago)) + "s ago"));
}

void RulesController::setStatus(const std::string& text, bool error) {
    window_->set_status(shared(text));
    window_->set_status_is_error(error);
}

} // namespace takt4::ui
