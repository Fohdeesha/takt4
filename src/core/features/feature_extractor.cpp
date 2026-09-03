#include "core/features/feature_extractor.hpp"

#include "core/features/filterbank.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace takt4::features {

namespace {

// numpy.hanning(M), term for term: 0.5 + 0.5 cos(π n / (M − 1)) for n = 1 − M, 3 − M, … ,
// M − 1. The same values as the textbook 0.5 − 0.5 cos(2πk / (M − 1)) up to rounding;
// numpy's own expression is used so that not even that differs.
std::vector<double> hanning(std::size_t length) {
    std::vector<double> window(length);
    const double denominator = static_cast<double>(length - 1);
    for (std::size_t k = 0; k < length; ++k) {
        const double n = static_cast<double>(2 * k + 1) - static_cast<double>(length);
        window[k] = 0.5 + 0.5 * std::cos(std::numbers::pi * n / denominator);
    }
    return window;
}

} // namespace

FeatureExtractor::FeatureExtractor()
    : fft_(kFrameSize), window_(hanning(kFrameSize)), windowed_(kFrameSize), spectrum_(fft_.numBins()) {
}

void FeatureExtractor::reset() noexcept {
    history_.fill(0.0f);
    previousLog_.fill(0.0);
    havePrevious_ = false;
    hopsPushed_ = 0;
    frameIndex_ = 0;
    frame_.fill(0.0f);
}

bool FeatureExtractor::pushHop(std::span<const float, audio::kHopSize> hop) noexcept {
    return push(hop);
}

bool FeatureExtractor::flush() noexcept {
    static constexpr std::array<float, audio::kHopSize> kSilence{};
    return push(kSilence);
}

bool FeatureExtractor::push(std::span<const float, audio::kHopSize> hop) noexcept {
    // Slide the frame on by one hop: the oldest hop drops off the front.
    std::memmove(history_.data(), history_.data() + audio::kHopSize, (kFrameSize - audio::kHopSize) * sizeof(float));
    std::copy(hop.begin(), hop.end(), history_.end() - static_cast<std::ptrdiff_t>(audio::kHopSize));
    ++hopsPushed_;
    // Hop j completes frame j − kLatencyHops; the first kLatencyHops hops complete none.
    if (hopsPushed_ <= kLatencyHops) {
        return false;
    }
    frameIndex_ = hopsPushed_ - 1 - kLatencyHops;
    computeFrame();
    return true;
}

void FeatureExtractor::computeFrame() noexcept {
    for (std::size_t n = 0; n < kFrameSize; ++n) {
        windowed_[n] = static_cast<double>(history_[n]) * window_[n];
    }
    fft_.forward(windowed_, spectrum_);
    for (std::size_t k = 0; k < kNumBins; ++k) { // the Nyquist bin is dropped, as madmom drops it
        magnitudes_[k] = std::hypot(spectrum_[k].real(), spectrum_[k].imag());
    }
    applyFilterbank(magnitudes_, bands_);
    for (std::size_t b = 0; b < kNumBands; ++b) {
        // madmom's LogarithmicSpectrogramProcessor(mul=1, add=1): log10(1 · x + 1).
        const double logBand = std::log10(1.0 + bands_[b]);
        const double diff = havePrevious_ ? std::max(logBand - previousLog_[b], 0.0) : 0.0;
        frame_[b] = static_cast<float>(logBand);
        frame_[kNumBands + b] = static_cast<float>(diff);
        previousLog_[b] = logBand;
    }
    havePrevious_ = true;
}

} // namespace takt4::features
