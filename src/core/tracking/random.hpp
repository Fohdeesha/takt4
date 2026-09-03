#pragma once

#include <cstddef>
#include <cstdint>

namespace takt4::tracking {

/// xoshiro256++ over a splitmix64 seeding — the tracker's only source of randomness.
///
/// HANDOFF §5.4 asks for a deterministic seed because "reproducibility matters
/// enormously for debugging a stochastic filter against recorded audio". A seed is not
/// enough on its own: the reference implementation draws from numpy's legacy
/// `RandomState`, whose stream nothing outside numpy reproduces, so a C++ port seeded
/// the same way still diverges on the first draw. Specifying the generator instead —
/// twenty lines, stated identically here and in tools/pf_reference.py — is what turns
/// the Phase 4 gate from "close enough on average" into an exact comparison.
///
/// Everything is 64-bit integer arithmetic and one exact scaling, so the two languages
/// agree bit for bit. Nothing here allocates or locks.
class Xoshiro256pp {
public:
    explicit Xoshiro256pp(std::uint64_t seed = 1) noexcept { reseed(seed); }

    void reseed(std::uint64_t seed) noexcept {
        std::uint64_t state = seed;
        for (std::uint64_t& word : s_) {
            state += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            word = z ^ (z >> 31);
        }
    }

    std::uint64_t next() noexcept {
        const std::uint64_t sum = s_[0] + s_[3];
        const std::uint64_t result = ((sum << 23) | (sum >> 41)) + s_[0];
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = (s_[3] << 45) | (s_[3] >> 19);
        return result;
    }

    /// Uniform in [0, 1): the top 53 bits, scaled by an exact power of two.
    double nextDouble() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

    /// Uniform integer in [0, bound), by masked rejection. `bound` must be positive.
    std::uint64_t bounded(std::uint64_t bound) noexcept {
        std::uint64_t mask = 1;
        while (mask < bound) {
            mask = (mask << 1) | 1;
        }
        for (;;) {
            const std::uint64_t value = next() & mask;
            if (value < bound) {
                return value;
            }
        }
    }

private:
    std::uint64_t s_[4]{};
};

} // namespace takt4::tracking
