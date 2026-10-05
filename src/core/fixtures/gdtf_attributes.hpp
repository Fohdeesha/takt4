#pragma once

#include "core/fixtures/definition.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// What a GDTF attribute name means: the words a channel is labelled with, and the `Kind` it is
/// read as. GDTF names attributes in its Annex A ("Dimmer", "ColorAdd_RY", "Gobo2Pos"), with (n)
/// and (m) standing for any positive number; a file may also define attributes of its own
/// ("Green / Magenta", "Favorite"). **Attribute names are compared without regard to case**, as
/// Annex B says they are.
namespace takt4::fixtures::gdtf {

/// The words for an attribute: Annex A's from a fixed table with the numbers carried through
/// ("Shutter1Strobe" → "strobe 1", "Gobo2Pos" → "gobo 2 rotation"), and any other as the file
/// writes it with its CamelCase split ("ColorTemperatureCTO" → "Color Temperature CTO").
std::string labelOfAttribute(std::string_view attribute);

/// Whether the name is one of Annex A's, with numbers where it takes them.
bool isAnnexAttribute(std::string_view attribute);

/// The kind an attribute is read as when it is a channel's first logical channel's. `shutterInMode`
/// says whether the mode has a plain `Shutter(n)` channel: a `Shutter(n)Strobe…` channel is the
/// shutter only where there is none (a strobe-only channel), and a strobe rate beside one.
Kind kindOfAttribute(std::string_view attribute, bool shutterInMode, std::uint32_t& ordinal);

/// `Shutter(n)` exactly — the plain shutter, open or closed.
bool isShutterAttribute(std::string_view attribute);
/// `Shutter(n)Strobe`, `Shutter(n)StrobeRandom`, `Shutter(n)StrobeEffect`, …
bool isShutterStrobeAttribute(std::string_view attribute);
/// `NoFeature`.
bool isNoFeature(std::string_view attribute);

} // namespace takt4::fixtures::gdtf
