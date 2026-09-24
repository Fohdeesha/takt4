#include "core/io/blob_file.hpp"

#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>

namespace takt4::io {

std::uint32_t fnv1a32(std::span<const std::byte> bytes) noexcept {
    std::uint32_t hash = 0x811C9DC5u;
    for (const std::byte byte : bytes) {
        hash = (hash ^ static_cast<std::uint32_t>(byte)) * 0x01000193u;
    }
    return hash;
}

std::vector<std::byte> readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error(path.string() + ": cannot open");
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        throw std::runtime_error(path.string() + ": cannot tell how long it is");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!bytes.empty() &&
        !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        throw std::runtime_error(path.string() + ": could not be read whole");
    }
    return bytes;
}

} // namespace takt4::io
