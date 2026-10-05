#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace takt4::fixtures {

/// One member of a zip archive, by its exact name at the root, or nothing with `problem` saying
/// why in words an operator can read ("it has no description.xml", "description.xml is
/// encrypted").
///
/// **Only that member is ever inflated.** A GDTF archive carries 3D models and wheel images
/// beside the one file takt4 reads — the largest example is 15 MB — and none of it is touched.
/// The member's size is checked against `limit` twice: from the archive's central directory
/// before anything is inflated, and again byte by byte while inflating, so an archive that lies
/// about its size (a zip bomb) stops at the limit rather than at the end of memory.
///
/// Stored and deflated members only; encrypted ones are refused. Never throws.
std::optional<std::string> readZipMember(std::span<const std::uint8_t> archive,
                                         std::string_view name, std::size_t limit,
                                         std::string& problem);

/// The same, read from a file through takt4's own handle — the archive is never read whole.
std::optional<std::string> readZipMemberFromFile(const std::filesystem::path& path,
                                                 std::string_view name, std::size_t limit,
                                                 std::string& problem);

} // namespace takt4::fixtures
