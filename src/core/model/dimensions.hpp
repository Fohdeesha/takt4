#pragma once

#include "core/features/dimensions.hpp"

#include <cstddef>

// The shape of the BeatNet+ network (HANDOFF §5.3). Every number here was read out of
// the published weight files' state_dict rather than taken from the handoff's diagram;
// tools/beatnet_model.py states the same shapes on the Python side and checks them
// against each .pt it loads.

namespace takt4::model {

/// One feature frame in, three class probabilities out, every 20 ms.
inline constexpr std::size_t kFeatureDim = features::kFeatureDim; // 288
inline constexpr std::size_t kNumClasses = 3;                     // beat, downbeat, non-beat

/// Conv1d(1, 2, kernel_size=10), no padding, stride 1, then ReLU and MaxPool1d(2).
/// The pool halves 279 outputs to 139 and drops the odd last one — PyTorch's floor.
inline constexpr std::size_t kConvFilters = 2;
inline constexpr std::size_t kKernelSize = 10;
inline constexpr std::size_t kConvLength = kFeatureDim - kKernelSize + 1; // 279
inline constexpr std::size_t kPooledLength = kConvLength / 2;             // 139
static_assert(kConvLength % 2 == 1, "the pool discards the last convolution output");

/// The flattened pool, filter-major as PyTorch's view() leaves it, into Linear(278, 150).
inline constexpr std::size_t kDenseIn = kConvFilters * kPooledLength; // 278

/// LSTM(150, 150, num_layers=4), unidirectional, its state carried between frames.
inline constexpr std::size_t kHidden = 150;
inline constexpr std::size_t kLstmLayers = 4;
inline constexpr std::size_t kGates = 4; // input, forget, cell, output — PyTorch's order

/// What a whole branch weighs, checked against the blob's header when one is loaded.
inline constexpr std::size_t kConvParameters = kConvFilters * kKernelSize + kConvFilters;
inline constexpr std::size_t kDenseParameters = kHidden * kDenseIn + kHidden;
inline constexpr std::size_t kLstmLayerParameters =
    2 * kGates * kHidden * kHidden + 2 * kGates * kHidden;
inline constexpr std::size_t kOutputParameters = kNumClasses * kHidden + kNumClasses;
inline constexpr std::size_t kTotalParameters =
    kConvParameters + kDenseParameters + kLstmLayers * kLstmLayerParameters + kOutputParameters;
static_assert(kTotalParameters == 767125, "HANDOFF §5.3, and the .pt files themselves");
static_assert(kLstmLayers * kLstmLayerParameters == 724800, "HANDOFF §5.3");

} // namespace takt4::model
