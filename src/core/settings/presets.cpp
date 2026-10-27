#include "core/settings/presets.hpp"

#include "core/io/utf8.hpp"

#include <algorithm>
#include <array>
#include <system_error>

namespace takt4::settings {

namespace {

std::string loweredAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string raisedAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

/// A name the operator typed, as a path: through UTF-8 on every system, where a `std::string`
/// would go through the ANSI code page on Windows (see `io::pathText`).
std::filesystem::path utf8Path(std::string_view text) {
    std::u8string bytes;
    bytes.reserve(text.size());
    for (const char c : text) {
        bytes.push_back(static_cast<char8_t>(static_cast<unsigned char>(c)));
    }
    return std::filesystem::path(bytes);
}

} // namespace

std::filesystem::path presetsDirectory() {
    const std::filesystem::path directory = settingsDirectory();
    return directory.empty() ? directory : directory / "presets";
}

std::string trimmedPresetName(std::string_view name) {
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    while (!name.empty() && blank(name.front())) {
        name.remove_prefix(1);
    }
    while (!name.empty() && blank(name.back())) {
        name.remove_suffix(1);
    }
    return std::string(name);
}

std::string presetNameProblem(std::string_view name) {
    if (name.empty()) {
        return "a preset needs a name";
    }
    if (name.size() > kPresetNameMax) {
        return "a preset's name is " + std::to_string(kPresetNameMax) + " characters at most";
    }
    if (!io::isValidUtf8(name)) {
        return "a preset's name has to be text";
    }
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            return "a preset's name cannot hold a control character";
        }
        if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) {
            return std::string("a preset's name cannot hold ") + c;
        }
    }
    if (name.front() == '.') {
        return "a preset's name cannot start with a dot";
    }
    if (name.back() == '.' || name.back() == ' ') {
        return "a preset's name cannot end with a dot or a space";
    }
    // Windows opens a device for these, whatever comes after a dot: "CON.json" is the console.
    const std::string base = raisedAscii(name.substr(0, name.find('.')));
    static constexpr std::array<std::string_view, 4> kDevices{"CON", "PRN", "AUX", "NUL"};
    const bool device = std::find(kDevices.begin(), kDevices.end(), base) != kDevices.end() ||
                        (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) &&
                         base[3] >= '1' && base[3] <= '9');
    if (device) {
        return "Windows keeps the name " + base + " for a device";
    }
    return {};
}

std::vector<std::string> listPresets(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    if (folder.empty()) {
        return names;
    }
    std::error_code code;
    for (std::filesystem::directory_iterator entry(folder, code), end; !code && entry != end;
         entry.increment(code)) {
        std::error_code kind;
        if (!entry->is_regular_file(kind) || kind) {
            continue;
        }
        const std::filesystem::path& path = entry->path();
        if (loweredAscii(io::pathText(path.extension())) != ".json") {
            continue;
        }
        std::string name = io::pathText(path.stem());
        if (presetNameProblem(name).empty()) {
            names.push_back(std::move(name));
        }
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        const std::string la = loweredAscii(a);
        const std::string lb = loweredAscii(b);
        return la != lb ? la < lb : a < b;
    });
    return names;
}

std::optional<std::string> findPreset(const std::vector<std::string>& names,
                                      std::string_view name) {
    const std::string wanted = trimmedPresetName(name);
    for (const std::string& candidate : names) {
        if (candidate == wanted) {
            return candidate;
        }
    }
    const std::string lowered = loweredAscii(wanted);
    std::optional<std::string> found;
    for (const std::string& candidate : names) {
        if (loweredAscii(candidate) == lowered) {
            if (found) {
                return std::nullopt; // "Show" and "show": neither is meant more
            }
            found = candidate;
        }
    }
    return found;
}

std::filesystem::path presetFile(const std::filesystem::path& folder, std::string_view name) {
    return folder / utf8Path(std::string(name) + ".json");
}

bool savePreset(const Preset& preset, const std::filesystem::path& folder, std::string_view name,
                std::string& problem) {
    problem = presetNameProblem(name);
    if (!problem.empty()) {
        return false;
    }
    if (folder.empty()) {
        problem = "there is no folder to keep presets in on this machine";
        return false;
    }
    const std::filesystem::path file = presetFile(folder, name);
    if (!saveText(presetJson(preset), file)) {
        problem = "could not write " + io::pathText(file);
        return false;
    }
    return true;
}

bool deletePreset(const std::filesystem::path& folder, std::string_view name,
                  std::string& problem) {
    // Checked as a saved one would be, so no name reaches outside the folder.
    problem = presetNameProblem(name);
    if (!problem.empty() || folder.empty()) {
        if (problem.empty()) {
            problem = "there is no folder of presets on this machine";
        }
        return false;
    }
    const std::filesystem::path file = presetFile(folder, name);
    std::error_code code;
    if (!std::filesystem::remove(file, code)) {
        // Not `code.message()`: Windows writes it in the ANSI code page, and this is shown.
        problem =
            code ? "could not delete " + io::pathText(file) + " (another program may have it open)"
                 : "no preset \"" + std::string(name) + "\"";
        return false;
    }
    return true;
}

} // namespace takt4::settings
