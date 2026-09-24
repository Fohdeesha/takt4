#pragma once

// What the weight blob and the state space blob have in common: they are read whole, and they
// carry the same checksum. Each file kept its own copy of both until the audit's Low items.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace takt4::io {

/// FNV-1a, 32 bits, over `bytes`: the checksum every takt4 blob carries, and the walk
/// tools/convert_weights.py and tools/dump_statespace.py make when they write one.
std::uint32_t fnv1a32(std::span<const std::byte> bytes) noexcept;

/// The whole of a file, read in one go, for a parser that wants a blob from a file and a blob
/// compiled into the program to go through exactly the same checks. Throws
/// `std::runtime_error` ("<path>: cannot open") when it cannot be opened.
std::vector<std::byte> readWholeFile(const std::filesystem::path& path);

} // namespace takt4::io
