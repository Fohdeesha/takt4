#include "core/features/filterbank.hpp"

namespace takt4::features {

void applyFilterbank(std::span<const double, kNumBins> magnitudes, std::span<double, kNumBands> bands) noexcept {
    const std::span<const float> weights = filterbankWeights();
    std::size_t b = 0;
    for (const FilterBand& band : filterbankBands()) {
        double sum = 0.0;
        for (std::size_t i = 0; i < band.numBins; ++i) {
            sum += magnitudes[band.firstBin + i] * static_cast<double>(weights[band.weightOffset + i]);
        }
        bands[b++] = sum;
    }
}

} // namespace takt4::features
