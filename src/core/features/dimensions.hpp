#pragma once

#include "core/audio/rates.hpp"

#include <cstddef>

// The shape of the BeatNet+ feature front end (HANDOFF §5.2), as madmom computes it
// for BeatNet+; tools/beatnet_features.py is the reference for every number here.

namespace takt4::features {

/// Analysis frame, 80 ms at the internal rate: four hops, centred on a hop boundary.
inline constexpr std::size_t kFrameSize = 1764;
inline constexpr std::size_t kHopsPerFrame = kFrameSize / audio::kHopSize;
static_assert(kHopsPerFrame * audio::kHopSize == kFrameSize, "a frame is a whole number of hops");
static_assert(kHopsPerFrame % 2 == 0, "frames are centred on a hop boundary");

/// STFT bins madmom keeps: DC up to, not including, Nyquist. 12.5 Hz apart.
inline constexpr std::size_t kNumBins = kFrameSize / 2;

/// Bands of the log-spaced filterbank: 24 per octave from 30 Hz to 17 kHz, minus the
/// duplicates madmom removes where bins are too coarse to tell neighbouring bands apart.
inline constexpr std::size_t kNumBands = 144;

/// Values per feature frame: the log bands, then their positive first differences.
inline constexpr std::size_t kFeatureDim = 2 * kNumBands;

} // namespace takt4::features
