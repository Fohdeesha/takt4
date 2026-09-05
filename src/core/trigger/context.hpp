#pragma once

#include "core/features/intensity.hpp"

#include <cstdint>

namespace takt4::trigger {

/// HANDOFF §5.8's intensity classifier, which lives with the thing that computes it:
/// `features::IntensityClassifier` reads it straight out of the feature frame, so the type
/// belongs beside the features rather than up here. Aliased so a rule reads as one —
/// `labelOf` still resolves, by argument-dependent lookup.
using Intensity = features::Intensity;

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
    double confidence = 0.0;
    bool locked = false;
    /// Zero before the filter has an opinion, which is not the same as 4. Nothing here
    /// assumes a meter (§5.5), and a rule counting bars has to cope with not knowing one.
    std::uint32_t meter = 0;
    /// 1 on the downbeat, counting up; 0 before the first beat. As `tracking::TempoState`.
    std::uint32_t beatInBar = 0;
    std::uint64_t beats = 0;
    std::uint64_t bars = 0;
    Intensity intensity = Intensity::Normal;
    /// Seconds since the outputs started, on the same clock `output::OutputRunner` drives
    /// the transports from. Cooldowns and follow-up delays are measured against it.
    double now = 0.0;
};

} // namespace takt4::trigger
