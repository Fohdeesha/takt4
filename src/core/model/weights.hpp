#pragma once

#include "core/model/dimensions.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::model {

/// One LSTM layer's parameters, exactly as PyTorch stores them: the gates in the order
/// input, forget, cell, output, and the two bias vectors kept apart. Whatever the
/// inference engine wants instead is its own business (see beat_model.cpp).
struct LstmWeights {
    std::span<const float> weightIh; ///< (4 · kHidden) × kHidden, row-major
    std::span<const float> weightHh; ///< (4 · kHidden) × kHidden, row-major
    std::span<const float> biasIh;   ///< 4 · kHidden
    std::span<const float> biasHh;   ///< 4 · kHidden
};

/// One BeatNet+ weight set, read from an `assets/weights/*.bin` blob written by
/// tools/convert_weights.py. The numbers are PyTorch's, untouched by the conversion.
///
/// Reading one opens a file and allocates; nothing here belongs on the audio thread.
/// The spans point into this object, so it has to outlive every BeatModel built on it.
class ModelWeights {
public:
    /// Reads and validates the blob. Throws std::runtime_error on anything wrong with
    /// it: missing, truncated, wrong magic, a format version this build does not know,
    /// dimensions that are not this architecture's, or a checksum mismatch.
    static ModelWeights fromFile(const std::filesystem::path& path);

    /// The same, from bytes already in memory — the application's own copy, compiled in by
    /// `assets::weights()` so the program is one file. `from` is what `path()` will say and
    /// what an error message will name, since there is no file to point at.
    static ModelWeights fromBytes(std::span<const std::byte> blob, std::string_view from);

    /// Conv1d(1, kConvFilters, kKernelSize): kConvFilters × kKernelSize, row-major.
    std::span<const float> convWeight() const noexcept { return at(0); }
    std::span<const float> convBias() const noexcept { return at(1); }

    /// Linear(kDenseIn, kHidden): kHidden × kDenseIn, row-major — output-major, as
    /// PyTorch's `weight` is.
    std::span<const float> denseWeight() const noexcept { return at(2); }
    std::span<const float> denseBias() const noexcept { return at(3); }

    /// Layer `layer` of the stacked LSTM, counted from the input.
    LstmWeights lstm(std::size_t layer) const noexcept;

    /// Linear(kHidden, kNumClasses): kNumClasses × kHidden, row-major.
    std::span<const float> outputWeight() const noexcept { return at(kBlockCount - 2); }
    std::span<const float> outputBias() const noexcept { return at(kBlockCount - 1); }

    /// Where it came from, for logs and for the CLI to print.
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    /// The blob is these blocks of floats back to back, in this order. Same order as
    /// tools/convert_weights.py's PARAMETER_ORDER, which is what writes them.
    static constexpr std::size_t kBlockCount = 4 + 4 * kLstmLayers + 2;
    static constexpr std::size_t kLstmFirstBlock = 4;
    static constexpr std::size_t kLstmBlocksPerLayer = 4;

    std::span<const float> at(std::size_t block) const noexcept;

    std::filesystem::path path_;
    std::vector<float> values_;
};

} // namespace takt4::model
