#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Fixture definitions read from files — GDTF and the Open Fixture Library — and turned into
/// what the patch holds.
///
/// **The import is a projection.** A `dmx::Fixture` is a universe, a start address, one role per
/// channel and a parked level per channel, and that is all `DmxEngine` reads. A definition is far
/// richer: channel functions per DMX range, wheels, geometry, cells, relations. Turning one into
/// the other loses most of it, so the work is split where the loss happens:
///
///   file bytes ─reader (GDTF | OFL)─▶ Definition ─mapper─▶ FixtureProfile ─▶ the preset's library
///
/// A **reader** resolves its format's own structure (geometry references, fine aliases,
/// switching channels, matrices, value resolutions) into the `Definition` here and decides what
/// each channel *is* — a `Kind`, richer than a role. It never decides what takt4 does with it.
/// The **mapper** (`profile_mapper.hpp`) holds that policy, once, for both formats.
///
/// **The result is a pure function of the file's bytes**: no locale, no clock, no file name
/// (the identity comes from inside the file), document order wherever order matters. The only
/// randomness is the ids takt4 generates, which nothing compares by content.
namespace takt4::fixtures {

/// What a definition says one DMX channel is, before any policy. See the mapper for which of
/// these become roles.
enum class Kind : std::uint8_t {
    /// An address no channel claims, or a channel the file leaves empty.
    Nothing,
    Dimmer,
    Red,
    Green,
    Blue,
    White,
    WarmWhite,
    CoolWhite,
    Amber,
    Uv,
    /// The flags of a subtractive (CMY) head.
    CyanSub,
    MagentaSub,
    YellowSub,
    /// An emitter takt4 has no role for: lime, an additive cyan, blue-green, indigo…
    OtherEmitter,
    /// Red, green and blue of an "indirect" colour model — the fixture mixes from them itself.
    IndirectRed,
    IndirectGreen,
    IndirectBlue,
    /// Hue, saturation, CIE x/y: a colour takt4 cannot set.
    HsbOrCie,
    /// The brightness of an HSB or CIE colour.
    ColorBrightness,
    Pan,
    Tilt,
    Shutter,
    /// Strobe rate or duration on a channel of its own.
    StrobeRate,
    ColorWheel,
    Gobo,
    Zoom,
    Focus,
    PanTiltSpeed,
    Other,
};

/// The kind's name in the tests and the sweep's report — not shown to an operator.
std::string_view nameOf(Kind kind) noexcept;

/// One DMX address of a mode, with what the reader made of it.
struct DefChannel {
    Kind kind = Kind::Nothing;
    /// 0 for the coarse byte, 1 for the first fine byte, 2 for the next…
    std::uint8_t byte = 0;
    /// How many bytes this channel has in this mode — 2 for a 16-bit pan whose fine byte the
    /// mode lists, 1 when it lists only the coarse one. Every value below is at this resolution.
    std::uint8_t resolutionBytes = 1;
    /// What the file calls it: "dimmer · Beam 1", "Red 3", "Pan fine". At most 80 characters.
    std::string label;
    /// "" for the whole fixture, else the cell, pixel or group it belongs to.
    std::string cell;
    /// The file's default, at `resolutionBytes`, or nothing when the file gives none.
    std::optional<std::uint64_t> defaultValue;
    /// What stands in for a missing default on a channel parked at "the file's default": the
    /// initial function's start in a GDTF file, 0 in an OFL one.
    std::uint64_t fallbackValue = 0;
    /// For a shutter: the value that opens it, or nothing when the file names no open state.
    std::optional<std::uint64_t> openValue;
    /// The kind covers only part of the channel's range, and something else the rest — a
    /// "shutter / dimmer" channel. Driving it would drive it into that something else.
    bool shared = false;
    /// What it shares the range with, for the note that says so.
    std::string sharedWith;
    /// For colour wheels, gobo wheels, zooms and focuses: which one (1-based) — the mapper
    /// drives the lowest present and parks the rest.
    std::uint32_t ordinal = 0;
    /// Anything the reader found worth saying about this channel ("the library marks this as
    /// needing checking: …", "does several things (dimmer, red)").
    std::vector<std::string> notes;

    friend bool operator==(const DefChannel&, const DefChannel&) = default;
};

/// One mode — what the file calls a DMX mode or personality.
struct DefMode {
    /// Unique within the definition: a name the file repeats gets " (2)", " (3)" in document
    /// order.
    std::string name;
    /// One part for a mode with one start address, two for a mode that needs two. Index = the
    /// address within the part, minus one.
    std::vector<std::vector<DefChannel>> parts;
    /// Oddities the reader resolved ("channel 1 defined twice — the first kept").
    std::vector<std::string> notes;
    /// Empty, or why the mode cannot be imported. A refused mode has no parts.
    std::string refusal;

    friend bool operator==(const DefMode&, const DefMode&) = default;
};

struct Definition {
    /// "gdtf" or "ofl".
    std::string format;
    /// What identifies the fixture type across files and revisions: the GDTF FixtureTypeID,
    /// upper-cased, or the OFL "manufacturer-key/fixture-key".
    std::string key;
    std::string manufacturer;
    std::string model;
    std::string revision;
    /// The file's name, for the operator's reference only — nothing is identified by it.
    std::string fileName;
    /// File-level notes: a version read as a neighbouring one, a library flag, no identity.
    std::vector<std::string> notes;
    std::vector<DefMode> modes;

    friend bool operator==(const Definition&, const Definition&) = default;
};

/// What reading a file gave: a definition, or why there is none. A file is refused only when
/// it cannot be read at all; a mode that cannot be imported is a `DefMode::refusal` instead.
struct ReadResult {
    std::optional<Definition> definition;
    std::string problem;
};

} // namespace takt4::fixtures
