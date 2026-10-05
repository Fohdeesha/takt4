#include "core/fixtures/profile_json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <random>
#include <set>

namespace takt4::fixtures {
namespace {

using nlohmann::json;

json modeToJson(const ProfileMode& mode) {
    json parts = json::array();
    for (const std::vector<ProfileChannel>& part : mode.parts) {
        json channels = json::array();
        for (const ProfileChannel& channel : part) {
            json one = json::array({std::string(dmx::nameOf(channel.role)), channel.label,
                                    static_cast<unsigned int>(channel.parked)});
            if (!channel.note.empty()) {
                one.push_back(channel.note);
            }
            channels.push_back(std::move(one));
        }
        parts.push_back(std::move(channels));
    }
    return json{
        {"name", mode.name},
        {"parts", std::move(parts)},
        {"notes", mode.notes},
        {"refused", mode.refused},
    };
}

json modesToJson(const std::vector<ProfileMode>& modes) {
    json out = json::array();
    for (const ProfileMode& mode : modes) {
        out.push_back(modeToJson(mode));
    }
    return out;
}

std::string stringIn(const json& object, const char* key) {
    if (!object.is_object()) {
        return {};
    }
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::vector<std::string> stringsIn(const json& object, const char* key) {
    std::vector<std::string> out;
    if (!object.is_object()) {
        return out;
    }
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array()) {
        return out;
    }
    for (const json& item : *found) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

ProfileChannel channelFromJson(const json& value) {
    // Lenient on purpose: a channel that does not read is still a channel, so the ones after it
    // stay on their addresses.
    ProfileChannel channel;
    if (!value.is_array()) {
        return channel;
    }
    if (value.size() > 0 && value[0].is_string()) {
        channel.role = dmx::roleOf(value[0].get<std::string>()).value_or(dmx::Role::Unused);
    }
    if (value.size() > 1 && value[1].is_string()) {
        channel.label = value[1].get<std::string>();
    }
    if (value.size() > 2 && value[2].is_number()) {
        const double level = value[2].get<double>();
        channel.parked = static_cast<std::uint8_t>(std::clamp(level, 0.0, 255.0));
    }
    if (value.size() > 3 && value[3].is_string()) {
        channel.note = value[3].get<std::string>();
    }
    return channel;
}

ProfileMode modeFromJson(const json& value) {
    ProfileMode mode;
    mode.name = stringIn(value, "name");
    mode.notes = stringsIn(value, "notes");
    mode.refused = stringIn(value, "refused");
    if (value.is_object()) {
        if (const auto parts = value.find("parts"); parts != value.end() && parts->is_array()) {
            for (const json& part : *parts) {
                std::vector<ProfileChannel>& out = mode.parts.emplace_back();
                if (part.is_array()) {
                    for (const json& channel : part) {
                        out.push_back(channelFromJson(channel));
                    }
                }
            }
        }
    }
    // A mode with nothing in it is not one a fixture can be made from, whatever it says.
    if (mode.refused.empty() &&
        (mode.parts.empty() || mode.parts.size() > 2 ||
         std::any_of(mode.parts.begin(), mode.parts.end(),
                     [](const auto& part) { return part.empty() || part.size() > 512; }))) {
        mode.refused = "this mode is damaged in the saved library";
        mode.parts.clear();
    }
    if (!mode.refused.empty()) {
        mode.parts.clear();
    }
    return mode;
}

} // namespace

std::string digestOf(const FixtureProfile& profile) {
    const json canonical{
        {"format", profile.format},
        {"key", profile.key},
        {"manufacturer", profile.manufacturer},
        {"model", profile.model},
        {"revision", profile.revision},
        {"modes", modesToJson(profile.modes)},
    };
    // nlohmann keeps an object's keys sorted, so this text is the same for the same content.
    const std::string text = canonical.dump(-1, ' ', false, json::error_handler_t::replace);
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    char hex[17] = {};
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
    return hex;
}

std::string newProfileId(const std::vector<FixtureProfile>& library) {
    static thread_local std::mt19937_64 random{std::random_device{}()};
    for (;;) {
        char text[16] = {};
        std::snprintf(text, sizeof text, "p-%08x", static_cast<unsigned int>(random()));
        const std::string id(text);
        if (std::none_of(library.begin(), library.end(),
                         [&](const FixtureProfile& entry) { return entry.id == id; })) {
            return id;
        }
    }
}

std::string libraryToJson(const std::vector<FixtureProfile>& library) {
    json out = json::array();
    for (const FixtureProfile& profile : library) {
        out.push_back(json{
            {"id", profile.id},
            {"format", profile.format},
            {"key", profile.key},
            {"manufacturer", profile.manufacturer},
            {"model", profile.model},
            {"revision", profile.revision},
            {"file", profile.file},
            {"digest", digestOf(profile)},
            {"notes", profile.notes},
            {"modes", modesToJson(profile.modes)},
        });
    }
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::vector<FixtureProfile> libraryFromJson(std::string_view text) {
    std::vector<FixtureProfile> library;
    json parsed;
    try {
        parsed = json::parse(text);
    } catch (const std::exception&) {
        return library;
    }
    if (!parsed.is_array()) {
        return library;
    }
    for (const json& entry : parsed) {
        if (!entry.is_object()) {
            continue;
        }
        FixtureProfile profile;
        profile.id = stringIn(entry, "id");
        profile.format = stringIn(entry, "format");
        profile.key = stringIn(entry, "key");
        profile.manufacturer = stringIn(entry, "manufacturer");
        profile.model = stringIn(entry, "model");
        profile.revision = stringIn(entry, "revision");
        profile.file = stringIn(entry, "file");
        profile.notes = stringsIn(entry, "notes");
        if (const auto modes = entry.find("modes"); modes != entry.end() && modes->is_array()) {
            for (const json& mode : *modes) {
                profile.modes.push_back(modeFromJson(mode));
            }
        }
        if (profile.format.empty() || profile.key.empty()) {
            continue; // nothing to recognise a re-import by: not a library entry
        }
        const bool taken =
            std::any_of(library.begin(), library.end(),
                        [&](const FixtureProfile& other) { return other.id == profile.id; });
        if (profile.id.empty() || taken) {
            profile.id = newProfileId(library);
        }
        profile.digest = digestOf(profile);
        library.push_back(std::move(profile));
    }
    return library;
}

} // namespace takt4::fixtures
