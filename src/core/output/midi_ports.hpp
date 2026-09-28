#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::output {

/// A MIDI port that is not on this machine: nothing in the list matches what was asked for.
/// What an operator does about it is plug the device in.
class MidiPortMissing : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A MIDI port that **is** on this machine and would not open. On Windows that is nearly always
/// another program holding it — a WinMM port is one program's at a time — and saying "no such
/// device" about a device the operator can see plugged in sends them looking for the wrong fault
/// (2026-09-28: "if it can't bind ... it should say so in the UI so it doesn't just silently
/// fail").
class MidiPortBusy : public std::runtime_error {
public:
    /// `prefix` is who is saying it ("MIDI output: "), and `detail` the driver's own words, which
    /// go on the end of `what()` for a log and are left off `reason()`.
    MidiPortBusy(const std::string& prefix, const std::string& name, const std::string& detail);
    /// The sentence for a row or a line on screen: which port, and that another program probably
    /// has it — without RtMidi's "MidiOutWinMM::openPort: error creating ...", which is noise to
    /// an operator and pushed the rest of the line off the end of it.
    const std::string& reason() const noexcept { return reason_; }

private:
    std::string reason_;
};

/// What to tell an operator about a port `name` that is listed but would not open.
std::string midiPortBusyMessage(const std::string& name);

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
