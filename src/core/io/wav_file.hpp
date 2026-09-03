#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace takt4::io {

/// A WAV file decoded to float, channels interleaved, in [-1, 1) for integer PCM.
struct WavData {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::vector<float> samples; // frames × channels, interleaved

    std::size_t frames() const noexcept { return channels == 0 ? 0 : samples.size() / channels; }
};

/// Reads a RIFF/WAVE file: PCM 16, 24 or 32-bit, or IEEE float 32-bit, plain or
/// WAVE_FORMAT_EXTENSIBLE. Integer samples are divided by 2^(bits − 1) — exactly what
/// libsndfile does, so a 16-bit file read here and through Python's soundfile gives
/// the same float32 values. Chunks other than fmt and data are skipped. Throws
/// std::runtime_error on anything it cannot read.
WavData readWavFile(const std::filesystem::path& path);

} // namespace takt4::io
