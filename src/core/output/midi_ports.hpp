#pragma once

#include <string>
#include <vector>

namespace takt4::output {

struct MidiApiInfo {
    std::string name;        // RtMidi's short identifier: "winmm", "core", "alsa", ...
    std::string displayName; // "Windows MultiMedia", "CoreMidi", "ALSA", ...
};

/// The MIDI APIs RtMidi was compiled with.
std::vector<MidiApiInfo> compiledMidiApis();

/// Names of the MIDI output ports visible through the default API. Returns an empty list
/// when no API can be opened at all (a Linux box without an ALSA sequencer, for example);
/// that case is indistinguishable here from "no ports", which is acceptable for a listing.
std::vector<std::string> listMidiOutputPorts();

} // namespace takt4::output
