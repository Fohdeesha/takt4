#include "ui/rule_text.hpp"

#include "core/dmx/effect.hpp"
#include "core/dmx/fixture.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace takt4::ui::rule_text {

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    return text;
}

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
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::int32_t clampedInt(double value) noexcept {
    return static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0));
}

std::optional<int> typedNumber(std::string_view text) noexcept {
    const std::optional<double> number = readNumber(trim(text));
    if (!number || !std::isfinite(*number)) {
        return std::nullopt;
    }
    return static_cast<int>(std::clamp(std::round(*number), -1.0e9, 1.0e9));
}

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
        // Clamped to what a float holds, which converting past is undefined behaviour (L5).
        const double most = std::numeric_limits<float>::max();
        return trigger::Value::ofFloat(static_cast<float>(std::clamp(*number, -most, most)));
    }
    return trigger::Value::ofText(trimmed);
}

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

std::string spellWeights(const std::vector<trigger::WeightedChoice>& choices) {
    std::string text;
    for (const trigger::WeightedChoice& choice : choices) {
        if (!text.empty()) {
            text += ", ";
        }
        choice.value.appendTo(text);
        text += ":" + spellNumber(choice.weight);
    }
    return text;
}

std::string describeRate(double factor) {
    if (factor == 1.0) {
        return {};
    }
    if (factor > 1.0) {
        return spellNumber(factor) + "× slower";
    }
    return spellNumber(1.0 / factor) + "× faster";
}

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

bool targetTakes(trigger::Message::Kind kind, const output::OutputTarget& target) noexcept {
    if (trigger::isDmx(kind)) {
        return false; // the patch routes these; the editor hides the list entirely
    }
    return trigger::isMidi(kind) ? target.kind == output::OutputTarget::Kind::Midi
                                 : target.kind == output::OutputTarget::Kind::Osc;
}

std::string describeRouting(trigger::Message::Kind kind, const std::vector<std::string>& ids,
                            const std::vector<output::OutputTarget>& targets) {
    // Only what this kind can reach — the same filter the tick list uses, because a line
    // reading "every output: RDM10" over a MIDI rule names a node the rule can never send to.
    std::vector<const output::OutputTarget*> reachable;
    for (const output::OutputTarget& target : targets) {
        if (targetTakes(kind, target)) {
            reachable.push_back(&target);
        }
    }
    if (reachable.empty()) {
        return targets.empty()
                   ? "no outputs yet"
                   : std::string("no ") + (trigger::isMidi(kind) ? "MIDI" : "OSC") + " output yet";
    }
    std::string off;
    const auto noteOff = [&off](const output::OutputTarget& target) {
        off += off.empty() ? "" : ", ";
        off += target.name;
    };
    const auto switchedOff = [&off] {
        return off + (off.find(',') == std::string::npos ? " is" : " are") + " switched off";
    };
    if (ids.empty()) {
        std::string all;
        for (const output::OutputTarget* target : reachable) {
            if (!target->enabled) {
                noteOff(*target);
                continue;
            }
            all += all.empty() ? "" : ", ";
            all += target->name;
        }
        if (all.empty()) {
            return "every output: " + switchedOff();
        }
        return "every output: " + all + (off.empty() ? "" : "; " + switchedOff());
    }
    // Two ways a routed output can reach nothing, and they are different mistakes. One that
    // has been deleted is gone and can only be un-ticked. One this rig *has* on the wrong kind
    // of target — a MIDI rule still routed to an OSC feed after the send kind was changed — is
    // a routing the operator has to undo, and saying "gone" over an output plainly there would
    // read as a bug in takt4.
    std::size_t gone = 0;
    std::string wrongKind;
    std::size_t reached = 0;
    for (const std::string& id : ids) {
        const output::OutputTarget* const target = output::findTarget(targets, id);
        if (target != nullptr && targetTakes(kind, *target) && !target->enabled) {
            noteOff(*target);
        } else if (target != nullptr && targetTakes(kind, *target)) {
            ++reached;
        } else if (target != nullptr) {
            wrongKind += wrongKind.empty() ? "" : ", ";
            wrongKind += target->name;
        } else {
            ++gone;
        }
    }
    std::string trouble;
    if (gone > 0) {
        trouble = gone == 1 ? "1 output it was routed to is gone"
                            : std::to_string(gone) + " outputs it was routed to are gone";
    }
    if (!wrongKind.empty()) {
        trouble += trouble.empty() ? "" : "; ";
        trouble += wrongKind +
                   (wrongKind.find(',') == std::string::npos ? " is not " : " are not ") +
                   (trigger::isMidi(kind) ? "MIDI" : "OSC");
    }
    if (!off.empty()) {
        trouble += trouble.empty() ? "" : "; ";
        trouble += switchedOff();
    }
    if (trouble.empty()) {
        return "reaches " + std::to_string(reached) + (reached == 1 ? " output" : " outputs");
    }
    return reached == 0 ? trouble
                        : "reaches " + std::to_string(reached) +
                              (reached == 1 ? " output; " : " outputs; ") + trouble;
}

std::string releaseLabelOf(trigger::Message::Kind sent) {
    switch (sent) {
    case trigger::Message::Kind::MidiNote:
    case trigger::Message::Kind::MidiNoteOff:
        return "release (same note)";
    case trigger::Message::Kind::MidiCc:
        return "release (same cc)";
    case trigger::Message::Kind::MidiProgramChange:
        return "release (same program)";
    case trigger::Message::Kind::MidiPitchBend:
        return "release (centre)";
    case trigger::Message::Kind::Dmx:
        return "release (same fixtures)";
    case trigger::Message::Kind::Osc:
        break;
    }
    return "release (same address)";
}

std::string describeDmxRelease(const trigger::DmxSend& send) {
    if (dmx::takesMovement(send.effect)) {
        return "nothing — a move has no release, so this row sends nothing";
    }
    if (dmx::takesColor(send.effect)) {
        return "the same color at this level, same fixtures";
    }
    return std::string(dmx::labelOf(send.role)) + " to this level, same fixtures";
}

std::string describeFollowUp(const trigger::FollowUp& entry, const Rule::Config& rule) {
    if (entry.kind) {
        if (!trigger::followUpFits(*entry.kind, rule.sendKind)) {
            return "not sent — " + std::string(trigger::labelOf(*entry.kind)) + " cannot follow " +
                   std::string(trigger::labelOf(rule.sendKind));
        }
        const bool releasesTheSameThing = (rule.sendKind == trigger::Message::Kind::MidiNote &&
                                           *entry.kind == trigger::Message::Kind::MidiNoteOff) ||
                                          *entry.kind == rule.sendKind;
        // Only worth saying where the number moves. A fixed note followed by a fixed note off
        // is two spellings of the same thing and neither is wrong.
        if (releasesTheSameThing && trigger::sendsNumber(*entry.kind) &&
            rule.number.kind != trigger::GeneratorKind::Fixed) {
            return "fixed " + std::string(numberLabelOf(*entry.kind)) + " " +
                   std::to_string(entry.number) + " — the trigger sends a different one each " +
                   "time. Pick \"release\" to follow it.";
        }
        return {};
    }
    switch (rule.sendKind) {
    case trigger::Message::Kind::MidiNote:
        return "note off, same note, ch " + std::to_string(rule.channel);
    case trigger::Message::Kind::MidiNoteOff:
        return "note off again, same note";
    case trigger::Message::Kind::MidiCc:
        return "the same CC, ch " + std::to_string(rule.channel);
    case trigger::Message::Kind::MidiProgramChange:
        return "the same program — nothing to release";
    case trigger::Message::Kind::MidiPitchBend:
        return "pitch bend, ch " + std::to_string(rule.channel);
    case trigger::Message::Kind::Dmx:
        return describeDmxRelease(rule.dmx);
    case trigger::Message::Kind::Osc:
        break;
    }
    return "the same address";
}

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

} // namespace takt4::ui::rule_text
