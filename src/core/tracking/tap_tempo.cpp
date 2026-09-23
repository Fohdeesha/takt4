#include "core/tracking/tap_tempo.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>

namespace takt4::tracking {

TapTempo::TapTempo() : TapTempo(Options{}) {}

TapTempo::TapTempo(Options options) : options_(options) {
    if (!(options.timeoutSeconds > 0.0)) {
        throw std::invalid_argument("TapTempo: the timeout must be positive");
    }
    if (options.needTaps < 2) {
        throw std::invalid_argument("TapTempo: a tempo needs at least two taps");
    }
    if (options.keepTaps < options.needTaps) {
        throw std::invalid_argument("TapTempo: no more taps may be needed than are kept");
    }
    if (!(options.bounceSeconds >= 0.0) || !(options.minBpm > 0.0) ||
        !(options.maxBpm > options.minBpm)) {
        throw std::invalid_argument(
            "TapTempo: a bounce is a positive gap, and the tempi offered a positive range");
    }
    times_.reserve(options_.keepTaps);
    gaps_.reserve(options_.keepTaps);
}

void TapTempo::reset() noexcept {
    times_.clear();
    gaps_.clear();
    bpm_ = 0.0;
}

std::optional<double> TapTempo::tap(double seconds) noexcept {
    if (!times_.empty()) {
        const double gap = seconds - times_.back();
        // A bounce: the same press seen twice. Ignored, not counted — it is not a tap, and
        // starting a new set on it would throw away a good one. See `Options::bounceSeconds`.
        if (gap >= 0.0 && gap < options_.bounceSeconds) {
            return std::nullopt;
        }
        // Too long a wait is a new attempt, not a very slow tempo. A gap below zero is a clock
        // that went backwards, and dividing by it would report nonsense — start again from this
        // tap either way.
        if (!(gap > 0.0) || gap > options_.timeoutSeconds) {
            times_.clear();
            bpm_ = 0.0;
        }
    }
    if (times_.size() >= options_.keepTaps) {
        times_.erase(times_.begin());
    }
    times_.push_back(seconds);
    if (times_.size() < options_.needTaps) {
        return std::nullopt;
    }

    gaps_.clear();
    for (std::size_t i = 1; i < times_.size(); ++i) {
        gaps_.push_back(times_[i] - times_[i - 1]);
    }
    std::sort(gaps_.begin(), gaps_.end());
    const std::size_t count = gaps_.size();
    const double median =
        count % 2 == 1 ? gaps_[count / 2] : 0.5 * (gaps_[count / 2 - 1] + gaps_[count / 2]);
    if (!(median > 0.0)) {
        return std::nullopt;
    }
    const double tempo = 60.0 / median;
    // Only a tempo a person could have meant. See `Options::minBpm`.
    if (tempo < options_.minBpm || tempo > options_.maxBpm) {
        return std::nullopt;
    }
    bpm_ = tempo;
    return bpm_;
}

} // namespace takt4::tracking
