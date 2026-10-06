#include "core/settings/rule_json.hpp"

#include "core/dmx/color.hpp"
#include "core/dmx/effect.hpp"
#include "core/dmx/fixture.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/value.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace takt4::settings {
namespace {

using json = nlohmann::json;
using trigger::Generator;
using trigger::Rule;
using trigger::Value;

/// Reads a value only when it is there and is the right type. The same helper
/// `settings.cpp` has, and deliberately the same behaviour: a hand-edited file with a
/// string where a number belongs keeps the default rather than throwing.
template <typename T>
void read(const json& object, const char* key, T& out) {
    if (!object.is_object() || !object.contains(key)) {
        return;
    }
    try {
        out = object.at(key).get<T>();
    } catch (const std::exception&) {
        // Wrong type. The default stands.
    }
}

/// A named enum, through the `nameOf`/`xOf` pair every one of these types carries. An
/// unknown name keeps the default, which is what lets a file written by a later build —
/// with a generator kind this one has never heard of — still load the rest of the rule.
template <typename T, typename Parse>
void readNamed(const json& object, const char* key, T& out, Parse parse) {
    std::string name;
    read(object, key, name);
    if (name.empty()) {
        return;
    }
    if (const auto parsed = parse(name)) {
        out = *parsed;
    }
}

// --- Value ------------------------------------------------------------------------------
//
// Written as the JSON type it *is* rather than as a tagged object: 7, 0.5, "intro", true.
// A preset is a file a person may open and hand-write (Q8), and `{"kind":"int","int":7}`
// for the number seven would be the sort of format that makes them stop.

json valueToJson(const Value& value) {
    switch (value.kind()) {
    case Value::Kind::Int:
        return json(value.asInt());
    case Value::Kind::Float:
        return json(value.asFloat());
    case Value::Kind::Bool:
        return json(value.asBool());
    case Value::Kind::Text:
        return json(std::string(value.text()));
    }
    return json(0);
}

Value valueFromJson(const json& node) {
    // Integer before float, because nlohmann says yes to both for a whole number and an
    // int is what a clip index has to come back as.
    if (node.is_boolean()) {
        return Value::ofBool(node.get<bool>());
    }
    if (node.is_number_integer()) {
        return Value::ofInt(static_cast<std::int32_t>(node.get<std::int64_t>()));
    }
    if (node.is_number_float()) {
        return Value::ofFloat(node.get<float>());
    }
    if (node.is_string()) {
        return Value::ofText(node.get<std::string>());
    }
    return Value{}; // integer zero, as an unconfigured generator produces
}

// --- Generator --------------------------------------------------------------------------

json generatorToJson(const Generator::Config& config) {
    json out{
        {"kind", std::string(trigger::nameOf(config.kind))},
        {"seed", config.seed},
    };
    // Only what the kind actually reads, so a preset says what a rule does rather than
    // every field the struct happens to have. An operator opening this should be able to
    // see the shape of their own rule in it.
    if (trigger::takesPool(config.kind)) {
        out["pool"] = std::string(trigger::nameOf(config.pool));
        out["noRepeatWithin"] = config.noRepeatWithin;
        if (config.pool == trigger::Pool::List) {
            json values = json::array();
            for (const Value& value : config.values) {
                values.push_back(valueToJson(value));
            }
            out["values"] = values;
        } else {
            out["low"] = config.low;
            out["high"] = config.high;
        }
    }
    if (config.kind == trigger::GeneratorKind::Weighted) {
        json choices = json::array();
        for (const trigger::WeightedChoice& choice : config.choices) {
            choices.push_back(
                json{{"value", valueToJson(choice.value)}, {"weight", choice.weight}});
        }
        out["choices"] = choices;
    }
    if (config.kind == trigger::GeneratorKind::Fixed) {
        out["fixed"] = valueToJson(config.fixed);
    }
    if (config.kind == trigger::GeneratorKind::Live) {
        out["source"] = std::string(trigger::nameOf(config.source));
        if (config.source == trigger::LiveSource::BpmNormalised) {
            out["normaliseLow"] = config.normaliseLow;
            out["normaliseHigh"] = config.normaliseHigh;
        }
    }
    if (config.kind == trigger::GeneratorKind::Ramp) {
        out["shape"] = std::string(trigger::nameOf(config.shape));
        out["rampBars"] = config.rampBars;
        out["rampFloat"] = config.rampFloat;
        // The range a ramp sweeps, which `takesPool` does not cover — a ramp has a range and
        // no pool, so neither branch above would have written it.
        out["low"] = config.low;
        out["high"] = config.high;
    }
    return out;
}

Generator::Config generatorFromJson(const json& node) {
    Generator::Config config;
    if (!node.is_object()) {
        return config;
    }
    readNamed(node, "kind", config.kind, trigger::generatorKindOf);
    readNamed(node, "pool", config.pool, trigger::poolOf);
    read(node, "low", config.low);
    read(node, "high", config.high);
    read(node, "noRepeatWithin", config.noRepeatWithin);
    read(node, "seed", config.seed);
    readNamed(node, "source", config.source, trigger::liveSourceOf);
    read(node, "normaliseLow", config.normaliseLow);
    read(node, "normaliseHigh", config.normaliseHigh);
    readNamed(node, "shape", config.shape, trigger::rampShapeOf);
    read(node, "rampBars", config.rampBars);
    read(node, "rampFloat", config.rampFloat);

    if (node.contains("fixed")) {
        config.fixed = valueFromJson(node.at("fixed"));
    }
    if (node.contains("values") && node.at("values").is_array()) {
        for (const json& value : node.at("values")) {
            config.values.push_back(valueFromJson(value));
        }
    }
    if (node.contains("choices") && node.at("choices").is_array()) {
        for (const json& entry : node.at("choices")) {
            if (!entry.is_object() || !entry.contains("value")) {
                continue;
            }
            trigger::WeightedChoice choice;
            choice.value = valueFromJson(entry.at("value"));
            read(entry, "weight", choice.weight);
            config.choices.push_back(choice);
        }
    }
    // Not clamped here: `Generator`'s constructor does that, and doing it twice would mean
    // two places to keep in step about what "usable" is.
    return config;
}

// --- Conditions -------------------------------------------------------------------------

json conditionsToJson(const trigger::Conditions& conditions) {
    // The intensity set as the words it is offered under rather than three booleans, so a
    // hand-written preset reads `["calm","normal"]`. Written whole even when it excludes
    // nothing, because an empty array and an absent key would otherwise be the same text
    // for "everything" and "nothing".
    json intensities = json::array();
    for (const features::Intensity intensity : features::kIntensities) {
        if (conditions.allows(intensity)) {
            // The label *is* the name: one word each, so there is nothing to shorten for a
            // file and nothing to expand for a UI.
            intensities.push_back(std::string(features::labelOf(intensity)));
        }
    }
    return json{
        {"minConfidence", conditions.minConfidence},
        {"intensities", intensities},
        {"minBpm", conditions.minBpm},
        {"maxBpm", conditions.maxBpm},
        {"probability", conditions.probability},
    };
}

trigger::Conditions conditionsFromJson(const json& node) {
    trigger::Conditions conditions;
    if (!node.is_object()) {
        return conditions;
    }
    read(node, "minConfidence", conditions.minConfidence);
    read(node, "minBpm", conditions.minBpm);
    read(node, "maxBpm", conditions.maxBpm);
    read(node, "probability", conditions.probability);
    if (node.contains("intensities") && node.at("intensities").is_array()) {
        // Named ones in, everything else out. An array that is present says exactly what is
        // allowed, so an unreadable name excludes rather than includes — a rule firing on an
        // intensity the file did not name is the surprising direction.
        conditions.intensities = {false, false, false};
        for (const json& entry : node.at("intensities")) {
            if (!entry.is_string()) {
                continue;
            }
            if (const auto parsed = features::intensityOf(entry.get<std::string>())) {
                conditions.intensities[static_cast<std::size_t>(*parsed)] = true;
            }
        }
    }
    return conditions;
}

// --- DmxSend ----------------------------------------------------------------------------
//
// Only what the effect actually reads, like `generatorToJson` above and for the same reason:
// a preset should say what a rule does rather than restate every field the struct happens to
// have. A fade writes a level and a duration; it does not write a strobe's duty cycle.

json dmxToJson(const trigger::DmxSend& send) {
    json out{
        {"effect", std::string(dmx::nameOf(send.effect))},
        {"curve", std::string(dmx::nameOf(send.curve))},
        {"unit", std::string(trigger::nameOf(send.unit))},
        {"durationSeconds", send.durationSeconds},
        {"durationBeats", send.durationBeats},
    };
    json fixtures = json::array();
    for (const std::string& name : send.fixtures) {
        fixtures.push_back(name);
    }
    out["fixtures"] = std::move(fixtures);

    if (dmx::takesRole(send.effect)) {
        out["role"] = std::string(dmx::nameOf(send.role));
        out["level"] = generatorToJson(send.level);
    }
    // A laser clip: which clips, by their place in Liberation's deck order, and the intensity it
    // lands at.
    if (dmx::takesClip(send.effect)) {
        out["clip"] = generatorToJson(send.clip);
        out["level"] = generatorToJson(send.level);
    }
    if (dmx::takesBase(send.effect)) {
        out["base"] = send.base;
    }
    if (dmx::takesColor(send.effect)) {
        out["colorMode"] = std::string(trigger::nameOf(send.colorMode));
        // **Both, whichever mode is on.** A rule switched from a palette to a mix and saved
        // would otherwise come back with the palette the operator spent a minute building
        // replaced by white — the editor keeps both halves alive precisely so switching back
        // costs nothing, and a file that kept only one would throw that away on every save.
        out["color"] = generatorToJson(send.color);
        out["red"] = generatorToJson(send.red);
        out["green"] = generatorToJson(send.green);
        out["blue"] = generatorToJson(send.blue);
    }
    if (send.effect == dmx::EffectKind::HueSweep) {
        out["hueFrom"] = send.hueFrom;
        out["hueTo"] = send.hueTo;
    }
    if (dmx::takesCycles(send.effect)) {
        out["cycles"] = send.cycles;
    }
    if (send.effect == dmx::EffectKind::Strobe) {
        out["duty"] = send.duty;
    }
    if (send.effect == dmx::EffectKind::Position) {
        out["pan"] = generatorToJson(send.pan);
        out["tilt"] = generatorToJson(send.tilt);
    }
    if (send.effect == dmx::EffectKind::Path) {
        out["shape"] = std::string(dmx::nameOf(send.shape));
        out["size"] = send.size;
    }
    // Only when they name some heads, and only a spread there is: every head, together, is what
    // a rule meant before there was a choice, so a file of movement rules saves as it did. The
    // heads by their numbers, as the editor shows them.
    if (dmx::takesMovement(send.effect) && send.heads != 0) {
        json heads = json::array();
        for (int head = 0; head < 32; ++head) {
            if ((send.heads & (std::uint32_t{1} << head)) != 0) {
                heads.push_back(head + 1);
            }
        }
        out["heads"] = std::move(heads);
    }
    if (dmx::takesMovement(send.effect) && send.spread > 0.0) {
        out["spread"] = send.spread;
    }
    return out;
}

trigger::DmxSend dmxFromJson(const json& node) {
    trigger::DmxSend send;
    if (!node.is_object()) {
        return send;
    }
    readNamed(node, "effect", send.effect, dmx::effectKindOf);
    readNamed(node, "role", send.role, dmx::roleOf);
    readNamed(node, "curve", send.curve, dmx::curveOf);
    readNamed(node, "shape", send.shape, dmx::pathShapeOf);
    readNamed(node, "unit", send.unit, trigger::delayUnitOf);
    read(node, "durationSeconds", send.durationSeconds);
    read(node, "durationBeats", send.durationBeats);
    read(node, "base", send.base);
    read(node, "cycles", send.cycles);
    read(node, "duty", send.duty);
    read(node, "hueFrom", send.hueFrom);
    read(node, "hueTo", send.hueTo);
    read(node, "size", send.size);
    // Whole numbers 1 to 32 only, each read wide and checked: a hand-edited file's 1e20, -3 or
    // "2" names no head, rather than a cast that picks one at random. None left is every head.
    if (const auto heads = node.find("heads"); heads != node.end() && heads->is_array()) {
        for (const json& head : *heads) {
            if (head.is_number_integer()) {
                const auto value = head.get<std::int64_t>();
                if (value >= 1 && value <= 32) {
                    send.heads |= std::uint32_t{1} << (value - 1);
                }
            }
        }
    }
    if (const auto spread = node.find("spread"); spread != node.end() && spread->is_number()) {
        const double value = spread->get<double>();
        send.spread = std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
    }
    if (node.contains("fixtures") && node.at("fixtures").is_array()) {
        for (const json& name : node.at("fixtures")) {
            if (name.is_string()) {
                send.fixtures.push_back(name.get<std::string>());
            }
        }
    }
    if (node.contains("level")) {
        send.level = generatorFromJson(node.at("level"));
    }
    if (node.contains("clip")) {
        send.clip = generatorFromJson(node.at("clip"));
    }
    readNamed(node, "colorMode", send.colorMode, trigger::colorModeOf);
    // `colour` was the key until a rig asked for the other spelling on 2026-09-16. A preset
    // written before that holds it, and a color that does not read back is a rule that fires
    // white — which is exactly the failure this whole change was about. Read, never written.
    if (node.contains("color")) {
        send.color = generatorFromJson(node.at("color"));
    } else if (node.contains("colour")) {
        send.color = generatorFromJson(node.at("colour"));
    }
    if (node.contains("red")) {
        send.red = generatorFromJson(node.at("red"));
    }
    if (node.contains("green")) {
        send.green = generatorFromJson(node.at("green"));
    }
    if (node.contains("blue")) {
        send.blue = generatorFromJson(node.at("blue"));
    }
    if (node.contains("pan")) {
        send.pan = generatorFromJson(node.at("pan"));
    }
    if (node.contains("tilt")) {
        send.tilt = generatorFromJson(node.at("tilt"));
    }
    return send;
}

// --- Rule -------------------------------------------------------------------------------

json ruleToJson(const Rule::Config& rule) {
    json out{
        {"id", rule.id},
        {"name", rule.name},
        {"enabled", rule.enabled},
        {"trigger", std::string(trigger::nameOf(rule.trigger))},
        {"conditions", conditionsToJson(rule.conditions)},
        {"send", std::string(trigger::nameOf(rule.sendKind))},
        {"value", generatorToJson(rule.value)},
        {"sendValue", rule.sendValue},
        {"seed", rule.seed},
    };
    if (trigger::takesEvery(rule.trigger)) {
        out["every"] = rule.every;
    }
    if (trigger::takesPulses(rule.trigger)) {
        out["pulses"] = rule.pulses;
    }
    if (trigger::takesBeatOfBar(rule.trigger)) {
        out["onBeat"] = rule.onBeat;
    }
    // The wait between the trigger and the send: when it is on, and when it holds anything but
    // the defaults — kept switched off, like B's conditions, so ticking it again brings it back.
    {
        const Rule::Config fresh;
        if (rule.delayOn || rule.delayUnit != fresh.delayUnit ||
            rule.delaySeconds != fresh.delaySeconds || rule.delayBeats != fresh.delayBeats) {
            out["delay"] = json{
                {"on", rule.delayOn},
                {"unit", std::string(trigger::nameOf(rule.delayUnit))},
                {"delaySeconds", rule.delaySeconds},
                {"delayBeats", rule.delayBeats},
            };
        }
    }
    if (!rule.outputs.empty()) {
        // §5.6's rule subset. Left out when a rule goes everywhere, which is the default and
        // the majority — a preset should say what an operator chose, not restate the default
        // for every rule in it.
        json outputs = json::array();
        for (const std::string& name : rule.outputs) {
            outputs.push_back(name);
        }
        out["outputs"] = outputs;
    }
    if (rule.trigger == trigger::Trigger::TempoChange) {
        out["tempoChangeTolerance"] = rule.tempoChangeTolerance;
    }
    // B's switch, always: a file from before it existed is read by what its conditions exclude
    // (see `ruleFromJson`), so a key left out would be that guess made again on every load.
    out["conditionsOn"] = rule.conditionsOn;
    // The cooldown is the trigger's since 2026-09-30, and is written whatever the trigger —
    // ignored on a beat or a bar, but kept, so switching the rule to an onset brings it back.
    // **And where a build before then looks for it**, inside the conditions: given this file,
    // an older takt4 keeps the cooldown on the onset rules it was set for.
    out["cooldownSeconds"] = rule.cooldownSeconds;
    out["conditions"]["cooldownSeconds"] = rule.cooldownSeconds;
    // **Every half an operator has filled in, whichever kind the rule sends now** (the audit's
    // M22). `Rule::Config::dmx` promises that a rule switched to MIDI and back gets its fixtures
    // and its fade back, and the loader reads every half whatever the kind — but only the kind
    // in force was written, so a lighting rule tried as MIDI and then saved lost its whole
    // lighting setup, and a MIDI rule tried as OSC its channel and number. The halves not in
    // use are written only where they differ from a fresh rule's, so a preset still says what
    // was chosen rather than restating every default.
    const Rule::Config fresh;
    const bool osc = rule.sendKind == trigger::Message::Kind::Osc;
    const bool lighting = rule.sendKind == trigger::Message::Kind::Dmx;
    const bool midi = !osc && !lighting;
    if (osc || !rule.address.empty() || !rule.segments.empty()) {
        out["address"] = rule.address;
        json segments = json::array();
        for (const Generator::Config& segment : rule.segments) {
            segments.push_back(generatorToJson(segment));
        }
        out["segments"] = segments;
    }
    if (lighting || dmxToJson(rule.dmx) != dmxToJson(fresh.dmx)) {
        out["dmx"] = dmxToJson(rule.dmx);
    }
    if (midi || rule.channel != fresh.channel || !rule.numberChosen ||
        generatorToJson(rule.number) != generatorToJson(fresh.number)) {
        out["channel"] = rule.channel;
        // Null for a number nobody has chosen yet, so a half-built rule is still half-built
        // after a restart rather than coming back armed with a generator's default — and
        // still after a trip through another kind, which is only a question of which kind
        // reads it: pitch bend, which carries no number, never asks.
        out["number"] = rule.numberChosen ? generatorToJson(rule.number) : json(nullptr);
    }
    if (!rule.followUps.empty()) {
        json owed = json::array();
        for (const trigger::FollowUp& entry : rule.followUps) {
            json one{
                {"value", valueToJson(entry.value)},
                {"delaySeconds", entry.delaySeconds},
                // Both delays, always, so a preset switched between the two units does not
                // lose the number it is not currently using — they are different magnitudes
                // of the same idea and neither is derivable from the other.
                {"unit", std::string(trigger::nameOf(entry.unit))},
                {"delayBeats", entry.delayBeats},
            };
            // Left out for a release, which is the default and what most entries are. Its
            // absence is what says "the message that fired, with a new value" — see
            // `FollowUp::kind` — so writing a name here would be writing down a decision the
            // operator did not make.
            if (entry.kind) {
                one["send"] = std::string(trigger::nameOf(*entry.kind));
                if (trigger::sendsNumber(*entry.kind)) {
                    one["number"] = entry.number;
                }
            }
            // A DMX follow-up that brings its own effect. Left out for a release, like the
            // kind above and for the same reason: its absence is what says "the fired effect
            // again, dimmed to this level".
            if (entry.dmx) {
                one["dmxFollow"] = json{
                    {"effect", std::string(dmx::nameOf(entry.dmx->effect))},
                    {"color", dmx::formatColor(entry.dmx->color)},
                    {"curve", std::string(dmx::nameOf(entry.dmx->curve))},
                    {"unit", std::string(trigger::nameOf(entry.dmx->unit))},
                    {"durationSeconds", entry.dmx->durationSeconds},
                    {"durationBeats", entry.dmx->durationBeats},
                };
            }
            owed.push_back(std::move(one));
        }
        out["followUps"] = std::move(owed);
    }
    return out;
}

/// The one follow-up a file written before 2026-09-12 could hold, as an entry of the list
/// that replaced it.
///
/// Kept because a settings file is an operator's rig and upgrading takt4 must not empty it.
/// Read only when there is no `followUps` array, so a file written by this build is never
/// read twice. It comes back as a **release**, which is what the old fields meant: the
/// message that fired, with a different value.
void readLegacyFollowUp(const json& node, Rule::Config& rule) {
    bool had = false;
    read(node, "followUp", had);
    if (!had) {
        return;
    }
    trigger::FollowUp entry;
    if (node.contains("followUpValue")) {
        entry.value = valueFromJson(node.at("followUpValue"));
    }
    read(node, "followUpDelaySeconds", entry.delaySeconds);
    readNamed(node, "followUpUnit", entry.unit, trigger::delayUnitOf);
    read(node, "followUpDelayBeats", entry.delayBeats);
    rule.followUps.push_back(entry);
}

Rule::Config ruleFromJson(const json& node) {
    Rule::Config rule;
    read(node, "id", rule.id);
    read(node, "name", rule.name);
    read(node, "enabled", rule.enabled);
    readNamed(node, "trigger", rule.trigger, trigger::triggerOf);
    read(node, "every", rule.every);
    read(node, "pulses", rule.pulses);
    read(node, "onBeat", rule.onBeat);
    // Which beat of a bar there is: the first to the sixteenth (`trigger::kMaxBeatOfBar`).
    rule.onBeat = std::clamp<std::uint32_t>(rule.onBeat, 1, trigger::kMaxBeatOfBar);
    if (node.contains("delay") && node.at("delay").is_object()) {
        const json& delay = node.at("delay");
        read(delay, "on", rule.delayOn);
        readNamed(delay, "unit", rule.delayUnit, trigger::delayUnitOf);
        read(delay, "delaySeconds", rule.delaySeconds);
        read(delay, "delayBeats", rule.delayBeats);
        // A wait that is not a number, or is below none, is none — whoever wrote it. The engine
        // takes the longest to `trigger::kMaxFireDelaySeconds` itself.
        const Rule::Config fresh;
        if (!std::isfinite(rule.delaySeconds) || rule.delaySeconds < 0.0) {
            rule.delaySeconds = fresh.delaySeconds;
        }
        if (!std::isfinite(rule.delayBeats) || rule.delayBeats < 0.0) {
            rule.delayBeats = fresh.delayBeats;
        }
    }
    read(node, "tempoChangeTolerance", rule.tempoChangeTolerance);
    if (node.contains("outputs") && node.at("outputs").is_array()) {
        for (const json& name : node.at("outputs")) {
            if (name.is_string()) {
                rule.outputs.push_back(name.get<std::string>());
            }
        }
    }
    readNamed(node, "send", rule.sendKind, trigger::messageKindOf);
    read(node, "address", rule.address);
    read(node, "sendValue", rule.sendValue);
    read(node, "channel", rule.channel);
    read(node, "seed", rule.seed);

    if (node.contains("conditions")) {
        rule.conditions = conditionsFromJson(node.at("conditions"));
    }
    // **B's switch, and what a file from before it means** (HANDOFF §0.5, agreed 2026-09-30): a
    // rule that used any of the conditions loads with them on, so nothing fires differently
    // after the upgrade; one that used none loads with them off, as a new rule starts.
    if (node.contains("conditionsOn")) {
        read(node, "conditionsOn", rule.conditionsOn);
    } else {
        rule.conditionsOn = rule.conditions.excludesAnything();
    }
    // The cooldown where this build writes it, or where a build before 2026-09-30 did.
    if (node.contains("cooldownSeconds")) {
        read(node, "cooldownSeconds", rule.cooldownSeconds);
    } else if (node.contains("conditions")) {
        read(node.at("conditions"), "cooldownSeconds", rule.cooldownSeconds);
    }
    // A negative cooldown is no cooldown, whoever wrote it — and so is one that is not a number
    // at all, which would otherwise hold the rule silent for ever after its first fire.
    if (!std::isfinite(rule.cooldownSeconds) || rule.cooldownSeconds < 0.0) {
        rule.cooldownSeconds = 0.0;
    }
    if (node.contains("value")) {
        rule.value = generatorFromJson(node.at("value"));
    }
    if (node.contains("number")) {
        if (node.at("number").is_null()) {
            // As the editor leaves one: a fixed number with nothing in it, not the shuffle a
            // default generator would otherwise show in the box.
            rule.numberChosen = false;
            rule.number.kind = trigger::GeneratorKind::Fixed;
        } else {
            rule.number = generatorFromJson(node.at("number"));
        }
    }
    if (node.contains("dmx")) {
        rule.dmx = dmxFromJson(node.at("dmx"));
    }
    if (node.contains("followUps") && node.at("followUps").is_array()) {
        for (const json& one : node.at("followUps")) {
            if (!one.is_object() || rule.followUps.size() >= trigger::kMaxFollowUps) {
                continue;
            }
            trigger::FollowUp entry;
            if (one.contains("value")) {
                entry.value = valueFromJson(one.at("value"));
            }
            // Absent is a release, and an unreadable name is too — the alternative is
            // dropping the entry, and a rule that has lost its release is a clip that stays
            // held. See `FollowUp::kind`.
            if (one.contains("send") && one.at("send").is_string()) {
                entry.kind = trigger::messageKindOf(one.at("send").get<std::string>());
            }
            read(one, "number", entry.number);
            read(one, "delaySeconds", entry.delaySeconds);
            readNamed(one, "unit", entry.unit, trigger::delayUnitOf);
            read(one, "delayBeats", entry.delayBeats);
            if (one.contains("dmxFollow") && one.at("dmxFollow").is_object()) {
                const json& follow = one.at("dmxFollow");
                trigger::DmxFollow next;
                readNamed(follow, "effect", next.effect, dmx::effectKindOf);
                readNamed(follow, "curve", next.curve, dmx::curveOf);
                readNamed(follow, "unit", next.unit, trigger::delayUnitOf);
                read(follow, "durationSeconds", next.durationSeconds);
                read(follow, "durationBeats", next.durationBeats);
                std::string color;
                read(follow, "color", color);
                read(follow, "colour", color); // what this key was called before 2026-09-16
                if (const auto parsed = dmx::parseColor(color)) {
                    next.color = *parsed;
                }
                entry.dmx = next;
            }
            rule.followUps.push_back(entry);
        }
    } else {
        readLegacyFollowUp(node, rule);
    }
    if (node.contains("segments") && node.at("segments").is_array()) {
        for (const json& segment : node.at("segments")) {
            rule.segments.push_back(generatorFromJson(segment));
        }
    }
    // Nothing is validated here. `Rule`'s constructor decides whether this is usable and
    // `Rule::problem()` says why it is not — §5.8's policy, which is that an invalid rule is
    // held and shown rather than dropped. A preset that silently lost the rule an operator
    // is trying to fix would be the worst of both.
    return rule;
}

} // namespace

std::string rulesToJson(const std::vector<trigger::Rule::Config>& rules) {
    json out = json::array();
    for (const Rule::Config& rule : rules) {
        out.push_back(ruleToJson(rule));
    }
    // `error_handler_t::replace`, not the default `strict`. nlohmann refuses to write a
    // string that is not valid UTF-8 by *throwing* — and this is reached from
    // `settings::save`, which a Slint callback and `ui::run`'s save on the way out both
    // call, and neither can handle an exception: the process goes and the operator's
    // settings go with it. A byte nothing can decode becomes U+FFFD, which costs that byte
    // and keeps every rule in the file.
    return out.dump(2, ' ', /*ensure_ascii=*/false, json::error_handler_t::replace);
}

std::vector<trigger::Rule::Config> rulesFromJson(std::string_view text) {
    std::vector<Rule::Config> rules;
    const json document = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_array()) {
        return rules;
    }
    for (const json& node : document) {
        // Skipped rather than aborting the rest: one unreadable entry must not cost an
        // operator every other rule in the file.
        if (node.is_object()) {
            rules.push_back(ruleFromJson(node));
        }
    }
    return rules;
}

} // namespace takt4::settings
