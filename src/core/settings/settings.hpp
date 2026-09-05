#pragma once

#include "core/tracking/tempo_tracker.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takt4::settings {

/// HANDOFF Q7's two layers, and where they are kept.
///
/// Q7: *"Audio device and channel live in machine-local settings, never in a preset.
/// Presets carry outputs, rules and tracker tuning, and are portable by design."* The
/// split matters the moment a preset moves between machines — one that hardcodes "ASIO
/// Focusrite, channel 7" is useless on the other laptop, and one that carries no audio
/// config forces a re-setup on every load.
///
/// So the two halves are separate types even though they are written to one file today.
/// Storing them together is a storage decision — there is no preset UI yet, and a preset
/// without Phase 6's rules would be half of one — and keeping them apart in the format is
/// what lets `preset` be lifted into a file of its own later without a migration.

/// What only means anything on the machine it was saved on.
struct MachineSettings {
    /// By name, never by index: a device index moves the moment something else is plugged
    /// in, and restoring the wrong interface is worse than restoring none.
    std::string deviceName;
    std::string hostApiName;
    int channel = 0;
    /// The MIDI output port the clock goes to, by name. Empty for none. Machine-local for
    /// the same reason the interface is: it is a port on this box.
    std::string midiClockPort;

    /// §5.7's control input: the MIDI port the learn mode listens on, and what has been
    /// learned. Machine-local for the same reason again, and more strongly — "note 36 on
    /// channel 10 is tap" is a fact about the box of buttons on this desk, not about the
    /// music, so it must not travel in a preset to a machine with a different controller.
    std::string midiControlPort;
    /// Each one as `formatMidiBinding` writes it: "note 36 ch 10 -> tap". Text rather than
    /// a struct because this is a file a person may open, and Q8's headless mode is
    /// expected to hand-write one — a line that reads as a sentence can be typed, and
    /// `parseMidiBinding` drops anything that does not.
    std::vector<std::string> midiBindings;
};

/// What travels. Phase 6's rules join this.
struct Preset {
    /// §5.5's tuning — the octave-fold window, the confidence gate, the latency offset.
    tracking::TempoTracker::Options tempo;
    bool link = false;
    std::string oscPrefix = "/takt4";
    std::vector<std::pair<std::string, std::uint16_t>> oscTargets;
};

struct Settings {
    MachineSettings machine;
    Preset preset;
};

/// Where this machine keeps them: `%APPDATA%\takt4` on Windows,
/// `~/Library/Application Support/takt4` on macOS, `$XDG_CONFIG_HOME/takt4` or
/// `~/.config/takt4` elsewhere. Empty when the environment says nothing useful, which is
/// a machine where settings cannot be kept rather than an error.
std::filesystem::path settingsDirectory();

/// `settingsDirectory()/settings.json`, or empty when there is no directory.
std::filesystem::path settingsFile();

/// Reads `path`. **Never throws, and never fails**: a file that is missing, unreadable,
/// truncated or nonsense gives the defaults, because settings that cannot be parsed must
/// not be the reason an app will not open. Anything the file does not say keeps its
/// default, so a file written by an older build still loads.
Settings load(const std::filesystem::path& path);

/// Writes `path`, creating the directory if it is not there. False when it could not be
/// written — a read-only profile, a full disk — which is worth reporting once and not
/// worth stopping for.
bool save(const Settings& settings, const std::filesystem::path& path);

/// The file's text. Pretty-printed: this is a file a person may well open, and Q8's
/// headless mode is expected to hand-write one.
std::string toJson(const Settings& settings);

/// Parses what `toJson` writes. Never throws; see `load`. Values that cannot be honoured
/// — a fold window that is inverted, a channel below zero — fall back to the default
/// rather than being passed on to a tracker that trusts its caller.
Settings fromJson(std::string_view text);

} // namespace takt4::settings
