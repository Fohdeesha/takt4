#include "core/output/output_target.hpp"

#include <charconv>
#include <cstdio>
#include <random>
#include <cmath>
#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

namespace takt4::output {
namespace {

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// A port, 1 to 65535. Nothing for anything else — 0 is not a port to *send* to, whatever it
/// means to a listening socket.
bool readPort(std::string_view text, std::uint16_t& out) noexcept {
    unsigned int value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value == 0 || value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

/// Takes a trailing "+120ms" or "-300ms" off `body`, and hands back what is left.
///
/// Only ever a *suffix*, and only with the unit spelled out: a bare number on the end of a
/// line is far more likely to be part of an address somebody is halfway through typing than
/// an offset, and this parses a settings file a person may have edited by hand. The `+` is
/// optional to type and always written, because the sign is the interesting half — later or
/// earlier — and a reader should not have to notice its absence to know which.
///
/// Anything unreadable or outside `kMinOutputDelaySeconds`..`kMaxOutputDelaySeconds` leaves
/// the offset at zero and the text alone, so it fails as an address rather than silently
/// becoming one with an offset nobody asked for.
std::string_view takeDelay(std::string_view body, double& seconds) noexcept {
    if (!body.ends_with("ms") && !body.ends_with("MS")) {
        return body;
    }
    const std::string_view withoutUnit = trim(body.substr(0, body.size() - 2));
    const std::size_t space = withoutUnit.find_last_of(" \t");
    if (space == std::string_view::npos) {
        return body; // the whole line is a number; not a target with an offset on it
    }
    std::string_view number = withoutUnit.substr(space + 1);
    if (number.starts_with('+')) {
        number.remove_prefix(1);
    }
    // `from_chars` takes a leading '-' itself, so a negative offset needs nothing here.
    double value = 0.0;
    const char* const begin = number.data();
    const char* const end = begin + number.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end ||
        !(value >= kMinOutputDelaySeconds * 1000.0) ||
        !(value <= kMaxOutputDelaySeconds * 1000.0)) {
        return body;
    }
    seconds = value / 1000.0;
    return trim(withoutUnit.substr(0, space));
}

/// Whether `text` is an id of the shape `newOutputId` makes: "o-" and hex digits. Nothing
/// looser, because what it is looked for at the end of is a MIDI device's name, and a device
/// called "Launchpad #2" in a file written before ids existed must keep its "#2".
bool looksLikeId(std::string_view text) noexcept {
    if (text.size() < 3 || text.size() > 18 || !text.starts_with("o-")) {
        return false;
    }
    for (const char c : text.substr(2)) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

/// Takes a trailing " #id" off `body` into `id`, and hands back what is left. Only a whole
/// last token after whitespace, so a '#' inside a MIDI device's name is left where it is.
std::string_view takeId(std::string_view body, std::string& id) {
    const std::size_t hash = body.rfind('#');
    if (hash == std::string_view::npos || hash == 0 ||
        (body[hash - 1] != ' ' && body[hash - 1] != '\t')) {
        return body;
    }
    const std::string_view candidate = body.substr(hash + 1);
    if (!looksLikeId(candidate)) {
        return body;
    }
    id = std::string(candidate);
    return trim(body.substr(0, hash));
}

/// What an OSC target's line says when takt4's own messages are not sent to it — see
/// `OutputTarget::sendsNamespace`.
constexpr std::string_view kRulesOnly = "rules-only";

/// Takes a trailing " rules-only" off `body`, and hands back what is left. A whole last word
/// after whitespace, so a host that happens to end in it is left alone.
std::string_view takeRulesOnly(std::string_view body, bool& found) noexcept {
    if (!body.ends_with(kRulesOnly) || body.size() == kRulesOnly.size()) {
        return body;
    }
    const char before = body[body.size() - kRulesOnly.size() - 1];
    if (before != ' ' && before != '\t') {
        return body;
    }
    found = true;
    return trim(body.substr(0, body.size() - kRulesOnly.size()));
}

} // namespace

bool ensureLinkOutput(std::vector<OutputTarget>& targets, bool enabledIfAdded) {
    bool changed = false;
    std::size_t first = targets.size();
    for (std::size_t i = 0; i < targets.size();) {
        if (targets[i].kind != OutputTarget::Kind::Link) {
            ++i;
            continue;
        }
        if (first == targets.size()) {
            first = i;
            ++i;
            continue;
        }
        // A second Link: there is one session to join, so a second row could only ever be a
        // switch and a delay that fight the first.
        targets.erase(targets.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
    }
    if (first == targets.size()) {
        OutputTarget link;
        link.kind = OutputTarget::Kind::Link;
        link.name = "Link";
        link.host.clear();
        link.port = 0;
        link.enabled = enabledIfAdded;
        link.id = newOutputId(targets);
        targets.insert(targets.begin(), std::move(link));
        return true;
    }
    if (first != 0) {
        OutputTarget link = std::move(targets[first]);
        targets.erase(targets.begin() + static_cast<std::ptrdiff_t>(first));
        targets.insert(targets.begin(), std::move(link));
        changed = true;
    }
    return changed;
}

std::uint64_t resolveOutputs(const std::vector<std::string>& ids,
                             const std::vector<OutputTarget>& targets) noexcept {
    if (ids.empty()) {
        return kAllOutputs;
    }
    std::uint64_t mask = 0;
    for (std::size_t i = 0; i < targets.size() && i < kMaxRoutableTargets; ++i) {
        if (targets[i].id.empty()) {
            continue; // nothing can be routed to a target that has no id yet
        }
        for (const std::string& id : ids) {
            if (targets[i].id == id) {
                mask |= std::uint64_t{1} << i;
                break;
            }
        }
    }
    return mask;
}

std::string newOutputId(const std::vector<OutputTarget>& existing) {
    static thread_local std::mt19937_64 random{std::random_device{}()};
    for (;;) {
        char text[16] = {};
        std::snprintf(text, sizeof text, "o-%08x", static_cast<unsigned int>(random()));
        std::string id(text);
        if (findTarget(existing, id) == nullptr) {
            return id;
        }
    }
}

void ensureOutputIds(std::vector<OutputTarget>& targets) {
    for (std::size_t i = 0; i < targets.size(); ++i) {
        bool taken = targets[i].id.empty();
        for (std::size_t j = 0; j < i && !taken; ++j) {
            taken = targets[j].id == targets[i].id;
        }
        if (taken) {
            targets[i].id = newOutputId(targets);
        }
    }
}

void routeByIds(std::vector<std::string>& routing, const std::vector<OutputTarget>& targets) {
    for (std::string& entry : routing) {
        if (findTarget(targets, entry) != nullptr) {
            continue;
        }
        for (const OutputTarget& target : targets) {
            if (!target.id.empty() && target.name == entry) {
                entry = target.id;
                break;
            }
        }
    }
}

std::vector<OutputTarget>
oscOutputs(const std::vector<std::pair<std::string, std::uint16_t>>& targets) {
    std::vector<OutputTarget> outputs;
    outputs.reserve(targets.size());
    for (const auto& [host, port] : targets) {
        OutputTarget target;
        target.kind = OutputTarget::Kind::Osc;
        target.host = host;
        target.port = port;
        target.name = host + ":" + std::to_string(static_cast<unsigned int>(port));
        target.id = newOutputId(outputs);
        outputs.push_back(std::move(target));
    }
    return outputs;
}

const OutputTarget* findTarget(const std::vector<OutputTarget>& targets, std::string_view id) {
    if (id.empty()) {
        return nullptr;
    }
    for (const OutputTarget& target : targets) {
        if (target.id == id) {
            return &target;
        }
    }
    return nullptr;
}

namespace {

/// Whether `parseOutputTarget` would read `name` back as something else: a leading "off" is the
/// switch, the first '=' ends the name, the line's ends are trimmed, a quote starts a quoted name
/// and a comma separates the targets of a pasted line. "off stage" came back switched off and
/// called "stage", and "a=b" came back mangled (the audit of 2026-09-25, L28).
bool misreadUnquoted(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    const bool off = name == "off" || name.starts_with("off ") || name.starts_with("off\t");
    const bool spaced = name.front() == ' ' || name.front() == '\t' || name.back() == ' ' ||
                        name.back() == '\t';
    return off || spaced || name.find_first_of("=\",\n\\") != std::string_view::npos;
}

/// `name` in double quotes, a quote or a backslash in it written with a backslash before it.
std::string quoted(std::string_view name) {
    std::string text = "\"";
    for (const char c : name) {
        if (c == '"' || c == '\\') {
            text += '\\';
        }
        text += c;
    }
    text += '"';
    return text;
}

} // namespace

std::string formatOutputTarget(const OutputTarget& target) {
    std::string text;
    if (!target.enabled) {
        text += "off ";
    }
    // **Quoted only when it has to be**, so every name a file held before reads as it did, and
    // most still read as they were typed (the operator's call on the audit's Q5).
    text += misreadUnquoted(target.name) ? quoted(target.name) : target.name;
    text += " = ";
    text += formatOutputAddress(target);
    // Only when off, so every line a file held before reads as it did.
    if (target.kind == OutputTarget::Kind::Osc && !target.sendsNamespace) {
        text += " ";
        text += kRulesOnly;
    }
    // Whole milliseconds: the slider moves in them, the output thread's round is one, and a
    // settings file full of 0.12000000000000001 helps nobody read it.
    const long long ms = std::lround(target.delaySeconds * 1000.0);
    if (ms != 0) {
        // `to_string` writes the '-' itself; only the '+' has to be added.
        text += ms > 0 ? " +" : " ";
        text += std::to_string(ms);
        text += "ms";
    }
    if (!target.id.empty()) {
        text += " #";
        text += target.id;
    }
    return text;
}

std::string formatOutputAddress(const OutputTarget& target) {
    switch (target.kind) {
    case OutputTarget::Kind::Midi:
        return "midi " + target.device;
    case OutputTarget::Kind::MidiClock:
        return "midiclock " + target.device;
    case OutputTarget::Kind::Link:
        return "link";
    case OutputTarget::Kind::ArtNet:
        return "artnet " + target.host + ":" +
               std::to_string(static_cast<unsigned int>(target.port));
    case OutputTarget::Kind::Osc:
        break;
    }
    return target.host + ":" + std::to_string(static_cast<unsigned int>(target.port));
}

bool parseOutputTarget(std::string_view text, OutputTarget& out) noexcept try {
    std::string_view line = trim(text);
    if (line.empty()) {
        return false;
    }

    OutputTarget target;
    if (line.starts_with("off ") || line.starts_with("off\t")) {
        target.enabled = false;
        line = trim(line.substr(3));
    }

    // The name is optional. Splitting on the *first* '=' rather than the last: a MIDI device
    // called "Foo = Bar" is not a thing, and an operator who typed no name gets the address
    // as one, which is what the field used to mean before names existed.
    //
    // **Or a name in double quotes**, which is how `formatOutputTarget` writes one that would
    // otherwise be read as something else — "off stage", "a=b" (the audit of 2026-09-25, L28).
    // Everything to the closing quote, a backslash taking the character after it as it is.
    std::string_view body = line;
    if (line.starts_with('"')) {
        std::string name;
        std::size_t at = 1;
        bool closed = false;
        for (; at < line.size(); ++at) {
            if (line[at] == '\\' && at + 1 < line.size()) {
                name += line[++at];
            } else if (line[at] == '"') {
                closed = true;
                ++at;
                break;
            } else {
                name += line[at];
            }
        }
        const std::string_view rest = trim(line.substr(at));
        if (!closed || !rest.starts_with('=')) {
            return false;
        }
        target.name = std::move(name);
        body = trim(rest.substr(1));
    } else if (const std::size_t equals = line.find('='); equals != std::string_view::npos) {
        target.name = std::string(trim(line.substr(0, equals)));
        body = trim(line.substr(equals + 1));
    }
    if (body.empty()) {
        return false;
    }

    // The id and then the delay come off the end before anything else looks at the address,
    // because a MIDI device name runs to the end of the line and would otherwise swallow both.
    body = takeId(body, target.id);
    body = takeDelay(body, target.delaySeconds);
    if (body.empty()) {
        return false;
    }

    // Link has no destination: the word is the whole address.
    if (body == "link") {
        target.kind = OutputTarget::Kind::Link;
        target.host.clear();
        target.port = 0;
        if (target.name.empty()) {
            target.name = "Link";
        }
        out = std::move(target);
        return true;
    }

    if (body.starts_with("midiclock ") || body.starts_with("midiclock\t")) {
        target.kind = OutputTarget::Kind::MidiClock;
        target.device = std::string(trim(body.substr(9)));
        if (target.device.empty()) {
            return false;
        }
        if (target.name.empty()) {
            target.name = target.device;
        }
        out = std::move(target);
        return true;
    }

    if (body.starts_with("artnet ") || body.starts_with("artnet\t")) {
        target.kind = OutputTarget::Kind::ArtNet;
        body = trim(body.substr(6));
        // A universe list an older build wrote ("artnet 10.0.0.20:6454 u0,1,4") comes off the
        // end before the host:port split, as it always did, and is dropped: every node is fed
        // every universe now. Still checked, so a line that was not one stays refused.
        const std::size_t marker = body.rfind(" u");
        if (marker != std::string_view::npos) {
            std::string_view list = body.substr(marker + 2);
            body = trim(body.substr(0, marker));
            while (!list.empty()) {
                const std::size_t comma = list.find(',');
                const std::string_view field =
                    trim(comma == std::string_view::npos ? list : list.substr(0, comma));
                unsigned int universe = 0;
                const char* const begin = field.data();
                const char* const end = begin + field.size();
                const std::from_chars_result read = std::from_chars(begin, end, universe);
                if (read.ec != std::errc{} || read.ptr != end || universe > 32767) {
                    return false;
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                list = list.substr(comma + 1);
            }
        }
        const std::size_t split = body.rfind(':');
        if (split == std::string_view::npos || split == 0 || split + 1 >= body.size()) {
            return false;
        }
        const std::string_view where = trim(body.substr(0, split));
        if (where.empty() || !readPort(trim(body.substr(split + 1)), target.port)) {
            return false;
        }
        target.host = std::string(where);
        if (target.name.empty()) {
            target.name = std::string(body);
        }
        out = std::move(target);
        return true;
    }

    if (body.starts_with("midi ") || body.starts_with("midi\t")) {
        target.kind = OutputTarget::Kind::Midi;
        target.device = std::string(trim(body.substr(4)));
        if (target.device.empty()) {
            return false;
        }
        if (target.name.empty()) {
            target.name = target.device;
        }
        out = std::move(target);
        return true;
    }

    // An OSC target that takes only the rules says so after its address, before the delay and
    // the id that have already come off.
    bool rulesOnly = false;
    body = takeRulesOnly(body, rulesOnly);
    target.sendsNamespace = !rulesOnly;

    // `host:port`. rfind, so an IPv6 literal's own colons do not take the split — the port
    // is always what follows the last one.
    const std::size_t colon = body.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= body.size()) {
        return false;
    }
    const std::string_view host = trim(body.substr(0, colon));
    if (host.empty() || !readPort(trim(body.substr(colon + 1)), target.port)) {
        return false;
    }
    target.kind = OutputTarget::Kind::Osc;
    target.host = std::string(host);
    if (target.name.empty()) {
        target.name = std::string(body);
    }
    out = std::move(target);
    return true;
} catch (...) {
    // The header promises this never throws; the only thing here that can is an allocation.
    return false;
}

} // namespace takt4::output
