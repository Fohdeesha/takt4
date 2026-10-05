#pragma once

#include "core/fixtures/definition.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

namespace takt4::fixtures {

/// The largest Open Fixture Library file takt4 reads. The largest of the 91 examples is 94 KB.
inline constexpr std::size_t kOflFileLimit = 8u * 1024u * 1024u;

/// An Open Fixture Library fixture (.json) read into a `Definition`, with OFL's own model —
/// fine channels, switching channels, matrices and template channels — resolved as its
/// `lib/model` does. A file is refused only when it is not an OFL fixture at all; a mode with a
/// channel the file does not define is refused on its own.
///
/// The identity is the website download's `manufacturerKey/fixtureKey`; a file from the
/// repository has neither, and its folder and file name stand in, which is the repository's
/// layout (`fixtures/<manufacturer>/<fixture>.json`).
ReadResult readOflFile(const std::filesystem::path& path);

/// The same from the file's text. `folder` and `stem` are the identity a repository file does
/// not carry (its folder's name and its own name without ".json").
ReadResult readOflText(std::string_view json, std::string_view fileName, std::string_view folder,
                       std::string_view stem);

namespace ofl {

/// OFL's `scaleDmxValue`: a value at `from` bytes as `to` bytes — the last byte repeated
/// upwards, bytes dropped downwards.
std::uint64_t scaleValue(std::uint64_t value, unsigned from, unsigned to);

/// OFL's `scaleDmxRange`: the start padded with 0 and the end with 255 upwards; downwards both
/// cut, the start rounded up when a byte cut from it was not zero (and the range stays a range).
std::pair<std::uint64_t, std::uint64_t> scaleRange(std::uint64_t start, std::uint64_t end,
                                                   unsigned from, unsigned to);

} // namespace ofl

} // namespace takt4::fixtures
