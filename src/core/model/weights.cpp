#include "core/model/weights.hpp"

#include "core/io/blob_file.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The floats are copied straight from the file into float storage, which is only right
// on a little-endian host — the only kind takt4 is built for, as in io/npy_file.cpp.
static_assert(std::endian::native == std::endian::little,
              "model weights assume a little-endian host");

namespace takt4::model {

namespace {

constexpr std::string_view kMagic = "TAKT4WTS";
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderFields = 10; // version, eight dimensions, checksum
constexpr std::size_t kHeaderBytes = kMagic.size() + kHeaderFields * sizeof(std::uint32_t);

std::uint32_t readU32(const unsigned char* at) noexcept {
    return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
           (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}

void expect(std::uint32_t got, std::size_t want, const char* what, const std::string& name) {
    if (got != want) {
        throw std::runtime_error(name + ": " + what + " is " + std::to_string(got) +
                                 ", this build needs " + std::to_string(want));
    }
}

// The blob's blocks of floats, in order. tools/convert_weights.py writes exactly this.
constexpr std::array<std::size_t, 4 + 4 * kLstmLayers + 2> blockSizes() {
    std::array<std::size_t, 4 + 4 * kLstmLayers + 2> sizes{};
    sizes[0] = kConvFilters * kKernelSize;
    sizes[1] = kConvFilters;
    sizes[2] = kHidden * kDenseIn;
    sizes[3] = kHidden;
    for (std::size_t layer = 0; layer < kLstmLayers; ++layer) {
        const std::size_t block = 4 + 4 * layer;
        sizes[block + 0] = kGates * kHidden * kHidden; // weight_ih
        sizes[block + 1] = kGates * kHidden * kHidden; // weight_hh
        sizes[block + 2] = kGates * kHidden;           // bias_ih
        sizes[block + 3] = kGates * kHidden;           // bias_hh
    }
    sizes[sizes.size() - 2] = kNumClasses * kHidden;
    sizes[sizes.size() - 1] = kNumClasses;
    return sizes;
}

constexpr auto kBlockSizes = blockSizes();

constexpr auto blockOffsets() {
    std::array<std::size_t, kBlockSizes.size() + 1> offsets{};
    for (std::size_t i = 0; i < kBlockSizes.size(); ++i) {
        offsets[i + 1] = offsets[i] + kBlockSizes[i];
    }
    return offsets;
}

constexpr auto kBlockOffsets = blockOffsets();
static_assert(kBlockOffsets.back() == kTotalParameters, "the blocks are the whole branch, once");

} // namespace

std::span<const float> ModelWeights::at(std::size_t block) const noexcept {
    return std::span<const float>(values_).subspan(kBlockOffsets[block], kBlockSizes[block]);
}

LstmWeights ModelWeights::lstm(std::size_t layer) const noexcept {
    const std::size_t block = kLstmFirstBlock + kLstmBlocksPerLayer * layer;
    return {at(block + 0), at(block + 1), at(block + 2), at(block + 3)};
}

ModelWeights ModelWeights::fromFile(const std::filesystem::path& path) {
    // Read whole and then parsed, rather than parsed as it streams, so that a blob from a
    // file and a blob compiled into the program go through exactly the same checks. Three
    // megabytes read once at startup is not worth a second implementation to avoid.
    const std::vector<std::byte> bytes = io::readWholeFile(path);
    return fromBytes(bytes, path.string());
}

ModelWeights ModelWeights::fromBytes(std::span<const std::byte> blob, std::string_view from) {
    const std::string name(from);
    const auto* const raw = reinterpret_cast<const unsigned char*>(blob.data());
    if (blob.size() < kHeaderBytes) {
        throw std::runtime_error(name + ": too short to be a takt4 weight blob");
    }
    if (std::memcmp(raw, kMagic.data(), kMagic.size()) != 0) {
        throw std::runtime_error(name + ": not a takt4 weight blob");
    }
    std::array<std::uint32_t, kHeaderFields> fields{};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        fields[i] = readU32(raw + kMagic.size() + i * sizeof(std::uint32_t));
    }
    if (fields[0] != kFormatVersion) {
        throw std::runtime_error(name + ": weight blob format version " +
                                 std::to_string(fields[0]) + ", this build reads version " +
                                 std::to_string(kFormatVersion));
    }
    expect(fields[1], kFeatureDim, "feature dimension", name);
    expect(fields[2], kConvFilters, "convolution filter count", name);
    expect(fields[3], kKernelSize, "kernel size", name);
    expect(fields[4], kDenseIn, "dense input width", name);
    expect(fields[5], kHidden, "LSTM width", name);
    expect(fields[6], kLstmLayers, "LSTM layer count", name);
    expect(fields[7], kNumClasses, "class count", name);
    expect(fields[8], kTotalParameters, "parameter count", name);

    const std::size_t payload = kTotalParameters * sizeof(float);
    if (blob.size() < kHeaderBytes + payload) {
        throw std::runtime_error(name + ": weight blob holds fewer than " +
                                 std::to_string(kTotalParameters) + " parameters");
    }
    if (blob.size() > kHeaderBytes + payload) {
        throw std::runtime_error(name + ": weight blob is longer than its header says");
    }

    ModelWeights weights;
    weights.path_ = name;
    weights.values_.resize(kTotalParameters);
    // Byte-for-byte: the floats were written by a little-endian host and this is one, which
    // is what the static_assert at the top of this file is for.
    std::memcpy(weights.values_.data(), raw + kHeaderBytes, payload);
    const std::uint32_t checksum =
        io::fnv1a32(std::as_bytes(std::span(weights.values_)).first(payload));
    if (checksum != fields[9]) {
        throw std::runtime_error(name + ": weight blob checksum mismatch (file says " +
                                 std::to_string(fields[9]) + ", contents give " +
                                 std::to_string(checksum) + ")");
    }
    return weights;
}

} // namespace takt4::model
