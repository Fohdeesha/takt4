#pragma once

#include "core/features/dimensions.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace takt4::features {

/// One band of madmom's filterbank: a triangle over the STFT bins
/// [firstBin, firstBin + numBins), whose weights start at weightOffset in
/// filterbankWeights(). The weights of a band sum to 1.
struct FilterBand {
    std::uint16_t firstBin;
    std::uint16_t numBins;
    std::uint16_t weightOffset;
};

/// The bands in ascending frequency, and all their weights back to back — madmom's
/// (882 × 144) matrix with the zeros left out. Dumped by tools/dump_filterbank.py into
/// filterbank_table.cpp; never computed here (HANDOFF §5.2, §7.2).
std::span<const FilterBand, kNumBands> filterbankBands() noexcept;
std::span<const float> filterbankWeights() noexcept;

/// bands = magnitudes · filterbank, the matrix product of §5.2. Allocation-free.
void applyFilterbank(std::span<const double, kNumBins> magnitudes, std::span<double, kNumBands> bands) noexcept;

} // namespace takt4::features
