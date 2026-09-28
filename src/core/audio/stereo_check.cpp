#include "core/audio/stereo_check.hpp"

#include <cmath>

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

StereoCheck::StereoCheck(Options options) : options_(options) {}

void StereoCheck::reset() {
    samples_.clear();
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

StereoCheck::Reading StereoCheck::observe(const StereoSums& sums, double now) {
    // Sums that went backwards belong to a stream opened since: start again with it.
    if (!samples_.empty() && sums.frames < samples_.back().sums.frames) {
        samples_.clear();
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
    if (reading.correlation < options_.outOfPhaseBelow) {
        reading.verdict = Verdict::OutOfPhase;
        return reading;
    }
    // Unrelated over the short window as well as the long one: a pair that has just been put
    // right — a flipped leg wired back — has a long window that straddles both and sums to
    // nothing, and it is not two strangers.
    StereoSums longer;
    if (std::abs(reading.correlation) < options_.unrelatedWithin &&
        over(options_.unrelatedSeconds, longer) &&
        std::abs(correlation(longer)) < options_.unrelatedWithin &&
        levelDb(longer.left, longer.frames) >= options_.judgedAboveDb &&
        levelDb(longer.right, longer.frames) >= options_.judgedAboveDb) {
        reading.verdict = Verdict::Unrelated;
    }
    return reading;
}

std::string StereoCheck::describe(Verdict verdict, const std::string& left,
                                  const std::string& right) {
    switch (verdict) {
    case Verdict::OutOfPhase:
        return left + " and " + right +
               " are out of phase \xE2\x80\x94 averaging them cancels the kick and the bass. Check "
               "the cable (one leg may be wired backwards), or tick mono to hear " +
               left + " alone";
    case Verdict::LeftSilent:
        return left + " is silent and " + right + " is not \xE2\x80\x94 if the feed is mono on " +
               right + ", tick mono and pick it";
    case Verdict::RightSilent:
        return right + " is silent and " + left + " is not \xE2\x80\x94 if the feed is mono on " +
               left + ", tick mono";
    case Verdict::Unrelated:
        return left + " and " + right +
               " share almost nothing \xE2\x80\x94 if one of them is not the music, tick mono";
    case Verdict::Quiet:
    case Verdict::Fine:
        break;
    }
    return {};
}

} // namespace takt4::audio
