#include "core/settings/settings.hpp"

#include "core/io/atomic_file.hpp"
#include "core/io/utf8.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/settings/rule_json.hpp"

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

/// The format's version. Nothing reads it yet — every field already falls back to its
/// default, which handles a file from an older build — but a change that *cannot* be
/// expressed that way will need it, and adding it afterwards is too late.
constexpr int kFormatVersion = 1;

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
    return exe.empty() ? userSettingsDirectory() : exe.parent_path();
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
    if (!namedSettingsDirectory().empty()) {
        return beside; // a folder of its own, and nobody else's file read instead
    }
    // A build before this one kept the file under the user's profile. With none beside the
    // executable yet, that one is still this machine's settings, and reading it is how a rig
    // keeps its outputs, its device and its MIDI bindings across the change. It is read and
    // not moved: the next save writes beside the executable, so the copy left behind is what
    // an older build — or another copy of this one — still finds.
    const std::filesystem::path user = userSettingsDirectory();
    if (!user.empty() && std::filesystem::exists(user / "settings.json", code)) {
        return user / "settings.json";
    }
    return beside;
}

void migrateTransportOutputs(Settings& settings) {
    std::vector<output::OutputTarget>& outputs = settings.preset.outputs;
    // Link was a switch of its own; it is the first output now, carrying that switch.
    (void)output::ensureLinkOutput(outputs, settings.preset.link);
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
    output::ensureOutputIds(preset.outputs);
    dmx::ensureFixtureIds(preset.fixtures);
    for (trigger::Rule::Config& rule : preset.rules) {
        output::routeByIds(rule.outputs, preset.outputs);
        dmx::aimByIds(rule.dmx.fixtures, preset.fixtures);
    }
}

std::string toJson(const Settings& settings) {
    // One line each, as `formatOutputTarget` writes them: "main = 127.0.0.1:7000" and
    // "lights = midi MOTU Pro Audio Midi Out 1". Text rather than an object per target for
    // the reason the MIDI bindings are text — this is a file a person may open and Q8's
    // headless mode is expected to hand-write one, and a line that reads as a sentence can
    // be typed.
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
    json fixtures = json::array();
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
        fixtures.push_back(std::move(one));
    }

    const json document{
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
             {"inputsFolded", settings.machine.inputsFolded},
             {"outputsFolded", settings.machine.outputsFolded},
             {"ruleSectionsFolded", settings.machine.ruleSectionsFolded},
             {"ruleLogOpen", settings.machine.ruleLogOpen},
             {"ruleLogHeight", settings.machine.ruleLogHeight},
             {"patchSectionsFolded", settings.machine.patchSectionsFolded},
         }},
        {"preset",
         json{
             {"tempo", tempoToJson(settings.preset.tempo)},
             {"decoder", decoderName(settings.preset.decoder)},
             {"meters", metersToJson(settings.preset.meters)},
             {"link", link},
             {"oscPrefix", settings.preset.oscPrefix},
             {"outputs", targets},
             {"fixtures", fixtures},
             // Through `rule_json`, which owns the shape of a rule, and back through
             // `json::parse` so it nests as an array rather than as a string of JSON.
             // §5.8's rules are the largest thing a preset carries and the only part of it
             // with a format of its own.
             {"rules", json::parse(rulesToJson(settings.preset.rules))},
         }},
    };
    // `replace` rather than nlohmann's default `strict`, for `rulesToJson`' reason and one
    // more of its own: the machine half carries names this program did not choose. PortAudio
    // hands over a device name as the driver spelled it and RtMidi a port name as the
    // platform did, and neither promises UTF-8 — an ASIO driver with an accented character
    // in its name is enough. Refusing to write the file over one byte would lose the whole
    // of an operator's configuration; U+FFFD loses the byte.
    return document.dump(2, ' ', /*ensure_ascii=*/false, json::error_handler_t::replace) + "\n";
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
        std::string decoder;
        read(preset, "decoder", decoder);
        settings.preset.decoder = decoderFromName(decoder);
        readMeters(preset, settings.preset.meters);
        if (preset.is_object() && preset.contains("tempo")) {
            settings.preset.tempo = tempoFromJson(preset.at("tempo"));
        }
        if (preset.is_object() && preset.contains("rules")) {
            settings.preset.rules = rulesFromJson(preset.at("rules").dump());
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
                settings.preset.fixtures.push_back(std::move(fixture));
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
    // was, and renaming an output or a fixture from now on moves no rule (the operator's report
    // of 2026-09-23, and the audit's M28).
    assignIds(settings.preset);
    return settings;
}

} // namespace

std::optional<Settings> parse(std::string_view text, std::string& problem) {
    problem.clear();
    try {
        const json document = json::parse(text);
        if (!document.is_object()) {
            problem = "it is not a settings file";
            return std::nullopt;
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
    std::optional<Settings> parsed = parse(*text, problem);
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

Startup openAtStartup(const std::filesystem::path& file, const std::filesystem::path& readFrom) {
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
        // write, so a crash in the middle of this cannot leave a half backup either.
        if (isOurs) {
            std::string ignored;
            if (const std::optional<std::string> bytes = io::readFile(file, ignored)) {
                (void)io::replaceFile(backupFile(file), *bytes);
            }
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
