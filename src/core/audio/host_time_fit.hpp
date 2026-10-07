#pragma once

#include <array>
#include <cstddef>

namespace takt4::audio {

/// **When each sample of the input was in the room**, as a straight line through what the device's
/// buffers have said — the sample clock turned into the host time every output is timed on.
///
/// Each buffer the device delivers is one observation: where its first sample is on the sample
/// clock, and when that sample was at the input — the moment the buffer arrived, less the input
/// latency the driver reports. Any sample's moment is then read off the line through the last
/// `kPoints` of them, which follows the device's crystal against the host clock however the two
/// drift, and averages away when the buffers happened to be handed over.
///
/// **One observation per buffer, at the buffer's first sample**, rather than one per hop at the
/// moment the hop was finished — which is what Link's own `HostTimeFilter` was fed, and what made
/// every stamp carry where in a buffer its hop happened to end: the hops a callback finished all
/// shared the callback's moment, and the line through them sat later the more hops a buffer held
/// (+10 ms at two a callback, +30 at four, simulated 2026-10-07), so a change of buffer size moved
/// every output. A hop is now stamped by reading the line at its own first sample (`at`).
///
/// Fitted relative to the newest point, so the sums stay small whatever the sample clock or the
/// host clock has reached: an uptime of months is 10¹³ microseconds, and the line is wanted to the
/// microsecond. Until `kSettled` points are in, the slope is not fitted but taken as the nominal
/// one — two buffers a few milliseconds apart say nothing reliable about a rate, and the
/// regression's own answer for a single point is a slope of zero, which would stamp every hop of
/// the first buffers at one instant.
///
/// Not thread-safe: one writer and one reader, the same thread — the audio callback.
class HostTimeFit {
public:
    /// The observations a line is fitted through: Link's own choice, 512. A few seconds of
    /// buffers at the smallest sizes, half a minute at the largest.
    static constexpr std::size_t kPoints = 512;
    /// Points before the slope is fitted rather than assumed.
    static constexpr std::size_t kSettled = 16;

    /// `nominalSlope` is host time per unit of sample time at the nominal rates — microseconds
    /// per sample, 10⁶ / 22050 for the internal rate.
    explicit HostTimeFit(double nominalSlope) noexcept : nominalSlope_(nominalSlope) {}

    /// The sample at `sampleTime` was at the input at host time `hostTime`.
    void observe(double sampleTime, double hostTime) noexcept {
        points_[next_] = Point{sampleTime, hostTime};
        next_ = (next_ + 1) % kPoints;
        if (count_ < kPoints) {
            ++count_;
        }
        refit(sampleTime, hostTime);
    }

    /// The host time of `sampleTime` on the line, or zero before anything has been observed.
    double at(double sampleTime) const noexcept {
        if (count_ == 0) {
            return 0.0;
        }
        return anchorHost_ + offset_ + slope_ * (sampleTime - anchorSample_);
    }

    /// Forgets every observation — a stream that starts again counts its samples from zero.
    void reset() noexcept {
        count_ = 0;
        next_ = 0;
    }

    std::size_t points() const noexcept { return count_; }
    /// The fitted slope, host time per unit of sample time.
    double slope() const noexcept { return slope_; }

private:
    struct Point {
        double sample = 0.0;
        double host = 0.0;
    };

    void refit(double anchorSample, double anchorHost) noexcept {
        anchorSample_ = anchorSample;
        anchorHost_ = anchorHost;
        double sumX = 0.0;
        double sumY = 0.0;
        double sumXX = 0.0;
        double sumXY = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            const double x = points_[i].sample - anchorSample;
            const double y = points_[i].host - anchorHost;
            sumX += x;
            sumY += y;
            sumXX += x * x;
            sumXY += x * y;
        }
        const double n = static_cast<double>(count_);
        const double denominator = n * sumXX - sumX * sumX;
        slope_ = count_ >= kSettled && denominator > 0.0 ? (n * sumXY - sumX * sumY) / denominator
                                                        : nominalSlope_;
        offset_ = (sumY - slope_ * sumX) / n;
    }

    double nominalSlope_;
    std::array<Point, kPoints> points_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
    double anchorSample_ = 0.0;
    double anchorHost_ = 0.0;
    double slope_ = 0.0;
    double offset_ = 0.0;
};

} // namespace takt4::audio
