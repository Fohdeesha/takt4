#pragma once

#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Liberation's DMX Input (Liberation, the laser show software: https://liberationlaser.com/), as
// one of its zones sees it: what a Liberation zone is in the patch, and how a clip of its deck
// becomes the two channels that select it.
//
// **The source of truth is Liberation's own "DMX Input Fixture Profiles" document** (dated
// 2026-05-14, shipped with Liberation 1.2.1 Build 96 and unchanged through 1.4.0 Build 103). A
// zone renders only while its Arm channel is at 250 or more, a clip is selected, and its Intensity
// is above zero, and it goes dark two seconds after its data stops. takt4 sends a frame at least
// every `kKeepAliveSeconds`, which is what keeps it lit between changes.
namespace takt4::dmx::liberation {

/// Rows of Liberation's clip deck. A clip is named by its column across the whole deck and its
/// row, from 0 — "21-1" is column 21, row 1 — which is what Liberation prints in a clip's
/// settings header and the only name for a clip an operator can read off the screen.
inline constexpr int kRows = 5;
/// Columns on one page of the deck — one Gobo Bank.
inline constexpr int kColumnsPerBank = 8;
/// Clips on one page: the 40 slots Gobo Select is divided across.
inline constexpr int kSlotsPerBank = kColumnsPerBank * kRows;
/// The widest deck the Gobo Bank channel can reach: 256 banks of 8 columns.
inline constexpr int kMaxColumns = 256 * kColumnsPerBank;
/// The last clip in deck order — see `indexOf`.
inline constexpr int kMaxIndex = kMaxColumns * kRows - 1;

/// One clip of the deck, by Liberation's own name for it.
struct Clip {
    int column = 0;
    int row = 0;
    friend bool operator==(const Clip&, const Clip&) = default;
};

/// A clip's place in **deck order**: down the five rows of a column, then on to the next column —
/// Liberation's own slot order, so "1-1 to 21-1" is every clip between the two as Liberation
/// counts them (the operator's choice of 2026-10-05). 0-0 is 0, 0-4 is 4, 1-0 is 5.
constexpr int indexOf(Clip clip) noexcept {
    return clip.column * kRows + clip.row;
}

/// The clip at a place in deck order. An index out of range is clamped onto the deck.
constexpr Clip clipAt(int index) noexcept {
    const int at = index < 0 ? 0 : index > kMaxIndex ? kMaxIndex : index;
    return Clip{at / kRows, at % kRows};
}

/// "21-1".
std::string format(Clip clip);
inline std::string formatIndex(int index) {
    return format(clipAt(index));
}

/// "21-1", and "21 1" or "21.1" from a hand that did not reach for the dash. Nothing for a row past
/// 4, a column past the deck, or anything that is not two whole numbers.
std::optional<Clip> parseClip(std::string_view text);

/// "1-1 to 21-1", and "1-1 .. 21-1" or "1-1 – 21-1" (an en dash). The two ends as they were given;
/// a range written backwards is the caller's to turn round.
std::optional<std::pair<Clip, Clip>> parseRange(std::string_view text);

/// "1-1 to 21-1".
std::string formatRange(int low, int high);

/// The Gobo Bank and Gobo Select bytes that select a clip.
struct Gobo {
    std::uint8_t bank = 0;
    std::uint8_t select = 0;
    friend bool operator==(const Gobo&, const Gobo&) = default;
};

/// What selects the clip at `index` in deck order: its page, and the **middle** of its slot's band
/// of Gobo Select. Liberation divides 1-255 across 40 slots, about 6.4 values each, so a value at a
/// band's edge would be one rounding away from the neighbouring clip. Slot 1 is 4, slot 20 is 125,
/// slot 40 is 253 — the values the operator's Chataigne module sends.
constexpr Gobo goboOf(int index) noexcept {
    const Clip clip = clipAt(index);
    const int bank = clip.column / kColumnsPerBank;
    const int slot = (clip.column % kColumnsPerBank) * kRows + clip.row + 1;
    // 1 + round((2 * slot - 1) * 255 / 80), in integers: every term is positive.
    const int select = 1 + ((2 * slot - 1) * 255 + 40) / 80;
    return Gobo{static_cast<std::uint8_t>(bank),
                static_cast<std::uint8_t>(select > 255 ? 255 : select)};
}

/// What Liberation makes of the two bytes, as its document gives the decoding — nothing for a
/// Gobo Select of 0, which is no clip. What the tests hold `goboOf` to, and what a readout of the
/// wire says.
std::optional<Clip> decode(std::uint8_t bank, std::uint8_t select) noexcept;

/// Liberation's universe numbering starts at 1, and Art-Net's Port-Address — what takt4's patch
/// shows — at 0: Liberation's universe 1 is takt4's 0. The one place the two meet.
constexpr PortAddress portAddressOf(int liberationUniverse) noexcept {
    return clampPortAddress(liberationUniverse - 1);
}
constexpr int liberationUniverseOf(PortAddress universe) noexcept {
    return static_cast<int>(universe) + 1;
}

/// The Extended 32ch profile's channel count. The Basic 16ch profile is its first sixteen; takt4
/// patches the extended one only (the operator, 2026-10-05).
inline constexpr std::size_t kZoneChannels = 32;

/// One zone's channel map, its parked levels and its channel names, in Liberation's order.
///
/// Parked at Liberation's own recommended defaults except where the operator chose otherwise: Arm,
/// Intensity and the clip all at 0, so a zone is disarmed until a clip fires; and **Colour Blend at
/// 0**, the clip's own colours, where Liberation recommends 255 — which with its white desk colour
/// tints every clip white. Scale is 255 on both axes, the clip at its authored size: 128 is a scale
/// of nothing, and a zone parked there renders nothing however it is armed.
///
/// Position X and Y are the pan and tilt pairs, 16-bit, so takt4's movement effects move a zone's
/// content and its movement window limits how far (Position is -200 to +200, centre at 32768).
inline constexpr std::array<Role, kZoneChannels> kZoneRoles{
    Role::Arm,     Role::Dimmer,     Role::ClipBank, Role::ClipSelect, Role::Red,    Role::Green,
    Role::Blue,    Role::ColorBlend, Role::Zoom,     Role::Unused,     Role::Unused, Role::Pan,
    Role::PanFine, Role::Tilt,       Role::TiltFine, Role::Unused, // rotation
    Role::Unused,  Role::Unused,     Role::Unused,                 // FX 1
    Role::Unused,  Role::Unused,     Role::Unused,                 // FX 2
    Role::Unused,  Role::Unused,     Role::Unused,                 // FX 3
    Role::Unused,  Role::Unused,     Role::Unused,                 // FX 4
    Role::Unused,  Role::Unused,                                   // reserved
    Role::Unused,  Role::Unused};                                  // tempo override
inline constexpr std::array<std::uint8_t, kZoneChannels> kZoneParked{
    0, 0,   0,   0, 255, 255, 255, 0,   255, 255, 255, 128, 0, 128, 0, 128, //
    0, 128, 128, 0, 128, 128, 0,   128, 128, 0,   128, 128, 0, 0,   0, 0};
inline constexpr std::array<std::string_view, kZoneChannels> kZoneLabels{
    "Arm",
    "Intensity",
    "Gobo Bank",
    "Gobo Select",
    "Red",
    "Green",
    "Blue",
    "Colour Blend",
    "Zoom",
    "Scale X",
    "Scale Y",
    "Position X coarse",
    "Position X fine",
    "Position Y coarse",
    "Position Y fine",
    "Rotation",
    "FX 1 Level",
    "FX 1 Parameter 1",
    "FX 1 Parameter 2",
    "FX 2 Level",
    "FX 2 Parameter 1",
    "FX 2 Parameter 2",
    "FX 3 Level",
    "FX 3 Parameter 1",
    "FX 3 Parameter 2",
    "FX 4 Level",
    "FX 4 Parameter 1",
    "FX 4 Parameter 2",
    "Reserved 1",
    "Reserved 2",
    "Tempo override coarse",
    "Tempo override fine",
};

/// The name the patch editor's mode list gives a zone.
inline constexpr std::string_view kZoneModeName = "Liberation zone (32ch)";

/// A zone, patched at `universe` (takt4's numbering) and `address`.
Fixture zone(std::string name, PortAddress universe, std::uint16_t address);

/// Whether `fixture` is a Liberation zone as `zone` builds one — by its channel map, which is the
/// whole of what makes it one. What the preset reuses rather than patching a second time.
bool isZone(const Fixture& fixture) noexcept;

} // namespace takt4::dmx::liberation
