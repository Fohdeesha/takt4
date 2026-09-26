#pragma once

#include "core/dmx/artnet_packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::dmx {

/// What one DMX channel of a fixture does.
///
/// **The patch exists so that a rule can say "the moving heads, random position" instead of
/// "universe 0, channel 11, and also channel 13, and the fine ones at 12 and 14".** That is
/// the whole argument for this file. An operator asked for color on three channels, random
/// pan and tilt on a moving head, and preset movement — none of which can be said in channel
/// numbers without the operator holding the fixture's manual open while they build every
/// rule, and re-checking all of them the day a fixture is re-addressed.
///
/// A deliberately short list. Every role here is one a *rule* can aim an effect at; a fixture
/// channel that only ever gets parked at a value — a color wheel left on open white, a
/// dimmer curve selector, a lamp-control channel — is `Unused` as far as effects are
/// concerned and is set from the fixture's own parked levels instead. Adding a role is cheap;
/// adding one nothing can aim at is a longer dropdown for no gesture.
enum class Role : std::uint8_t {
    /// Not addressable by an effect. The default, and what a channel map pads with — a
    /// fixture whose channel 5 is "reset/macro" has a role for it so the *count* stays right
    /// and the channels after it land where the manual says they do.
    Unused,
    /// Master intensity. Effects that talk about a "level" mean this one unless told
    /// otherwise, and a fixture without one has its color scaled instead — the **virtual
    /// dimmer**, which `DmxEngine::buildFor` builds and `aims` answers for. Most LED pars
    /// have no intensity channel: their brightness is their color.
    Dimmer,
    Red,
    Green,
    Blue,
    White,
    Amber,
    Uv,
    /// Coarse pan and tilt, 0-255 across the head's whole travel.
    Pan,
    Tilt,
    /// The low byte of a 16-bit pan or tilt. **Optional and detected**: a head with these
    /// gets 65,536 positions and moves smoothly across a slow sweep; one without gets 256 and
    /// steps. An effect never asks which it is: `DmxEngine` looks, and writes the fine byte
    /// beside the coarse one wherever the patch has one.
    PanFine,
    TiltFine,
    /// Shutter / strobe rate. A fixture's own strobe, which is not the same thing as takt4
    /// strobing the dimmer: the fixture's is faster and cleaner, and the rule can use either.
    Strobe,
    ColorWheel,
    Gobo,
    Zoom,
    Focus,
    /// Pan/tilt movement speed, where the fixture has one. takt4 moves a head by sending it
    /// positions on a ramp, so this usually wants parking at "fastest" — which is what a
    /// fixture's parked levels are for — but a rule can sweep it for a deliberate lag.
    Speed,
};

/// Every role, in the order a channel-map dropdown should offer them: `Unused` first,
/// because it is the default and the commonest answer for a channel nothing aims at, then
/// intensity, then color, then movement, then the rest.
inline constexpr std::array<Role, 18> kRoles{
    Role::Unused, Role::Dimmer,      Role::Red,  Role::Green, Role::Blue,    Role::White,
    Role::Amber,  Role::Uv,          Role::Pan,  Role::Tilt,  Role::PanFine, Role::TiltFine,
    Role::Strobe, Role::ColorWheel, Role::Gobo, Role::Zoom,  Role::Focus,   Role::Speed};

/// The roles an *effect* may be aimed at — `kRoles` without `Unused`, which is not a target,
/// and without the two fine bytes, which are written by their coarse partner rather than on
/// their own. What the rule editor's "channel" dropdown offers.
inline constexpr std::array<Role, 15> kAimableRoles{
    Role::Dimmer,      Role::Red,  Role::Green, Role::Blue,  Role::White,
    Role::Amber,       Role::Uv,   Role::Pan,   Role::Tilt,  Role::Strobe,
    Role::ColorWheel, Role::Gobo, Role::Zoom,  Role::Focus, Role::Speed};

std::string_view labelOf(Role role) noexcept;
/// The word a settings file spells the role with, and what `roleOf` reads back.
std::string_view nameOf(Role role) noexcept;
std::optional<Role> roleOf(std::string_view name) noexcept;

/// True for the three an RGB color is written to.
constexpr bool isColor(Role role) noexcept {
    return role == Role::Red || role == Role::Green || role == Role::Blue;
}

/// True for a role that is the low byte of a 16-bit pair, and so is never aimed at directly.
constexpr bool isFine(Role role) noexcept {
    return role == Role::PanFine || role == Role::TiltFine;
}

/// One lighting fixture, patched.
///
/// **Addresses are 1-based**, because that is what is printed on the back of the fixture and
/// set on its display. Everything inside a universe buffer is 0-based, and `channelOf` is the
/// one place the two meet — which is deliberate, since an off-by-one here is a rig where
/// every color is on the wrong component and nothing says why.
struct Fixture {
    /// What a rule aims at it by: generated once (`newFixtureId`), saved with it, never shown
    /// or edited. **Not the name** — a rule aimed at a fixture by name stopped reaching it the
    /// moment it was renamed (the audit's M28, and the operator's call of 2026-09-23 that
    /// nothing may hang off a name somebody can edit). Unique within a patch.
    std::string id;
    /// What the patch editor and the rule editor show. The operator's to change.
    std::string name;
    /// An optional label a rule may aim at, shared by any number of fixtures — "heads",
    /// "washes", "floor". A rule aimed at a group reaches every fixture carrying the label, so
    /// re-patching a rig is editing the patch rather than editing every rule.
    ///
    /// A rule holds the label itself rather than an id, and that is not the thing the id
    /// above fixes: a group is not a thing with a name, it is the label. Changing one fixture's
    /// label takes that fixture out of the group, which is what the gesture means.
    std::string group;

    /// Which universe, as a flat Port-Address. See `PortAddress`.
    PortAddress universe = 0;
    /// The fixture's own start address, 1 to 512, exactly as it is set on the fixture.
    std::uint16_t address = 1;

    /// What each channel from `address` onwards does. `channels[0]` is `address`,
    /// `channels[1]` is `address + 1`, and so on — so this is both the channel map *and* the
    /// fixture's footprint, and the two cannot disagree.
    std::vector<Role> channels;

    /// The part of the head's travel a rule may use, as fractions of its full range.
    ///
    /// **This is a safety limit, not a preference.** A moving head told to pan at random
    /// across its whole travel will at some point point straight at the audience, and at a
    /// small venue that is a beam in somebody's eyes. An operator sets the window once from
    /// the stage and every random position and every path stays inside it. Defaults to the
    /// whole range, because a fixture that has not been limited must not silently behave as
    /// though it has been.
    double panMin = 0.0;
    double panMax = 1.0;
    double tiltMin = 0.0;
    double tiltMax = 1.0;

    /// Levels this fixture sits at when takt4 starts and whenever nothing is driving them —
    /// the shutter that has to be open before a dimmer means anything, the color wheel on
    /// open white, the movement speed on fastest. One entry per channel of `channels`,
    /// shorter or longer being read as zero-filled or cut.
    ///
    /// Without these a patch is not usable: most moving heads have a shutter channel that
    /// must be at some value before any light comes out at all, and a rig where the dimmer
    /// rule "does nothing" because channel 5 is at zero is the single most common way an
    /// operator loses an evening.
    std::vector<std::uint8_t> parked;

    /// Switched off contributes no channels and is reached by no rule — the fixture is on the
    /// truss but out of the show tonight. A switch rather than deleting it, for the reason
    /// `output::OutputTarget::enabled` gives.
    bool enabled = true;

    friend bool operator==(const Fixture&, const Fixture&) = default;
};

/// How many fixtures a rule can be routed to.
///
/// A rule carries its fixtures as a bit each (`trigger::Message::fixtures`), which is what
/// keeps a fire allocation-free and a follow-up safe to hold after the patch has been
/// replaced — the same argument `output::kMaxRoutableTargets` makes, and the same number.
/// Fixtures past it are patched, shown and driven by their own rules; what they cannot be is
/// *named* by one. `resolveFixtures` says so rather than failing quietly.
inline constexpr std::size_t kMaxRoutableFixtures = 64;

/// The 1-based DMX channel carrying `role`, or 0 when the fixture has no such channel.
///
/// Zero is a usable "no" because DMX channels count from one: there is no channel 0 to
/// confuse it with, which is why the addresses here are 1-based in the first place.
///
/// `nth` counts the channels that carry the role, from 0: a bar of four RGB cells has four
/// reds, and every one of them is driven (`DmxEngine`). It used to be the first only, so such
/// a bar lit one cell and Blackout left the others lit (the 2026-09-25 audit's L3).
std::uint16_t channelOf(const Fixture& fixture, Role role, std::size_t nth = 0) noexcept;

/// Whether the fixture has that channel at all.
inline bool has(const Fixture& fixture, Role role) noexcept {
    return channelOf(fixture, role) != 0;
}

/// Every role that puts light out, in the order a color is mixed from them. What a
/// `Blackout` drives to zero and what a **virtual dimmer** drives instead of a dimmer.
inline constexpr std::array<Role, 6> kEmitters{Role::Red,   Role::Green, Role::Blue,
                                               Role::White, Role::Amber, Role::Uv};

/// Whether the fixture emits light on any channel but a dimmer — the LED par's shape, where
/// brightness *is* color and there is no master intensity at all.
bool emits(const Fixture& fixture) noexcept;

/// Whether an effect aimed at `role` reaches this fixture.
///
/// The same question as `has` for every role but one. **A level aimed at `Role::Dimmer`
/// reaches a fixture that has no dimmer channel**, because `DmxEngine` fakes one out of the
/// color channels — see `Role::Dimmer`, which has always promised this. Most LED pars are
/// exactly that shape, so without it the commonest fixture on any rig answered the commonest
/// rule by staying dark.
bool aims(const Fixture& fixture, Role role) noexcept;

/// The last channel this fixture occupies, or 0 when it occupies none.
std::uint16_t lastChannelOf(const Fixture& fixture) noexcept;

/// Empty when the fixture is usable, and otherwise why it is not — for the patch editor to
/// show. Same policy as `trigger::Rule::problem`: an invalid fixture is held, edited and
/// saved, and only refuses to be driven.
std::string problemWith(const Fixture& fixture);

/// The bits for `aims` within `patch` — each a fixture's id or a group's label, contributing
/// every fixture it matches.
///
/// **An empty list is no fixtures, which is the opposite of what an empty output list
/// means**, and the difference is deliberate. A rule that names no output means "every
/// output", because that is what a rule written before there was more than one target meant
/// and sending a clip change to two media servers is harmless. A rule that named no fixture
/// and was read as "every fixture" would swing every moving head in the building the moment
/// an operator added it. So an unrouted DMX rule reaches nothing and `trigger::Rule` reports
/// it as a problem, which is a rule that visibly does not work rather than a rig that
/// visibly does the wrong thing.
///
/// An entry that matches nothing contributes no bit and is kept — a preset written on a rig
/// with "heads" opened on one without should still say "heads", so that plugging the rig back
/// in restores the routing.
std::uint64_t resolveFixtures(const std::vector<Fixture>& patch,
                              const std::vector<std::string>& aims);

/// A fresh fixture id no fixture in `patch` has — random, for `output::newOutputId`'s reason.
std::string newFixtureId(const std::vector<Fixture>& patch);

/// Gives every fixture without an id one, and a second fixture holding an id already taken a
/// new one.
void ensureFixtureIds(std::vector<Fixture>& patch);

/// Re-points what a rule aims at by fixture name at those fixtures' ids: what every settings
/// file written before fixtures had ids holds. A group label stays a label, an entry that is
/// already an id stays, and one that matches nothing is kept as it is.
void aimByIds(std::vector<std::string>& aims, const std::vector<Fixture>& patch);

/// The fixture with this id, or null.
const Fixture* findFixture(const std::vector<Fixture>& patch, std::string_view id) noexcept;

/// Every distinct universe the patch uses, ascending. What an Art-Net target with no explicit
/// universe list carries, and what the frame buffers are built from.
std::vector<PortAddress> universesOf(const std::vector<Fixture>& patch);

/// Two fixtures of the patch whose channels share at least one DMX channel on one universe:
/// their indices, and the first channel they share.
struct Overlap {
    std::size_t first = 0;
    std::size_t second = 0;
    std::uint16_t channel = 0;
};

/// Every pair of fixtures that overlap, in patch order. Two fixtures on the same channels
/// drive each other's lamps — a par that dims when the head beside it pans — and nothing said
/// so (the audit's M21); the patch editor shows the first of these.
std::vector<Overlap> overlappingFixtures(const std::vector<Fixture>& patch);

/// A ready-made channel map, for the patch editor's "mode" dropdown.
///
/// Not a fixture library — there are thousands of fixtures and takt4 is not going to ship a
/// database of them. These are the handful of *shapes* nearly every LED fixture takes, so
/// that patching a 4-channel RGB par is picking one line rather than setting four dropdowns,
/// and patching something unusual is picking the nearest and editing it.
struct FixtureMode {
    std::string_view name;
    std::span<const Role> channels;
    /// What the channels sit at when nothing is driving them — `Fixture::parked`. Mostly
    /// zeroes; the moving-head modes open the shutter and set movement to its fastest, which
    /// is the difference between a head that responds and one that appears dead.
    std::span<const std::uint8_t> parked;
};

std::span<const FixtureMode> builtinModes() noexcept;

/// A fixture built from one of `builtinModes()`, at `universe` and `address`. The mode's
/// channel map and parked levels, and nothing else touched.
Fixture fixtureFromMode(std::string_view name, std::size_t mode, PortAddress universe,
                        std::uint16_t address);

} // namespace takt4::dmx
