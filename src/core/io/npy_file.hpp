#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace takt4::io {

/// A two-dimensional float32 array, row-major (C order), as stored in a numpy `.npy`
/// file: the golden feature matrices of tests/data/features are these.
struct NpyMatrix {
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::vector<float> values; // rows × cols

    float at(std::size_t row, std::size_t col) const noexcept { return values[row * cols + col]; }
};

/// Reads a `.npy` file holding a 2-D little-endian float32 array in C order (format
/// versions 1.0, 2.0 and 3.0). Anything else throws std::runtime_error.
NpyMatrix readNpyFloat32(const std::filesystem::path& path);

/// Writes `values` (rows × cols, row-major) as a version 1.0 `.npy` file that numpy
/// loads as a float32 array of shape (rows, cols). Throws std::runtime_error on I/O
/// failure and std::invalid_argument when the size does not match.
void writeNpyFloat32(const std::filesystem::path& path, std::size_t rows, std::size_t cols,
                     std::span<const float> values);

} // namespace takt4::io
