#pragma once

#include "core/dmx/fixture.hpp"
#include "core/fixtures/fixture_profile.hpp"
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

/// The settings' two layers, and where they are kept.
///
/// The audio device and channel live in machine-local settings, never in a preset; presets
/// carry outputs, rules and tracker tuning, and are portable by design. The split matters the
/// moment a preset moves between machines — one that hardcodes "ASIO Focusrite, channel 7" is
/// useless on the other laptop, and one that carries no audio config forces a re-setup on
/// every load.
///
/// So the two halves are separate types even though they are written to one file. Storing
/// them together is a storage decision; keeping them apart in the format is what lets IMPORT
/// take the `preset` half of a file exported on another rig and leave this machine's devices
/// alone.

/// What only means anything on the machine it was saved on.
/// Where the beats come from, as `MachineSettings::timingSource` names it: the audio, heard by
/// the tracker, or a source followed instead of it.
inline constexpr std::string_view kTimingAudio = "audio";
inline constexpr std::string_view kTimingLink = "link";
inline constexpr std::string_view kTimingMidiClock = "midi-clock";
inline constexpr std::string_view kTimingProLink = "prolink";
/// `MachineSettings::proLinkFollow`'s "the tempo master"; otherwise a player's number.
inline constexpr std::uint8_t kProLinkFollowMaster = 0;

struct MachineSettings {
    /// By name, never by index: a device index moves the moment something else is plugged
    /// in, and restoring the wrong interface is worse than restoring none.
    std::string deviceName;
    std::string hostApiName;
    /// The input listened to, from 0 — or, in stereo, one of the pair's two: the pair is always
    /// an odd input and the even one after it as an interface numbers them (1 and 2, 11 and 12),
    /// which is how a stereo feed is patched into one.
    int channel = 0;
    /// One input rather than the pair `channel` is in — the operator's "mono" tick (2026-09-28).
    /// **Off unless asked for**: a stereo feed is heard as the average of its two sides, which is
    /// what the model was trained on, and which measured as well as the better side alone and
    /// better on downbeats (`tools/refeval/stereo.py`).
    bool mono = false;
    /// Read from a file written before there was a choice — a `channel` and no `mono` — which is
    /// taken as the stereo pair that input is in, and said so once by the window. Never written.
    bool stereoFromMono = false;
    /// Whether the file had a machine section at all. Never written. IMPORT restores this half
    /// only from a file that has one: a preset written by hand, or shared without it, must not
    /// reset this desk's input, controls and layout to a fresh install's.
    bool inFile = false;
    /// The MIDI output port the clock went to, by name, in a file written before 2026-09-25 —
    /// read so that file keeps its clock, which `load` turns into a MIDI clock output in
    /// `Preset::outputs`, and empty after that. Still written, as the first clock output's
    /// device, so an older build given the file keeps its clock too.
    std::string midiClockPort;

    /// The control input: the MIDI port the learn mode listens on, and what has been
    /// learned. Machine-local for the same reason again, and more strongly — "note 36 on
    /// channel 10 is tap" is a fact about the box of buttons on this desk, not about the
    /// music, so it must not travel in a preset to a machine with a different controller.
    std::string midiControlPort;
    /// Each one as `formatMidiBinding` writes it: "note 36 ch 10 -> tap". Text rather than
    /// a struct because this is a file a person may open, and a headless mode is expected
    /// to hand-write one — a line that reads as a sentence can be typed, and
    /// `parseMidiBinding` drops anything that does not.
    std::vector<std::string> midiBindings;

    /// The inbound OSC control: whether to listen, on which port, and whether to accept
    /// anything but loopback.
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
    /// **Remote setup** over that socket — START, STOP, the input, the source and the outputs, from
    /// a show controller (`control::OscControl::RemoteSetup`) — off unless the operator ticks it,
    /// and from the senders in `oscRemoteFrom` only, when it names any: IPv4 addresses separated
    /// by commas, as the window keeps them. Machine-local for the reason the port is.
    bool oscRemoteSetup = false;
    std::string oscRemoteFrom;

    /// **Where the beats come from** — `kTimingAudio`, or a source followed instead of the
    /// audio: `kTimingLink`, `kTimingMidiClock`, `kTimingProLink` (the CDJs). Machine-local, as the
    /// audio device is: which DJ network or mixer this desk is cabled to is a fact about the desk.
    /// The last one used is picked again at launch, and not started — START is pressed as ever. A
    /// file that does not say, or names a source this build does not know, is the audio, as a fresh
    /// install is.
    std::string timingSource{kTimingAudio};
    /// The MIDI input a MIDI clock is followed from, by name, as `midiControlPort` is.
    std::string midiClockInPort;
    /// Whether a followed MIDI clock's Stop halts the beats. **Off unless asked for**: a DJ
    /// mixer's Stop button keeps its clock running, and a stray press must not black out the rig.
    bool midiClockObeyStartStop = false;
    /// Whose beats the CDJs' are: `kProLinkFollowMaster`, or a player's number, 1 to 15 — saved as
    /// `"master"` or `"player N"`, and anything else read as the master.
    std::uint8_t proLinkFollow = kProLinkFollowMaster;
    /// The network card the CDJs are joined on, by its IPv4 address; empty for the one the first
    /// device heard is on. Only a dotted address is kept, and written plainly ("10.0.0.5").
    std::string proLinkInterface;
    /// The same card's hardware address, "74:5e:1c:00:00:02" — which finds it when its address
    /// has changed — and its name when it was picked, for the screen alone. Empty for a card
    /// picked by address alone, and cleared with the address. **Kept when the card is not on this
    /// computer**: a settings file beside a takt4 on a shared drive is every computer's that runs
    /// it from there, and the card is still on the one it was picked on.
    std::string proLinkInterfaceMac;
    std::string proLinkInterfaceName;

    /// The main window's Inputs and Outputs folded down to their headings, as they were left —
    /// the operator's call of 2026-09-29, when folding arrived. Machine-local: how much of the
    /// window a desk's screen has room for is that desk's business, not the music's. Inputs —
    /// MIDI and OSC control, which a rig sets once — **folded** in a fresh install and in a file
    /// from before there was folding (the operator, 2026-10-08); its heading still says what
    /// each is doing. Outputs open.
    bool inputsFolded = true;
    bool outputsFolded = false;

    /// The rule editor's sections folded — A when, B only if, C send, D then send — its event log
    /// open, and how tall the log's lines are, in logical pixels: remembered with the main window's
    /// folds (the rule editor's design, locked 2026-09-30). As it opens the first time: B folded,
    /// as a new rule's B starts, and the log folded to one line; open, five lines of 18.
    std::array<bool, 4> ruleSectionsFolded{false, true, false, false};
    bool ruleLogOpen = false;
    double ruleLogHeight = 90.0;
    /// The patch editor's sheets folded — A where, B channels, C how far it moves — remembered the
    /// same way (its redesign, 2026-09-30). All open the first time.
    std::array<bool, 3> patchSectionsFolded{false, false, false};

    friend bool operator==(const MachineSettings&, const MachineSettings&) = default;
};

/// The tracker's tuning as a **fresh install** has it, which is not the same as `TempoTracker`'s
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

/// The latency offset, either side of zero, as far as the window's slider goes and as far as a
/// settings file is taken at its word. Beyond half a beat the control stops meaning anything —
/// it is the next beat — and half a beat at 120 BPM is 250 ms. A file holding more, short of a
/// second, is read as this much: it used to be taken as it was, and ran that late behind a
/// slider pinned at its end, which the first touch then snapped to 250 ms. A second or more is
/// refused, as it always was.
inline constexpr double kMaxLatencyOffsetSeconds = 0.25;

/// What travels.
struct Preset {
    /// The tracker's tuning — the tempo window, the confidence gate, the latency offset.
    tracking::TempoTracker::Options tempo = freshTempoOptions();
    /// Which decoder turns the network's activations into beats: the exact forward filter,
    /// the default since it met every criterion set for replacing the particle filter
    /// (see `tracking::Decoder`), or the particle filter that shipped before it. Tracker
    /// tuning, so it travels with a preset; written as `"forward"` or `"particle"`, and
    /// anything else is the default. No control in the window yet — a settings file is
    /// the way to switch it.
    tracking::Decoder decoder = tracking::Decoder::Forward;
    /// The bar lengths the forward filter models — `ForwardFilter::Options::meters`, and
    /// its default: four alone, measured as the better decoder on electronic material and
    /// chosen by the operator for the rig on 2026-09-09.
    /// Written as `"meters": [4]`; `[3, 4]` puts the waltz back for a set that has one. Up
    /// to four bar lengths of 1 to 16, zero-filled; a file that names none keeps the
    /// default. No control in the window yet, like `decoder`.
    std::array<std::uint8_t, 4> meters{4, 0, 0, 0};
    /// Link's switch in a file written before 2026-09-25, which `load` turns into the Link
    /// output's. `outputs` holds it since; still written, as that output's switch, for an
    /// older build given the file.
    bool link = false;
    std::string oscPrefix = "/takt4";

    /// Beats to the bar while a source is followed: neither Link nor a MIDI clock carries a
    /// meter, so it is the operator's, and it is Link's quantum too. 1 to 16; a file that says
    /// anything else, or nothing, is a bar of four.
    std::uint32_t externalBeatsPerBar = 4;
    /// The latency offset while each source is followed, in seconds, as
    /// `tempo.latencyOffsetSeconds` is beat detection's: the rig's lag behind the audio is not
    /// its lag behind a Link session or a mixer's clock. Held to the same range.
    struct TimingLatency {
        double link = 0.0;
        double midiClock = 0.0;
        double proLink = 0.0;
        friend bool operator==(const TimingLatency&, const TimingLatency&) = default;
    };
    TimingLatency timingLatency;

    /// Multiple simultaneous targets, each with its own host, port, enabled state and rule
    /// subset — the named outputs a rule routes to.
    ///
    /// Portable, and the judgement is worth stating because it is not obvious. A MIDI
    /// target names a *device*, which is a fact about this box — but the routing that
    /// names it is a fact about the show, and splitting the two would mean a preset that
    /// said "send the stabs to the lighting output" and could not say what that was. So the
    /// whole target travels, and a device that is not on this machine leaves that one
    /// output unreachable and the rest of the rig working. The window says which.
    ///
    /// Link and every MIDI clock are outputs here too since 2026-09-25, with the Link output
    /// always first (`output::ensureLinkOutput`, which `load` applies).
    std::vector<output::OutputTarget> outputs;

    /// The trigger rules, which belong here rather than in `MachineSettings`: a rule is about
    /// the *show* — "a random clip on every fourth downbeat" — and travels to whatever
    /// laptop is running it. The one machine-shaped thing a rule could carry is a MIDI
    /// port, and it does not: a MIDI rule names a channel and a note, and which port they
    /// leave by is the business of the MIDI output rows it is routed to (`outputs`), which
    /// name the device.
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

    /// The fixture definitions imported into this preset (GDTF and Open Fixture Library files,
    /// converted), each with every mode of its file — what a fixture's `profile` link points at,
    /// so it can be re-moded and brought up to date by a re-import. **In the preset**, so EXPORT
    /// carries the definitions with the rig and the preset opens anywhere; an entry no fixture
    /// uses is kept until the operator removes it. Written as `"fixtureLibrary"`; a build before
    /// it ignores the key, and its fixtures still work, unlinked.
    std::vector<fixtures::FixtureProfile> library;

    friend bool operator==(const Preset&, const Preset&) = default;
};

struct Settings {
    MachineSettings machine;
    Preset preset;

    /// Every field of both, so settings that differ in anything at all differ: what the autosave
    /// asks before it builds the file's text, which costs a large rig a dozen milliseconds.
    friend bool operator==(const Settings&, const Settings&) = default;
};

/// Where settings are kept: **the directory the running executable is in** — on macOS, the one
/// `takt4.app` is in (`programFolder`).
///
/// takt4 is a program somebody copies onto a stick and carries to a venue, so its settings
/// belong beside it rather than in a profile on one machine. Two copies in two folders are
/// two rigs, which is what an operator with a rehearsal setup and a show setup wants; and a
/// machine that is not theirs keeps none of their configuration after they unplug.
///
/// Falls back to `userSettingsDirectory()` where the platform will not name the executable at
/// all, and **on macOS where that folder cannot be written**: an app downloaded and opened
/// where it landed is run by macOS from a read-only copy of itself in a random place until it
/// is moved ("App Translocation"), and a stick can be locked. The operator chose this on
/// 2026-10-08 over keeping nothing until the app is moved. Once the folder can be written, the
/// first save there takes what was kept in the profile with it — see `existingSettingsFile`.
///
/// **Unless `TAKT4_SETTINGS_DIR` names another.** The test binaries set it (tests/support/
/// crt_dialogs.cpp): they are built into the same folder as takt4.exe, and on a rig the
/// settings.json there is a real show's — a test whose click landed on SAVE would have
/// written over it. Nothing else is expected to set it.
std::filesystem::path settingsDirectory();

/// The folder a program at `executable` counts as being in: the folder holding the bundle for a
/// macOS app's executable (`…/takt4.app/Contents/MacOS/takt4` is in `…`), which is the folder the
/// operator put it in; the executable's own folder for anything else. A pure function of the
/// path, so it is tested on every system; `settingsDirectory` uses it on macOS only.
std::filesystem::path programFolder(const std::filesystem::path& executable);

/// Where settings *used* to be kept: `%APPDATA%\takt4` on Windows, `~/Library/Application
/// Support/takt4` on macOS, `$XDG_CONFIG_HOME/takt4` or `~/.config/takt4` elsewhere. Empty
/// when the environment says nothing useful, which is a machine where settings cannot be
/// kept rather than an error. Used only where the platform will not name the executable; the
/// file an older build left there is not read any more — see `existingSettingsFile`.
std::filesystem::path userSettingsDirectory();

/// `settingsDirectory()/settings.json`, or empty when there is no directory. **Where to
/// write.** Created on the first save if it is not there.
std::filesystem::path settingsFile();

/// Where takt4 writes a text it hands to another program to show — the About box's licence and
/// notices: `%TEMP%\takt4` (the temp directory's "takt4" elsewhere), or empty if there is no
/// temp directory. **In a process given `TAKT4_SETTINGS_DIR`, a folder inside that one**: a test
/// process's texts went to the same `%TEMP%\takt4` the real takt4 uses, over whatever it had open.
std::filesystem::path scratchDirectory();

/// **Where to read**: `settingsFile()`, which `load` reports as the defaults when it is not
/// there. Never the file a build before 2026-09-07 left under `userSettingsDirectory()`: that
/// was read, for the move, until 2026-10-08 — see the .cpp. **On macOS the per-user file is read
/// while nothing is beside the app**: it is where `settingsDirectory` kept the rig while the app
/// could not write beside itself.
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
    /// The format the file says it is in (`kFormatVersion`), for `Loaded`: 1 for a file that says
    /// none, as every file written before the version was read said 1.
    int version = 0;
};

/// The settings format this takt4 writes, and the newest it reads whole. A file that says a
/// later one was written by a newer takt4, with fields this one does not know and would leave
/// out of anything it saved — so it is not saved over automatically (`openAtStartup`). One more
/// whenever a field is added that an older takt4 would lose.
inline constexpr int kFormatVersion = 3;

/// `load`, saying what happened. Never throws.
Loaded loadChecked(const std::filesystem::path& path);

/// What an operator is told of the rules in `preset` a newer takt4 wrote, with names this one does
/// not know (`trigger::Rule::Config::unknown`): how many, and that they are switched off. Empty
/// when there are none.
std::string newerRulesNotice(const Preset& preset);

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
///
/// `keepBackup` leaves the backup as it is however the file reads: the last session ended before
/// it had settled, and the file it started from may be why — the copy from the start before it is
/// then the one worth keeping.
Startup openAtStartup(const std::filesystem::path& file, const std::filesystem::path& readFrom,
                      bool keepBackup = false);

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

/// The file's text. Pretty-printed: this is a file a person may well open, and a headless
/// mode is expected to hand-write one.
std::string toJson(const Settings& settings);

/// Link's switch and the MIDI clock port of a file written before 2026-09-25, as the output
/// rows that hold them since: the Link output first (switched on as the old switch was) and a
/// MIDI clock output to the old port. `fromJson` runs it on everything it reads, and so does
/// `load`; a caller building settings by hand can too. Leaves `machine.midiClockPort` empty.
void migrateTransportOutputs(Settings& settings);

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
/// fall back one by one, exactly as `fromJson` does. `version`, given, is set to the format the
/// text says it is in (`kFormatVersion`).
std::optional<Settings> parse(std::string_view text, std::string& problem, int* version = nullptr);

} // namespace takt4::settings
