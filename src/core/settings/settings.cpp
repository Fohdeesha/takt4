#include "core/settings/settings.hpp"

#include "core/fixtures/fixture_library.hpp"
#include "core/fixtures/profile_json.hpp"
#include "core/fixtures/profile_json_node.hpp"
#include "core/io/atomic_file.hpp"
#include "core/io/json_depth.hpp"
#include "core/io/utf8.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/settings/rule_json.hpp"
#include "core/settings/rule_json_node.hpp"
#include "core/timing/prolink/packets.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

namespace takt4::settings {

tracking::TempoTracker::Options freshTempoOptions() noexcept {
    tracking::TempoTracker::Options tempo;
    // The one field a fresh install disagrees with the tracker about. See the header for
    // why the application ships with the tempo window off and the tracker does not.
    tempo.octaveFold = false;
    return tempo;
}

namespace {

using nlohmann::json;

/// The format a document says it is in: 1 when it says none — every file written before the
/// version was read said 1, or nothing — and 1 for anything that is not a whole number.
int formatOf(const json& document) {
    const auto found = document.find("version");
    return found != document.end() && found->is_number_integer() ? found->get<int>() : 1;
}

/// Wide enough for anything the tracker can be asked to follow, and narrow enough that a
/// nonsense file cannot put the fold window somewhere the filter can never reach.
constexpr double kBpmFloor = 20.0;
constexpr double kBpmCeiling = 400.0;

std::string environmentVariable(const char* name) {
#if defined(_WIN32)
    // Not std::getenv: MSVC deprecates it, and the buffer it returns is not ours.
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    const std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

/// The folder `TAKT4_SETTINGS_DIR` names, or empty. Read wide on Windows, since it is a
/// path: a temp folder under a user name outside the ANSI code page would not survive the
/// narrow read `environmentVariable` makes.
std::filesystem::path namedSettingsDirectory() {
#if defined(_WIN32)
    wchar_t* value = nullptr;
    std::size_t size = 0;
    if (_wdupenv_s(&value, &size, L"TAKT4_SETTINGS_DIR") != 0 || value == nullptr) {
        return {};
    }
    std::filesystem::path named(value);
    std::free(value);
    return named;
#else
    const std::string named = environmentVariable("TAKT4_SETTINGS_DIR");
    return named.empty() ? std::filesystem::path{} : std::filesystem::path(named);
#endif
}

/// This program's own file, or empty when the platform will not say.
///
/// Not `argv[0]`, which is whatever the caller felt like passing and is a bare name when the
/// program was found on PATH. Every platform has a real answer and this asks for it.
std::filesystem::path executablePath() {
#if defined(_WIN32)
    // A path can be longer than MAX_PATH, and `GetModuleFileNameW` says so only by filling
    // the buffer exactly and setting ERROR_INSUFFICIENT_BUFFER, so it is asked in a loop
    // rather than once with a number somebody guessed.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path(buffer);
        }
        if (buffer.size() >= 32768) {
            return {}; // past the longest path Windows has
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // fails, and fills in the size it wants
    std::string buffer(size, '\0');
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    buffer.resize(std::strlen(buffer.c_str()));
    std::error_code code;
    // Through the symlink: an app run from a bin/ symlink keeps its settings with the real
    // program, not with the link.
    const std::filesystem::path resolved = std::filesystem::canonical(buffer, code);
    return code ? std::filesystem::path(buffer) : resolved;
#else
    std::error_code code;
    const std::filesystem::path resolved = std::filesystem::read_symlink("/proc/self/exe", code);
    return code ? std::filesystem::path{} : resolved;
#endif
}

/// Reads a value only when it is there and is the right type. A hand-edited file with a
/// string where a number belongs keeps the default rather than throwing.
template <typename T>
void read(const json& object, const char* key, T& out) {
    if (!object.is_object() || !object.contains(key)) {
        return;
    }
    try {
        out = object.at(key).get<T>();
    } catch (const std::exception&) {
        // Wrong type. The default stands.
    }
}

const char* decoderName(tracking::Decoder decoder) noexcept {
    return decoder == tracking::Decoder::Forward ? "forward" : "particle";
}

tracking::Decoder decoderFromName(const std::string& name) noexcept {
    return name == "particle" ? tracking::Decoder::ParticleFilter : tracking::Decoder::Forward;
}

json metersToJson(const std::array<std::uint8_t, 4>& meters) {
    json out = json::array();
    for (const std::uint8_t meter : meters) {
        if (meter != 0) {
            out.push_back(static_cast<int>(meter));
        }
    }
    return out;
}

/// `[4]`, `[3, 4]`: up to four bar lengths of 1 to 16. Anything else — not an array, an
/// empty one, a value out of range — leaves the default standing, since a decoder with no
/// bar at all cannot be built and a file is not a caller worth trusting.
void readMeters(const json& preset, std::array<std::uint8_t, 4>& meters) {
    if (!preset.is_object() || !preset.contains("meters") || !preset.at("meters").is_array()) {
        return;
    }
    std::array<std::uint8_t, 4> out{0, 0, 0, 0};
    std::size_t count = 0;
    for (const json& entry : preset.at("meters")) {
        if (!entry.is_number_integer()) {
            return;
        }
        const std::int64_t value = entry.get<std::int64_t>();
        if (value < 1 || value > 16 || count >= out.size()) {
            return;
        }
        out[count++] = static_cast<std::uint8_t>(value);
    }
    if (count > 0) {
        meters = out;
    }
}

/// A latency offset as a file gives it: refused when a second or more either way — no offset
/// anybody set — and held to the slider's ends short of that, as beat detection's is.
double latencyFrom(double seconds) noexcept {
    if (!(seconds > -1.0) || !(seconds < 1.0)) {
        return 0.0;
    }
    return std::clamp(seconds, -kMaxLatencyOffsetSeconds, kMaxLatencyOffsetSeconds);
}

/// "master", or "player N" for a player 1 to 15.
std::string proLinkFollowText(std::uint8_t follow) {
    return follow >= 1 && follow <= 15 ? "player " + std::to_string(follow) : "master";
}

/// The other way; anything that is not a player 1 to 15 is the master.
std::uint8_t proLinkFollowFrom(const std::string& text) {
    constexpr std::string_view kPlayer = "player ";
    if (text.size() <= kPlayer.size() || text.compare(0, kPlayer.size(), kPlayer) != 0) {
        return kProLinkFollowMaster;
    }
    const std::string number = text.substr(kPlayer.size());
    if (number.size() > 2 ||
        !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return kProLinkFollowMaster;
    }
    const int player = std::stoi(number);
    return player >= 1 && player <= 15 ? static_cast<std::uint8_t>(player) : kProLinkFollowMaster;
}

/// Four numbers 0 to 255 between three dots, and nothing else: an IPv4 address as typed.
bool isDottedAddress(const std::string& text) {
    int parts = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t dot = text.find('.', at);
        const std::string part =
            text.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
        if (part.empty() || part.size() > 3 ||
            !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            std::stoi(part) > 255) {
            return false;
        }
        ++parts;
        if (dot == std::string::npos) {
            break;
        }
        at = dot + 1;
    }
    return parts == 4;
}

/// `text` as `isDottedAddress` takes it, written plainly — "10.0.0.5" for "010.000.000.005", which
/// one system's parser reads and another's refuses — or empty for anything else.
std::string plainAddress(const std::string& text) {
    if (!isDottedAddress(text)) {
        return {};
    }
    std::string plain;
    std::size_t at = 0;
    for (int part = 0; part < 4; ++part) {
        const std::size_t dot = text.find('.', at);
        plain += std::to_string(std::stoi(text.substr(at, dot - at)));
        plain += part < 3 ? "." : "";
        at = dot + 1;
    }
    return plain;
}

/// The preset's half of the timing input: beats to the bar and each source's latency.
json timingToJson(const Preset& preset) {
    return json{
        {"beatsPerBar", preset.externalBeatsPerBar},
        {"latencySeconds", json{{"link", preset.timingLatency.link},
                                {"midiClock", preset.timingLatency.midiClock},
                                {"proLink", preset.timingLatency.proLink}}},
    };
}

void timingFromJson(const json& object, Preset& preset) {
    if (!object.is_object()) {
        return;
    }
    // A whole number of beats, 1 to 16 — the bar indicator's own range; a fraction, a string or a
    // bar of 99 is no meter and leaves four.
    if (object.contains("beatsPerBar") && object.at("beatsPerBar").is_number_integer()) {
        const std::int64_t beats = object.at("beatsPerBar").get<std::int64_t>();
        if (beats >= 1 && beats <= 16) {
            preset.externalBeatsPerBar = static_cast<std::uint32_t>(beats);
        }
    }
    if (object.contains("latencySeconds") && object.at("latencySeconds").is_object()) {
        const json& latency = object.at("latencySeconds");
        read(latency, "link", preset.timingLatency.link);
        read(latency, "midiClock", preset.timingLatency.midiClock);
        read(latency, "proLink", preset.timingLatency.proLink);
        preset.timingLatency.link = latencyFrom(preset.timingLatency.link);
        preset.timingLatency.midiClock = latencyFrom(preset.timingLatency.midiClock);
        preset.timingLatency.proLink = latencyFrom(preset.timingLatency.proLink);
    }
}

json tempoToJson(const tracking::TempoTracker::Options& tempo) {
    return json{
        {"minBpm", tempo.minBpm},
        {"maxBpm", tempo.maxBpm},
        {"octaveFold", tempo.octaveFold},
        {"confidenceThreshold", tempo.confidenceThreshold},
        {"latencyOffsetSeconds", tempo.latencyOffsetSeconds},
        {"keepOctaveShift", tempo.keepOctaveShift},
    };
}

tracking::TempoTracker::Options tempoFromJson(const json& object) {
    // Started from the defaults, so a field the file does not mention keeps the value a fresh
    // install has rather than a zero. `freshTempoOptions` rather than the tracker's own,
    // because the two differ in exactly one field — see its header.
    tracking::TempoTracker::Options tempo = freshTempoOptions();
    read(object, "minBpm", tempo.minBpm);
    read(object, "maxBpm", tempo.maxBpm);
    read(object, "octaveFold", tempo.octaveFold);
    read(object, "confidenceThreshold", tempo.confidenceThreshold);
    read(object, "latencyOffsetSeconds", tempo.latencyOffsetSeconds);
    read(object, "keepOctaveShift", tempo.keepOctaveShift);

    // `TempoTracker::setOptions` is noexcept and trusts its caller, and `foldInto` returns
    // the tempo *unfolded* when the window is inverted — which reads as the fold quietly
    // not working. A file is not a caller worth trusting, so an impossible window is
    // refused outright rather than half-applied.
    const tracking::TempoTracker::Options defaults;
    if (!(tempo.minBpm >= kBpmFloor) || !(tempo.maxBpm <= kBpmCeiling) ||
        !(tempo.maxBpm > tempo.minBpm)) {
        tempo.minBpm = defaults.minBpm;
        tempo.maxBpm = defaults.maxBpm;
    }
    if (!(tempo.confidenceThreshold >= 0.0) || !(tempo.confidenceThreshold <= 1.0)) {
        tempo.confidenceThreshold = defaults.confidenceThreshold;
    }
    // Beyond half a beat the offset is the next beat — `kMaxLatencyOffsetSeconds`, where the
    // slider ends. A second or more either way is not an offset anybody set and is refused, as
    // any setting a tracker could not honour is; short of that, past the end is the end.
    if (!(tempo.latencyOffsetSeconds > -1.0) || !(tempo.latencyOffsetSeconds < 1.0)) {
        tempo.latencyOffsetSeconds = defaults.latencyOffsetSeconds;
    }
    tempo.latencyOffsetSeconds = std::clamp(tempo.latencyOffsetSeconds, -kMaxLatencyOffsetSeconds,
                                            kMaxLatencyOffsetSeconds);
    return tempo;
}

/// nlohmann's message for a parse failure, as an operator would want it on a status line:
/// "line 12, column 3: syntax error while parsing object - unexpected '}'", without the
/// library's `[json.exception.parse_error.101] parse error at ` in front of it.
std::string describeParseError(std::string_view text, const std::exception& error) {
    // The two a crash or an unlucky editor actually leave, said plainly rather than as the
    // parser's account of reading nothing.
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        return "it is empty";
    }
    std::string message = error.what();
    for (const std::string_view drop : {std::string_view("] "), std::string_view("parse error at ")}) {
        const std::size_t at = message.find(drop);
        if (at != std::string::npos) {
            message.erase(0, at + drop.size());
        }
    }
    // An error that quotes the file can quote bytes that are not UTF-8, and this is headed for
    // the window.
    return io::validUtf8(message);
}

/// `file` renamed to `settings.json.corrupt-20260923-101500` beside itself, or empty when it
/// could not be moved. Never replaces anything: a second damaged file within the same second
/// gets `-1` on the end rather than taking the first one's name.
std::filesystem::path setAside(const std::filesystem::path& file) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &now) != 0) {
        return {};
    }
#else
    if (localtime_r(&now, &local) == nullptr) {
        return {};
    }
#endif
    char stamp[32] = {};
    if (std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local) == 0) {
        return {};
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::filesystem::path target = file;
        target += std::string(".corrupt-") + stamp +
                  (attempt == 0 ? std::string{} : "-" + std::to_string(attempt));
        std::error_code code;
        if (std::filesystem::exists(target, code) || code) {
            continue;
        }
        std::filesystem::rename(file, target, code);
        // A rename that failed for any reason but the name will fail again under another one.
        return code ? std::filesystem::path{} : target;
    }
    return {};
}

} // namespace

std::filesystem::path settingsDirectory() {
    // The executable's own, which is what makes this a program that can be copied to a
    // stick and run: the settings travel with it, and two copies in two folders are two
    // rigs rather than one fighting over a shared file. `userSettingsDirectory` is what
    // this used to be, and is still read once — see `existingSettingsFile`.
    // `parent_path`, not `remove_filename`: the latter leaves the trailing separator, so the
    // directory would not compare equal to the `parent_path()` of the file inside it.
    //
    // A folder named in the environment comes first: see the header — it is how a test
    // process is kept off the rig's file.
    if (std::filesystem::path named = namedSettingsDirectory(); !named.empty()) {
        return named;
    }
    const std::filesystem::path exe = executablePath();
    if (exe.empty()) {
        return userSettingsDirectory();
    }
#if defined(__APPLE__)
    // `access` answers for a read-only volume too (EROFS), which is what a translocated app
    // runs from: see the header.
    const std::filesystem::path folder = programFolder(exe);
    if (::access(folder.c_str(), W_OK) != 0) {
        return userSettingsDirectory();
    }
    return folder;
#else
    return exe.parent_path();
#endif
}

std::filesystem::path programFolder(const std::filesystem::path& executable) {
    const std::filesystem::path folder = executable.parent_path();
    const std::filesystem::path contents = folder.parent_path();
    const std::filesystem::path bundle = contents.parent_path();
    if (folder.filename() == "MacOS" && contents.filename() == "Contents" &&
        bundle.extension() == ".app") {
        return bundle.parent_path();
    }
    return folder;
}

std::filesystem::path userSettingsDirectory() {
#if defined(_WIN32)
    const std::string appData = environmentVariable("APPDATA");
    if (appData.empty()) {
        return {};
    }
    return std::filesystem::path(appData) / "takt4";
#elif defined(__APPLE__)
    const std::string home = environmentVariable("HOME");
    if (home.empty()) {
        return {};
    }
    return std::filesystem::path(home) / "Library" / "Application Support" / "takt4";
#else
    const std::string configHome = environmentVariable("XDG_CONFIG_HOME");
    if (!configHome.empty()) {
        return std::filesystem::path(configHome) / "takt4";
    }
    const std::string home = environmentVariable("HOME");
    if (home.empty()) {
        return {};
    }
    return std::filesystem::path(home) / ".config" / "takt4";
#endif
}

std::filesystem::path settingsFile() {
    const std::filesystem::path directory = settingsDirectory();
    return directory.empty() ? std::filesystem::path{} : directory / "settings.json";
}

std::filesystem::path scratchDirectory() {
    const std::filesystem::path named = namedSettingsDirectory();
    if (!named.empty()) {
        return named / "texts";
    }
    std::error_code code;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(code);
    return code ? std::filesystem::path{} : temp / "takt4";
}

std::filesystem::path existingSettingsFile() {
    const std::filesystem::path beside = settingsFile();
    std::error_code code;
    if (!beside.empty() && std::filesystem::exists(beside, code)) {
        return beside;
    }
    // **Nothing else, the per-user file included.** Builds before 2026-09-07 kept settings there,
    // and until 2026-10-08 one was read whenever nothing was beside the executable, so a rig
    // kept its outputs across the move. A month on, all it still did was hand every copy started
    // in a new folder the rig as it stood that September — "keep BPM in" switched on with its
    // window among it. A new folder is a new rig.
#if defined(__APPLE__)
    // **Except on a Mac, where the per-user folder is where an app that could not write beside
    // itself kept its rig** (`settingsDirectory`): a download opened where it landed runs from a
    // read-only copy of itself until it is moved. Read from there while nothing is beside the
    // moved app, the first save there takes the rig with it. Never in
    // a process given a folder of its own.
    if (namedSettingsDirectory().empty()) {
        const std::filesystem::path user = userSettingsDirectory() / "settings.json";
        if (user != beside && std::filesystem::exists(user, code)) {
            return user;
        }
    }
#endif
    return beside;
}

void migrateTransportOutputs(Settings& settings) {
    std::vector<output::OutputTarget>& outputs = settings.preset.outputs;
    // Link was a switch of its own; it is the first output now, carrying that switch. And the
    // old field says what that output says from here on, which is what a save writes into it:
    // it held the file's own, so a file whose Link row and field disagreed — or that had no field
    // — read as one setting and saved as another.
    (void)output::ensureLinkOutput(outputs, settings.preset.link);
    settings.preset.link = outputs.front().enabled;
    // And the MIDI clock was one port of its own; it is an output now, unless the file already
    // has a clock output on that device (one this build wrote, where the port is only there
    // for an older build).
    const std::string port = settings.machine.midiClockPort;
    settings.machine.midiClockPort.clear();
    if (port.empty()) {
        return;
    }
    for (const output::OutputTarget& target : outputs) {
        if (target.kind == output::OutputTarget::Kind::MidiClock && target.device == port) {
            return;
        }
    }
    output::OutputTarget clock;
    clock.kind = output::OutputTarget::Kind::MidiClock;
    clock.name = "MIDI clock";
    clock.device = port;
    outputs.push_back(std::move(clock));
}

void assignIds(Preset& preset) {
    // **Only what was given an id here can be named by a rule.** A file written before ids, and a
    // line typed by hand, hold none, and their rules name what they point at; a file this build
    // wrote holds ids everywhere, and its rules point at those. So a name in a rule is looked for
    // among the entries that had no id — or a duplicated one — until now, and nowhere else: it
    // used to be looked for on every load, so a fixture renamed to a group's label joined that
    // group's rules after a restart, and a deleted output's name found the next output called it.
    const auto givenIds = [](const auto& before, const auto& after) {
        std::vector<bool> given(after.size(), false);
        for (std::size_t i = 0; i < after.size(); ++i) {
            given[i] = i >= before.size() || before[i].id != after[i].id;
        }
        return given;
    };
    const std::vector<output::OutputTarget> outputsBefore = preset.outputs;
    const std::vector<dmx::Fixture> fixturesBefore = preset.fixtures;
    output::ensureOutputIds(preset.outputs);
    dmx::ensureFixtureIds(preset.fixtures);
    trigger::Rule::ensureIds(preset.rules);
    const std::vector<bool> newOutputs = givenIds(outputsBefore, preset.outputs);
    const std::vector<bool> newFixtures = givenIds(fixturesBefore, preset.fixtures);
    for (trigger::Rule::Config& rule : preset.rules) {
        output::routeByIds(rule.outputs, preset.outputs, &newOutputs);
        dmx::aimByIds(rule.dmx.fixtures, preset.fixtures, &newFixtures);
    }
}

namespace {

/// The whole file as a document: what `toJson` writes, and `presetJson` less the machine half.
json documentOf(const Settings& settings) {
    // One line each, as `formatOutputTarget` writes them: "main = 127.0.0.1:7000" and
    // "lights = midi MOTU Pro Audio Midi Out 1". Text rather than an object per target for
    // the reason the MIDI bindings are text — this is a file a person may open and a headless
    // mode is expected to hand-write one, and a line that reads as a sentence can be typed.
    json targets = json::array();
    // Link's switch and the first clock's device, in the fields an older build reads them from
    // — from the rows that hold them, or from those fields themselves for settings built by
    // hand without the rows (`migrateTransportOutputs` makes the rows when the file is read).
    bool link = settings.preset.link;
    std::string clockPort = settings.machine.midiClockPort;
    bool clockRow = false;
    for (const output::OutputTarget& target : settings.preset.outputs) {
        targets.push_back(output::formatOutputTarget(target));
        if (target.kind == output::OutputTarget::Kind::Link) {
            link = target.enabled;
        } else if (target.kind == output::OutputTarget::Kind::MidiClock && target.enabled &&
                   !clockRow) {
            clockPort = target.device;
            clockRow = true;
        }
    }

    json bindings = json::array();
    for (const std::string& binding : settings.machine.midiBindings) {
        bindings.push_back(binding);
    }

    // The lighting patch. An object per fixture rather than a line of text, unlike the
    // outputs above: a channel map is a *list*, and squeezing "pan, pan-fine, tilt, tilt-fine,
    // speed, dimmer, strobe, red, green, blue, white, gobo" onto one line beside an address
    // and a pair of movement limits would be a line nobody can read and nobody can edit.
    json patch = json::array();
    for (const dmx::Fixture& fixture : settings.preset.fixtures) {
        json channels = json::array();
        for (const dmx::Role role : fixture.channels) {
            channels.push_back(std::string(dmx::nameOf(role)));
        }
        json parked = json::array();
        for (const std::uint8_t level : fixture.parked) {
            parked.push_back(static_cast<unsigned int>(level));
        }
        json one{
            {"id", fixture.id},
            {"name", fixture.name},
            {"universe", static_cast<unsigned int>(fixture.universe)},
            {"address", static_cast<unsigned int>(fixture.address)},
            {"channels", std::move(channels)},
            {"parked", std::move(parked)},
            {"enabled", fixture.enabled},
        };
        if (!fixture.group.empty()) {
            one["group"] = fixture.group;
        }
        // The movement window, only where it has been narrowed. A fixture that cannot move has
        // no window worth writing, and one that has not been limited should not carry four
        // numbers saying so.
        if (fixture.panMin != 0.0 || fixture.panMax != 1.0 || fixture.tiltMin != 0.0 ||
            fixture.tiltMax != 1.0) {
            one["panMin"] = fixture.panMin;
            one["panMax"] = fixture.panMax;
            one["tiltMin"] = fixture.tiltMin;
            one["tiltMax"] = fixture.tiltMax;
        }
        // What each channel is called, from an imported definition — only where there is one,
        // so a fixture patched by hand writes nothing more than it did.
        if (std::any_of(fixture.labels.begin(), fixture.labels.end(),
                        [](const std::string& label) { return !label.empty(); })) {
            one["labels"] = fixture.labels;
        }
        // And which library entry and mode it was made from, while it is linked.
        if (fixture.profile.linked()) {
            json linkTo{
                {"id", fixture.profile.id},
                {"mode", fixture.profile.mode},
                {"part", fixture.profile.part},
            };
            if (!fixture.profile.pair.empty()) {
                linkTo["pair"] = fixture.profile.pair;
            }
            one["profile"] = std::move(linkTo);
        }
        patch.push_back(std::move(one));
    }

    json preset{
        {"tempo", tempoToJson(settings.preset.tempo)},
        {"decoder", decoderName(settings.preset.decoder)},
        {"meters", metersToJson(settings.preset.meters)},
        {"link", link},
        {"oscPrefix", settings.preset.oscPrefix},
        {"phraseBars", settings.preset.phraseBars},
        {"timing", timingToJson(settings.preset)},
        {"outputs", targets},
        {"fixtures", patch},
        // Through `rule_json`, which owns the shape of a rule — as a node of this document, not
        // as text parsed back in. The rules are the largest thing a preset carries and the only
        // part of it with a format of its own.
        {"rules", rulesToNode(settings.preset.rules)},
    };
    // The imported definitions, the same way through `profile_json`. Only when there are any.
    if (!settings.preset.library.empty()) {
        preset["fixtureLibrary"] = fixtures::libraryToNode(settings.preset.library);
    }

    return json{
        {"version", kFormatVersion},
        {"machine",
         json{
             {"deviceName", settings.machine.deviceName},
             {"hostApiName", settings.machine.hostApiName},
             {"channel", settings.machine.channel},
             {"mono", settings.machine.mono},
             {"midiClockPort", clockPort},
             {"midiControlPort", settings.machine.midiControlPort},
             {"midiBindings", bindings},
             {"oscControlEnabled", settings.machine.oscControlEnabled},
             {"oscControlPort", settings.machine.oscControlPort},
             {"oscControlLocalOnly", settings.machine.oscControlLocalOnly},
             {"oscRemoteSetup", settings.machine.oscRemoteSetup},
             {"oscRemoteFrom", settings.machine.oscRemoteFrom},
             {"timingSource", settings.machine.timingSource},
             {"midiClockInPort", settings.machine.midiClockInPort},
             {"midiClockObeyStartStop", settings.machine.midiClockObeyStartStop},
             {"proLinkFollow", proLinkFollowText(settings.machine.proLinkFollow)},
             {"proLinkInterface", settings.machine.proLinkInterface},
             {"proLinkInterfaceMac", settings.machine.proLinkInterfaceMac},
             {"proLinkInterfaceName", settings.machine.proLinkInterfaceName},
             {"inputsFolded", settings.machine.inputsFolded},
             {"outputsFolded", settings.machine.outputsFolded},
             {"ruleSectionsFolded", settings.machine.ruleSectionsFolded},
             {"ruleRemoteFolded", settings.machine.ruleRemoteFolded},
             {"ruleLogOpen", settings.machine.ruleLogOpen},
             {"ruleLogHeight", settings.machine.ruleLogHeight},
             {"patchSectionsFolded", settings.machine.patchSectionsFolded},
         }},
        {"preset", std::move(preset)},
    };
}

std::string dumped(const json& document) {
    // `replace` rather than nlohmann's default `strict`, for `rulesToJson`' reason and one
    // more of its own: the machine half carries names this program did not choose. PortAudio
    // hands over a device name as the driver spelled it and RtMidi a port name as the
    // platform did, and neither promises UTF-8 — an ASIO driver with an accented character
    // in its name is enough. Refusing to write the file over one byte would lose the whole
    // of an operator's configuration; U+FFFD loses the byte.
    return document.dump(2, ' ', /*ensure_ascii=*/false, json::error_handler_t::replace) + "\n";
}

} // namespace

std::string toJson(const Settings& settings) {
    return dumped(documentOf(settings));
}

std::string presetJson(const Preset& preset) {
    Settings settings;
    settings.preset = preset;
    json document = documentOf(settings);
    document.erase("machine");
    return dumped(document);
}

namespace {

Settings fromDocument(const json& document) {
    Settings settings;
    if (document.contains("machine")) {
        settings.machine.inFile = true;
        const json& machine = document.at("machine");
        read(machine, "deviceName", settings.machine.deviceName);
        read(machine, "hostApiName", settings.machine.hostApiName);
        read(machine, "channel", settings.machine.channel);
        // A file from before the choice existed listened to one input, because that was all
        // there was: it is heard as the stereo pair that input is in now, as a fresh install is,
        // and the window says so — "mono" puts it back (2026-09-28).
        if (machine.contains("mono")) {
            read(machine, "mono", settings.machine.mono);
        } else if (machine.contains("channel")) {
            settings.machine.mono = false;
            settings.machine.stereoFromMono = true;
        }
        read(machine, "midiClockPort", settings.machine.midiClockPort);
        read(machine, "midiControlPort", settings.machine.midiControlPort);
        read(machine, "oscControlEnabled", settings.machine.oscControlEnabled);
        read(machine, "oscControlLocalOnly", settings.machine.oscControlLocalOnly);
        read(machine, "oscRemoteSetup", settings.machine.oscRemoteSetup);
        read(machine, "oscRemoteFrom", settings.machine.oscRemoteFrom);
        read(machine, "timingSource", settings.machine.timingSource);
        // A source this build cannot follow — a newer build's, or a typo — is the audio: the
        // tracker is always there to fall back on, and a picker showing nothing helps nobody.
        if (settings.machine.timingSource != kTimingLink &&
            settings.machine.timingSource != kTimingMidiClock &&
            settings.machine.timingSource != kTimingProLink) {
            settings.machine.timingSource = std::string(kTimingAudio);
        }
        read(machine, "midiClockInPort", settings.machine.midiClockInPort);
        read(machine, "midiClockObeyStartStop", settings.machine.midiClockObeyStartStop);
        std::string follow;
        read(machine, "proLinkFollow", follow);
        settings.machine.proLinkFollow = proLinkFollowFrom(follow);
        read(machine, "proLinkInterface", settings.machine.proLinkInterface);
        read(machine, "proLinkInterfaceMac", settings.machine.proLinkInterfaceMac);
        read(machine, "proLinkInterfaceName", settings.machine.proLinkInterfaceName);
        settings.machine.proLinkInterface = plainAddress(settings.machine.proLinkInterface);
        // A hardware address that is not one, or is all zeros, is none: the card is found by its
        // address alone. And with no address there is no card picked at all.
        const std::optional<timing::prolink::Mac> mac =
            timing::prolink::parseMac(settings.machine.proLinkInterfaceMac);
        settings.machine.proLinkInterfaceMac =
            mac && *mac != timing::prolink::Mac{} ? timing::prolink::macText(*mac) : std::string();
        if (settings.machine.proLinkInterface.empty()) {
            settings.machine.proLinkInterfaceMac.clear();
            settings.machine.proLinkInterfaceName.clear();
        }
        read(machine, "inputsFolded", settings.machine.inputsFolded);
        read(machine, "outputsFolded", settings.machine.outputsFolded);
        // The rule editor's folds, four booleans in A-to-D order; anything else keeps the default.
        if (machine.is_object() && machine.contains("ruleSectionsFolded") &&
            machine.at("ruleSectionsFolded").is_array() &&
            machine.at("ruleSectionsFolded").size() == settings.machine.ruleSectionsFolded.size()) {
            const json& folds = machine.at("ruleSectionsFolded");
            for (std::size_t i = 0; i < settings.machine.ruleSectionsFolded.size(); ++i) {
                if (folds.at(i).is_boolean()) {
                    settings.machine.ruleSectionsFolded[i] = folds.at(i).get<bool>();
                }
            }
        }
        // The patch editor's, three in A-to-C order, read the same way.
        if (machine.is_object() && machine.contains("patchSectionsFolded") &&
            machine.at("patchSectionsFolded").is_array() &&
            machine.at("patchSectionsFolded").size() == settings.machine.patchSectionsFolded.size()) {
            const json& folds = machine.at("patchSectionsFolded");
            for (std::size_t i = 0; i < settings.machine.patchSectionsFolded.size(); ++i) {
                if (folds.at(i).is_boolean()) {
                    settings.machine.patchSectionsFolded[i] = folds.at(i).get<bool>();
                }
            }
        }
        read(machine, "ruleRemoteFolded", settings.machine.ruleRemoteFolded);
        read(machine, "ruleLogOpen", settings.machine.ruleLogOpen);
        read(machine, "ruleLogHeight", settings.machine.ruleLogHeight);
        // A line's height to half the height of a large screen: a hand-edited 0 or a million
        // would otherwise open a log with no lines, or one that pushes the editor off the window.
        if (!std::isfinite(settings.machine.ruleLogHeight)) {
            settings.machine.ruleLogHeight = MachineSettings{}.ruleLogHeight;
        }
        settings.machine.ruleLogHeight = std::clamp(settings.machine.ruleLogHeight, 18.0, 800.0);
        if (settings.machine.channel < 0) {
            settings.machine.channel = 0;
        }
        // Read by hand rather than through `read`, because a port has two ways of being
        // wrong that a hand-edited file really does contain and the generic path takes
        // neither seriously. `read` into a `uint16_t` would take the low bits of 99999 —
        // port 33465, which nobody meant and nobody could guess — and nlohmann *converts*
        // a JSON float, so `1.5` would arrive as port 1. Both are rejected here so the
        // default stands, which is what "settings never fail" has to mean for a value that
        // opens a socket.
        //
        // 0 is legal and means "any free port": `OscReceiver` supports it and `port()`
        // reports which one it got.
        if (machine.is_object() && machine.contains("oscControlPort")) {
            const json& port = machine.at("oscControlPort");
            if (port.is_number_integer()) {
                const std::int64_t value = port.get<std::int64_t>();
                if (value >= 0 && value <= 65535) {
                    settings.machine.oscControlPort = static_cast<std::uint16_t>(value);
                }
            }
        }
        if (machine.is_object() && machine.contains("midiBindings") &&
            machine.at("midiBindings").is_array()) {
            for (const json& binding : machine.at("midiBindings")) {
                // Kept as the text it was written as. Whether it *parses* is
                // `control::parseMidiBinding`'s question and is asked where the table is
                // built — settings does not depend on control, and a line this cannot
                // read is better preserved than silently dropped from the file on the
                // next save.
                if (binding.is_string()) {
                    settings.machine.midiBindings.push_back(binding.get<std::string>());
                }
            }
        }
    }

    if (document.contains("preset")) {
        const json& preset = document.at("preset");
        read(preset, "link", settings.preset.link);
        read(preset, "oscPrefix", settings.preset.oscPrefix);
        // Refused here rather than trusted: `OscPublisher` throws on a prefix it cannot build
        // an address from, from inside the transports' constructor, and a hand-edited "vj"
        // with no leading slash used to stop takt4 opening at all — no window, no message,
        // every launch.
        if (!output::isValidOscPrefix(settings.preset.oscPrefix)) {
            settings.preset.oscPrefix = Preset{}.oscPrefix;
        }
        // A whole number of bars, or eight: `read` would take 2.5 as 2.
        if (preset.contains("phraseBars") && preset.at("phraseBars").is_number_integer()) {
            const std::int64_t bars = preset.at("phraseBars").get<std::int64_t>();
            if (bars >= 1 &&
                bars <= static_cast<std::int64_t>(output::OscPublisher::kMaxPhraseBars)) {
                settings.preset.phraseBars = static_cast<std::uint32_t>(bars);
            }
        }
        std::string decoder;
        read(preset, "decoder", decoder);
        settings.preset.decoder = decoderFromName(decoder);
        readMeters(preset, settings.preset.meters);
        if (preset.is_object() && preset.contains("tempo")) {
            settings.preset.tempo = tempoFromJson(preset.at("tempo"));
        }
        if (preset.is_object() && preset.contains("timing")) {
            timingFromJson(preset.at("timing"), settings.preset);
        }
        if (preset.is_object() && preset.contains("rules")) {
            settings.preset.rules = rulesFromNode(preset.at("rules"));
        }
        if (preset.is_object() && preset.contains("fixtures") && preset.at("fixtures").is_array()) {
            for (const json& one : preset.at("fixtures")) {
                if (!one.is_object()) {
                    continue; // one unreadable fixture must not cost the rest of the patch
                }
                dmx::Fixture fixture;
                read(one, "id", fixture.id);
                read(one, "name", fixture.name);
                read(one, "group", fixture.group);
                read(one, "enabled", fixture.enabled);
                unsigned int universe = 0;
                unsigned int address = 1;
                read(one, "universe", universe);
                read(one, "address", address);
                fixture.universe = dmx::clampPortAddress(static_cast<int>(universe));
                // Clamped rather than refused, like everything else `load` reads: a file
                // hand-edited to address 900 gives a fixture the editor shows as wrong, not a
                // settings file that will not open.
                fixture.address = static_cast<std::uint16_t>(
                    std::clamp<unsigned int>(address, 1, dmx::kChannelsPerUniverse));
                if (one.contains("channels") && one.at("channels").is_array()) {
                    for (const json& role : one.at("channels")) {
                        if (!role.is_string()) {
                            continue;
                        }
                        // An unknown role becomes `Unused` rather than being dropped, so the
                        // channels *after* it stay where the fixture's manual says they are.
                        // Dropping one would silently re-address everything below it.
                        fixture.channels.push_back(
                            dmx::roleOf(role.get<std::string>()).value_or(dmx::Role::Unused));
                    }
                }
                if (one.contains("parked") && one.at("parked").is_array()) {
                    for (const json& level : one.at("parked")) {
                        if (level.is_number()) {
                            fixture.parked.push_back(
                                static_cast<std::uint8_t>(std::clamp(level.get<int>(), 0, 255)));
                        }
                    }
                }
                read(one, "panMin", fixture.panMin);
                read(one, "panMax", fixture.panMax);
                read(one, "tiltMin", fixture.tiltMin);
                read(one, "tiltMax", fixture.tiltMax);
                if (one.contains("labels") && one.at("labels").is_array()) {
                    for (const json& label : one.at("labels")) {
                        // A label that is not text is an empty one, so those after it stay
                        // beside their own channels.
                        fixture.labels.push_back(label.is_string() ? label.get<std::string>()
                                                                   : std::string());
                    }
                    if (fixture.labels.size() > fixture.channels.size()) {
                        fixture.labels.resize(fixture.channels.size());
                    }
                }
                if (one.contains("profile") && one.at("profile").is_object()) {
                    const json& link = one.at("profile");
                    read(link, "id", fixture.profile.id);
                    read(link, "mode", fixture.profile.mode);
                    read(link, "part", fixture.profile.part);
                    read(link, "pair", fixture.profile.pair);
                }
                settings.preset.fixtures.push_back(std::move(fixture));
            }
        }
        if (preset.is_object() && preset.contains("fixtureLibrary")) {
            settings.preset.library = fixtures::libraryFromNode(preset.at("fixtureLibrary"));
        }
        // A link to an entry the file does not have, or that cannot be a link, is no link: the
        // fixture keeps its channels and is a fixture patched by hand.
        for (dmx::Fixture& fixture : settings.preset.fixtures) {
            if (fixture.profile.linked() &&
                (fixtures::findProfile(settings.preset.library, fixture.profile.id) == nullptr ||
                 fixture.profile.mode.empty() || fixture.profile.part < 1 ||
                 fixture.profile.part > 2)) {
                fixture.profile = dmx::Fixture::ProfileLink{};
            }
        }
        // `outputs` is what this build writes; `oscTargets` is what older ones did. Both
        // are read, so an upgrade keeps the targets an operator had typed rather than
        // silently losing them — the one failure they would notice and could not diagnose.
        for (const char* key : {"outputs", "oscTargets"}) {
            if (!preset.contains(key) || !preset.at(key).is_array()) {
                continue;
            }
            for (const json& entry : preset.at(key)) {
                output::OutputTarget target;
                // A line, as this build writes them: "main = 127.0.0.1:7000".
                if (entry.is_string() &&
                    output::parseOutputTarget(entry.get<std::string>(), target)) {
                    settings.preset.outputs.push_back(std::move(target));
                    continue;
                }
                // Or the object an older build wrote.
                std::string host;
                int port = 0;
                read(entry, "host", host);
                read(entry, "port", port);
                if (!host.empty() && port > 0 && port <= 65535) {
                    target.kind = output::OutputTarget::Kind::Osc;
                    target.host = host;
                    target.port = static_cast<std::uint16_t>(port);
                    target.name = host + ":" + std::to_string(port);
                    settings.preset.outputs.push_back(std::move(target));
                }
            }
        }
    }
    migrateTransportOutputs(settings);
    // **Everything a rule points at, by id.** A file written before outputs and fixtures had
    // ids routes rules by their names, and a hand-written line has no id: each gets one here,
    // and each rule is re-pointed from the name to that id — so the rig loads exactly as it
    // was, and renaming an output or a fixture from now on moves no rule.
    assignIds(settings.preset);
    return settings;
}

} // namespace

std::optional<Settings> parse(std::string_view text, std::string& problem, int* version) {
    problem.clear();
    try {
        const json document = io::parseJsonCapped(text);
        if (!document.is_object()) {
            problem = "it is not a settings file";
            return std::nullopt;
        }
        if (version != nullptr) {
            *version = formatOf(document);
        }
        return fromDocument(document);
    } catch (const std::exception& e) {
        problem = describeParseError(text, e);
        return std::nullopt;
    }
}

Settings fromJson(std::string_view text) {
    std::string ignored;
    std::optional<Settings> parsed = parse(text, ignored);
    return parsed ? std::move(*parsed) : Settings{};
}

Loaded loadChecked(const std::filesystem::path& path) {
    Loaded out;
    if (path.empty()) {
        return out;
    }
    std::string problem;
    const std::optional<std::string> text = io::readFile(path, problem);
    if (!text) {
        if (!problem.empty()) {
            out.status = LoadStatus::Unreadable;
            out.problem = std::move(problem);
        }
        return out;
    }
    std::optional<Settings> parsed = parse(*text, problem, &out.version);
    if (!parsed) {
        out.status = LoadStatus::Corrupt;
        out.problem = std::move(problem);
        return out;
    }
    out.settings = std::move(*parsed);
    out.status = LoadStatus::Loaded;
    return out;
}

Settings load(const std::filesystem::path& path) {
    return loadChecked(path).settings;
}

std::filesystem::path backupFile(const std::filesystem::path& file) {
    std::filesystem::path backup = file;
    backup += ".bak";
    return backup;
}

std::string newerRulesNotice(const Preset& preset) {
    const auto count =
        std::count_if(preset.rules.begin(), preset.rules.end(),
                      [](const trigger::Rule::Config& rule) { return !rule.unknown.empty(); });
    if (count == 0) {
        return {};
    }
    return (count == 1 ? std::string("A rule was") : std::to_string(count) + " rules were") +
           " written by a newer takt4, with what this one does not have, and " +
           (count == 1 ? "is" : "are") +
           " switched off: the rule editor says what, and switches them on.";
}

Startup openAtStartup(const std::filesystem::path& file, const std::filesystem::path& readFrom,
                      bool keepBackup) {
    Startup out;
    const Loaded loaded = loadChecked(readFrom);
    const std::string name = io::pathText(readFrom.filename());
    // Only the file takt4 writes to is protected by `writable`: a damaged or locked file in the
    // old per-user location is not what the next save replaces.
    const bool isOurs = !file.empty() && readFrom == file;

    switch (loaded.status) {
    case LoadStatus::Missing:
        return out;
    case LoadStatus::Loaded:
        out.settings = loaded.settings;
        // The rig as it stood when takt4 last started cleanly, kept beside it. Taken here and
        // nowhere else, so an evening of autosaved edits never overwrites it: the one copy that
        // is worth going back to is the one from before tonight. Atomic like every other
        // write, so a crash in the middle of this cannot leave a half backup either. Not after a
        // session that ended at once (`keepBackup`), which this file may have caused.
        if (isOurs && !keepBackup) {
            std::string ignored;
            if (const std::optional<std::string> bytes = io::readFile(file, ignored)) {
                (void)io::replaceFile(backupFile(file), *bytes);
            }
        }
        // **Written by a newer takt4: not saved over without asking**.
        // Every field this one does not know was left out as it was read, and the first autosave
        // wrote the file back without them — a run of an older takt4 on another computer cost a
        // preset its timing source, its latencies and its CDJ settings, and the next start its
        // backup.
        if (isOurs && loaded.version > kFormatVersion) {
            out.writable = false;
            out.notice = name + " was written by a newer takt4 (settings format " +
                         std::to_string(loaded.version) + ", where this one reads " +
                         std::to_string(kFormatVersion) +
                         "), so takt4 will not save over it automatically: what only the newer "
                         "one knows is kept. EXPORT keeps changes made here; SAVE writes the "
                         "file over without them.";
        }
        if (const std::string rules = newerRulesNotice(out.settings.preset); !rules.empty()) {
            out.notice += (out.notice.empty() ? "" : " ") + rules;
        }
        return out;
    case LoadStatus::Unreadable:
        out.writable = !isOurs;
        out.notice = name + " could not be opened (" + loaded.problem +
                     "), so takt4 started with default settings" +
                     (isOurs ? " and will not save over it automatically. Close whatever has "
                               "it open, then restart takt4."
                             : ".");
        return out;
    case LoadStatus::Corrupt:
        break;
    }

    // Damaged. Moved aside first — **never** saved over, which is what used to happen: the
    // defaults it loaded as went back out on exit, over the one copy of the rig there was.
    const std::filesystem::path aside = setAside(readFrom);
    if (aside.empty() && isOurs) {
        out.writable = false;
    }
    std::string from = "default settings";
    if (!file.empty()) {
        const Loaded backup = loadChecked(backupFile(file));
        if (backup.status == LoadStatus::Loaded) {
            out.settings = backup.settings;
            from = io::pathText(backupFile(file).filename()) +
                   ", the copy kept when takt4 last started";
        }
    }
    out.notice = name + " is damaged (" + loaded.problem + "), so takt4 started from " + from +
                 ". " +
                 (aside.empty()
                      ? "The damaged file could not be moved aside, so takt4 will not save over "
                        "it automatically: move or fix it, then restart."
                      : "The damaged file was kept as " + io::pathText(aside.filename()) + ".");
    return out;
}

bool saveText(std::string_view json, const std::filesystem::path& path) {
    try {
        if (path.empty()) {
            return false;
        }
        std::error_code code;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), code);
            // Not checked: create_directories reports false with no error when the directory
            // was already there, and the write below is the real test either way.
        }
        return io::replaceFile(path, json);
    } catch (...) {
        return false;
    }
}

bool save(const Settings& settings, const std::filesystem::path& path) try {
    return saveText(toJson(settings), path);
} catch (...) {
    // A function-try-block, so the header's "false when it could not be written" is true of
    // *every* way it could fail rather than only of the file system. `toJson` is not
    // supposed to throw any more — see the `error_handler_t::replace` above — and this is
    // what makes that a belt rather than the only strap: both callers are places an
    // exception cannot go. `ui::run` saves after the event loop has returned, where nothing
    // would catch it, and `WindowController::saveNow` is a Slint callback, where an
    // exception crossing back into the toolkit takes the process with it. Either way the
    // operator loses the session they were trying to keep.
    return false;
}

} // namespace takt4::settings
