#include "core/output/output_target.hpp"

#include <charconv>
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

} // namespace

std::uint64_t resolveOutputs(const std::vector<std::string>& names,
                             const std::vector<OutputTarget>& targets) noexcept {
    if (names.empty()) {
        return kAllOutputs;
    }
    std::uint64_t mask = 0;
    for (std::size_t i = 0; i < targets.size() && i < kMaxRoutableTargets; ++i) {
        for (const std::string& name : names) {
            if (targets[i].name == name) {
                mask |= std::uint64_t{1} << i;
                break;
            }
        }
    }
    return mask;
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
        outputs.push_back(std::move(target));
    }
    return outputs;
}

const OutputTarget* findTarget(const std::vector<OutputTarget>& targets, std::string_view name) {
    for (const OutputTarget& target : targets) {
        if (target.name == name) {
            return &target;
        }
    }
    return nullptr;
}

std::string formatOutputTarget(const OutputTarget& target) {
    std::string text;
    if (!target.enabled) {
        text += "off ";
    }
    text += target.name;
    text += " = ";
    text += formatOutputAddress(target);
    // Whole milliseconds: the slider moves in them, the output thread's round is one, and a
    // settings file full of 0.12000000000000001 helps nobody read it.
    const long long ms = std::lround(target.delaySeconds * 1000.0);
    if (ms != 0) {
        // `to_string` writes the '-' itself; only the '+' has to be added.
        text += ms > 0 ? " +" : " ";
        text += std::to_string(ms);
        text += "ms";
    }
    return text;
}

std::string formatOutputAddress(const OutputTarget& target) {
    if (target.kind == OutputTarget::Kind::Midi) {
        return "midi " + target.device;
    }
    const std::string where =
        target.host + ":" + std::to_string(static_cast<unsigned int>(target.port));
    if (target.kind != OutputTarget::Kind::ArtNet) {
        return where;
    }
    // "artnet 10.0.0.20:6454" for a node fed everything, and "artnet 10.0.0.20:6454 u0,1,4"
    // for one fed a slice. The `u` prefix is what keeps the list from being mistaken for
    // anything else on a line a person may be editing by hand.
    std::string text = "artnet " + where;
    for (std::size_t i = 0; i < target.universes.size(); ++i) {
        text += i == 0 ? " u" : ",";
        text += std::to_string(static_cast<unsigned int>(target.universes[i]));
    }
    return text;
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
    std::string_view body = line;
    const std::size_t equals = line.find('=');
    if (equals != std::string_view::npos) {
        target.name = std::string(trim(line.substr(0, equals)));
        body = trim(line.substr(equals + 1));
    }
    if (body.empty()) {
        return false;
    }

    // The delay comes off the end before anything else looks at the address, because a MIDI
    // device name runs to the end of the line and would otherwise swallow it whole.
    body = takeDelay(body, target.delaySeconds);
    if (body.empty()) {
        return false;
    }

    if (body.starts_with("artnet ") || body.starts_with("artnet\t")) {
        target.kind = OutputTarget::Kind::ArtNet;
        body = trim(body.substr(6));
        // The universe list comes off the end before the host:port split, for the same reason
        // the delay did: it would otherwise be read as part of the port.
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
                target.universes.push_back(static_cast<std::uint16_t>(universe));
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
