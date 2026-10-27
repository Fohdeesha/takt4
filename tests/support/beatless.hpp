#pragma once

// Loud sound with no beat in it, for the tests of what takt4 does before it hears one: the
// engine's (no lock on it) and the window's (nothing drawn as found in it).

#include "core/audio/rates.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace takt4::testing {

/// `seconds` of sound with no beat in it, at -12 dBFS RMS: loud, as an intro, a pad or a crowd is
/// through a mixer. Deterministic, from a fixed seed, the same on every compiler.
enum class Beatless { PinkNoise, HeldChord, SwellingPad, RandomKnocks };

inline std::vector<float> beatless(Beatless kind, double seconds) {
    const double rate = takt4::audio::kInternalSampleRate;
    std::vector<double> out(static_cast<std::size_t>(seconds * rate));
    std::uint32_t state = 2463534242U;
    const auto uniform = [&state] {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return static_cast<double>(state) / 4294967295.0 * 2.0 - 1.0;
    };
    // A saw chord: C, E, G and the octave, each a sum of its first dozen partials.
    const auto chord = [&](std::size_t i) {
        const double t = static_cast<double>(i) / rate;
        double sum = 0.0;
        for (const double f : {130.81, 164.81, 196.0, 261.63}) {
            for (int k = 1; k <= 12; ++k) {
                sum += std::sin(2.0 * 3.141592653589793 * f * k * t) / k;
            }
        }
        return sum;
    };
    switch (kind) {
    case Beatless::PinkNoise: {
        // Paul Kellet's economy filter: white noise to within a decibel of pink.
        double b0 = 0.0;
        double b1 = 0.0;
        double b2 = 0.0;
        for (double& sample : out) {
            const double white = uniform();
            b0 = 0.99765 * b0 + white * 0.0990460;
            b1 = 0.96300 * b1 + white * 0.2965164;
            b2 = 0.57000 * b2 + white * 1.0526913;
            sample = b0 + b1 + b2 + white * 0.1848;
        }
        break;
    }
    case Beatless::HeldChord:
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = chord(i);
        }
        break;
    case Beatless::SwellingPad:
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double t = static_cast<double>(i) / rate;
            out[i] = chord(i) * (0.6 + 0.4 * std::sin(2.0 * 3.141592653589793 * 0.2 * t));
        }
        break;
    case Beatless::RandomKnocks: {
        // Bursts of noise two a second on average, at times nothing predicts.
        const auto decay = static_cast<std::size_t>(0.08 * rate);
        for (std::size_t i = 0; i < out.size(); ++i) {
            if ((uniform() + 1.0) / 2.0 < 2.0 / rate) {
                const double gain = 0.5 + 0.25 * (uniform() + 1.0);
                for (std::size_t k = 0; k < 6 * decay && i + k < out.size(); ++k) {
                    out[i + k] += gain * uniform() *
                                  std::exp(-static_cast<double>(k) / static_cast<double>(decay));
                }
            }
        }
        break;
    }
    }
    double mean = 0.0;
    for (const double sample : out) {
        mean += sample;
    }
    mean /= static_cast<double>(out.size());
    double power = 0.0;
    for (const double sample : out) {
        power += (sample - mean) * (sample - mean);
    }
    const double gain =
        std::pow(10.0, -12.0 / 20.0) / std::sqrt(power / static_cast<double>(out.size()));
    std::vector<float> signal(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        signal[i] = static_cast<float>(std::clamp((out[i] - mean) * gain, -1.0, 1.0));
    }
    // Whole hops, as the engine is fed.
    signal.resize(signal.size() / audio::kHopSize * audio::kHopSize);
    return signal;
}

inline const char* nameOf(Beatless kind) {
    switch (kind) {
    case Beatless::PinkNoise:
        return "pink noise";
    case Beatless::HeldChord:
        return "a held chord";
    case Beatless::SwellingPad:
        return "a swelling pad";
    case Beatless::RandomKnocks:
        return "knocks at random times";
    }
    return "";
}

} // namespace takt4::testing
