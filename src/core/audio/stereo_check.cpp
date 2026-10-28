#include "core/audio/stereo_check.hpp"

#include <cmath>
#include <iterator>
#include <limits>

namespace takt4::audio {

namespace {

/// A side's RMS level over `frames`, in dB below full scale; −120 for nothing at all.
double levelDb(double energy, std::uint64_t frames) {
    if (frames == 0 || !(energy > 0.0)) {
        return -120.0;
    }
    return 10.0 * std::log10(energy / static_cast<double>(frames));
}

StereoSums between(const StereoSums& later, const StereoSums& earlier) {
    return StereoSums{later.frames - earlier.frames, later.left - earlier.left,
                      later.right - earlier.right, later.both - earlier.both};
}

double correlation(const StereoSums& sums) {
    const double scale = std::sqrt(sums.left * sums.right);
    return scale > 0.0 ? sums.both / scale : 0.0;
}

} // namespace

StereoCheck::StereoCheck() : StereoCheck(Options{}) {}

StereoCheck::StereoCheck(Options options)
    : options_(options), relatedAt_(-std::numeric_limits<double>::infinity()) {}

void StereoCheck::reset() {
    samples_.clear();
    relatedAt_ = -std::numeric_limits<double>::infinity();
}

bool StereoCheck::over(double seconds, StereoSums& out) const {
    if (samples_.size() < 2) {
        return false;
    }
    const Sample& last = samples_.back();
    for (auto it = samples_.rbegin(); it != samples_.rend(); ++it) {
        if (last.at - it->at >= seconds) {
            out = between(last.sums, it->sums);
            return out.frames > 0;
        }
    }
    return false;
}

double StereoCheck::loudnessTogether(double seconds) const {
    if (samples_.size() < 3) {
        return 0.0;
    }
    const Sample& last = samples_.back();
    auto from = samples_.rbegin();
    while (from != samples_.rend() && last.at - from->at < seconds) {
        ++from;
    }
    if (from == samples_.rend()) {
        return 0.0;
    }
    // Each look's level, from the sums between it and the one before, over the span.
    double n = 0.0;
    double sa = 0.0;
    double sb = 0.0;
    double saa = 0.0;
    double sbb = 0.0;
    double sab = 0.0;
    for (auto later = samples_.rbegin(); later != from; ++later) {
        const StereoSums step = between(later->sums, std::next(later)->sums);
        const double a = levelDb(step.left, step.frames);
        const double b = levelDb(step.right, step.frames);
        n += 1.0;
        sa += a;
        sb += b;
        saa += a * a;
        sbb += b * b;
        sab += a * b;
    }
    const double va = saa - sa * sa / n;
    const double vb = sbb - sb * sb / n;
    // A level that has not moved says nothing about whether the other moved with it.
    if (!(va > 1e-9 * n) || !(vb > 1e-9 * n)) {
        return 0.0;
    }
    return (sab - sa * sb / n) / std::sqrt(va * vb);
}

StereoCheck::Reading StereoCheck::observe(const StereoSums& sums, double now) {
    // Sums that went backwards belong to a stream opened since: start again with it.
    if (!samples_.empty() && sums.frames < samples_.back().sums.frames) {
        reset();
    }
    if (samples_.empty() || sums.frames != samples_.back().sums.frames) {
        samples_.push_back(Sample{now, sums});
    }
    while (samples_.size() > 2 && samples_.back().at - samples_[1].at >= options_.unrelatedSeconds) {
        samples_.pop_front();
    }

    Reading reading;
    StereoSums recent;
    if (!over(options_.windowSeconds, recent)) {
        return reading;
    }
    reading.correlation = correlation(recent);
    reading.leftDb = levelDb(recent.left, recent.frames);
    reading.rightDb = levelDb(recent.right, recent.frames);
    const bool leftSilent = reading.leftDb < options_.silentBelowDb;
    const bool rightSilent = reading.rightDb < options_.silentBelowDb;
    const bool leftLive = reading.leftDb >= options_.judgedAboveDb;
    const bool rightLive = reading.rightDb >= options_.judgedAboveDb;
    if (leftSilent && rightSilent) {
        return reading; // nothing playing: nothing to judge
    }
    if (leftSilent && rightLive) {
        reading.verdict = Verdict::LeftSilent;
        return reading;
    }
    if (rightSilent && leftLive) {
        reading.verdict = Verdict::RightSilent;
        return reading;
    }
    reading.verdict = Verdict::Fine;
    if (!leftLive || !rightLive) {
        return reading; // quiet on a side: its correlation with the other says little
    }
    if (std::abs(reading.correlation) >= options_.relatedBeyond) {
        relatedAt_ = now;
    }
    if (reading.correlation < options_.outOfPhaseBelow) {
        reading.verdict = Verdict::OutOfPhase;
        return reading;
    }
    // Unrelated over the short window as well as the long one, and only once no short window
    // in the long one has shown the two related: a pair that has just been put right — a
    // flipped leg wired back — has a long window that straddles both and sums to nothing, and
    // a short one that passes through nothing on the way, and it is not two strangers. And
    // only when the two sides' loudness has not moved together either: music whose sides
    // share no waveform still rises and falls on both (see the class note).
    StereoSums longer;
    if (std::abs(reading.correlation) < options_.unrelatedWithin &&
        now - relatedAt_ >= options_.unrelatedSeconds && over(options_.unrelatedSeconds, longer) &&
        std::abs(correlation(longer)) < options_.unrelatedWithin &&
        levelDb(longer.left, longer.frames) >= options_.judgedAboveDb &&
        levelDb(longer.right, longer.frames) >= options_.judgedAboveDb &&
        loudnessTogether(options_.unrelatedSeconds) < options_.envelopeBelow) {
        reading.verdict = Verdict::Unrelated;
    }
    return reading;
}

std::string StereoCheck::describe(Verdict verdict, const std::string& left,
                                  const std::string& right) {
    switch (verdict) {
    // What to do first, where a line cut short keeps it: under the meter there is room for about
    // eighty characters, and the remedy at the end of these was lost. And "stop, then": mono is
    // locked while the input runs, and this is said while it runs.
    case Verdict::OutOfPhase:
        return left + " and " + right + " are out of phase: stop, then tick mono to hear " + left +
               " alone, or check the cable (one leg may be wired backwards) \xE2\x80\x94 averaged, "
               "they cancel the kick and the bass";
    case Verdict::LeftSilent:
        return left + " is silent and " + right + " is not: if the feed is mono on " + right +
               ", stop, then tick mono and pick it";
    case Verdict::RightSilent:
        return right + " is silent and " + left + " is not: if the feed is mono on " + left +
               ", stop, then tick mono";
    case Verdict::Unrelated:
        return left + " and " + right +
               " share almost nothing: if one of them is not the music, stop, then tick mono";
    case Verdict::Quiet:
    case Verdict::Fine:
        break;
    }
    return {};
}

} // namespace takt4::audio
