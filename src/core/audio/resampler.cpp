#include "core/audio/resampler.hpp"

#include <CDSPResampler.h>

#include <limits>
#include <stdexcept>
#include <vector>

namespace takt4::audio {

namespace {

// r8brain filter design: the transition band as a percentage of the output spectrum
// (when downsampling) between the -3 dB point and Nyquist, and the stop-band depth in
// dB. The filter length, and with it inputDelayFrames(), follows from them, and that
// delay sits in front of everything the tracker does.
//
// Measured with r8brain 7.5, device rate → 22050 Hz (44.1k / 48k / 96k input):
//   2 % / 180 dB  (the CDSPResampler24 preset)   77 / 70 / 70 ms
//   5 % / 144 dB  (chosen)                       18 / 16 / 17 ms
//  10 % / 144 dB                                  9 /  8 /  8 ms
// 5 % puts the -3 dB point at 10.47 kHz, which touches only the top two or so of the
// 24-per-octave bands the features use (HANDOFF §5.2); 144 dB is the 24-bit noise
// floor, below any converter. The resampler test logs the delay and fails past 25 ms.
constexpr double kTransitionBandPercent = 5.0;
constexpr double kStopbandAttenuationDb = 144.0;

} // namespace

struct Resampler::Impl {
    Impl(double inputRate, double outputRate, int chunk)
        : converter(inputRate, outputRate, chunk, kTransitionBandPercent, kStopbandAttenuationDb),
          input(static_cast<std::size_t>(chunk)),
          output(static_cast<std::size_t>(converter.getMaxOutLen(chunk))) {}

    r8b::CDSPResampler converter;
    std::vector<double> input;  // one chunk, widened from float
    std::vector<float> output;  // one chunk's output, narrowed back
};

Resampler::Resampler(double inputRate, double outputRate, std::size_t chunkFrames)
    : inputRate_(inputRate), outputRate_(outputRate), chunkFrames_(chunkFrames) {
    if (!(inputRate > 0.0) || !(outputRate > 0.0)) {
        throw std::invalid_argument("resampler rates must be positive");
    }
    if (chunkFrames == 0 || chunkFrames > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("resampler chunk size out of range");
    }
    impl_ = std::make_unique<Impl>(inputRate, outputRate, static_cast<int>(chunkFrames));
    // getInputRequiredForOutput(1) counts the input sample that finally produces
    // output; the delay is everything before it.
    inputDelayFrames_ = static_cast<std::size_t>(impl_->converter.getInputRequiredForOutput(1) - 1);
}

Resampler::~Resampler() = default;

std::size_t Resampler::maxOutputPerChunk() const noexcept {
    return impl_->output.size();
}

void Resampler::reset() noexcept {
    impl_->converter.clear();
}

const char* Resampler::libraryVersion() noexcept {
    return R8B_VERSION;
}

std::size_t Resampler::processChunk(const float* input, std::size_t frames, const float*& output) noexcept {
    Impl& impl = *impl_;
    double* in = impl.input.data();
    for (std::size_t i = 0; i < frames; ++i) {
        in[i] = static_cast<double>(input[i]);
    }
    double* out = nullptr;
    const int produced = impl.converter.process(in, static_cast<int>(frames), out);
    const auto count = static_cast<std::size_t>(produced);
    float* narrowed = impl.output.data();
    for (std::size_t i = 0; i < count; ++i) {
        narrowed[i] = static_cast<float>(out[i]);
    }
    output = narrowed;
    return count;
}

} // namespace takt4::audio
