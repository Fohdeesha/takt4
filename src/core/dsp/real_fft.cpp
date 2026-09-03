#include "core/dsp/real_fft.hpp"

#include <kiss_fftr.h>

#include <cassert>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace takt4::dsp {

// Only this file sees KissFFT's headers, so kiss_fft_scalar=double (a public compile
// definition of the kissfft target) does not have to reach anything that includes ours.
static_assert(sizeof(kiss_fft_scalar) == sizeof(double), "KissFFT must be built in double precision");

struct RealFft::Plan {
    kiss_fftr_cfg cfg = nullptr;
    std::vector<kiss_fft_cpx> bins; // KissFFT's own complex type; copied out in forward()

    ~Plan() {
        kiss_fftr_free(cfg);
    }
};

RealFft::RealFft(std::size_t size) : size_(size), plan_(std::make_unique<Plan>()) {
    if (size < 2 || size % 2 != 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("RealFft: size must be even and at least 2, got " + std::to_string(size));
    }
    plan_->cfg = kiss_fftr_alloc(static_cast<int>(size), 0, nullptr, nullptr);
    if (plan_->cfg == nullptr) {
        throw std::bad_alloc();
    }
    plan_->bins.resize(numBins());
}

RealFft::~RealFft() = default;

void RealFft::forward(std::span<const double> input, std::span<std::complex<double>> spectrum) noexcept {
    assert(input.size() == size_);
    assert(spectrum.size() >= numBins());
    // kiss_fftr only ever writes its output buffer, and its scratch space is on the
    // stack (KISS_FFT_USE_ALLOCA, cmake/deps.cmake): no heap use from here on.
    kiss_fftr(plan_->cfg, input.data(), plan_->bins.data());
    const std::size_t bins = numBins();
    for (std::size_t k = 0; k < bins; ++k) {
        spectrum[k] = std::complex<double>(plan_->bins[k].r, plan_->bins[k].i);
    }
}

} // namespace takt4::dsp
