// `RulesController`, the edits to a rule's slots: the generator behind each number and color,
// the color picker, and the palette. The rest of the edits are in `rules_controller.cpp`,
// what is drawn from them in `rules_controller_publish.cpp`.

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
    typed(TypedIn::Slot, slot, 6);
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

void RulesController::pickSlotKind(int slot, int kind) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr || kind < 0 ||
        static_cast<std::size_t>(kind) >= trigger::kGeneratorKinds.size()) {
        return;
    }
    const GeneratorKind picked = trigger::kGeneratorKinds[static_cast<std::size_t>(kind)];
    if (config == paletteConfig() && !trigger::handsBackValues(picked)) {
        return; // not offered there (`color-generator-kinds`); nothing else may set it either
    }
    config->kind = picked;

    // **A color switched to shuffle gets a palette, not a range.**
    //
    // `Generator::Config` draws from the integers 1 to 8 by default, which is §5.8's own
    // reading and right for a clip index. On a color it is nonsense in both directions: the
    // draws are numbers `dmx::parseColor` cannot read, so every fire fell back to white, and
    // the editor drew a box saying "1 - 8" over a generator whose values are `#ff2040`.
    // Reported from a rig on 2026-09-16: *"I define a range 1-9 which maps to what!? it just
    // stays the same #ffff color code"*.
    //
    // So picking shuffle, random or cycle on the color chip seeds six colors the operator
    // can then edit, which is what picking them meant.
    // **And a DMX number switched to shuffle gets that number's range.** The same default,
    // the same nonsense: a level shuffled over 1 to 8 is a lamp between 0.4 % and 3 % of full,
    // and a pan over 1 to 8 is the extreme edge of the head's own window. Only when the range
    // is still the untouched 1-8 — a range the operator has narrowed on purpose is theirs.
    if (const auto natural = slotRange(slot);
        natural && trigger::takesPool(config->kind) && config->low == 1 && config->high == 8) {
        config->low = natural->first;
        config->high = natural->second;
    }

    if (config == paletteConfig() && trigger::takesPool(config->kind)) {
        config->pool = trigger::Pool::List;
        if (config->values.empty()) {
            std::string text;
            config->fixed.appendTo(text);
            const std::optional<dmx::Color> was = dmx::parseColor(text);
            config->values = trigger::defaultPalette();
            if (was && *was != dmx::kWhite) {
                // The color they had stays, at the front — switching to a shuffle should add
                // colors rather than replace the one already chosen.
                config->values.insert(config->values.begin(),
                                      trigger::Value::ofText(dmx::formatColor(*was)));
            }
        }
        pickedPalette_.clear();
    }
    // **A kind is not a number.** Picking "shuffle" on a MIDI number nobody had chosen marked it
    // chosen and armed the rule over the generator's default range, so it fired CC 1 to 8 at
    // 100 before a range was typed — C7 through another door (the 2026-09-25 audit's L11). The
    // number is chosen when its numbers are entered; a live value's are its source's.
    if (picked == GeneratorKind::Live) {
        choseSlot(slot);
    }
    commit();
    publishSelected();
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
    // Nor is the tick a number: a list seeded from the default range is not one anybody chose
    // (L11). Entering it is.
    commit();
}

void RulesController::setSlotRange(int slot, const std::string& text) {
    typed(TypedIn::Slot, slot, 1);
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
    config->low = clampedInt(*low);
    config->high = clampedInt(*high);
    choseSlot(slot);
    commit();
}

void RulesController::setSlotValues(int slot, const std::string& text) {
    typed(TypedIn::Slot, slot, 2);
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
    choseSlot(slot);
    commit();
}

void RulesController::setSlotNormalise(int slot, const std::string& text) {
    typed(TypedIn::Slot, slot, 4);
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    const std::vector<std::string_view> parts = split(text, "-– ");
    const std::optional<double> low = parts.size() == 2 ? readNumber(parts[0]) : std::nullopt;
    const std::optional<double> high = parts.size() == 2 ? readNumber(parts[1]) : std::nullopt;
    if (!low || !high || !(*high > *low)) {
        setStatus("A tempo range is two numbers, the lower first, like 20 - 500.", true);
        return;
    }
    config->normaliseLow = *low;
    config->normaliseHigh = *high;
    choseSlot(slot);
    commit();
}

void RulesController::setSlotWeights(int slot, const std::string& text) {
    typed(TypedIn::Slot, slot, 3);
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    std::vector<trigger::WeightedChoice> choices;
    for (const std::string_view part : split(text, ",;")) {
        const std::string_view entry = trim(part);
        if (entry.empty()) {
            continue;
        }
        trigger::WeightedChoice choice;
        const std::size_t colon = entry.rfind(':');
        if (colon != std::string_view::npos) {
            const std::optional<double> weight = readNumber(trim(entry.substr(colon + 1)));
            if (!weight || *weight < 0.0) {
                setStatus("A weight is a number of 0 or more, as in 7:3 — 7 three times as often.",
                          true);
                return;
            }
            choice.value = parseValue(trim(entry.substr(0, colon)));
            // Bounded, so a sum of them stays a number the draw can divide by (L5).
            choice.weight = std::min(*weight, 1.0e9);
        } else {
            choice.value = parseValue(entry);
        }
        choices.push_back(choice);
    }
    config->choices = std::move(choices);
    // **No repeat guard on a weighted draw.** The guard refuses the last value and draws again,
    // so with two values it alternates them and the weights mean nothing — measured: 7:3, 12:1
    // came out 201 to 199. A weighted generator is asked how *often* each value comes up, and
    // this row has no "no repeat" box to turn the guard off with, so it is off.
    config->noRepeatWithin = 0;
    choseSlot(slot);
    commit();
}

void RulesController::setSlotNoRepeat(int slot, int within) {
    typed(TypedIn::Slot, slot, 5);
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    const GeneratorKind kind = config->kind;
    config->noRepeatWithin = static_cast<std::size_t>(std::max(0, within));
    commit();
    // **Said out loud, because it has been asked twice.** A number beside the word "no
    // repeat" does not say what it guards, and what it guards is different on the two kinds
    // an operator is choosing between. On shuffle it is *not* redundant — a bag of eight
    // cannot repeat inside itself, but the last draw of one bag and the first of the next are
    // independent, so it repeats at the seam one time in eight, and that is the repeat an
    // audience sees. See `Generator::Config::noRepeatWithin`.
    if (within <= 0) {
        setStatus("Repeats allowed — including the same value twice in a row.", false);
        return;
    }
    const std::string draws = std::to_string(within) + (within == 1 ? " draw" : " draws");
    setStatus(kind == GeneratorKind::Shuffle
                  ? "Never the same as the last " + draws +
                        ". A shuffle cannot repeat inside one pass of its values; this is what "
                        "closes the join between one pass and the next."
                  : "Never the same as the last " + draws + ".",
              false);
}

void RulesController::setSlotFixed(int slot, const std::string& text) {
    typed(TypedIn::Slot, slot, 0);
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    const trigger::Value value = parseValue(text);
    // A MIDI note or controller is a number. Text here would go out as note 0 — and, typed
    // into a number nobody has chosen yet, would arm the rule on it.
    const Rule::Config* rule = current();
    if (slot == 0 && trigger::sendsNumber(rule->sendKind) &&
        value.kind() == trigger::Value::Kind::Text) {
        setStatus(std::string("A ") + numberLabelOf(rule->sendKind) + " number is a number, 0 to 127.",
                  true);
        return;
    }
    config->fixed = value;
    // Typed, so a color's sliders follow what was typed, as the palette's do (`setPaletteHex`):
    // held, they kept the saturation and brightness the picker was last left at, and a hex
    // typed under them came back on the next drag as something else (the audit of 2026-09-25,
    // L14).
    pickedColors_.erase(slot);
    choseSlot(slot);
    commit();
}

void RulesController::setSlotColor(int slot, float hue, float saturation, float brightness) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    const dmx::Color picked =
        dmx::fromHsv(static_cast<double>(hue), static_cast<double>(saturation) / 100.0,
                     static_cast<double>(brightness) / 100.0);
    config->fixed = trigger::Value::ofText(dmx::formatColor(picked));
    // The three numbers the picker moved, kept as they were moved rather than re-derived from
    // the color they produced. Dragging brightness to zero makes a black, and black has no
    // hue to read back — so a picker that re-derived would snap to red under the operator's
    // hand the moment they dimmed it, and again when they desaturated it.
    pickedColors_[slot] = Hsv{hue, saturation, brightness};
    // And out to the fixtures as it is dragged — see `previewColor`, which is the whole
    // difference between choosing a color and choosing a swatch.
    previewColor(picked);
    // The publishing `commit` does is the controller echoing a drag back at the element that
    // caused it, which must not be read as a row the operator has to have rebuilt under them.
    // See `pickingColor_`: this is the crash.
    const PickingColor picking(pickingColor_);
    commit();
}

void RulesController::setPickerOpen(bool open) {
    pickerOpen_ = open;
    // A swatch's hex box lives in its picker, and a click outside drops the picker with the box
    // in it — before the box can say it lost the focus. What was typed there is committed now.
    if (!open) {
        commitTyping();
    }
}

void RulesController::previewColor(dmx::Color color) {
    // **The color goes to the lamp while it is being chosen.**
    //
    // Asked for on 2026-09-16, and the reason is one nobody who has programmed lights will
    // argue with: `#20ff80` on a screen and `#20ff80` out of a fixture are not the same
    // color. Three LEDs, a diffuser and a wall between them, and what an operator is
    // choosing is what comes out of the *fixture* — so the picker sends it as they drag and
    // they pick against the real thing rather than against a swatch.
    //
    // The same fixtures the rule names, so the preview lands where the rule will. A rule that
    // names none is a rule that reaches nothing, which the editor already says beside the
    // fixture picker; there is nothing to preview on and nothing is sent.
    //
    // A snap, not a fade: a preview that took two bars to arrive would be a preview of
    // whatever the slider was doing two bars ago. And it is **left showing** afterwards, which
    // is what programming a light means — the next rule to fire on those fixtures takes them
    // back, and Stop's blackout clears it. PANIC does not: it freezes the lights where they
    // are, the preview with them (`DmxEngine::cancelAll`).
    const Rule::Config* rule = current();
    if (rule == nullptr || rule->sendKind != trigger::Message::Kind::Dmx) {
        return;
    }
    // And none while PANIC is engaged — the runner drops it then (the audit's M17), so the
    // editor says why the fixtures are not following the picker.
    if (runner_.panicked()) {
        setStatus("PANIC is engaged, so the preview is not sent to the lights. Press RELEASE "
                  "on the main window first.",
                  true);
        return;
    }
    const dmx::FixtureSet mask = dmx::resolveFixtures(patch_, rule->dmx.fixtures);
    if (mask.none()) {
        return;
    }
    dmx::Payload payload;
    payload.kind = dmx::EffectKind::Color;
    payload.color = color;
    payload.durationSeconds = 0.0f;
    runner_.post(output::OutputCommand::effect(mask, payload));
}

void RulesController::pickColorMode(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= trigger::kColorModes.size()) {
        return;
    }
    rule->dmx.colorMode = trigger::kColorModes[static_cast<std::size_t>(index)];
    commit();
    publishSelected();
}

std::optional<std::pair<int, int>> RulesController::slotRange(int slot) noexcept {
    // What the whole of this slot's range *is*, for the slots where that is a fact rather than
    // a preference: a DMX level is a byte and a pan is a percentage of the fixture's own
    // window. Nothing for an OSC segment or a MIDI value, where the operator's range is the
    // only one that means anything.
    Rule::Config* rule = current();
    if (rule == nullptr || rule->sendKind != trigger::Message::Kind::Dmx || slot < 0) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(slot);
    std::size_t at = 0;
    if (dmx::takesRole(rule->dmx.effect)) {
        if (index == at) {
            return std::pair{0, 255};
        }
        ++at;
    }
    if (dmx::takesColor(rule->dmx.effect)) {
        if (rule->dmx.colorMode == trigger::ColorMode::Mix) {
            if (index >= at && index < at + 3) {
                return std::pair{0, 255};
            }
            at += 3;
        } else {
            ++at; // the palette, which is a list of colors and has no range
        }
    }
    if (rule->dmx.effect == dmx::EffectKind::Position && index >= at && index < at + 2) {
        return std::pair{0, 100};
    }
    return std::nullopt;
}

trigger::Generator::Config* RulesController::paletteConfig() noexcept {
    Rule::Config* rule = current();
    if (rule == nullptr || rule->sendKind != trigger::Message::Kind::Dmx ||
        !dmx::takesColor(rule->dmx.effect) || rule->dmx.colorMode != trigger::ColorMode::Palette) {
        return nullptr;
    }
    return &rule->dmx.color;
}

void RulesController::addPaletteColor() {
    Generator::Config* config = paletteConfig();
    if (config == nullptr) {
        return;
    }
    // Adding a color is saying the color comes from a list, so the pool follows — the same
    // reasoning `setSlotValues` gives, and without it the first + on a fixed color would add
    // an entry to a list nothing draws from.
    config->pool = trigger::Pool::List;
    if (config->kind == GeneratorKind::Fixed) {
        // The color that was showing becomes the palette's first entry rather than being
        // thrown away, and the kind becomes the one an operator adding a second color means.
        config->values.clear();
        std::string text;
        config->fixed.appendTo(text);
        if (const auto parsed = dmx::parseColor(text)) {
            config->values.push_back(trigger::Value::ofText(dmx::formatColor(*parsed)));
        }
        config->kind = GeneratorKind::Shuffle;
    }
    if (config->values.empty()) {
        config->values = trigger::defaultPalette();
    } else {
        // A new swatch is white, which is unmistakably "I have not picked this yet" — a
        // duplicate of the last one would look like the + had done nothing.
        config->values.push_back(trigger::Value::ofText(dmx::formatColor(dmx::kWhite)));
    }
    commit();
    rebuildAll_ = true;
    rowsDirty_ = true;
    publishSelected();
}

void RulesController::removePaletteColor(int index) {
    Generator::Config* config = paletteConfig();
    if (config == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= config->values.size()) {
        return;
    }
    config->values.erase(config->values.begin() + index);
    rowRemoved(TypedIn::Palette, index);
    pickedPalette_.clear(); // every entry after this one has moved
    // **No picker is open now**, whichever swatch this was. REMOVE is inside the picker and
    // closes it first, and any other click would have closed an open one on its way in. The
    // swatch's own watcher says so for every swatch but the last: the last one's element goes
    // at once, the watcher with it, and the editor held every rebuild for good (the audit of
    // 2026-09-25, M20).
    pickerOpen_ = false;
    commit();
    rebuildAll_ = true;
    rowsDirty_ = true;
    publishSelected();
}

void RulesController::setPaletteColor(int index, float hue, float saturation, float brightness) {
    Generator::Config* config = paletteConfig();
    if (config == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= config->values.size()) {
        return;
    }
    const dmx::Color picked =
        dmx::fromHsv(static_cast<double>(hue), static_cast<double>(saturation) / 100.0,
                     static_cast<double>(brightness) / 100.0);
    config->values[static_cast<std::size_t>(index)] =
        trigger::Value::ofText(dmx::formatColor(picked));
    // The sliders as they were dragged, for the reason `setSlotColor` gives at length: a
    // black has no hue to read back, so re-deriving them would snap the picker to red.
    pickedPalette_[index] = Hsv{hue, saturation, brightness};
    previewColor(picked);
    // As in `setSlotColor`, and for the same reason: a row a slider is driving is a row whose
    // element must survive. See `pickingColor_`.
    const PickingColor picking(pickingColor_);
    commit();
    publishSelected();
}

void RulesController::setPaletteHex(int index, const std::string& text) {
    typed(TypedIn::Palette, index, 0);
    Generator::Config* config = paletteConfig();
    if (config == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= config->values.size()) {
        return;
    }
    const std::optional<dmx::Color> parsed = dmx::parseColor(text);
    if (!parsed) {
        setStatus("A color is #ff2040, ff2040, #f24, or 255, 32, 64.", true);
        publishSelected();
        return;
    }
    config->values[static_cast<std::size_t>(index)] =
        trigger::Value::ofText(dmx::formatColor(*parsed));
    pickedPalette_.erase(index); // typed, so the sliders should follow what was typed
    commit();
    publishSelected();
}

void RulesController::pickSlotLive(int slot, int source) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr || source < 0 ||
        static_cast<std::size_t>(source) >= trigger::kLiveSources.size()) {
        return;
    }
    config->source = trigger::kLiveSources[static_cast<std::size_t>(source)];
    choseSlot(slot);
    commit();
}

} // namespace takt4::ui
