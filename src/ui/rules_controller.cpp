#include "ui/rules_controller.hpp"

#include "core/features/intensity.hpp"
#include "core/trigger/generator.hpp"

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
/// The first entry is §5.6's *"blank custom option"*, and picking it changes nothing rather
/// than clearing what is there — an operator who has typed an address and then brushed the
/// picker has not asked to lose it.
const std::array<HostPreset, 5> kHostPresets{{
    {"custom", ""},
    {"Resolume 7 — clip", "/composition/layers/{layer}/clips/{clip}/connect"},
    {"Resolume 7 — resync", "/composition/tempocontroller/resync"},
    {"TouchDesigner", "/takt4/{name}"},
    {"MadMapper — cue", "/medias/{cue}/play"},
}};

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
    rule.enabled = false; // built switched off, like every rule the editor makes
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
        tempo.enabled = false;
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
        resync.enabled = false;
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
        breathe.enabled = false;
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
        stabs.enabled = false;
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

constexpr std::array<const char*, 5> kRigPresets{
    "add a preset...", "Resolume: clips on 3 layers", "Resolume: tempo and resync",
    "Resolume: breathing dashboard", "MIDI: euclidean stabs"};

} // namespace

RulesController::RulesController(output::OutputRunner& runner,
                                 std::vector<trigger::Rule::Config> rules)
    : runner_(runner), rules_(std::move(rules)), window_(RulesWindow::create()),
      listModel_(std::make_shared<slint::VectorModel<RuleRow>>()),
      slotModel_(std::make_shared<slint::VectorModel<SlotRow>>()),
      logModel_(std::make_shared<slint::VectorModel<slint::SharedString>>()) {
    window_->set_rules(listModel_);
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

    window_->on_rule_picked([this](int index) { pick(index); });
    window_->on_rule_added([this] { add(); });
    window_->on_rule_removed([this] { remove(); });
    window_->on_rule_duplicated([this] { duplicate(); });
    window_->on_rig_added([this](int index) { addRig(index); });
    window_->on_rule_enabled_changed([this](bool on) { setEnabled(on); });
    window_->on_rule_renamed([this](const slint::SharedString& n) { rename(std::string(n)); });
    window_->on_rule_tested([this] { test(); });
    window_->on_panic_clicked([this] { panic(); });

    window_->on_trigger_picked([this](int index) { pickTrigger(index); });
    window_->on_every_changed([this](int every) { setEvery(every); });
    window_->on_pulses_changed([this](int pulses) { setPulses(pulses); });
    window_->on_outputs_edited(
        [this](const slint::SharedString& text) { setOutputs(std::string(text)); });

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

    if (!rules_.empty()) {
        selected_ = 0;
    }
    publishAll();
}

void RulesController::show() {
    window_->show();
    visible_ = true;
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
    rules_ = std::move(rules);
    if (rules_.empty()) {
        selected_ = -1;
    } else if (selected_ < 0 || static_cast<std::size_t>(selected_) >= rules_.size()) {
        selected_ = 0;
    }
    publishAll();
}

void RulesController::pick(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= rules_.size()) {
        return;
    }
    selected_ = index;
    // A new selection is a new last-fired line: the old one belonged to another rule and
    // leaving it up would credit this one with what that one sent.
    lastFired_.clear();
    lastFiredAt_ = -1.0;
    publishSelected();
    publishSlots();
    publishFiring();
    window_->set_selected(selected_);
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
    // Distinct seeds, so two rules in a preset do not fire the same clip as each other —
    // `Generator::Config::seed`'s whole reason.
    rule.seed = static_cast<std::uint64_t>(rules_.size()) * 977 + 1;
    rule.value.kind = GeneratorKind::Fixed;
    rule.value.fixed = trigger::Value::ofInt(1);
    // **Switched off until it is finished.** A new rule has no address yet, so it cannot
    // fire either way — but the moment an operator picks a host preset it *could*, and a
    // half-built rule firing into somebody's rig while they are still typing is the one
    // thing an editor must not do. §5.9's [test] button works on a disabled rule for
    // exactly this reason, so nothing about building one needs it switched on.
    rule.enabled = false;
    rules_.push_back(rule);
    selected_ = static_cast<int>(rules_.size()) - 1;
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
}

void RulesController::remove() {
    if (current() == nullptr) {
        return;
    }
    rules_.erase(rules_.begin() + selected_);
    if (rules_.empty()) {
        selected_ = -1;
    } else if (static_cast<std::size_t>(selected_) >= rules_.size()) {
        selected_ = static_cast<int>(rules_.size()) - 1;
    }
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
}

void RulesController::duplicate() {
    const Rule::Config* source = current();
    if (source == nullptr) {
        return;
    }
    Rule::Config copy = *source;
    copy.id += "-copy";
    for (int n = 2;
         std::count_if(rules_.begin(), rules_.end(),
                       [&copy](const Rule::Config& other) { return other.id == copy.id; }) > 0;
         ++n) {
        copy.id = source->id + "-copy" + std::to_string(n);
    }
    if (!copy.name.empty()) {
        copy.name += " copy";
    }
    // A different stream, or the copy would fire exactly what the original fires — which is
    // never what duplicating a rule is for.
    copy.seed = source->seed + 977;
    rules_.insert(rules_.begin() + selected_ + 1, copy);
    ++selected_;
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
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
    selected_ = static_cast<int>(rules_.size()) - static_cast<int>(added.size());
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
    if (rule == nullptr || index <= 0 || static_cast<std::size_t>(index) >= kHostPresets.size()) {
        return; // 0 is "custom", which changes nothing
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
    logModel_->set_vector({});
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
        logModel_->set_vector(std::move(lines));
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
        row.fires = 0;
        rows.push_back(std::move(row));
    }
    // The fire counts, kept here because the live rules cannot be asked for them safely.
    firesSeen_.resize(rules_.size(), 0);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].fires = static_cast<int>(firesSeen_[i]);
    }
    listModel_->set_vector(std::move(rows));
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

    // §5.6's rule subset, and what it currently reaches. Both are needed: the names are what
    // the operator typed and the second line is whether this rig has them, which is the one
    // question a preset from another rig raises.
    window_->set_outputs(shared(join(rule->outputs)));
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
    window_->set_channel(rule->channel);
    window_->set_send_value(rule->sendValue);
    window_->set_follow_up(rule->followUp);
    window_->set_follow_up_value(shared(spellValue(rule->followUpValue)));
    window_->set_follow_up_ms(static_cast<float>(rule->followUpDelaySeconds * 1000.0));
}

void RulesController::publishSlots() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        slotModel_->set_vector({});
        return;
    }

    std::vector<SlotRow> rows;
    const auto push = [&rows](const std::string& label, const Generator::Config& raw) {
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
        row.last = slint::SharedString("");
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
    slotModel_->set_vector(std::move(rows));
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
