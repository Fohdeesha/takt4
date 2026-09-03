#pragma once

#include "core/audio/rates.hpp"
#include "core/dsp/real_fft.hpp"
#include "core/features/dimensions.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace takt4::features {

/// The BeatNet+ front end (HANDOFF §5.2), hop by hop: STFT → magnitude → filterbank →
/// log10(1 + x) → positive difference to the previous frame, 288 values per frame.
/// Frame k is madmom's frame k — the 1764 samples centred on sample 441 k, zero-padded
/// before the start of the signal — so it can only be computed once hop k + 1 has
/// arrived: pushHop() of hop j delivers frame j − 1. That is the 40 ms of inherent
/// lookahead the centred framing costs; nothing here adds to it.
///
/// Construction allocates. pushHop(), flush() and reset() do not, and are meant for
/// the audio thread (§4.2). The maths is in double; the output is float, as madmom's.
class FeatureExtractor {
public:
    using Frame = std::array<float, kFeatureDim>;

    /// Frame k is delivered by the push of hop k + kLatencyHops (the frame runs to the
    /// end of that hop). Counted from the frame's centre it is one hop more: 40 ms.
    static constexpr std::uint64_t kLatencyHops = kHopsPerFrame / 2 - 1;

    FeatureExtractor();

    FeatureExtractor(const FeatureExtractor&) = delete;
    FeatureExtractor& operator=(const FeatureExtractor&) = delete;

    /// Back to the state after construction: no hops seen, silence before them.
    void reset() noexcept;

    /// Feeds the next hop, kHopSize samples. Returns true when a frame was completed;
    /// it is then in frame(), and frameIndex() says which one.
    [[nodiscard]] bool pushHop(std::span<const float, audio::kHopSize> hop) noexcept;

    /// Feeds one hop of silence, which completes the frame centred on the last real
    /// hop's start. For finite signals only: madmom's frame count for N samples is
    /// ⌈N / 441⌉, and the last of those frames needs this to be reached.
    [[nodiscard]] bool flush() noexcept;

    /// The last completed frame: kNumBands log bands, then their differences.
    const Frame& frame() const noexcept { return frame_; }
    std::uint64_t frameIndex() const noexcept { return frameIndex_; }

    /// Hops fed so far, flush() included.
    std::uint64_t hopsPushed() const noexcept { return hopsPushed_; }

private:
    bool push(std::span<const float, audio::kHopSize> hop) noexcept;
    void computeFrame() noexcept;

    dsp::RealFft fft_;
    std::vector<double> window_;                     // numpy.hanning(kFrameSize)
    std::array<float, kFrameSize> history_{};        // the last kHopsPerFrame hops, oldest first
    std::vector<double> windowed_;                   // kFrameSize
    std::vector<std::complex<double>> spectrum_;     // kNumBins + 1 (KissFFT gives Nyquist too)
    std::array<double, kNumBins> magnitudes_{};
    std::array<double, kNumBands> bands_{};
    std::array<double, kNumBands> previousLog_{};    // for the difference
    bool havePrevious_ = false;
    std::uint64_t hopsPushed_ = 0;
    std::uint64_t frameIndex_ = 0;
    Frame frame_{};
};

} // namespace takt4::features
