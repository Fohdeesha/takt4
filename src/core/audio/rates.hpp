#pragma once

#include <cstddef>

namespace takt4::audio {

/// The one rate everything past the input stage runs at (HANDOFF §4.1, §5.1): every
/// device rate is resampled to this, mono.
inline constexpr double kInternalSampleRate = 22050.0;

/// Analysis hop, 20 ms at the internal rate (HANDOFF §5.1, §5.2): the feature front end
/// and the tracker see the signal in frames of this many samples, 50 per second.
inline constexpr std::size_t kHopSize = 441;

/// Hops per second at the internal rate.
inline constexpr double kHopRate = kInternalSampleRate / static_cast<double>(kHopSize);

/// One hop in microseconds — exactly 20000, since 441 samples at 22050 Hz is 20 ms.
inline constexpr std::size_t kHopMicros = 20000;
static_assert(kHopSize * 1000000 == kHopMicros * static_cast<std::size_t>(kInternalSampleRate),
              "the hop is a whole number of microseconds");

} // namespace takt4::audio
