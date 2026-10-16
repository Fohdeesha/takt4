#include "core/timing/prolink_follower.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace takt4::timing {

namespace {

constexpr double kMicrosPerMinute = 60e6;
/// The CDJs' bar, always.
constexpr std::int64_t kBar = 4;

/// The beat period a beat packet says, in microseconds: its tempo, pitch and all — or, where it
/// carries none, the time to its next beat at 0 % pitch, scaled by the pitch. Zero when neither
/// is a tempo to believe.
double periodOf(const prolink::VirtualPlayer::Heard& beat) noexcept {
    const auto believable = [](double bpm) {
        return bpm >= ProLinkFollower::kMinBpm && bpm <= ProLinkFollower::kMaxBpm;
    };
    if (believable(beat.bpm)) {
        return kMicrosPerMinute / beat.bpm;
    }
    if (beat.nextBeatMs > 0 && beat.nextBeatMs != 0xffffffffu && beat.pitch > 0) {
        const double period = static_cast<double>(beat.nextBeatMs) * 1000.0 * 1048576.0 /
                              static_cast<double>(beat.pitch);
        if (believable(kMicrosPerMinute / period)) {
            return period;
        }
    }
    return 0.0;
}

std::int64_t mod(std::int64_t value, std::int64_t by) noexcept {
    return ((value % by) + by) % by;
}

/// The first place on the grid at or after `from` that is beat `beatInBar` of a bar — or `from`
/// itself when the beat's place in the bar is not known.
std::int64_t atOrAfter(std::int64_t from, std::uint8_t beatInBar) noexcept {
    if (beatInBar == 0) {
        return from;
    }
    return from + mod(static_cast<std::int64_t>(beatInBar) - 1 - from, kBar);
}

/// The place on the grid nearest `position` that is beat `beatInBar` of a bar.
std::int64_t nearest(double position, std::uint8_t beatInBar) noexcept {
    const std::int64_t rounded = std::llround(position);
    if (beatInBar == 0) {
        return rounded;
    }
    const std::int64_t above = atOrAfter(rounded, beatInBar);
    const std::int64_t below = above - kBar;
    return std::abs(static_cast<double>(above) - position) <=
                   std::abs(static_cast<double>(below) - position)
               ? above
               : below;
}

} // namespace

ProLinkFollower::ProLinkFollower(CdjFeed& feed) : feed_(feed) {}

double ProLinkFollower::absentAfter(double periodMicros) noexcept {
    return kAbsentBeats * periodMicros + static_cast<double>(kAbsentMarginMicros);
}

void ProLinkFollower::reset() noexcept {
    have_ = false;
    index_ = 0;
    beatAt_ = 0;
    periodMicros_ = 0.0;
    device_ = 0;
    beatInBar_ = 0;
    placements_ = 0;
    placedAt_ = 0.0;
    beats_ = 0;
    repeats_ = 0;
}

void ProLinkFollower::publish(bool present) noexcept {
    Status status;
    status.device = have_ ? device_ : std::uint8_t{0};
    status.beatInBar = have_ ? beatInBar_ : std::uint8_t{0};
    status.present = present;
    status.bpm = have_ && periodMicros_ > 0.0 ? kMicrosPerMinute / periodMicros_ : 0.0;
    status.beats = beats_;
    status.placements = placements_;
    status.repeats = repeats_;
    status_.publish(status);
}

void ProLinkFollower::begin(std::uint32_t) noexcept {
    reset();
    // Beats heard before START are not this run's.
    prolink::VirtualPlayer::Heard stale;
    while (feed_.popBeat(stale)) {
    }
    feed_.join();
    publish(false);
}

void ProLinkFollower::end() noexcept {
    feed_.leave();
    reset();
    publish(false);
}

void ProLinkFollower::place(std::int64_t index) noexcept {
    index_ = index;
    ++placements_;
    placedAt_ = static_cast<double>(index);
}

void ProLinkFollower::take(const prolink::VirtualPlayer::Heard& beat) noexcept {
    double period = periodOf(beat);
    if (period <= 0.0) {
        if (!have_) {
            return; // a first beat with no tempo to run on from it
        }
        period = periodMicros_;
    }
    if (have_ && beat.device == device_) {
        const double since = static_cast<double>(std::max<std::int64_t>(beat.at - beatAt_, 0));
        if (since < kRepeat * periodMicros_ && beat.beatInBar == beatInBar_) {
            ++repeats_;
            return;
        }
        const double expected = static_cast<double>(index_) + since / periodMicros_;
        const std::int64_t next = nearest(expected, beat.beatInBar);
        if (since <= absentAfter(periodMicros_) && next > index_ &&
            std::abs(static_cast<double>(next) - expected) <= kOnGrid) {
            index_ = next; // on the grid: the next beat, or one after a beat lost on the way
        } else {
            place(atOrAfter(index_ + 1, beat.beatInBar));
        }
    } else {
        // The first beat, or another device's: the grid begins here.
        place(have_ ? atOrAfter(index_ + 1, beat.beatInBar) : atOrAfter(0, beat.beatInBar));
    }
    have_ = true;
    beatAt_ = beat.at;
    periodMicros_ = period;
    device_ = beat.device;
    beatInBar_ = beat.beatInBar;
    ++beats_;
}

GridReading ProLinkFollower::read() noexcept {
    prolink::VirtualPlayer::Heard beat;
    while (feed_.popBeat(beat)) {
        take(beat);
    }
    GridReading reading;
    reading.at = feed_.now();
    if (!have_) {
        publish(false);
        return reading;
    }
    const double since = static_cast<double>(std::max<std::int64_t>(reading.at - beatAt_, 0));
    reading.present = since <= absentAfter(periodMicros_);
    reading.bpm = kMicrosPerMinute / periodMicros_;
    double ahead = since / periodMicros_;
    // Each beat still to come is waited for a moment past when the tempo puts it: the packet that
    // is the beat is usually just behind the prediction, and says where in the bar it is.
    const double whole = std::floor(ahead);
    if (whole >= 1.0 && (ahead - whole) * periodMicros_ < static_cast<double>(kWaitMicros)) {
        ahead = whole - kHeld;
    }
    reading.beat = static_cast<double>(index_) + ahead;
    reading.anchored = beatInBar_ != 0;
    reading.placements = placements_;
    reading.placedAt = placedAt_;
    publish(reading.present);
    return reading;
}

} // namespace takt4::timing
