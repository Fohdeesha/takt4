#include "core/output/midi_ports.hpp"

#include <RtMidi.h>

#include <algorithm>

namespace takt4::output {

std::vector<MidiApiInfo> compiledMidiApis() {
    std::vector<RtMidi::Api> apis;
    RtMidi::getCompiledApi(apis);

    std::vector<MidiApiInfo> result;
    result.reserve(apis.size());
    for (const RtMidi::Api api : apis) {
        result.push_back({RtMidi::getApiName(api), RtMidi::getApiDisplayName(api)});
    }
    return result;
}

namespace {

/// Both listings are the same four lines over a different RtMidi class, and both have to
/// swallow the constructor throwing — RtMidi reports "no usable MIDI API at all" that way
/// rather than by offering an empty list, and CI runs on exactly such a box.
template <typename Midi>
std::vector<std::string> listPorts() {
    std::vector<std::string> ports;
    try {
        Midi midi;
        const unsigned int count = midi.getPortCount();
        ports.reserve(count);
        for (unsigned int i = 0; i < count; ++i) {
            ports.push_back(midi.getPortName(i));
        }
    } catch (const RtMidiError&) {
        // No usable MIDI API on this machine; see the header.
    }
    return ports;
}

} // namespace

std::vector<std::string> listMidiOutputPorts() {
    return listPorts<RtMidiOut>();
}

std::vector<std::string> listMidiInputPorts() {
    return listPorts<RtMidiIn>();
}

std::string midiPortBusyMessage(const std::string& name) {
    return "\"" + name + "\" is on this machine but would not open \xE2\x80\x94 another program " +
           "is probably using it";
}

MidiPortBusy::MidiPortBusy(const std::string& prefix, const std::string& name,
                           const std::string& detail)
    : std::runtime_error(prefix + midiPortBusyMessage(name) + " (" + detail + ")"),
      reason_(midiPortBusyMessage(name)) {}

std::string_view midiPortBaseName(std::string_view name) noexcept {
    std::size_t end = name.size();
    while (end > 0 && name[end - 1] >= '0' && name[end - 1] <= '9') {
        --end;
    }
    // Only a number set off by a space, after something: "Port 2" is "Port", and "808" is "808".
    if (end == name.size() || end < 2 || name[end - 1] != ' ') {
        return name;
    }
    return name.substr(0, end - 1);
}

std::optional<std::size_t> findMidiPort(const std::vector<std::string>& names,
                                        std::string_view spec) {
    if (spec.empty()) {
        return std::nullopt;
    }
    const bool numeric = std::all_of(spec.begin(), spec.end(),
                                     [](char c) { return c >= '0' && c <= '9'; });
    if (numeric) {
        // A number too long for an index is no index at all.
        if (spec.size() > 9) {
            return std::nullopt;
        }
        std::size_t index = 0;
        for (const char c : spec) {
            index = index * 10 + static_cast<std::size_t>(c - '0');
        }
        return index < names.size() ? std::optional<std::size_t>(index) : std::nullopt;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == spec) {
            return i;
        }
    }
    const std::string_view base = midiPortBaseName(spec);
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (midiPortBaseName(names[i]) == base) {
            return i;
        }
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i].find(spec) != std::string::npos) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace takt4::output
