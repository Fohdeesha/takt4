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

std::vector<std::string> listMidiOutputPorts() {
    std::vector<std::string> ports;
    try {
        RtMidiOut out;
        const unsigned int count = out.getPortCount();
        ports.reserve(count);
        for (unsigned int i = 0; i < count; ++i) {
            ports.push_back(out.getPortName(i));
        }
    } catch (const RtMidiError&) {
        // No usable MIDI API on this machine; see the header.
    }
    return ports;
}

} // namespace takt4::output
