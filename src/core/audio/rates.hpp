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

} // namespace takt4::audio
