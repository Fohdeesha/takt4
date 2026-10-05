#pragma once

#include "core/fixtures/definition.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace takt4::fixtures {

/// The largest description.xml takt4 reads. The largest of the 145 example files is 5.8 MB.
inline constexpr std::size_t kGdtfDescriptionLimit = 32u * 1024u * 1024u;
/// The largest .gdtf archive it opens at all — models and images included, none of which is
/// read. The largest example is 15 MB.
inline constexpr std::uintmax_t kGdtfArchiveLimit = 512u * 1024u * 1024u;

/// A GDTF file (a zip archive) read into a `Definition`. A file is refused — `problem` set — only
/// when it cannot be read at all: not an archive, no description.xml, not XML, not GDTF, a major
/// version this does not know. Anything less is a mode refused or a note.
ReadResult readGdtfFile(const std::filesystem::path& path);

/// The same from an archive in memory. `fileName` is kept for the operator's reference only.
ReadResult readGdtfArchive(std::span<const std::uint8_t> archive, std::string_view fileName);

/// The same from the text of description.xml itself.
ReadResult readGdtfDescription(std::string_view xml, std::string_view fileName);

namespace gdtf {

/// A GDTF DMXValue ("255/1", "32768/2", "128/1s") at a channel of `bytes` bytes, or nothing when
/// it is not one.
///
/// `v/n` converts by **byte mirroring**: copies of v's low byte appended, so 255/1 on a 16-bit
/// channel is 65535 and 128/1 is 32896. `v/ns` by **byte shifting**: 255/1s on 16 bits is 65280.
/// An n larger than the channel keeps the top bytes. A bare number is read as `v/1`. A v too
/// large for its own n is clamped to the largest it can be, and `clamped` is set.
std::optional<std::uint64_t> parseDmxValue(std::string_view text, unsigned bytes, bool& clamped);

} // namespace gdtf

} // namespace takt4::fixtures
