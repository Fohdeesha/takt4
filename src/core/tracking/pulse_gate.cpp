#include "core/tracking/pulse_gate.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>

namespace takt4::tracking {

namespace {

/// The beat periods the typical lag is taken between: 200 BPM's and 40 BPM's.
constexpr double kShortestBeatSeconds = 0.3;
constexpr double kLongestBeatSeconds = 1.5;
constexpr double kNotANumber = std::numeric_limits<double>::quiet_NaN();

std::size_t framesIn(double seconds, double framesPerSecond) noexcept {
    const double frames = std::round(seconds * framesPerSecond);
    return frames > 0.0 ? static_cast<std::size_t>(frames) : 0;
}

/// The nearest whole frame to a non-negative number of them, a half rounded up.
std::size_t nearestFrame(double frames) noexcept {
    return static_cast<std::size_t>(std::floor(frames + 0.5));
}

/// Linear between the two nearest ranks of `sorted`, as numpy's default percentile is.
double percentile(const float* sorted, std::size_t count, double percent) noexcept {
    const double at = percent / 100.0 * static_cast<double>(count - 1);
    const auto below = static_cast<std::size_t>(at);
    const std::size_t above = std::min(below + 1, count - 1);
    const double low = static_cast<double>(sorted[below]);
    return low + (at - static_cast<double>(below)) * (static_cast<double>(sorted[above]) - low);
}

} // namespace

PulseGate::PulseGate() noexcept : PulseGate(Options{}) {}

PulseGate::PulseGate(Options options) noexcept : options_(options) {
    const double rate = options_.framesPerSecond > 0.0 ? options_.framesPerSecond : 50.0;
    window_ = std::clamp<std::size_t>(framesIn(options_.windowSeconds, rate), 1, kMaxFrames);
    every_ = std::max<std::size_t>(framesIn(options_.everySeconds, rate), 1);
    lagLow_ = std::max<std::size_t>(framesIn(kShortestBeatSeconds, rate), 2);
    lagHigh_ = framesIn(kLongestBeatSeconds, rate);
}

void PulseGate::reset() noexcept {
    head_ = 0;
    filled_ = 0;
    sinceJudged_ = 0;
    open_ = false;
    spread_ = 0.0;
    repeat_ = -1.0;
    floor_ = 0.0;
    twice_ = 0.0;
    bar_ = 0.0;
}

void PulseGate::push(float beat, float downbeat, double periodFrames) noexcept {
    // Not a number is no activation: one would make every judgement after it false, and the
    // sort below undefined.
    beats_[head_] = std::isfinite(beat) ? beat : 0.0f;
    downbeats_[head_] = std::isfinite(downbeat) ? downbeat : 0.0f;
    head_ = (head_ + 1) % window_;
    filled_ = std::min(filled_ + 1, window_);
    if (++sinceJudged_ >= every_) {
        sinceJudged_ = 0;
        judge(periodFrames);
    }
}

void PulseGate::autocorrelate(std::size_t count, double energy, std::size_t upTo) noexcept {
    for (std::size_t lag = 0; lag <= upTo && lag < count; ++lag) {
        double sum = 0.0;
        for (std::size_t i = 0; i + lag < count; ++i) {
            sum += centred_[i] * centred_[i + lag];
        }
        acf_[lag] = sum / energy;
    }
}

double PulseGate::peakNear(std::size_t lag, std::size_t count) const noexcept {
    if (lag < 2 || lag + 1 >= count) {
        return kNotANumber;
    }
    return std::max({acf_[lag - 1], acf_[lag], acf_[lag + 1]});
}

double PulseGate::medianOf(std::size_t from, std::size_t to) noexcept {
    const std::size_t count = to - from + 1;
    std::copy(acf_.begin() + static_cast<std::ptrdiff_t>(from),
              acf_.begin() + static_cast<std::ptrdiff_t>(to + 1), scratch_.begin());
    const auto middle = scratch_.begin() + static_cast<std::ptrdiff_t>(count / 2);
    std::nth_element(scratch_.begin(), middle, scratch_.begin() + static_cast<std::ptrdiff_t>(count));
    double median = *middle;
    if (count % 2 == 0) {
        median = (median + *std::max_element(scratch_.begin(), middle)) / 2.0;
    }
    return median;
}

void PulseGate::judge(double periodFrames) noexcept {
    const std::size_t count = filled_;
    // Oldest first: the ring's start once it has gone round, its first slot until then.
    const std::size_t oldest = filled_ == window_ ? head_ : 0;
    const auto at = [&](std::size_t i) { return (oldest + i) % window_; };

    repeat_ = -1.0;
    twice_ = kNotANumber;
    bar_ = kNotANumber;
    if (lagLow_ <= lagHigh_ && count > lagHigh_ + 1 && periodFrames >= static_cast<double>(lagLow_) &&
        periodFrames <= static_cast<double>(lagHigh_)) {
        double mean = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            mean += static_cast<double>(beats_[at(i)]);
        }
        mean /= static_cast<double>(count);
        double energy = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            centred_[i] = static_cast<double>(beats_[at(i)]) - mean;
            energy += centred_[i] * centred_[i];
        }
        // Flat: nothing repeats more at one lag than at another.
        repeat_ = 0.0;
        if (energy > 1e-12) {
            autocorrelate(count, energy, 2 * lagHigh_ + 2);
            const double typical = medianOf(lagLow_, lagHigh_);
            // The decoder's period, and an octave either side of it: a decoder counting twice or
            // half the music's beat is still on a period the activation repeats at. The highest,
            // and of two as high the longer.
            std::size_t period = nearestFrame(periodFrames);
            double best = peakNear(period, count);
            for (const double octave : {2.0, 0.5}) {
                const std::size_t lag = nearestFrame(periodFrames * octave);
                if (lag >= lagLow_ && lag <= lagHigh_) {
                    const double peak = peakNear(lag, count);
                    if (peak > best || (peak == best && lag > period)) {
                        best = peak;
                        period = lag;
                    }
                }
            }
            repeat_ = best - typical;
            twice_ = peakNear(2 * period, count) - typical;

            // The downbeat activation, at a bar of three or four of those beats, against its own
            // typical lag between two and a half beats and four and a half.
            double downMean = 0.0;
            for (std::size_t i = 0; i < count; ++i) {
                downMean += static_cast<double>(downbeats_[at(i)]);
            }
            downMean /= static_cast<double>(count);
            double downEnergy = 0.0;
            for (std::size_t i = 0; i < count; ++i) {
                centred_[i] = static_cast<double>(downbeats_[at(i)]) - downMean;
                downEnergy += centred_[i] * centred_[i];
            }
            if (downEnergy > 1e-12 && count > 4 * period + 2) {
                autocorrelate(count, downEnergy, count - 1);
                const std::size_t low = std::max<std::size_t>(2, (5 * period) / 2);
                const std::size_t high = std::min(count - 3, (9 * period) / 2);
                double bars = kNotANumber;
                for (const std::size_t beatsInBar : {std::size_t{3}, std::size_t{4}}) {
                    const std::size_t lag = beatsInBar * period;
                    if (lag + 3 <= count) {
                        const double peak = *std::max_element(
                            acf_.begin() + static_cast<std::ptrdiff_t>(lag - 2),
                            acf_.begin() + static_cast<std::ptrdiff_t>(lag + 3));
                        bars = std::isnan(bars) ? peak : std::max(bars, peak);
                    }
                }
                if (!std::isnan(bars) && high > low) {
                    bar_ = bars - medianOf(low, high);
                }
            }
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        sorted_[i] = beats_[at(i)];
    }
    std::sort(sorted_.begin(), sorted_.begin() + static_cast<std::ptrdiff_t>(count));
    spread_ = percentile(sorted_.data(), count, 95.0) - percentile(sorted_.data(), count, 5.0);
    floor_ = percentile(sorted_.data(), count, 10.0);

    // Not a number fails every test it is in, as it should: there was no room to look.
    const int signs = (twice_ >= options_.minTwice ? 1 : 0) + (bar_ >= options_.minBar ? 1 : 0) +
                      (repeat_ >= options_.strongRepeat ? 1 : 0);
    if (spread_ >= options_.minSpread && repeat_ >= options_.minRepeat &&
        (floor_ <= options_.maxFloor || signs >= 2)) {
        open_ = true;
    }
}

} // namespace takt4::tracking
