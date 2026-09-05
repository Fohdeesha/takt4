#include "core/settings/settings.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <exception>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
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

std::string toJson(const Settings& settings) {
    json targets = json::array();
    for (const auto& [host, port] : settings.preset.oscTargets) {
        targets.push_back(json{{"host", host}, {"port", port}});
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
         }},
        {"preset",
         json{
             {"tempo", tempoToJson(settings.preset.tempo)},
             {"link", settings.preset.link},
             {"oscPrefix", settings.preset.oscPrefix},
             {"oscTargets", targets},
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
        if (settings.machine.channel < 0) {
            settings.machine.channel = 0;
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
        if (preset.is_object() && preset.contains("oscTargets") &&
            preset.at("oscTargets").is_array()) {
            for (const json& target : preset.at("oscTargets")) {
                std::string host;
                int port = 0;
                read(target, "host", host);
                read(target, "port", port);
                if (!host.empty() && port > 0 && port <= 65535) {
                    settings.preset.oscTargets.emplace_back(host, static_cast<std::uint16_t>(port));
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
