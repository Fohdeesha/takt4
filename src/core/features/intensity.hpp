#pragma once

#include "core/features/dimensions.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace takt4::features {

/// HANDOFF §5.8's three intensity states.
///
/// The numbers are the wire format — §5.6 gives `/<app>/intensity  int  0 | 1 | 2` — so
/// they are fixed rather than an implementation detail, and everything that indexes by
/// intensity (a rule's "intensity in set", a UI's three checkboxes) indexes by these.
enum class Intensity : std::uint8_t { Calm = 0, Normal = 1, Intense = 2 };

/// The three, in §5.6's own order — which is also their wire number, so a UI list and an
/// OSC argument cannot drift apart about which is which.
inline constexpr std::array<Intensity, 3> kIntensities{Intensity::Calm, Intensity::Normal,
                                                       Intensity::Intense};

std::string_view labelOf(Intensity intensity) noexcept;
/// The inverse of `labelOf`. One word each, so the label doubles as the name a preset
/// spells a rule's intensity set with — there is nothing to shorten and nothing to expand.
std::optional<Intensity> intensityOf(std::string_view label) noexcept;

/// §5.8's intensity classifier: *"Spectral flux across the full band, classified calm /
/// normal / intense with hysteresis to stop it flickering between states. Feeds the ONLY IF
/// stage and is published over OSC. Cheap to compute — it reuses the STFT magnitude we
/// already have."*
///
/// **Cheaper than that, in fact: it reuses the features.** `FeatureExtractor` already
/// computes `max(log10(1 + band) - previous, 0)` for all 144 bands and puts it in the second
/// half of every frame — that is the positive spectral difference across the full band, and
/// summing it *is* the flux. So there is no second FFT, no second filterbank and nothing
/// new on the audio path: 144 additions on the thread that just did the network.
///
/// **The judgement is relative, and has to be.** Absolute flux is a fact about the gain
/// staging of whoever mastered the track. Calm and intense are meant to separate a breakdown
/// from a drop *within* what is playing, so the classifier compares a short average against
/// a long one and thresholds the ratio. A uniformly busy track therefore reads normal
/// throughout, which is right: nothing in it is more intense than the rest of it.
///
/// Nothing here allocates, and `push` is a few dozen flops.
class IntensityClassifier {
public:
    struct Options {
        /// Time constants of the two followers, in 50 Hz frames. The short one is what is
        /// happening now; the long one is what this track is like. 1000 frames is 20 s,
        /// long enough that a breakdown does not drag the baseline down with it.
        ///
        /// **75 — 1.5 s — is measured, and it is the constant that matters.** Over the 17
        /// tracks in `references/audio`, 68 minutes, re-running this classifier across the
        /// flux of every frame (`tools/trace_stability.py`, and the `flux` column
        /// `--trace` now writes):
        ///
        /// | fast | hold | changes | one every | worst track |
        /// |---|---|---|---|---|
        /// | 20 | 25 | 891 | 4.6 s | 1.7 s |
        /// | 75 | 25 | 130 | 31.3 s | 8.3 s |
        /// | 75 | 150 | **116** | **35.0 s** | **13.0 s** |
        /// | 150 | 150 | 77 | 52.8 s | 21.0 s |
        ///
        /// A state that changes every 4.6 seconds is not naming a section of a track, it is
        /// naming the last bar. An intensity is meant to separate an intro from a drop, and
        /// those are tens of seconds long — so the target is tens of seconds, and 0.4 s was
        /// an order of magnitude out. 150 was rejected the other way: three seconds of
        /// follower means a drop takes three seconds to register, and `Trigger::
        /// IntensityChange` is already the slowest thing a rule can fire on.
        double fastFrames = 75.0;
        double slowFrames = 1000.0;

        /// The ratio of the two at which each state is entered, and the ratio at which it is
        /// given up. §5.8 asks for hysteresis by name; these four numbers are it. Enter
        /// intense at 1.35 and hold it down to 1.10; enter calm at 0.70 and hold it up to
        /// 0.85. The gaps are what stop a passage sitting on a threshold from flickering.
        double intenseAt = 1.35;
        double intenseUntil = 1.10;
        double calmAt = 0.70;
        double calmUntil = 0.85;
        /// And a floor on how long a state lasts, in frames. Threshold hysteresis alone
        /// still flickers when the fast follower crosses back and forth inside the gap;
        /// this makes a change cost something. 150 frames is three seconds, and the table
        /// above is where it comes from: with the follower already at 75 it buys the worst
        /// track 8.3 s to 13.0 s between changes for 14 changes across 68 minutes, which is
        /// the cheapest part of the tuning.
        std::uint32_t holdFrames = 150;

        /// Frames before anything but `Normal` is reported, so the long average has
        /// something in it. 2 s: publishing "calm" because a track has only just started is
        /// a guess, and §5.5's rule throughout is that nothing known is published as
        /// nothing rather than as a guess.
        std::uint32_t settleFrames = 100;

        /// An onset is a flux peak: above `onsetRatio` times the short average, and higher
        /// than the frame before it, and not within `onsetGapFrames` of the last one. Three
        /// frames is 60 ms, which is about the fastest a person hears two hits as two.
        double onsetRatio = 1.6;
        std::uint32_t onsetGapFrames = 3;
    };

    IntensityClassifier();
    explicit IntensityClassifier(Options options);

    void reset() noexcept;

    /// One feature frame, as `FeatureExtractor::frame()` gives it. Returns true when this
    /// frame is an onset (§5.8's *"on onset"*).
    bool push(std::span<const float, kFeatureDim> frame) noexcept;

    /// The flux of the last frame, before any smoothing — the raw sum. Published for a
    /// diagnostic trace; nothing downstream should threshold it directly, for the reason
    /// the class note gives.
    double flux() const noexcept { return flux_; }
    /// The short and long averages, and their ratio. What the classification is actually made
    /// on; worth having out here because a state that looks wrong is nearly always one of
    /// these looking wrong.
    double fast() const noexcept { return fast_; }
    double slow() const noexcept { return slow_; }
    double ratio() const noexcept;

    Intensity intensity() const noexcept { return intensity_; }
    /// Onsets since the last reset. The output thread has no frames of its own, so this is
    /// how it learns that one happened.
    std::uint64_t onsets() const noexcept { return onsets_; }
    std::uint64_t frames() const noexcept { return frames_; }
    const Options& options() const noexcept { return options_; }

private:
    Options options_;
    double flux_ = 0.0;
    double fast_ = 0.0;
    double slow_ = 0.0;
    Intensity intensity_ = Intensity::Normal;
    std::uint64_t frames_ = 0;
    std::uint64_t onsets_ = 0;
    std::uint32_t held_ = 0;
    /// The frame before this one, for the "higher than its neighbour" half of a peak.
    double previousFlux_ = 0.0;
    std::uint32_t sinceOnset_ = 0;
};

} // namespace takt4::features
