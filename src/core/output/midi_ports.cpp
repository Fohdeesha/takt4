#include "core/output/midi_ports.hpp"

#include <RtMidi.h>

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

} // namespace takt4::output
