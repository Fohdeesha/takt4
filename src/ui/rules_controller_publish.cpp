// `RulesController`, the half that draws: what the window shows of the rule list, the rule
// picked, its slots, follow-ups, routing, lighting and firing. The edits are in
// `rules_controller.cpp` and `rules_controller_slots.cpp`.

#include "core/features/intensity.hpp"
#include "core/io/utf8.hpp"
#include "core/trigger/generator.hpp"
#include "ui/model_rows.hpp"
#include "ui/native_window.hpp"
#include "ui/rule_presets.hpp"
#include "ui/rule_text.hpp"
#include "ui/rules_controller.hpp"
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

void RulesController::publishPalette() {
    const Generator::Config* config = paletteConfig();
    if (config == nullptr || !trigger::takesPool(config->kind)) {
        // A fixed color has its own swatch on the slot row; there is no palette to show.
        paletteRows_.clear();
        window_->set_palette_shown(false);
        return;
    }
    window_->set_palette_shown(true);

    std::vector<PaletteEntry> rows;
    rows.reserve(config->values.size());
    for (std::size_t i = 0; i < config->values.size(); ++i) {
        std::string text;
        config->values[i].appendTo(text);
        // What could not be read is shown as white and says so in its own box, which is the
        // same answer `Rule::buildPayload` gives the wire — the editor must not show a color
        // the fire would not send.
        const dmx::Color color = dmx::parseColor(text).value_or(dmx::kWhite);
        PaletteEntry row{};
        row.swatch = slint::Color::from_rgb_uint8(color.r, color.g, color.b);
        row.hex = shared(dmx::formatColor(color));
        const auto held = pickedPalette_.find(static_cast<int>(i));
        if (held != pickedPalette_.end()) {
            row.hue = held->second.hue;
            row.sat = held->second.saturation;
            row.val = held->second.brightness;
        } else {
            double hue = 0.0;
            double saturation = 1.0;
            double value = 1.0;
            dmx::toHsv(color, hue, saturation, value);
            row.hue = static_cast<float>(hue);
            row.sat = static_cast<float>(saturation * 100.0);
            row.val = static_cast<float>(value * 100.0);
        }
        rows.push_back(std::move(row));
    }
    // **Not while a slider is driving it.** A swatch the operator is dragging is a swatch
    // whose row moved on every pixel, and rebuilding the repeater would destroy the popup
    // holding the slider they are still holding. See `pickingColor_` — this is the crash of
    // 2026-09-16, and taking this condition out makes the test below it fail with ninety
    // resets for a ninety-pixel drag.
    if (!pickingColor_ &&
        paletteRows_.markStale(
            rows, [](const PaletteEntry& was, const PaletteEntry& now) { return was != now; })) {
        rowsDirty_ = true;
    }
    paletteRows_.write(rows);
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
    // And the card's own line for the one being edited, from the same `Rule` — here rather than
    // only in `publishSelected`, because `commit` does not call that and nearly every edit that
    // changes whether a rule can fire goes through `commit`.
    window_->set_rule_problem(selected_ >= 0 && static_cast<std::size_t>(selected_) < rows.size()
                                  ? rows[static_cast<std::size_t>(selected_)].problem
                                  : slint::SharedString(""));
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
        window_->set_rule_problem(slint::SharedString(""));
        window_->set_address(slint::SharedString(""));
        return;
    }
    window_->set_rule_name(shared(rule->name));
    window_->set_rule_enabled(rule->enabled);
    window_->set_rule_problem(shared(Rule(*rule).problem()));

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
    window_->set_outputs_available(
        shared(describeRouting(rule->sendKind, rule->outputs, targets_)));

    window_->set_min_confidence(static_cast<float>(rule->conditions.minConfidence));
    window_->set_allow_calm(rule->conditions.allows(features::Intensity::Calm));
    window_->set_allow_normal(rule->conditions.allows(features::Intensity::Normal));
    window_->set_allow_intense(rule->conditions.allows(features::Intensity::Intense));
    window_->set_probability(static_cast<float>(rule->conditions.probability));
    // Spelled here rather than by the markup, because both boxes are two-way bound and so the
    // property holds *text* — see `bpm-field`, where the one-way version's failure is written
    // down. `spellNumber` so a range reads "70 - 140" and not "70.000000 - 140.000000".
    //
    // And so writing one writes over what is being typed in it. Every action commits that first
    // (`commitTyping`), so this skips only a republish from outside — the main window's outputs
    // or patch changing — which would otherwise take the keystrokes out from under the operator
    // (the audit of 2026-09-25, M18).
    if (!typingInto(kBpmRangeBox)) {
        window_->set_bpm_range(shared(spellNumber(rule->conditions.minBpm) + " - " +
                                      spellNumber(rule->conditions.maxBpm)));
    }
    if (!typingInto(kCooldownBox)) {
        window_->set_cooldown_ms(
            shared(spellNumber(rule->conditions.cooldownSeconds * 1000.0)));
    }

    const auto sendIndex =
        std::find(trigger::kMessageKinds.begin(), trigger::kMessageKinds.end(), rule->sendKind) -
        trigger::kMessageKinds.begin();
    window_->set_send_index(static_cast<int>(sendIndex));
    window_->set_sends_osc(rule->sendKind == trigger::Message::Kind::Osc);
    // Three families now rather than two, so "not OSC" is no longer "MIDI": the channel
    // spinner has to stay off a lighting rule, which has no channel at all.
    window_->set_sends_midi(trigger::isMidi(rule->sendKind));
    publishDmx();
    if (!typingInto(kAddressBox)) {
        window_->set_address(shared(rule->address));
    }
    // Which preset this address is, so the picker describes the rule in front of it rather
    // than the last thing anybody clicked in it.
    window_->set_host_preset_index(presetOf(rule->address));
    window_->set_channel(rule->channel);
    window_->set_send_value(rule->sendValue);

    // The live gestures, from this class's own mirror — the output thread's rules are not safe
    // to read while it runs. See `mutedSeen_`.
    const auto muted = mutedSeen_.find(rule->id);
    window_->set_rule_muted(muted != mutedSeen_.end() && muted->second);
    const auto rate = rateSeen_.find(rule->id);
    const double factor = rate == rateSeen_.end() ? 1.0 : rate->second;
    window_->set_rule_rate(shared(describeRate(factor)));

    publishFollowUps();
}

void RulesController::publishFollowUps() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        followRows_.clear();
        followsBuiltFor_.clear();
        followKinds_.clear();
        followRelease_.clear();
        window_->set_can_add_follow_up(false);
        return;
    }
    // Another rule's rows are built from nothing, as the chips are (`slotsBuiltFor_`). The boxes
    // on them no longer go deaf, so nothing else rebuilds them — and a box that had the keyboard
    // would go on holding what was typed for the rule before, over this one's value, and commit
    // it into this one when it let go.
    if (rule->id != followsBuiltFor_) {
        followRows_.clear();
        followsBuiltFor_ = rule->id;
    }

    // What a follow-up on *this* rule may be, and the two things left out of it.
    //
    // Kinds on the far side of the OSC/MIDI divide go, because `Rule::followUpsFor` would
    // drop them — a MIDI follow-up to an OSC rule has no channel or note to inherit — and
    // offering a choice that does nothing is worse than not offering it. `pickSend` turns any
    // that a change of send kind stranded into releases, so no row can name one.
    //
    // And OSC goes, because on an OSC rule it *is* the first entry: the same address with a
    // different argument is what a release means, so the two would behave identically. DMX
    // goes for the same reason on a lighting rule (the audit's M10): a DMX follow-up with no
    // effect of its own — and nothing here can give it one — is exactly the release, so the
    // list offered "release" and "DMX" and both did the same thing. A MIDI kind matching the
    // rule's own is a different matter and stays — it carries a number of its own where a
    // release inherits the one that fired, which is a real second gesture.
    std::vector<trigger::Message::Kind> allowed;
    for (const trigger::Message::Kind kind : trigger::kMessageKinds) {
        if (kind != trigger::Message::Kind::Osc && kind != trigger::Message::Kind::Dmx &&
            trigger::followUpFits(kind, rule->sendKind)) {
            allowed.push_back(kind);
        }
    }
    // **"release", and what it releases.** It is the default, and the only entry that can
    // follow a *drawn* number, since the note to let go of is the one the shuffle picked —
    // which is exactly the thing a rig went looking for and did not find: it read the bare
    // word "release" as some other gesture, picked the explicit note off, and found a box
    // demanding one fixed note. The entry now says what it inherits, so the dynamic one is the
    // one that looks dynamic.
    const std::string release = releaseLabelOf(rule->sendKind);
    if (allowed != followKinds_ || release != followRelease_) {
        followKinds_ = allowed;
        followRelease_ = release;
        auto labels = std::make_shared<slint::VectorModel<slint::SharedString>>();
        labels->push_back(shared(release));
        for (const trigger::Message::Kind kind : followKinds_) {
            labels->push_back(shared(std::string(trigger::labelOf(kind))));
        }
        window_->set_follow_kinds(labels);
    }
    window_->set_can_add_follow_up(rule->followUps.size() < trigger::kMaxFollowUps);

    std::vector<FollowRow> rows;
    rows.reserve(rule->followUps.size());
    for (const trigger::FollowUp& entry : rule->followUps) {
        FollowRow row{};
        int choice = 0;
        if (entry.kind) {
            const auto at = std::find(followKinds_.begin(), followKinds_.end(), *entry.kind);
            // A kind the rule can no longer reach — the send kind was changed under it — reads
            // as a release, which is what it will actually behave as (`followUpFits` drops it,
            // and a release is what the row then means). The configuration is left alone until
            // the operator touches the row, so switching kinds back restores what they had.
            choice = at == followKinds_.end() ? 0 : static_cast<int>(at - followKinds_.begin()) + 1;
        }
        row.kind_index = choice;
        // A release inherits the fired message's number, so it has none of its own to show.
        row.takes_number = entry.kind && trigger::sendsNumber(*entry.kind);
        row.number_label = shared(row.takes_number ? numberLabelOf(*entry.kind) : "");
        row.number = entry.number;
        row.takes_value = !entry.kind || trigger::sendsValue(*entry.kind) ||
                          *entry.kind == trigger::Message::Kind::Osc;
        // Except the release of a move, which is skipped whole: a level box beside it would
        // be a setting that does nothing. The summary says why.
        if (!entry.kind && rule->sendKind == trigger::Message::Kind::Dmx &&
            dmx::takesMovement(rule->dmx.effect)) {
            row.takes_value = false;
        }
        // Named for whatever the row resolves to, which for a release is the *released* kind
        // and not the rule's: a note on's release carries a release velocity, and a box
        // holding a bare 0 had an operator asking what it was.
        row.value_label =
            shared(valueLabelOf(entry.kind ? *entry.kind : releasedAs(rule->sendKind)));
        row.value = shared(spellValue(entry.value));
        const auto unitIndex =
            std::find(trigger::kDelayUnits.begin(), trigger::kDelayUnits.end(), entry.unit) -
            trigger::kDelayUnits.begin();
        row.unit = static_cast<int>(unitIndex);
        row.delay = shared(entry.unit == trigger::DelayUnit::Milliseconds
                               ? spellNumber(entry.delaySeconds * 1000.0)
                               : spellNumber(entry.delayBeats));
        row.summary = shared(describeFollowUp(entry, *rule));
        rows.push_back(std::move(row));
    }

    // Nothing on these rows moves on its own — no live readout — so a change is one the
    // controller made and has to get back into a widget that may have gone deaf. See
    // `rowsDirty_`. **Except the three boxes**: a `NumberBox` and a `LiveField` never go deaf,
    // and a row rebuilt for one of them was the row, and the box the operator had just tabbed
    // into, destroyed under the next keystroke (the audit of 2026-09-25, M17).
    if (followRows_.markStale(rows, [](const FollowRow& was, const FollowRow& now) {
            FollowRow ignoring = was;
            ignoring.number = now.number;
            ignoring.value = now.value;
            ignoring.delay = now.delay;
            return ignoring != now;
        })) {
        rowsDirty_ = true;
    }
    followRows_.write(rows);
}

void RulesController::publishOutputChoices() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        choiceRows_.clear();
        window_->set_outputs_all(false);
        window_->set_outputs_summary(slint::SharedString(""));
        return;
    }

    const auto ids = rule->outputs;
    const auto routed = [&ids](const std::string& id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };

    std::vector<OutputChoice> rows;
    rows.reserve(targets_.size() + ids.size());
    for (const output::OutputTarget& target : targets_) {
        // **Only the targets this rule's kind can actually reach.** See `targetTakes`: a MIDI
        // rule ticked against an OSC target sends nothing, and nothing routes to an Art-Net
        // node by name at all. A target this rule *names* but cannot reach is still listed,
        // below, as a name that reaches nothing — which is the truth and is how it gets
        // un-ticked.
        if (!targetTakes(rule->sendKind, target)) {
            continue;
        }
        OutputChoice row{};
        row.key = shared(target.id);
        row.name = shared(target.name);
        row.chosen = routed(target.id);
        row.missing = false;
        rows.push_back(std::move(row));
    }
    // Outputs this rule is routed to that the rig no longer has, and ones it has that this
    // kind cannot reach — a rule switched from OSC to MIDI while still routed to an OSC target.
    // Listed rather than dropped, because the outcome is the same either way: the rule holds
    // it and it reaches nothing, and the list is where that is seen and un-ticked.
    for (const std::string& id : ids) {
        const output::OutputTarget* const target = output::findTarget(targets_, id);
        if (target == nullptr || !targetTakes(rule->sendKind, *target)) {
            OutputChoice row{};
            row.key = shared(id);
            row.name = shared(target != nullptr ? target->name : std::string("an output that is gone"));
            row.chosen = true;
            row.missing = true;
            rows.push_back(std::move(row));
        }
    }

    // **These are tick boxes, and a tick box drops its binding the moment it is clicked.**
    //
    // The same mechanism the generator chips and the THEN SEND rows are rebuilt for, on the
    // one publisher that did not take part in it. A `CheckBox` bound `checked: choice.chosen`
    // stops following the model as soon as somebody ticks it — so after routing one rule to
    // "lights", every rule selected afterwards showed "lights" already ticked, and clicking
    // it to route *that* rule un-ticked it and did nothing. Nothing on this row moves on its
    // own, so any change at all is one the controller made and has to get back into a box
    // that may have gone deaf.
    if (choiceRows_.markStale(
            rows, [](const OutputChoice& was, const OutputChoice& now) { return was != now; })) {
        rowsDirty_ = true;
    }
    choiceRows_.write(rows);

    std::vector<std::string> shown;
    for (const std::string& id : ids) {
        const output::OutputTarget* const target = output::findTarget(targets_, id);
        shown.push_back(target != nullptr ? target->name : std::string("an output that is gone"));
    }
    window_->set_outputs_all(ids.empty());
    window_->set_outputs_summary(shared(ids.empty() ? "every output" : join(shown)));
}

void RulesController::publishFixtureChoices() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        fixtureRows_.clear();
        window_->set_fixtures_summary(slint::SharedString(""));
        window_->set_fixtures_available(slint::SharedString(""));
        window_->set_fixtures_reaches_nothing(false);
        return;
    }

    const std::vector<std::string>& aims = rule->dmx.fixtures;
    const auto aimed = [&aims](const std::string& key) {
        return std::find(aims.begin(), aims.end(), key) != aims.end();
    };

    std::vector<OutputChoice> rows;
    rows.reserve(patch_.size() + aims.size());
    // Groups first, because aiming at "heads" is what an operator reaches for and a list that
    // buried it under six fixture names would hide the useful half. Each group once, in the
    // order the patch introduces it.
    std::vector<std::string> groups;
    for (const dmx::Fixture& fixture : patch_) {
        if (fixture.group.empty() ||
            std::find(groups.begin(), groups.end(), fixture.group) != groups.end()) {
            continue;
        }
        groups.push_back(fixture.group);
        OutputChoice row{};
        row.key = shared(fixture.group); // a group is its label; see `dmx::Fixture::group`
        row.name = shared(fixture.group);
        row.chosen = aimed(fixture.group);
        row.missing = false;
        rows.push_back(std::move(row));
    }
    // Only the first `dmx::kMaxRoutableFixtures` (512): a rule carries its fixtures as a bit
    // each, and `dmx::resolveFixtures` reaches no further, so a tick past them would look like
    // aiming and reach nothing (the audit's M21). The patch editor says as much when one past the
    // last is added.
    const std::size_t reachable = std::min(patch_.size(), dmx::kMaxRoutableFixtures);
    for (std::size_t i = 0; i < reachable; ++i) {
        const dmx::Fixture& fixture = patch_[i];
        OutputChoice row{};
        row.key = shared(fixture.id);
        row.name = shared(fixture.name);
        row.chosen = aimed(fixture.id);
        row.missing = false;
        rows.push_back(std::move(row));
    }
    // What this rule aims at that the patch has no fixture or group for — a fixture since
    // deleted, or a group label no fixture carries any more. Listed rather than dropped, so
    // the routing that reaches nothing is seen and can be un-ticked.
    for (const std::string& key : aims) {
        const bool here = std::find(groups.begin(), groups.end(), key) != groups.end() ||
                          dmx::findFixture(patch_, key) != nullptr;
        if (!here) {
            OutputChoice row{};
            row.key = shared(key);
            // A group's label reads as itself; a fixture's id means nothing to anybody.
            row.name = shared(key.rfind("f-", 0) == 0 ? std::string("a fixture that is gone") : key);
            row.chosen = true;
            row.missing = true;
            rows.push_back(std::move(row));
        }
    }

    // Tick boxes, which drop their binding the moment they are clicked — see
    // `publishOutputChoices`, which found that the hard way.
    if (fixtureRows_.markStale(
            rows, [](const OutputChoice& was, const OutputChoice& now) { return was != now; })) {
        rowsDirty_ = true;
    }
    fixtureRows_.write(rows);

    std::vector<std::string> shown;
    for (const std::string& key : aims) {
        const dmx::Fixture* const fixture = dmx::findFixture(patch_, key);
        shown.push_back(fixture != nullptr ? fixture->name
                        : key.rfind("f-", 0) == 0 ? std::string("a fixture that is gone")
                                                  : key);
    }
    window_->set_fixtures_summary(
        shared(aims.empty() ? "nothing — this rule sends nowhere" : join(shown)));
    // What the rule actually reaches on *this* rig, in fixtures. The count matters: "heads"
    // reaching three fixtures and "heads" reaching none look identical in a list of ticks.
    const std::size_t reached = dmx::resolveFixtures(patch_, aims).count();
    std::string available;
    if (aims.empty()) {
        available = "pick at least one — a DMX rule with no fixtures does nothing";
    } else if (reached == 0) {
        available = "reaches nothing on this rig";
    } else {
        available =
            "reaches " + std::to_string(reached) + (reached == 1 ? " fixture" : " fixtures");
    }
    window_->set_fixtures_available(shared(available));
    window_->set_fixtures_reaches_nothing(reached == 0);
}

std::string RulesController::describeRoleReach(const trigger::DmxSend& send) const {
    // **What the rule editor could not say, and should have.** A rule aimed at `dimmer` over
    // an RGB par used to be a rule that fired, logged, counted and lit nothing — the fixture
    // has no dimmer channel, so the effect reached no channel and the only trace was a number
    // in `DmxEngine::missed()` that nothing shows. Reported from a rig on 2026-09-16:
    // *"Choosing the closest thing available right now, dimmer, does absolutely nothing."*
    //
    // The engine now fakes a dimmer out of the color (see `dmx::aims`), so the honest line
    // says which of the two is happening — and still says "reaches nothing" for the aim that
    // really does, a pan on a wash.
    if (!dmx::takesRole(send.effect)) {
        return {};
    }
    const dmx::FixtureSet mask = dmx::resolveFixtures(patch_, send.fixtures);
    if (mask.none()) {
        return {}; // "reaches no fixtures" is already said beside the fixture picker
    }
    std::size_t reached = 0;
    std::size_t faked = 0;
    std::size_t total = 0;
    const std::size_t count = std::min(patch_.size(), dmx::kMaxRoutableFixtures);
    for (std::size_t i = 0; i < count; ++i) {
        if (!mask.test(i)) {
            continue;
        }
        ++total;
        if (dmx::has(patch_[i], send.role)) {
            ++reached;
        } else if (dmx::aims(patch_[i], send.role)) {
            ++faked;
        }
    }
    if (total == 0) {
        return {};
    }
    const std::string channel(dmx::labelOf(send.role));
    if (reached + faked == 0) {
        return "none of them has a " + channel + " channel — this reaches nothing";
    }
    if (faked > 0 && reached == 0) {
        return faked == total ? "no " + channel + " channel: drives the color instead"
                              : "some have no " + channel + " channel: those drive the color";
    }
    if (reached < total) {
        return std::to_string(total - reached) + " of " + std::to_string(total) + " have no " +
               channel + " channel";
    }
    return {};
}

void RulesController::publishDmx() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        window_->set_sends_dmx(false);
        return;
    }
    const trigger::DmxSend& send = rule->dmx;
    window_->set_sends_dmx(rule->sendKind == trigger::Message::Kind::Dmx);

    const auto effectIndex =
        std::find(dmx::kEffectKinds.begin(), dmx::kEffectKinds.end(), send.effect) -
        dmx::kEffectKinds.begin();
    window_->set_effect_index(static_cast<int>(effectIndex));
    const auto roleIndex =
        std::find(dmx::kAimableRoles.begin(), dmx::kAimableRoles.end(), send.role) -
        dmx::kAimableRoles.begin();
    window_->set_effect_role_index(static_cast<int>(
        roleIndex < static_cast<long long>(dmx::kAimableRoles.size()) ? roleIndex : 0));
    const auto curveIndex =
        std::find(dmx::kCurves.begin(), dmx::kCurves.end(), send.curve) - dmx::kCurves.begin();
    window_->set_effect_curve_index(static_cast<int>(curveIndex));
    const auto shapeIndex =
        std::find(dmx::kPathShapes.begin(), dmx::kPathShapes.end(), send.shape) -
        dmx::kPathShapes.begin();
    window_->set_effect_shape_index(static_cast<int>(shapeIndex));

    // Which controls this effect actually uses. Everything else is hidden rather than
    // disabled: a strobe's duty cycle greyed out on a fade is a control an operator has to
    // work out is irrelevant, and there are eight of them.
    window_->set_effect_takes_role(dmx::takesRole(send.effect));
    window_->set_effect_takes_base(dmx::takesBase(send.effect));
    window_->set_effect_takes_cycles(dmx::takesCycles(send.effect));
    window_->set_effect_takes_duty(send.effect == dmx::EffectKind::Strobe);
    window_->set_effect_takes_hue(send.effect == dmx::EffectKind::HueSweep);
    window_->set_effect_takes_shape(send.effect == dmx::EffectKind::Path);
    window_->set_effect_takes_curve(send.effect != dmx::EffectKind::Strobe);
    window_->set_effect_takes_color(dmx::takesColor(send.effect));

    const auto colorModeIndex =
        std::find(trigger::kColorModes.begin(), trigger::kColorModes.end(), send.colorMode) -
        trigger::kColorModes.begin();
    window_->set_color_mode_index(static_cast<int>(colorModeIndex));
    window_->set_effect_role_note(shared(describeRoleReach(send)));

    window_->set_effect_base(send.base);
    window_->set_effect_duty(static_cast<float>(send.duty * 100.0));
    window_->set_effect_size(static_cast<float>(send.size * 100.0));
    // The three text boxes are bound both ways, so writing one writes over whatever is in it —
    // left alone while it holds keystrokes nobody has committed (see `typingInto`).
    if (!typingInto(kHueBox)) {
        window_->set_effect_hue(
            shared(spellNumber(send.hueFrom) + " - " + spellNumber(send.hueTo)));
    }
    if (!typingInto(kCyclesBox)) {
        window_->set_effect_cycles(shared(spellNumber(send.cycles)));
    }
    const auto unitIndex =
        std::find(trigger::kDelayUnits.begin(), trigger::kDelayUnits.end(), send.unit) -
        trigger::kDelayUnits.begin();
    window_->set_effect_unit(static_cast<int>(unitIndex));
    if (!typingInto(kDurationBox)) {
        window_->set_effect_duration(shared(send.unit == trigger::DelayUnit::Milliseconds
                                                ? spellNumber(send.durationSeconds * 1000.0)
                                                : spellNumber(send.durationBeats)));
    }
    publishFixtureChoices();
}

void RulesController::publishSlots() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        slotRows_.clear();
        slotsBuiltFor_.clear();
        return;
    }

    // **Rebuild from scratch when the rule or its send kind changes, and only then.**
    //
    // `writeRows` updates rows in place so that a box being typed into is not destroyed under
    // the cursor — see its header, which is right about that and did not go far enough. The
    // consequence it records is that a widget that assigns its own value — a `ComboBox` picked
    // from, and the `LineEdit`s these boxes were until the audit of 2026-09-25 — loses its
    // binding the moment it is used, because Slint drops a binding when the property is
    // assigned. Within one rule that is harmless: what was picked is what is meant.
    //
    // Across rules it is not. Selecting a different rule updates the model rows, the dead
    // widget ignores them, and the operator sees the *previous* rule's range, list and mode
    // on every rule they click — which reads exactly like one edit having changed them all.
    // It was reported that way on 2026-09-12, and the giveaway was that toggling the `list`
    // checkbox "fixed" it: that flips an `if`, so the element is destroyed and the new one
    // comes up bound.
    //
    // Clearing first makes the repeater build fresh items, so every box is bound again. It
    // costs the focus, which is right here — the operator just clicked another rule — and it
    // never happens on the `tick` path, where the id and kind are unchanged.
    // The DMX effect counts as a change of kind here, because it decides *which* generators a
    // rule has: switching from a fade to a position takes the level chip away and puts pan and
    // tilt in its place, and a row reused across that would be a pan box still bound to the
    // level it used to be.
    const bool rebuild = rule->id != slotsBuiltFor_ || rule->sendKind != slotsKind_ ||
                         rule->dmx.effect != slotsEffect_ || rule->dmx.colorMode != slotsColorMode_;
    if (rebuild) {
        slotRows_.clear();
        slotsBuiltFor_ = rule->id;
        slotsKind_ = rule->sendKind;
        slotsEffect_ = rule->dmx.effect;
        // The color mode counts for the same reason the effect does: it decides *which*
        // generators the rule has — one color chip or three component chips — and a row
        // reused across that would be a red box still bound to the palette it used to be.
        slotsColorMode_ = rule->dmx.colorMode;
        // The picker's sliders belong to the rows that were on screen, and the rows are about
        // to be different ones. Kept across an ordinary republish — that is the whole point of
        // holding them — and dropped when the slots themselves change, so another rule's
        // picker opens on that rule's own color.
        pickedColors_.clear();
    }

    // What this rule's generators produced last time it fired, in the order the chips are
    // built below — which is the order `Rule::lastSlots` promises. Empty until it has fired.
    const auto seen = slotsSeen_.find(rule->id);
    const std::vector<trigger::Value>* const produced =
        seen == slotsSeen_.end() ? nullptr : &seen->second;

    std::vector<SlotRow> rows;
    const auto push = [&rows, produced, this](const std::string& label,
                                              const Generator::Config& raw, bool color = false) {
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
        row.is_normalised = row.is_live && config.source == trigger::LiveSource::BpmNormalised;
        row.normalise = shared(spellNumber(config.normaliseLow) + " - " +
                               spellNumber(config.normaliseHigh));
        row.is_weighted = config.kind == GeneratorKind::Weighted;
        row.weights = shared(spellWeights(config.choices));
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

        // A color slot gets a swatch and a picker, because `#20ff80` is the one value in
        // this window nobody can read. The swatch shows what the generator is *set* to — a
        // fixed color, or the first of a palette — so an operator glancing at the row sees
        // the color rather than its arithmetic.
        row.is_color = color;
        if (color) {
            std::string text;
            if (config.kind == GeneratorKind::Fixed) {
                config.fixed.appendTo(text);
            } else if (!config.values.empty()) {
                config.values.front().appendTo(text);
            }
            const dmx::Color swatch = dmx::parseColor(text).value_or(dmx::kWhite);
            row.swatch = slint::Color::from_rgb_uint8(swatch.r, swatch.g, swatch.b);
            // The sliders as the operator last left them, or read off the color the first
            // time this row is drawn. See `pickedColors_` for why they are not re-derived
            // every time.
            const auto held = pickedColors_.find(static_cast<int>(rows.size()));
            if (held != pickedColors_.end()) {
                row.hue = held->second.hue;
                row.sat = held->second.saturation;
                row.val = held->second.brightness;
            } else {
                double hue = 0.0;
                double saturation = 1.0;
                double value = 1.0;
                dmx::toHsv(swatch, hue, saturation, value);
                row.hue = static_cast<float>(hue);
                row.sat = static_cast<float>(saturation * 100.0);
                row.val = static_cast<float>(value * 100.0);
            }
        }
        rows.push_back(std::move(row));
    };

    // Named by the placeholder they fill, so a chip and the address read together — §5.9's
    // "template segments render as editable chips".
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
    // One row per generator the rule draws, in `trigger::slotLayout`'s order — the order a fire
    // records in `Rule::lastSlots`, and the order `slotConfig` finds a row's generator by. It
    // used to be walked here and in `slotConfig` beside the core's own, with comments saying the
    // three must not drift apart (the audit's Low items); there is one walk now, and a test
    // holds it to what a fire records. Program change has nowhere to put a value and pitch
    // bend is nothing but one; a lighting effect has only the generators it uses.
    for (const trigger::Slot& slot : trigger::slotLayout(*rule)) {
        const Generator::Config& generator = trigger::slotGenerator(*rule, slot);
        switch (slot.role) {
        case trigger::SlotRole::Segment:
            push(slot.segment < names.size() ? names[slot.segment] : "{?}", generator);
            break;
        case trigger::SlotRole::Value:
            push(valueLabelOf(rule->sendKind), generator);
            break;
        case trigger::SlotRole::Number:
            push(numberLabelOf(rule->sendKind), generator);
            if (!rule->numberChosen) {
                // An empty box asking for one, not the zero a fixed generator holds — a zero
                // would read as a number already chosen, and the rule is not firing because it
                // is not.
                rows.back().fixed = slint::SharedString("");
                rows.back().wanting = true;
            }
            break;
        case trigger::SlotRole::Level:
            push(std::string(dmx::labelOf(rule->dmx.role)), generator);
            break;
        case trigger::SlotRole::Color:
            push("color", generator, /*color=*/true);
            break;
        // A mix is three chips holding numbers, and the numbers are DMX bytes, so they are
        // labelled and ranged like every other byte in this window rather than pretending to be
        // colors.
        case trigger::SlotRole::Red:
            push("red", generator);
            break;
        case trigger::SlotRole::Green:
            push("green", generator);
            break;
        case trigger::SlotRole::Blue:
            push("blue", generator);
            break;
        // Percentages of each fixture's own movement window — see `DmxSend::pan`. Labelled with
        // the unit, because 50 meaning "half way across what I allowed" rather than "DMX 50" is
        // the one thing about this pair that is not obvious.
        case trigger::SlotRole::Pan:
            push("pan %", generator);
            break;
        case trigger::SlotRole::Tilt:
            push("tilt %", generator);
            break;
        }
    }
    // In place, and this is the one that mattered: these rows are the generator chips, they
    // are full of text boxes, and `tick` republishes them every time a rule fires. See
    // `writeRows`. Nothing here moves between fires *except* the last-produced readout, so
    // the common case writes no rows at all and the boxes are left entirely alone.
    //
    // And when something else moves — a range this clamped back the right way round, a list
    // seeded from a range, another rule's values — the row has to come back as a new element
    // or a box that has been typed into will go on showing what was typed. That readout is
    // the field to exclude: it ticks over on every beat, and rebuilding for it would tear the
    // boxes down under the operator's hands.
    //
    // The other exclusion is not a field but a cause: a color slider being dragged changes
    // this row's swatch and its hex on every pixel, and the picker holding that slider is a
    // popup inside this very repeater. See `pickingColor_`.
    if (!pickingColor_ && slotRows_.markStale(rows, [](const SlotRow& was, const SlotRow& now) {
            SlotRow ignoring = was;
            ignoring.last = now.last;
            // And whatever a `LiveField` or a `NumberBox` shows, since neither goes deaf: a row
            // rebuilt for one of them took the box beside it — the one the operator had just
            // tabbed into — down with it (the audit of 2026-09-25, M17). What is left is what the
            // std widgets hold, which do assign their own values: the dropdowns, the tick boxes
            // and the picker's sliders.
            ignoring.low = now.low;
            ignoring.high = now.high;
            ignoring.values = now.values;
            ignoring.fixed = now.fixed;
            ignoring.normalise = now.normalise;
            ignoring.weights = now.weights;
            ignoring.ramp_bars = now.ramp_bars;
            ignoring.no_repeat = now.no_repeat;
            return ignoring != now;
        })) {
        rowsDirty_ = true;
    }
    slotRows_.write(rows);
    publishPalette();
}

void RulesController::rebuildRows() {
    // **Never while a picker is open.** A color picker is a `PopupWindow` belonging to one of
    // the items about to be thrown away, so a rebuild here destroys the popup — and with it
    // the slider the operator has hold of. The rebuild is not cancelled, only held: the flag
    // stays set and `tick` takes it on the first redraw after the picker closes, which is
    // before the operator can have typed into anything. See `setPickerOpen`.
    if (pickerOpen_) {
        return;
    }
    rowsDirty_ = false;
    if (!rebuildAll_) {
        // **Only the rows that moved** — see `renewRows`. A new element is the only way a
        // `ComboBox` that has been picked from, or a `CheckBox` that has been clicked, starts
        // following its model again, and the rows around it — the box just clicked into among
        // them — have no reason to be thrown away with it.
        eachRepeater([](auto& repeater) { repeater.renew(); });
        return;
    }
    rebuildAll_ = false;
    // Emptied, so the repeaters throw their items away and build new ones — a palette swatch
    // added or taken away moves every one after it. Publishing straight afterwards leaves
    // nothing on screen for a frame.
    eachRepeater([](auto& repeater) { repeater.clear(); });
    publishSlots();
    publishFollowUps();
    publishOutputChoices();
    publishFixtureChoices();
    // Every publisher compares against an empty model, so none can find a surviving row that
    // moved. Asserting that here rather than trusting it: a rebuild that set the flag again
    // would spin at thirty frames a second, tearing every box down as fast as it drew.
    rowsDirty_ = false;
}

void RulesController::publishFiring() {
    window_->set_last_fired(shared(lastFired_));
    if (lastFiredAt_ < 0.0) {
        window_->set_last_fired_ago(slint::SharedString(""));
        return;
    }
    // **The runner's clock, because that is the clock the timestamp was taken on.** This used
    // to subtract `Fired::when` from seconds since *this controller* was built, which are two
    // clocks with no shared origin: the runner's began at Start and this one at launch, so
    // "just now" read as however long the operator had spent setting up. Both halves of a
    // subtraction have to come off one clock, and `OutputRunner::elapsed` is it.
    const double ago = runner_.elapsed() - lastFiredAt_;
    // Seconds, coarsely. A message that landed a moment ago and one that landed a minute ago
    // are different facts; a tenth of a second between them is not.
    window_->set_last_fired_ago(
        shared(ago < 1.0 ? "just now" : spellNumber(std::floor(ago)) + "s ago"));
}

} // namespace takt4::ui
