#include "core/timing/tick_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace takt4::timing {

namespace {

/// The spread of a normal distribution whose median distance from its middle is one: what turns
/// a median distance into the standard deviation it would be for well-behaved scatter.
constexpr double kSpreadPerMedian = 1.4826;

/// What the line's slope is taken as before two ticks have given it one: 120 BPM. Never read —
/// nothing is asked of a line before it is `fitted` — but the fit wants one.
constexpr double kNominalMicrosPerTick = 60e6 / (120.0 * 24.0);

} // namespace

TickFit::TickFit() : TickFit(Options{}) {}

TickFit::TickFit(Options options)
    : options_(options), line_(kNominalMicrosPerTick, options.window, 2),
      runLine_(kNominalMicrosPerTick, std::max({options.earlyRun, options.lateRun, std::size_t{2}}),
               2) {
    run_.reserve(std::max<std::size_t>(options.giveUpAfter, 1) + 1);
}

void TickFit::startAgain() noexcept {
    line_.reset(kNominalMicrosPerTick, 2);
    assumed_ = false;
    runStartedAt_ = 0.0;
    starting_ = true;
    arrived_ = false;
    run_.clear();
    runSide_ = 0;
}

void TickFit::startAgainAt(double microsPerTick) noexcept {
    startAgain();
    line_.reset(microsPerTick, options_.firstTaken);
    assumed_ = true;
    runStartedAt_ = microsPerTick;
}

void TickFit::reset() noexcept {
    startAgain();
    heldBack_ = 0;
    tempoChanges_ = 0;
    bunched_ = 0;
}

double TickFit::indexAt(std::int64_t atMicros) const noexcept {
    const double newest = static_cast<double>(lastIndex_);
    return newest + (static_cast<double>(atMicros) - line_.at(newest)) / line_.slope();
}

void TickFit::take(const Tick& tick) noexcept {
    line_.observe(static_cast<double>(tick.index), static_cast<double>(tick.at));
    lastIndex_ = tick.index;
}

void TickFit::add(std::int64_t index, std::int64_t atMicros) noexcept {
    const Tick tick{index, atMicros};
    const double sincePrevious = static_cast<double>(atMicros - lastArrival_);
    const bool after = arrived_;
    arrived_ = true;
    lastArrival_ = atMicros;
    if (line_.points() < options_.firstTaken) {
        // Taken unchecked, since there is no spread yet to check it by — unless, as the run
        // starts, it came bunched with the one before (`Options::leastGap`): then neither says
        // where the ticks are, and the line starts again from the next that comes at its own
        // pace, at the tempo the run was started at if it had one.
        if (starting_ && after &&
            sincePrevious < std::max(options_.leastGap, runStartedAt_ / 4.0)) {
            ++bunched_;
            const double startedAt = runStartedAt_;
            if (startedAt > 0.0) {
                startAgainAt(startedAt);
            } else {
                startAgain();
            }
            arrived_ = true;
            lastArrival_ = atMicros;
            return;
        }
        take(tick);
        starting_ = starting_ && line_.points() < options_.firstTaken;
        return;
    }
    const double spread = kSpreadPerMedian * line_.medianDistance();
    const double tolerance = std::max(options_.leastTolerance, options_.spreads * spread);
    const double off = static_cast<double>(atMicros) - line_.at(static_cast<double>(index));
    if (std::abs(off) <= tolerance) {
        run_.clear();
        take(tick);
        return;
    }
    ++heldBack_;
    const int side = off > 0.0 ? 1 : -1;
    if (side != runSide_) {
        run_.clear();
        runSide_ = side;
    }
    run_.push_back(tick);
    if (run_.size() >= options_.giveUpAfter) {
        startFromRun(run_.size());
        return;
    }
    const std::size_t needed = side < 0 ? options_.earlyRun : options_.lateRun;
    if (run_.size() >= needed && isNewTempo(needed, spread)) {
        startFromRun(needed);
    }
}

bool TickFit::isNewTempo(std::size_t count, double spread) noexcept {
    runLine_.reset();
    for (auto tick = run_.end() - static_cast<std::ptrdiff_t>(count); tick != run_.end(); ++tick) {
        runLine_.observe(static_cast<double>(tick->index), static_cast<double>(tick->at));
    }
    const double straight = std::max(options_.leastStraight, options_.straightSpreads * spread);
    const double current = line_.slope();
    const double slope = runLine_.slope();
    return kSpreadPerMedian * runLine_.medianDistance() <= straight &&
           slope >= current / options_.widestChange && slope <= current * options_.widestChange;
}

void TickFit::startFromRun(std::size_t count) noexcept {
    // The run is the tempo now: the line starts again from it, and those ticks were not held back
    // after all. Its own slope from its first two: nothing from before is known about it.
    line_.reset(kNominalMicrosPerTick, 2);
    assumed_ = false;
    runStartedAt_ = 0.0;
    starting_ = false;
    for (auto tick = run_.end() - static_cast<std::ptrdiff_t>(count); tick != run_.end(); ++tick) {
        take(*tick);
    }
    heldBack_ -= count;
    ++tempoChanges_;
    run_.clear();
    runSide_ = 0;
}

} // namespace takt4::timing
