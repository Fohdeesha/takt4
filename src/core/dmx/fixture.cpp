#include "core/dmx/fixture.hpp"

#include <algorithm>
#include <array>

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
constexpr std::array<RoleName, 18> kRoleNames{{
    {Role::Unused, "unused", "unused"},
    {Role::Dimmer, "dimmer", "dimmer"},
    {Role::Red, "red", "red"},
    {Role::Green, "green", "green"},
    {Role::Blue, "blue", "blue"},
    {Role::White, "white", "white"},
    {Role::Amber, "amber", "amber"},
    {Role::Uv, "UV", "uv"},
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

constexpr std::array<FixtureMode, 7> kModes{{
    {"dimmer (1ch)", kDimmerChannels, kDimmerParked},
    {"RGB (3ch)", kRgbChannels, kRgbParked},
    {"RGBW (4ch)", kRgbwChannels, kRgbwParked},
    {"dimmer + RGB (4ch)", kDimmerRgbChannels, kDimmerRgbParked},
    {"LED par (6ch)", kParChannels, kParParked},
    {"moving head 8-bit (8ch)", kHead8Channels, kHead8Parked},
    {"moving head 16-bit (12ch)", kHead16Channels, kHead16Parked},
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

std::uint16_t channelOf(const Fixture& fixture, Role role) noexcept {
    if (role == Role::Unused) {
        return 0;
    }
    for (std::size_t i = 0; i < fixture.channels.size(); ++i) {
        if (fixture.channels[i] == role) {
            const std::size_t channel = std::size_t{fixture.address} + i;
            // A fixture patched so that one of its channels falls off the end of the universe
            // has that channel and no way to send it. Reported by `problemWith`; answered
            // here as "no such channel", so that nothing writes past a frame.
            return channel > kChannelsPerUniverse ? 0 : static_cast<std::uint16_t>(channel);
        }
    }
    return 0;
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
        return "no name — a rule has nothing to aim at";
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

std::uint64_t resolveFixtures(const std::vector<Fixture>& patch,
                              const std::vector<std::string>& names) {
    std::uint64_t mask = 0;
    if (names.empty()) {
        return mask; // see the header: no fixtures, not every fixture
    }
    const std::size_t count = std::min(patch.size(), kMaxRoutableFixtures);
    for (std::size_t i = 0; i < count; ++i) {
        const Fixture& fixture = patch[i];
        for (const std::string& name : names) {
            if (name == fixture.name || (!fixture.group.empty() && name == fixture.group)) {
                mask |= std::uint64_t{1} << i;
                break;
            }
        }
    }
    return mask;
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
    return fixture;
}

} // namespace takt4::dmx
