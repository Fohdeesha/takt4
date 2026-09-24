#pragma once

#include "core/dmx/fixture.hpp"
#include "core/output/output_target.hpp"
#include "core/tracking/beat_decoder.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "core/trigger/rule.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
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
/// So the two halves are separate types even though they are written to one file. Storing
/// them together is a storage decision; keeping them apart in the format is what lets IMPORT
/// take the `preset` half of a file exported on another rig and leave this machine's devices
/// alone.

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

    /// §5.7's inbound OSC: whether to listen, on which port, and whether to accept anything
    /// but loopback.
    ///
    /// **Machine-local, and more strongly than the ports above.** A listening socket is a
    /// fact about this box and its network — a preset carrying "listen on 0.0.0.0:7001" to
    /// somebody else's laptop would open a port they never asked for.
    ///
    /// Off by default, and `oscControlLocalOnly` true by default, because OSC has no
    /// authentication and takt4 invents none. Turning either on is the operator's call, and
    /// it is the same call they make when they put a Stream Deck on that network.
    bool oscControlEnabled = false;
    std::uint16_t oscControlPort = 7001;
    bool oscControlLocalOnly = true;
};

/// §5.5's tuning as a **fresh install** has it, which is not the same as `TempoTracker`'s
/// own defaults — one field differs, and only one.
///
/// `TempoTracker::Options::octaveFold` defaults to true, because a tracker asked to publish a
/// tempo into a window should do it: that default belongs to the class, and `takt4-cli`'s
/// documented 70-140 default and every evaluation run depend on it.
///
/// **The application ships with it off.** The fold is a promise about the material, and takt4
/// is handed a set — one record after another, not one track being tuned for. A window that
/// suited the last track silently halves or doubles the next one, and the operator's first
/// experience of the app is a tempo that is wrong by a factor of two for reasons nothing on
/// screen explains. Reported from a rig on 2026-09-08, having been switched on by a tap:
/// *"octave fold keeps getting automatically turned on, which then breaks the next track in
/// the mix"*. Off, the tracker publishes what it hears; the window is there for an operator
/// who knows what is coming and says so.
///
/// This is only the value a settings file that does not mention the field starts from. Once
/// somebody has switched it on, `save` writes it and it stays on.
tracking::TempoTracker::Options freshTempoOptions() noexcept;

/// What travels.
struct Preset {
    /// §5.5's tuning — the tempo window, the confidence gate, the latency offset.
    tracking::TempoTracker::Options tempo = freshTempoOptions();
    /// Which decoder turns the network's activations into beats: the exact forward filter
    /// of TRACKING-PROPOSAL.md §2.4, which is the default since it met §5's exit criteria
    /// (see `tracking::Decoder`), or the particle filter that shipped before it. Tracker
    /// tuning, so it travels with a preset; written as `"forward"` or `"particle"`, and
    /// anything else is the default. No control in the window yet — a settings file is
    /// the way to switch it.
    tracking::Decoder decoder = tracking::Decoder::Forward;
    /// The bar lengths the forward filter models — `ForwardFilter::Options::meters`, and
    /// its default: four alone, which TRACKING-PROPOSAL.md §7.7 measured as the better
    /// decoder on electronic material and the operator chose for the rig on 2026-09-09.
    /// Written as `"meters": [4]`; `[3, 4]` puts the waltz back for a set that has one. Up
    /// to four bar lengths of 1 to 16, zero-filled; a file that names none keeps the
    /// default. No control in the window yet, like `decoder`.
    std::array<std::uint8_t, 4> meters{4, 0, 0, 0};
    bool link = false;
    std::string oscPrefix = "/takt4";

    /// §5.6's *"multiple simultaneous targets, each with its own host, port, enabled state
    /// and rule subset"* — the named outputs a rule routes to.
    ///
    /// Portable, and the judgement is worth stating because it is not obvious. A MIDI
    /// target names a *device*, which is a fact about this box — but the routing that
    /// names it is a fact about the show, and splitting the two would mean a preset that
    /// said "send the stabs to the lighting output" and could not say what that was. So the
    /// whole target travels, and a device that is not on this machine leaves that one
    /// output unreachable and the rest of the rig working. §5.9 says which.
    std::vector<output::OutputTarget> outputs;

    /// §5.8's rules, which Q7 puts here rather than in `MachineSettings`: a rule is about
    /// the *show* — "a random clip on every fourth downbeat" — and travels to whatever
    /// laptop is running it. The one machine-shaped thing a rule could carry is a MIDI
    /// port, and it does not: a MIDI rule names a channel and a note, and which port they
    /// leave by is `midiClockPort`'s business.
    ///
    /// Held as configuration rather than as live `trigger::Rule`s, which is what makes them
    /// storable at all — see `Rule::Config`, which is plain data for exactly this.
    std::vector<trigger::Rule::Config> rules;

    /// The lighting patch — what fixtures there are, where they are addressed and what each
    /// of their channels does.
    ///
    /// **Portable, and the judgement is the same one the outputs make.** A patch is a fact
    /// about a *rig* rather than about a machine: the same three moving heads at the same
    /// addresses are the same three heads whichever laptop is driving them, and a rule that
    /// says "the heads" is meaningless without the patch that says what those are. Carrying
    /// the two together is what makes a preset a show somebody can hand over.
    ///
    /// A patch opened on a rig that has not got those fixtures costs nothing: the universes
    /// are still built and the frames are still sent, into a node that is not there.
    std::vector<dmx::Fixture> fixtures;
};

struct Settings {
    MachineSettings machine;
    Preset preset;
};

/// Where settings are kept: **the directory the running executable is in**.
///
/// takt4 is a program somebody copies onto a stick and carries to a venue, so its settings
/// belong beside it rather than in a profile on one machine. Two copies in two folders are
/// two rigs, which is what an operator with a rehearsal setup and a show setup wants; and a
/// machine that is not theirs keeps none of their configuration after they unplug.
///
/// Falls back to `userSettingsDirectory()` only where the platform will not name the
/// executable at all.
///
/// **Unless `TAKT4_SETTINGS_DIR` names another.** The test binaries set it (tests/support/
/// crt_dialogs.cpp): they are built into the same folder as takt4.exe, and on a rig the
/// settings.json there is a real show's — a test whose click landed on SAVE would have
/// written over it. Nothing else is expected to set it.
std::filesystem::path settingsDirectory();

/// Where settings *used* to be kept: `%APPDATA%\takt4` on Windows, `~/Library/Application
/// Support/takt4` on macOS, `$XDG_CONFIG_HOME/takt4` or `~/.config/takt4` elsewhere. Empty
/// when the environment says nothing useful, which is a machine where settings cannot be
/// kept rather than an error. Still read once — see `existingSettingsFile`.
std::filesystem::path userSettingsDirectory();

/// `settingsDirectory()/settings.json`, or empty when there is no directory. **Where to
/// write.** Created on the first save if it is not there.
std::filesystem::path settingsFile();

/// **Where to read.** `settingsFile()` when that exists; otherwise the file under
/// `userSettingsDirectory()` if a build before this one left one there, so a rig keeps its
/// outputs, its device and its MIDI bindings across the move; otherwise `settingsFile()`
/// again, which `load` will report as the defaults. With `TAKT4_SETTINGS_DIR` set there is
/// no looking in the profile: a process given a folder of its own reads nobody else's.
std::filesystem::path existingSettingsFile();

/// Reads `path`. **Never throws, and never fails**: a file that is missing, unreadable,
/// truncated or nonsense gives the defaults, because settings that cannot be parsed must
/// not be the reason an app will not open. Anything the file does not say keeps its
/// default, so a file written by an older build still loads.
///
/// **Which is also why it must not be the only way in.** A file that did not parse and a file
/// that was not there both come back as the defaults, and the exit save then wrote those
/// defaults over the one that was merely damaged — a trailing comma from a hand edit was
/// enough to lose a whole rig. `loadChecked` says which it was; startup goes through
/// `openAtStartup`, which acts on the answer.
Settings load(const std::filesystem::path& path);

/// What `loadChecked` found at a path.
enum class LoadStatus : std::uint8_t {
    /// Nothing there: a fresh install. The defaults, and nothing to report.
    Missing,
    /// Read and understood.
    Loaded,
    /// There, and not settings — truncated, empty, hand-edited into a syntax error, or not
    /// JSON at all.
    Corrupt,
    /// There, and could not be opened: another program holds it, or it is not permitted.
    Unreadable,
};

struct Loaded {
    /// The defaults unless `status` is `Loaded`.
    Settings settings;
    LoadStatus status = LoadStatus::Missing;
    /// What was wrong, in a few words, for `Corrupt` and `Unreadable` — "line 12, column 3:
    /// unexpected '}'", "it is empty", "permission denied". Empty otherwise.
    std::string problem;
};

/// `load`, saying what happened. Never throws.
Loaded loadChecked(const std::filesystem::path& path);

/// Where the copy of the settings file taken at each good startup is kept:
/// `settings.json.bak` beside it. See `openAtStartup`.
std::filesystem::path backupFile(const std::filesystem::path& file);

/// What `ui::run` starts from, and anything the operator has to be told about how it got it.
struct Startup {
    Settings settings;
    /// One or two sentences for the status line when the file could not be used as it was;
    /// empty when it was read, or when there was none.
    std::string notice;
    /// False only when the file that is there could neither be read nor moved out of the way —
    /// the one case in which an automatic save would destroy something. Nothing is written
    /// over it automatically then.
    bool writable = true;
};

/// Reads the settings for a launch, and **never lets a damaged file be lost**.
///
/// `file` is where settings are written (`settingsFile()`); `readFrom` is where they are
/// read this time (`existingSettingsFile()`), which differs only on the first run after the
/// file moved beside the executable.
///
///   * **Read** — the settings, and a copy of the file as it stands goes to `backupFile(file)`:
///     the rig as it was when takt4 last started cleanly, which is what somebody wants back
///     after an evening's edits went wrong.
///   * **Damaged** — the file is renamed to `settings.json.corrupt-<date>-<time>` beside itself,
///     never overwritten, and the backup is loaded instead if there is one that reads. The
///     notice names both.
///   * **Could not be opened** — the defaults, a notice, and `writable` false, since a save
///     would replace a file that may be perfectly good and merely locked.
Startup openAtStartup(const std::filesystem::path& file, const std::filesystem::path& readFrom);

/// Writes `path`, creating the directory if it is not there — **atomically**: see
/// `io::replaceFile`. The file on disk is always a whole settings file, the old one or the new
/// one, whatever happens part-way through; it used to be truncated and rewritten in place,
/// and a crash or a full disk mid-save left an empty file where the rig had been.
///
/// False when it could not be written — a read-only folder, a full disk — which is worth
/// reporting once and not worth stopping for. Never throws.
bool save(const Settings& settings, const std::filesystem::path& path);

/// `save` for text `toJson` has already produced — what an autosave that compared the text
/// against the last save has in its hand. Never throws.
bool saveText(std::string_view json, const std::filesystem::path& path);

/// The file's text. Pretty-printed: this is a file a person may well open, and Q8's
/// headless mode is expected to hand-write one.
std::string toJson(const Settings& settings);

/// Gives every output and every fixture an id, and points every rule at them by id rather
/// than by name. `fromJson` runs it on everything it reads, so a file written before ids
/// existed loads routed exactly as it was; anything that builds a preset in code runs it
/// before handing the preset over. See `output::OutputTarget::id`.
void assignIds(Preset& preset);

/// Parses what `toJson` writes. Never throws; see `load`. Values that cannot be honoured
/// — a fold window that is inverted, a channel below zero, an OSC prefix that is not an
/// address — fall back to the default rather than being passed on to code that trusts its
/// caller.
Settings fromJson(std::string_view text);

/// `fromJson`, or nothing with `problem` saying why the text is not a settings file at all.
/// A file that parses and merely holds odd values is still a settings file: those values
/// fall back one by one, exactly as `fromJson` does.
std::optional<Settings> parse(std::string_view text, std::string& problem);

} // namespace takt4::settings
