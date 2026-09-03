#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace takt4::io {

/// A two-dimensional array of `T`, row-major (C order), as stored in a numpy `.npy`
/// file: the golden feature matrices of tests/data/features are these.
template <typename T>
struct NpyArray {
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::vector<T> values; // rows × cols

    T at(std::size_t row, std::size_t col) const noexcept { return values[row * cols + col]; }
};

using NpyMatrix = NpyArray<float>;
using NpyInt32Matrix = NpyArray<std::int32_t>;

/// Reads a `.npy` file holding a 2-D little-endian float32 array in C order (format
/// versions 1.0, 2.0 and 3.0). Anything else — including an int32 file — throws
/// std::runtime_error.
NpyMatrix readNpyFloat32(const std::filesystem::path& path);

/// The same for little-endian int32, which is what tools/pf_reference.py writes.
NpyInt32Matrix readNpyInt32(const std::filesystem::path& path);

/// Writes `values` (rows × cols, row-major) as a version 1.0 `.npy` file that numpy
/// loads as a float32 array of shape (rows, cols). Throws std::runtime_error on I/O
/// failure and std::invalid_argument when the size does not match.
void writeNpyFloat32(const std::filesystem::path& path, std::size_t rows, std::size_t cols,
                     std::span<const float> values);

} // namespace takt4::io
