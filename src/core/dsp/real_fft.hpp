#pragma once

#include <complex>
#include <cstddef>
#include <memory>
#include <span>

namespace takt4::dsp {

/// Forward real-to-complex FFT of one fixed, even size, in double precision (KissFFT).
///
/// Construction plans the transform and allocates; forward() then allocates nothing
/// and may run on the audio thread (HANDOFF §4.2). The output is the unnormalised
/// half spectrum X[k] = Σ x[n] e^(-2πikn/N) for k = 0 .. N/2, the convention of
/// numpy.fft.rfft and of the scipy FFT madmom uses.
class RealFft {
public:
    /// `size` must be even and at least 2; any factorisation is fine.
    explicit RealFft(std::size_t size);
    ~RealFft();

    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;

    std::size_t size() const noexcept { return size_; }

    /// Bins of the half spectrum: size() / 2 + 1, DC through Nyquist.
    std::size_t numBins() const noexcept { return size_ / 2 + 1; }

    /// `input` holds size() samples; `spectrum` receives numBins() bins. Out of place:
    /// the two must not overlap.
    void forward(std::span<const double> input, std::span<std::complex<double>> spectrum) noexcept;

private:
    struct Plan;
    std::size_t size_;
    std::unique_ptr<Plan> plan_;
};

} // namespace takt4::dsp
