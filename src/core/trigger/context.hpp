#pragma once

#include "core/features/intensity.hpp"

#include <cstdint>
#include <optional>

namespace takt4::trigger {

/// HANDOFF §5.8's intensity is `features::Intensity`, which lives with the thing that
/// computes it: `features::IntensityClassifier` reads it straight out of the feature frame,
/// so the type belongs beside the features rather than up here.
///
/// **It used to be aliased into this namespace as `Intensity`, and must not be again.** GCC's
/// `-Wshadow` counts `LiveSource::Intensity` in `generator.hpp` as shadowing a namespace-scope
/// `Intensity`, even though the enum is scoped — so the alias and that enumerator cannot both
/// exist, and `linux-core` failed on `-Werror` at `dea67e3e` for exactly that. Neither MSVC nor
/// the clang-tidy `tools/lint_clang.ps1` runs reproduces it, which is why it reached the
/// remote: it is a GCC-only diagnostic and CI has not built with GCC since 2026-09-06 (§1).
/// Writing `features::Intensity` costs eleven characters twice and removes the collision.

/// Everything a rule can be asked about at the moment it might fire.
///
/// One struct rather than a reference to the tracker, for the reason §4.2 gives the whole
/// output side: the rules run on the output thread and the tracker belongs to the inference
/// thread. This is a copy taken once per round, so every rule in a round sees the same
/// instant — which matters more than it looks. Two rules with the same "BPM in range"
/// condition must agree about whether it held, or an operator watching one fire and not the
/// other has no way to tell a race from a bug.
///
/// Nothing here is a pointer and nothing is owned, so a round's context costs one copy of
/// forty-odd bytes.
struct Context {
    /// §5.5's published tempo — the one on screen, after the fold and any ÷2 or ×2, because
    /// that is the number an operator set their "BPM in range" condition against.
    double bpm = 0.0;
    /// How far apart the beats going out are, in seconds: `tracking::TempoState::gridBpm` as a
    /// spacing. **What a rule's lengths in beats and bars are counted in** — the beats the rule
    /// fires on, which are not sixty over `bpm` under a ×2, or when the number has stayed where a
    /// lock left it and the beats have moved on: a number frozen at 188 over beats at 94 made
    /// "release after one beat" last half of one. Zero when unknown; see `secondsPerBeat`.
    double beatSeconds = 0.0;
    double confidence = 0.0;
    bool locked = false;
    /// Zero before the filter has an opinion, which is not the same as 4. Nothing here
    /// assumes a meter (§5.5), and a rule counting bars has to cope with not knowing one.
    std::uint32_t meter = 0;
    /// 1 on the downbeat, counting up; 0 before the first beat. As `tracking::TempoState`.
    std::uint32_t beatInBar = 0;
    std::uint64_t beats = 0;
    std::uint64_t bars = 0;
    features::Intensity intensity = features::Intensity::Normal;
    /// Seconds since the outputs started, on the same clock `output::OutputRunner` drives
    /// the transports from. Cooldowns and follow-up delays are measured against it.
    double now = 0.0;
    /// When the thing being judged happens, where that is not `now`: a beat the output thread
    /// is firing ahead of time on a prediction, whose own moment is still to come — or one
    /// heard late, whose moment has gone. Unset for everything that happens as it is judged.
    /// Carried onto every message sent from it; see `Message::moment`.
    std::optional<double> moment;
};

/// One beat of `context`, in seconds: `Context::beatSeconds`, or sixty over the published tempo
/// where that is not known. Zero with neither.
inline double secondsPerBeat(const Context& context) noexcept {
    if (context.beatSeconds > 0.0) {
        return context.beatSeconds;
    }
    return context.bpm > 0.0 ? 60.0 / context.bpm : 0.0;
}

} // namespace takt4::trigger
