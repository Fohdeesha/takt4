#include "core/output/output_target.hpp"

#include <charconv>
#include <cstddef>
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
    if (target.kind == OutputTarget::Kind::Midi) {
        text += "midi ";
        text += target.device;
        return text;
    }
    text += target.host;
    text += ':';
    text += std::to_string(static_cast<unsigned int>(target.port));
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
