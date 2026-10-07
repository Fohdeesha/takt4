#include "core/dmx/fixture.hpp"

#include "core/dmx/liberation.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <random>

namespace takt4::dmx {
namespace {

struct RoleName {
    Role role;
    std::string_view label;
    std::string_view name;
};

/// The label is what a dropdown shows; the name is what a settings file holds. They differ
/// only where a space reads better than a hyphen, and a file keeps the hyphen so that
/// `roleOf` never has to cope with one that has been re-cased or re-spaced by an editor.
constexpr std::array<RoleName, 25> kRoleNames{{
    {Role::Unused, "unused", "unused"},
    {Role::Dimmer, "dimmer", "dimmer"},
    {Role::Red, "red", "red"},
    {Role::Green, "green", "green"},
    {Role::Blue, "blue", "blue"},
    {Role::White, "white", "white"},
    {Role::Amber, "amber", "amber"},
    {Role::Uv, "UV", "uv"},
    {Role::Cyan, "cyan", "cyan"},
    {Role::Magenta, "magenta", "magenta"},
    {Role::Yellow, "yellow", "yellow"},
    {Role::Pan, "pan", "pan"},
    {Role::Tilt, "tilt", "tilt"},
    {Role::PanFine, "pan fine", "pan-fine"},
    {Role::TiltFine, "tilt fine", "tilt-fine"},
    {Role::Strobe, "strobe", "strobe"},
    {Role::ColorWheel, "color wheel", "color-wheel"},
    {Role::Gobo, "gobo", "gobo"},
    {Role::Zoom, "zoom", "zoom"},
    {Role::Focus, "focus", "focus"},
    {Role::Speed, "speed", "speed"},
    {Role::Arm, "arm", "arm"},
    {Role::ClipBank, "clip bank", "clip-bank"},
    {Role::ClipSelect, "clip select", "clip-select"},
    {Role::ColorBlend, "color blend", "color-blend"},
}};

// --- the built-in channel maps ------------------------------------------------------
//
// Each is one of the shapes an LED fixture actually takes, not a named product. The parked
// levels beside them are the reason these are worth shipping at all: a moving head with its
// shutter at zero emits nothing however hard a rule drives its dimmer, and an operator
// meeting that on the night has no way to tell it from takt4 not sending.

constexpr std::array<Role, 1> kDimmerChannels{Role::Dimmer};
constexpr std::array<std::uint8_t, 1> kDimmerParked{0};

constexpr std::array<Role, 3> kRgbChannels{Role::Red, Role::Green, Role::Blue};
constexpr std::array<std::uint8_t, 3> kRgbParked{0, 0, 0};

constexpr std::array<Role, 4> kRgbwChannels{Role::Red, Role::Green, Role::Blue, Role::White};
constexpr std::array<std::uint8_t, 4> kRgbwParked{0, 0, 0, 0};

constexpr std::array<Role, 4> kDimmerRgbChannels{Role::Dimmer, Role::Red, Role::Green, Role::Blue};
constexpr std::array<std::uint8_t, 4> kDimmerRgbParked{0, 0, 0, 0};

constexpr std::array<Role, 6> kParChannels{Role::Dimmer, Role::Red,   Role::Green,
                                           Role::Blue,   Role::White, Role::Strobe};
constexpr std::array<std::uint8_t, 6> kParParked{0, 0, 0, 0, 0, 0};

/// A small 8-channel head — 8-bit movement, which is what the cheap ones have.
constexpr std::array<Role, 8> kHead8Channels{Role::Pan,    Role::Tilt, Role::Speed, Role::Dimmer,
                                             Role::Strobe, Role::Red,  Role::Green, Role::Blue};
/// Shutter open, movement at its fastest (0 is "fastest" on nearly every head — the channel
/// is a *delay*), dimmer down, color black. The head is lit and still and ready.
constexpr std::array<std::uint8_t, 8> kHead8Parked{128, 128, 0, 0, 255, 0, 0, 0};

/// The 16-bit shape: pan and tilt each carry a fine byte, which is what makes a slow sweep
/// smooth rather than stepped.
constexpr std::array<Role, 12> kHead16Channels{
    Role::Pan,    Role::PanFine, Role::Tilt,  Role::TiltFine, Role::Speed, Role::Dimmer,
    Role::Strobe, Role::Red,     Role::Green, Role::Blue,     Role::White, Role::Gobo};
constexpr std::array<std::uint8_t, 12> kHead16Parked{128, 0, 128, 0, 0, 0, 255, 0, 0, 0, 0, 0};

constexpr std::array<FixtureMode, 8> kModes{{
    {"dimmer (1ch)", kDimmerChannels, kDimmerParked},
    {"RGB (3ch)", kRgbChannels, kRgbParked},
    {"RGBW (4ch)", kRgbwChannels, kRgbwParked},
    {"dimmer + RGB (4ch)", kDimmerRgbChannels, kDimmerRgbParked},
    {"LED par (6ch)", kParChannels, kParParked},
    {"moving head 8-bit (8ch)", kHead8Channels, kHead8Parked},
    {"moving head 16-bit (12ch)", kHead16Channels, kHead16Parked},
    // Not a lamp: one zone of Pangolin Liberation's DMX Input, the laser software's own profile.
    // See `liberation::kZoneRoles` for its channels and why each is parked where it is.
    {liberation::kZoneModeName, liberation::kZoneRoles, liberation::kZoneParked,
     liberation::kZoneLabels},
}};

} // namespace

std::string_view labelOf(Role role) noexcept {
    for (const RoleName& entry : kRoleNames) {
        if (entry.role == role) {
            return entry.label;
        }
    }
    return "unused";
}

std::string_view nameOf(Role role) noexcept {
    for (const RoleName& entry : kRoleNames) {
        if (entry.role == role) {
            return entry.name;
        }
    }
    return "unused";
}

std::optional<Role> roleOf(std::string_view name) noexcept {
    for (const RoleName& entry : kRoleNames) {
        if (entry.name == name) {
            return entry.role;
        }
    }
    // **The spelling this used to write.** The app said "colour" everywhere until a rig asked
    // for the other spelling on 2026-09-16; a patch saved before that holds `colour-wheel`,
    // and a role that will not read back becomes `Unused` — which is a channel map silently
    // one role short. One line, and it never needs another: nothing writes this name now.
    if (name == "colour-wheel") {
        return Role::ColorWheel;
    }
    return std::nullopt;
}

std::uint16_t channelOf(const Fixture& fixture, Role role, std::size_t nth) noexcept {
    if (role == Role::Unused) {
        return 0;
    }
    for (std::size_t i = 0; i < fixture.channels.size(); ++i) {
        if (fixture.channels[i] == role && nth-- == 0) {
            const std::size_t channel = std::size_t{fixture.address} + i;
            // A fixture patched so that one of its channels falls off the end of the universe
            // has that channel and no way to send it. Reported by `problemWith`; answered
            // here as "no such channel", so that nothing writes past a frame.
            return channel > kChannelsPerUniverse ? 0 : static_cast<std::uint16_t>(channel);
        }
    }
    return 0;
}

std::size_t headsOf(const Fixture& fixture) noexcept {
    const auto pans = std::count(fixture.channels.begin(), fixture.channels.end(), Role::Pan);
    const auto tilts = std::count(fixture.channels.begin(), fixture.channels.end(), Role::Tilt);
    return static_cast<std::size_t>(std::max(pans, tilts));
}

std::string describeHeads(std::uint32_t heads) {
    std::vector<int> numbers;
    for (int head = 0; head < 32; ++head) {
        if ((heads & (std::uint32_t{1} << head)) != 0) {
            numbers.push_back(head + 1);
        }
    }
    if (numbers.empty()) {
        return {};
    }
    std::string text = numbers.size() == 1 ? "head " : "heads ";
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        text += i == 0 ? "" : i + 1 == numbers.size() ? " and " : ", ";
        text += std::to_string(numbers[i]);
    }
    return text;
}

bool emits(const Fixture& fixture) noexcept {
    return std::any_of(kEmitters.begin(), kEmitters.end(),
                       [&fixture](Role role) { return has(fixture, role); });
}

bool aims(const Fixture& fixture, Role role) noexcept {
    if (has(fixture, role)) {
        return true;
    }
    // The one role that reaches a fixture it has no channel for. See `Role::Dimmer` and
    // `DmxEngine::buildFor`: on an RGB par the dimmer is faked out of the color, because
    // that is what brightness *is* on a fixture with no intensity channel.
    return role == Role::Dimmer && emits(fixture);
}

std::uint16_t lastChannelOf(const Fixture& fixture) noexcept {
    if (fixture.channels.empty()) {
        return 0;
    }
    const std::size_t last = std::size_t{fixture.address} + fixture.channels.size() - 1;
    return last > kChannelsPerUniverse ? static_cast<std::uint16_t>(kChannelsPerUniverse)
                                       : static_cast<std::uint16_t>(last);
}

std::string problemWith(const Fixture& fixture) {
    if (fixture.name.empty()) {
        return "no name — give it one so a rule can pick it";
    }
    if (fixture.channels.empty()) {
        return "no channels — pick a mode";
    }
    if (fixture.address < 1 || fixture.address > kChannelsPerUniverse) {
        return "address must be 1 to 512";
    }
    if (std::size_t{fixture.address} + fixture.channels.size() - 1 > kChannelsPerUniverse) {
        return "runs off the end of the universe — " + std::to_string(fixture.channels.size()) +
               " channels from " + std::to_string(fixture.address);
    }
    // A fine byte without its coarse partner is a channel map that has been half edited. It
    // is worth saying, because the symptom is a head that moves in steps for no visible
    // reason — the fine byte is simply never written.
    if (std::find(fixture.channels.begin(), fixture.channels.end(), Role::PanFine) !=
            fixture.channels.end() &&
        !has(fixture, Role::Pan)) {
        return "has pan fine but no pan";
    }
    if (std::find(fixture.channels.begin(), fixture.channels.end(), Role::TiltFine) !=
            fixture.channels.end() &&
        !has(fixture, Role::Tilt)) {
        return "has tilt fine but no tilt";
    }
    if (fixture.panMin > fixture.panMax || fixture.tiltMin > fixture.tiltMax) {
        return "movement limits are inverted";
    }
    return {};
}

FixtureSet resolveFixtures(const std::vector<Fixture>& patch, const std::vector<std::string>& aims) {
    FixtureSet mask;
    if (aims.empty()) {
        return mask; // see the header: no fixtures, not every fixture
    }
    const std::size_t count = std::min(patch.size(), kMaxRoutableFixtures);
    for (std::size_t i = 0; i < count; ++i) {
        const Fixture& fixture = patch[i];
        for (const std::string& aim : aims) {
            if ((!fixture.id.empty() && aim == fixture.id) ||
                (!fixture.group.empty() && aim == fixture.group)) {
                mask.set(i);
                break;
            }
        }
    }
    return mask;
}

const Fixture* findFixture(const std::vector<Fixture>& patch, std::string_view id) noexcept {
    if (id.empty()) {
        return nullptr;
    }
    for (const Fixture& fixture : patch) {
        if (fixture.id == id) {
            return &fixture;
        }
    }
    return nullptr;
}

std::string newFixtureId(const std::vector<Fixture>& patch) {
    static thread_local std::mt19937_64 random{std::random_device{}()};
    for (;;) {
        char text[16] = {};
        std::snprintf(text, sizeof text, "f-%08x", static_cast<unsigned int>(random()));
        std::string id(text);
        if (findFixture(patch, id) == nullptr) {
            return id;
        }
    }
}

void ensureFixtureIds(std::vector<Fixture>& patch) {
    for (std::size_t i = 0; i < patch.size(); ++i) {
        bool taken = patch[i].id.empty();
        for (std::size_t j = 0; j < i && !taken; ++j) {
            taken = patch[j].id == patch[i].id;
        }
        if (taken) {
            patch[i].id = newFixtureId(patch);
        }
    }
}

void aimByIds(std::vector<std::string>& aims, const std::vector<Fixture>& patch,
              const std::vector<bool>* named) {
    std::vector<std::string> out;
    out.reserve(aims.size());
    for (const std::string& aim : aims) {
        if (findFixture(patch, aim) != nullptr) {
            out.push_back(aim);
            continue;
        }
        bool isGroup = false;
        bool found = false;
        for (std::size_t i = 0; i < patch.size(); ++i) {
            const Fixture& fixture = patch[i];
            isGroup = isGroup || (!fixture.group.empty() && fixture.group == aim);
            const bool nameable = named == nullptr || (i < named->size() && (*named)[i]);
            if (nameable && !fixture.id.empty() && fixture.name == aim &&
                std::find(out.begin(), out.end(), fixture.id) == out.end()) {
                // Every fixture of that name: two called the same were both reached before,
                // and still are.
                out.push_back(fixture.id);
                found = true;
            }
        }
        // A label is kept as a label, and so is something that matched nothing at all — a
        // name that was a group and a fixture both keeps reaching the group too.
        if (isGroup || !found) {
            out.push_back(aim);
        }
    }
    aims = std::move(out);
}

std::vector<PortAddress> universesOf(const std::vector<Fixture>& patch) {
    std::vector<PortAddress> universes;
    for (const Fixture& fixture : patch) {
        if (!fixture.enabled || fixture.channels.empty()) {
            continue;
        }
        if (std::find(universes.begin(), universes.end(), fixture.universe) == universes.end()) {
            universes.push_back(fixture.universe);
        }
    }
    std::sort(universes.begin(), universes.end());
    return universes;
}

std::vector<Overlap> overlappingFixtures(const std::vector<Fixture>& patch) {
    std::vector<Overlap> out;
    // Pairwise: a patch is tens of fixtures, and a sweep over channels would need a buffer per
    // universe for an answer this gives in a few hundred comparisons. Switched-off fixtures
    // send nothing, so they overlap nothing.
    for (std::size_t i = 0; i < patch.size(); ++i) {
        const Fixture& a = patch[i];
        if (!a.enabled || a.channels.empty()) {
            continue;
        }
        const std::size_t aEnd = std::size_t{a.address} + a.channels.size(); // one past
        for (std::size_t j = i + 1; j < patch.size(); ++j) {
            const Fixture& b = patch[j];
            if (!b.enabled || b.channels.empty() || b.universe != a.universe) {
                continue;
            }
            const std::size_t bEnd = std::size_t{b.address} + b.channels.size();
            const std::size_t from = std::max<std::size_t>(a.address, b.address);
            if (from < std::min(aEnd, bEnd)) {
                out.push_back(Overlap{i, j, static_cast<std::uint16_t>(from)});
            }
        }
    }
    return out;
}

std::span<const FixtureMode> builtinModes() noexcept {
    return kModes;
}

Fixture fixtureFromMode(std::string_view name, std::size_t mode, PortAddress universe,
                        std::uint16_t address) {
    Fixture fixture;
    fixture.name = std::string(name);
    fixture.universe = universe;
    fixture.address = address;
    // A mode index out of range takes the first, rather than leaving a fixture with no
    // channels — a settings file or a UI that has drifted should still produce something an
    // operator can see and correct.
    const FixtureMode& chosen = kModes[mode < kModes.size() ? mode : 0];
    fixture.channels.assign(chosen.channels.begin(), chosen.channels.end());
    fixture.parked.assign(chosen.parked.begin(), chosen.parked.end());
    for (const std::string_view label : chosen.labels) {
        fixture.labels.emplace_back(label);
    }
    return fixture;
}

} // namespace takt4::dmx
