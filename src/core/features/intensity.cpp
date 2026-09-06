#include "core/features/intensity.hpp"

#include <algorithm>
#include <cmath>

namespace takt4::features {

namespace {

/// Below this the two followers are treated as silence rather than as a ratio. Flux is a sum
/// of 144 log-magnitude differences, so a passage with nothing in it sits near zero and a
/// ratio of two near-zeroes is noise amplified into a classification.
constexpr double kSilence = 1e-4;

double onePole(double state, double value, double frames) noexcept {
    const double alpha = 1.0 / std::max(1.0, frames);
    return state + alpha * (value - state);
}

} // namespace

std::string_view labelOf(Intensity intensity) noexcept {
    switch (intensity) {
    case Intensity::Calm:
        return "calm";
    case Intensity::Normal:
        return "normal";
    case Intensity::Intense:
        return "intense";
    }
    return "";
}

std::optional<Intensity> intensityOf(std::string_view label) noexcept {
    for (const Intensity intensity : kIntensities) {
        if (labelOf(intensity) == label) {
            return intensity;
        }
    }
    return std::nullopt;
}

IntensityClassifier::IntensityClassifier() : IntensityClassifier(Options{}) {}

IntensityClassifier::IntensityClassifier(Options options) : options_(options) {
    // Clamped rather than refused, for `Generator`'s reason: these travel in a preset, and
    // `settings::load` is documented never to fail. What is enforced is only that the
    // hysteresis is a hysteresis — the ratio a state is given up at must be on the inside of
    // the one it is entered at, or the gap is negative and the flicker it exists to stop is
    // worse than with no gap at all.
    options_.fastFrames = std::max(1.0, options_.fastFrames);
    options_.slowFrames = std::max(options_.fastFrames, options_.slowFrames);
    options_.intenseAt = std::max(1.0, options_.intenseAt);
    options_.intenseUntil = std::clamp(options_.intenseUntil, 1.0, options_.intenseAt);
    options_.calmAt = std::clamp(options_.calmAt, 0.0, 1.0);
    options_.calmUntil = std::clamp(options_.calmUntil, options_.calmAt, 1.0);
    reset();
}

void IntensityClassifier::reset() noexcept {
    flux_ = 0.0;
    fast_ = 0.0;
    slow_ = 0.0;
    intensity_ = Intensity::Normal;
    frames_ = 0;
    onsets_ = 0;
    held_ = 0;
    previousFlux_ = 0.0;
    sinceOnset_ = 0;
}

double IntensityClassifier::ratio() const noexcept {
    if (slow_ <= kSilence) {
        return 1.0; // nothing to be relative to, so nothing is louder than anything
    }
    return fast_ / slow_;
}

bool IntensityClassifier::push(std::span<const float, kFeatureDim> frame) noexcept {
    // The second half of a feature frame is already the positive first difference of every
    // log band — see the class note. Summing it is the spectral flux across the full band.
    double sum = 0.0;
    for (std::size_t band = 0; band < kNumBands; ++band) {
        sum += static_cast<double>(frame[kNumBands + band]);
    }
    flux_ = sum;
    ++frames_;

    if (frames_ == 1) {
        // Start both followers at the first frame rather than at zero. Starting at zero
        // makes the long one climb for twenty seconds, and everything measured against it
        // during the climb reads as intense.
        fast_ = flux_;
        slow_ = flux_;
    } else {
        fast_ = onePole(fast_, flux_, options_.fastFrames);
        slow_ = onePole(slow_, flux_, options_.slowFrames);
    }

    // --- the state, with §5.8's hysteresis and a floor on how long one lasts -------------
    if (held_ < options_.holdFrames) {
        ++held_;
    }
    const bool settled = frames_ >= options_.settleFrames;
    const double now = ratio();
    Intensity wanted = intensity_;
    if (!settled || slow_ <= kSilence) {
        wanted = Intensity::Normal;
    } else {
        switch (intensity_) {
        case Intensity::Normal:
            wanted = now >= options_.intenseAt ? Intensity::Intense
                     : now <= options_.calmAt  ? Intensity::Calm
                                               : Intensity::Normal;
            break;
        case Intensity::Intense:
            // Leaves only when it has come back inside the *inner* threshold. Going
            // straight from intense to calm is possible and correct: a drop cutting to
            // nothing does exactly that.
            wanted = now < options_.intenseUntil
                         ? (now <= options_.calmAt ? Intensity::Calm : Intensity::Normal)
                         : Intensity::Intense;
            break;
        case Intensity::Calm:
            wanted = now > options_.calmUntil
                         ? (now >= options_.intenseAt ? Intensity::Intense : Intensity::Normal)
                         : Intensity::Calm;
            break;
        }
    }
    if (wanted != intensity_ && held_ >= options_.holdFrames) {
        intensity_ = wanted;
        held_ = 0;
    }

    // --- the onset ------------------------------------------------------------------------
    if (sinceOnset_ < options_.onsetGapFrames) {
        ++sinceOnset_;
    }
    const bool peak = frames_ > 1 && flux_ > previousFlux_ && fast_ > kSilence &&
                      flux_ >= options_.onsetRatio * fast_;
    const bool onset = peak && sinceOnset_ >= options_.onsetGapFrames;
    previousFlux_ = flux_;
    if (onset) {
        sinceOnset_ = 0;
        ++onsets_;
    }
    return onset;
}

} // namespace takt4::features
