#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
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

/// The same for input ports, which §5.7's learn mode needs. Same caveat: empty means
/// either no ports or no usable API, and a listing cannot tell those apart.
std::vector<std::string> listMidiInputPorts();

/// A port's name without the number RtMidi's Windows backend puts on the end of every one
/// ("USB MIDI 3" → "USB MIDI"). That number is the port's *place in the list*, not part of the
/// device's name: a device unplugged and plugged back in, or a machine that lists its ports in
/// another order, gives the same device another number. A name with no such number is itself.
std::string_view midiPortBaseName(std::string_view name) noexcept;

/// Which of `names` a port specification means — how an output, a MIDI clock and the control
/// input all find their port, first time and every reconnect after (the audit of 2026-09-25,
/// C1). In order:
///
/// 1. A number is an index into the list.
/// 2. The exact name.
/// 3. **The same device under another number** — `midiPortBaseName` equal. Without this a
///    device that came back at another place in the list was never found again by the name it
///    was saved under, and a saved "USB MIDI 1" would find "USB MIDI 10" by (4) instead.
/// 4. The first name that contains it, which is what a partial name typed on the command line
///    means.
///
/// Empty when nothing matches.
std::optional<std::size_t> findMidiPort(const std::vector<std::string>& names,
                                        std::string_view spec);

} // namespace takt4::output
