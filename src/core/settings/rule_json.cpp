#include "core/settings/rule_json.hpp"

#include "core/trigger/generator.hpp"
#include "core/trigger/value.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

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
        {"cooldownSeconds", conditions.cooldownSeconds},
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
    read(node, "cooldownSeconds", conditions.cooldownSeconds);
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
    if (rule.trigger == trigger::Trigger::TempoChange) {
        out["tempoChangeTolerance"] = rule.tempoChangeTolerance;
    }
    if (rule.sendKind == trigger::Message::Kind::Osc) {
        out["address"] = rule.address;
        json segments = json::array();
        for (const Generator::Config& segment : rule.segments) {
            segments.push_back(generatorToJson(segment));
        }
        out["segments"] = segments;
    } else {
        out["channel"] = rule.channel;
        out["number"] = generatorToJson(rule.number);
    }
    if (rule.followUp) {
        out["followUp"] = true;
        out["followUpValue"] = valueToJson(rule.followUpValue);
        out["followUpDelaySeconds"] = rule.followUpDelaySeconds;
    }
    return out;
}

Rule::Config ruleFromJson(const json& node) {
    Rule::Config rule;
    read(node, "id", rule.id);
    read(node, "name", rule.name);
    read(node, "enabled", rule.enabled);
    readNamed(node, "trigger", rule.trigger, trigger::triggerOf);
    read(node, "every", rule.every);
    read(node, "tempoChangeTolerance", rule.tempoChangeTolerance);
    readNamed(node, "send", rule.sendKind, trigger::messageKindOf);
    read(node, "address", rule.address);
    read(node, "sendValue", rule.sendValue);
    read(node, "channel", rule.channel);
    read(node, "followUp", rule.followUp);
    read(node, "followUpDelaySeconds", rule.followUpDelaySeconds);
    read(node, "seed", rule.seed);

    if (node.contains("conditions")) {
        rule.conditions = conditionsFromJson(node.at("conditions"));
    }
    if (node.contains("value")) {
        rule.value = generatorFromJson(node.at("value"));
    }
    if (node.contains("number")) {
        rule.number = generatorFromJson(node.at("number"));
    }
    if (node.contains("followUpValue")) {
        rule.followUpValue = valueFromJson(node.at("followUpValue"));
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
    return out.dump(2);
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
