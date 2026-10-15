#include "core/timing/beat_grid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace takt4::timing {

namespace {

constexpr double kMicrosPerSecond = 1e6;

std::int64_t wholeBelow(double position) noexcept {
    return static_cast<std::int64_t>(std::floor(position));
}

} // namespace

BeatGrid::BeatGrid() : BeatGrid(Options{}) {}

BeatGrid::BeatGrid(Options options) : options_(options) {
    state_.beatsPerBar = options.beatsPerBar;
}

void BeatGrid::reset(Options options) noexcept {
    *this = BeatGrid(options);
}

double BeatGrid::scale() const noexcept {
    return std::ldexp(1.0, octaveShift_);
}

void BeatGrid::halve() noexcept {
    if (octaveShift_ <= -kMaxOctaveShift) {
        return;
    }
    --octaveShift_;
    // The published position is scaled about the anchor, so where the grid had got to is scaled
    // with it: the next reading is no jump, and the next whole number is the next beat kept.
    lastP_ *= 0.5;
}

void BeatGrid::redouble() noexcept {
    if (octaveShift_ >= kMaxOctaveShift) {
        return;
    }
    ++octaveShift_;
    lastP_ *= 2.0;
}

void BeatGrid::snapDownbeat() noexcept {
    snapPending_ = true;
}

std::optional<BeatGrid::Grid> BeatGrid::follow(const GridReading& reading) noexcept {
    if (reading.stopped) {
        goQuiet();
        return std::nullopt;
    }
    if (reading.present && reading.bpm > 0.0) {
        good_ = reading;
        haveGood_ = true;
        coasting_ = false;
        // A lock since START, or since the source last went quiet: the rig may fire from here.
        state_.locked = true;
        state_.acquired = true;
        state_.noSignal = false;
        return Grid{reading.bpm, reading.beat, reading.anchored, reading.placements};
    }
    if (!state_.locked || !haveGood_) {
        return std::nullopt; // nothing heard since START, or gone quiet since
    }
    const double gone = static_cast<double>(reading.at - good_.at) / kMicrosPerSecond;
    if (gone >= options_.coastSeconds) {
        goQuiet();
        return std::nullopt;
    }
    // Gone, and run on at its last tempo and phase: the lock stays, as the tracker's does while
    // it flywheels.
    coasting_ = true;
    return Grid{good_.bpm, good_.beat + gone * good_.bpm / 60.0, good_.anchored, good_.placements};
}

void BeatGrid::goQuiet() noexcept {
    state_.locked = false;
    state_.acquired = false;
    state_.noSignal = true;
    coasting_ = false;
    haveGood_ = false;
    // Whatever comes back is followed from wherever it is then.
    haveLast_ = false;
}

void BeatGrid::moveAnchor(double anchor) noexcept {
    lastP_ += (anchor_ - anchor) * scale();
    anchor_ = anchor;
}

void BeatGrid::declareBar() noexcept {
    const bool wasBarStart = state_.beatInBar == 1;
    state_.beatInBar = 1;
    if (!wasBarStart) {
        // A bar the count did not make, whose first beat has already gone out as another: the
        // output thread fires its rules now, late by the press (the tracker's M4 path).
        ++state_.bars;
        ++state_.barsDeclared;
        state_.declaredBar = state_.bars;
    }
}

void BeatGrid::snap(std::int64_t at, const Grid& grid) noexcept {
    snapUnsent_ = true;
    barSet_ = true;
    const double scaled = scale();
    const double period = 60.0 * kMicrosPerSecond / (grid.bpm * scaled);
    const bool lastIsNearer = sent_ && static_cast<double>(at - lastSentAt_) < 0.5 * period;
    if (!grid.anchored) {
        // No phase of the source's own — a MIDI clock that has had no Start — so the operator,
        // pressing on a beat they can hear, is the only one who knows where the beats are. The
        // grid moves under the press, which is beat 1: sent by this very reading as the grid
        // crosses it, unless a beat went out less than half a beat ago, which was then that
        // beat, sent a little off the music.
        moveAnchor(grid.beat);
        if (lastIsNearer) {
            declareBar();
        }
        return;
    }
    if (lastIsNearer) {
        // The beat just sent starts the bar, at once — its successor is beat 2.
        moveAnchor(lastSentBeat_);
        declareBar();
        return;
    }
    // The beat still to come starts the bar. Every beat stays where it was.
    const double p = (grid.beat - anchor_) * scaled;
    moveAnchor(anchor_ + (std::floor(p) + 1.0) / scaled);
}

std::uint32_t BeatGrid::beatInBarOf(std::int64_t k) const noexcept {
    const auto meter = static_cast<std::int64_t>(std::max<std::uint32_t>(options_.beatsPerBar, 1));
    return static_cast<std::uint32_t>(((k % meter) + meter) % meter) + 1;
}

engine::EngineBeat BeatGrid::send(std::int64_t k, std::int64_t moment,
                                  std::int64_t crossed) noexcept {
    for (std::int64_t j = k - crossed + 1; j <= k; ++j) {
        ++state_.beats;
        if (barSet_ && beatInBarOf(j) == 1) {
            ++state_.bars;
        }
    }
    // With no bar known, say so with 0 rather than guess one, as the tracker does.
    state_.beatInBar = barSet_ ? beatInBarOf(k) : 0;
    sent_ = true;
    lastSentAt_ = moment;
    lastSentBeat_ = anchor_ + static_cast<double>(k) / scale();
    if (sentCount_ < sentAt_.size()) {
        sentAt_[sentCount_++] = moment;
    } else {
        std::rotate(sentAt_.begin(), sentAt_.begin() + 1, sentAt_.end());
        sentAt_.back() = moment;
    }

    engine::EngineBeat beat;
    beat.event.frameIndex = state_.beats;
    beat.event.time = static_cast<double>(moment - origin_) / kMicrosPerSecond;
    beat.event.beatInBar = state_.beatInBar;
    beat.event.downbeat = state_.beatInBar == 1;
    beat.event.snapped = snapUnsent_;
    snapUnsent_ = false;
    beat.hostMicros = moment;
    return beat;
}

void BeatGrid::publish(std::int64_t now) noexcept {
    state_.beatsPerBar = options_.beatsPerBar;
    state_.confidence = state_.locked ? 1.0 : 0.0;
    // Exact, not estimated from beat spacing: nothing here refines.
    state_.refined = true;
    state_.pinned = false;
    state_.holding = false;
    state_.beatDivisor = 1;
    state_.clockOctaves = 0;
    // The rate the beats are going out at, as the tracker measures its own: the median gap of
    // the last few, and nothing once the next is two gaps late.
    state_.beatsBpm = 0.0;
    if (sentCount_ == sentAt_.size()) {
        std::array<double, kBeatRateGaps> gaps{};
        for (std::size_t i = 0; i < kBeatRateGaps; ++i) {
            gaps[i] = static_cast<double>(sentAt_[i + 1] - sentAt_[i]);
        }
        std::sort(gaps.begin(), gaps.end());
        const double median = 0.5 * (gaps[kBeatRateGaps / 2 - 1] + gaps[kBeatRateGaps / 2]);
        if (median > 0.0 && static_cast<double>(now - sentAt_.back()) <= 2.0 * median) {
            state_.beatsBpm = 60.0 * kMicrosPerSecond / median;
        }
    }
}

std::optional<engine::EngineBeat> BeatGrid::advance(const GridReading& reading) noexcept {
    if (!started_) {
        started_ = true;
        origin_ = reading.at;
    }
    const std::optional<Grid> grid = follow(reading);
    if (!grid) {
        // A DOWNBEAT pressed with nothing to follow points at no beat. Kept, it would put bar 1
        // wherever the source happened to come back in.
        snapPending_ = false;
        publish(reading.at);
        return std::nullopt;
    }
    // A source that has just said where its bars are — a MIDI Start, or another — takes its own
    // bar back, from any DOWNBEAT before it, and is followed from there.
    if (grid->anchored && (!wasAnchored_ || grid->placements != placements_)) {
        // A bar that was somewhere else has moved, which a bridging Link output has to be told
        // at once, as it is told of a DOWNBEAT.
        snapUnsent_ = snapUnsent_ || barSet_;
        anchor_ = 0.0;
        barSet_ = true;
        haveLast_ = false;
        placedUncrossed_ = true;
    }
    wasAnchored_ = grid->anchored;
    placements_ = grid->placements;

    const double scaled = scale();
    const double bpm = grid->bpm * scaled;
    const double period = 60.0 * kMicrosPerSecond / bpm;
    state_.rawBpm = grid->bpm;
    state_.calledBpm = grid->bpm;
    state_.bpm = bpm;
    state_.gridBpm = bpm;
    state_.octaveShift = octaveShift_;

    if (snapPending_) {
        snapPending_ = false;
        snap(reading.at, *grid);
    }

    const double p = (grid->beat - anchor_) * scaled;
    std::optional<engine::EngineBeat> beat;
    // A beat is a whole number of the published position passed since the last reading — unless
    // the grid jumped there rather than ran (`kJumpBeats`), or the beat is the one just sent,
    // moved a little (`kSameBeat`).
    if (haveLast_ &&
        std::abs((p - lastP_) - static_cast<double>(reading.at - lastAt_) / period) <= kJumpBeats) {
        const std::int64_t before = wholeBelow(lastP_);
        const std::int64_t now = wholeBelow(p);
        if (now > before) {
            // Its own moment, worked back from the reading by the tempo: when it was, not when it
            // was noticed.
            const std::int64_t moment =
                reading.at - std::llround((p - static_cast<double>(now)) * period);
            if (!sent_ || static_cast<double>(moment - lastSentAt_) >= kSameBeat * period) {
                beat = send(now, moment, now - before);
            } else if (placedUncrossed_ && beatInBarOf(now) == 1) {
                // The source has just placed its bar, and the bar's first beat comes too soon
                // after the beat just sent to go out as another. The bar is declared on its
                // moment instead — as a DOWNBEAT pressed just after a beat declares one — so its
                // rules fire on time, and the beat's do not fire twice.
                declareBar();
            }
            placedUncrossed_ = false;
        }
    }
    haveLast_ = true;
    lastP_ = p;
    lastAt_ = reading.at;

    publish(reading.at);
    if (beat) {
        beat->event.bpm = state_.bpm;
        beat->event.gridBpm = state_.gridBpm;
        beat->event.clockOctaves = 0;
        beat->event.beatsPerBar = state_.beatsPerBar;
        beat->event.locked = state_.locked;
        beat->event.acquired = state_.acquired;
        beat->event.confidence = state_.confidence;
        beat->state = state_;
    }
    return beat;
}

} // namespace takt4::timing
