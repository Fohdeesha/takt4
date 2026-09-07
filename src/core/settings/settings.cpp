#include "core/settings/settings.hpp"

#include "core/settings/rule_json.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace takt4::settings {
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

json tempoToJson(const tracking::TempoTracker::Options& tempo) {
    return json{
        {"minBpm", tempo.minBpm},
        {"maxBpm", tempo.maxBpm},
        {"octaveFold", tempo.octaveFold},
        {"confidenceThreshold", tempo.confidenceThreshold},
        {"latencyOffsetSeconds", tempo.latencyOffsetSeconds},
    };
}

tracking::TempoTracker::Options tempoFromJson(const json& object) {
    // Started from the defaults, so a field the file does not mention keeps the value the
    // tracker was built to have rather than a zero.
    tracking::TempoTracker::Options tempo;
    read(object, "minBpm", tempo.minBpm);
    read(object, "maxBpm", tempo.maxBpm);
    read(object, "octaveFold", tempo.octaveFold);
    read(object, "confidenceThreshold", tempo.confidenceThreshold);
    read(object, "latencyOffsetSeconds", tempo.latencyOffsetSeconds);

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
    // Beyond half a beat the offset is the next beat; see ui/window_state.hpp's slider.
    if (!(tempo.latencyOffsetSeconds > -1.0) || !(tempo.latencyOffsetSeconds < 1.0)) {
        tempo.latencyOffsetSeconds = defaults.latencyOffsetSeconds;
    }
    return tempo;
}

} // namespace

std::filesystem::path settingsDirectory() {
    // The executable's own, which is what makes this a program that can be copied to a
    // stick and run: the settings travel with it, and two copies in two folders are two
    // rigs rather than one fighting over a shared file. `userSettingsDirectory` is what
    // this used to be, and is still read once — see `existingSettingsFile`.
    // `parent_path`, not `remove_filename`: the latter leaves the trailing separator, so the
    // directory would not compare equal to the `parent_path()` of the file inside it.
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

std::filesystem::path existingSettingsFile() {
    const std::filesystem::path beside = settingsFile();
    std::error_code code;
    if (!beside.empty() && std::filesystem::exists(beside, code)) {
        return beside;
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

std::string toJson(const Settings& settings) {
    // One line each, as `formatOutputTarget` writes them: "main = 127.0.0.1:7000" and
    // "lights = midi MOTU Pro Audio Midi Out 1". Text rather than an object per target for
    // the reason the MIDI bindings are text — this is a file a person may open and Q8's
    // headless mode is expected to hand-write one, and a line that reads as a sentence can
    // be typed.
    json targets = json::array();
    for (const output::OutputTarget& target : settings.preset.outputs) {
        targets.push_back(output::formatOutputTarget(target));
    }

    json bindings = json::array();
    for (const std::string& binding : settings.machine.midiBindings) {
        bindings.push_back(binding);
    }

    const json document{
        {"version", kFormatVersion},
        {"machine",
         json{
             {"deviceName", settings.machine.deviceName},
             {"hostApiName", settings.machine.hostApiName},
             {"channel", settings.machine.channel},
             {"midiClockPort", settings.machine.midiClockPort},
             {"midiControlPort", settings.machine.midiControlPort},
             {"midiBindings", bindings},
             {"oscControlEnabled", settings.machine.oscControlEnabled},
             {"oscControlPort", settings.machine.oscControlPort},
             {"oscControlLocalOnly", settings.machine.oscControlLocalOnly},
         }},
        {"preset",
         json{
             {"tempo", tempoToJson(settings.preset.tempo)},
             {"link", settings.preset.link},
             {"oscPrefix", settings.preset.oscPrefix},
             {"outputs", targets},
             // Through `rule_json`, which owns the shape of a rule, and back through
             // `json::parse` so it nests as an array rather than as a string of JSON.
             // §5.8's rules are the largest thing a preset carries and the only part of it
             // with a format of its own.
             {"rules", json::parse(rulesToJson(settings.preset.rules))},
         }},
    };
    return document.dump(2) + "\n";
}

Settings fromJson(std::string_view text) {
    Settings settings;
    const json document = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) {
        return settings;
    }

    if (document.contains("machine")) {
        const json& machine = document.at("machine");
        read(machine, "deviceName", settings.machine.deviceName);
        read(machine, "hostApiName", settings.machine.hostApiName);
        read(machine, "channel", settings.machine.channel);
        read(machine, "midiClockPort", settings.machine.midiClockPort);
        read(machine, "midiControlPort", settings.machine.midiControlPort);
        read(machine, "oscControlEnabled", settings.machine.oscControlEnabled);
        read(machine, "oscControlLocalOnly", settings.machine.oscControlLocalOnly);
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
        if (preset.is_object() && preset.contains("tempo")) {
            settings.preset.tempo = tempoFromJson(preset.at("tempo"));
        }
        if (preset.is_object() && preset.contains("rules")) {
            settings.preset.rules = rulesFromJson(preset.at("rules").dump());
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
    return settings;
}

Settings load(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return fromJson(text.str());
}

bool save(const Settings& settings, const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code code;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), code);
        // Not checked: create_directories reports false with no error when the directory
        // was already there, and the open below is the real test either way.
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << toJson(settings);
    return out.good();
}

} // namespace takt4::settings
