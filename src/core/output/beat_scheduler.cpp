#include "core/output/beat_scheduler.hpp"

#include <algorithm>
#include <cmath>

namespace takt4::output {

double beatSeconds(const tracking::TempoState& state, double bpm) noexcept {
    if (!(bpm > 0.0)) {
        return 0.0;
    }
    double octave = 1.0;
    if (state.calledBpm > 0.0) {
        // Beats a minute actually emitted: the filter's own rate, divided by the fold. Only its
        // octave is taken — the refined tempo is the finer number.
        const double emitted =
            state.calledBpm / static_cast<double>(std::max<std::uint32_t>(state.beatDivisor, 1));
        octave = std::exp2(std::round(std::log2(emitted / bpm)));
    }
    return 60.0 / (bpm * octave);
}

void BeatScheduler::reset() noexcept {
    *this = BeatScheduler{};
}

ScheduledBeat BeatScheduler::stepped(std::uint32_t steps) const noexcept {
    ScheduledBeat next = anchor_;
    next.predicted = true;
    next.event.snapped = false; // a DOWNBEAT belongs to the beat it was pressed on
    next.beats = anchor_.beats + steps;
    const std::uint32_t meter = anchor_.event.beatsPerBar;
    const std::uint32_t at = anchor_.event.beatInBar;
    if (meter > 0 && at > 0) {
        // Counted on from the anchor's place in the bar, and a bar counted each time beat 1
        // comes round — `TempoTracker::advanceBar`'s rule, so a rule counting bars agrees.
        std::uint64_t bars = anchor_.bars;
        for (std::uint32_t step = 1; step <= steps; ++step) {
            if ((at - 1 + step) % meter == 0) {
                ++bars;
            }
        }
        next.event.beatInBar = (at - 1 + steps) % meter + 1;
        next.bars = bars;
    }
    next.event.downbeat = next.event.beatInBar == 1;
    next.moment = anchor_.moment + static_cast<double>(steps) * period_;
    // What the tracker would stamp on the beat, so a reader of `event.time` sees a time that
    // moves on with the beat rather than the anchor's.
    next.event.time = anchor_.event.time + static_cast<double>(steps) * period_;
    return next;
}

ScheduledBeat BeatScheduler::fire(ScheduledBeat beat) noexcept {
    // Never backwards. A prediction that carried the outputs over a beat the tracker missed
    // has counted one the tracker has not, and the tracker's next count would repeat it —
    // which to a rule on every fourth beat is a fire twice running.
    if (firedAny_) {
        beat.beats = std::max(beat.beats, firedBeats_ + 1);
        beat.bars = std::max(beat.bars, firedBars_);
    }
    firedAny_ = true;
    firedMoment_ = beat.moment;
    firedBeats_ = beat.beats;
    firedBars_ = beat.bars;
    return beat;
}

std::optional<ScheduledBeat> BeatScheduler::heard(const engine::EngineBeat& beat,
                                                  std::optional<double> stamped, double now,
                                                  double staleAfter) {
    if (beat.state.beats < firedCount_) {
        // The tracker's count went backwards, which only a restart of it does: a new run, whose
        // beats are nothing to do with what was fired in the last one.
        reset();
    }
    firedCount_ = beat.state.beats;
    ScheduledBeat heard;
    heard.event = beat.event;
    heard.beats = beat.state.beats;
    heard.bars = beat.state.bars;
    heard.moment = stamped.value_or(now);
    if (!stamped) {
        // No timeline: see the header. Fired now, and nothing to predict from.
        haveAnchor_ = false;
        ++heardFired_;
        return fire(heard);
    }
    const double moment = *stamped;

    const double period = beatSeconds(beat.state, beat.event.bpm);
    // Fired already: this is the beat a prediction sent, or one older than it.
    const bool fired = firedAny_ && period > 0.0 && moment <= firedMoment_ + kSameBeat * period;

    anchor_ = heard;
    period_ = period;
    haveAnchor_ = true;
    holding_ = beat.state.holding;
    ahead_ = 0;

    if (fired) {
        ++matched_;
        return std::nullopt;
    }
    if (now - moment > staleAfter) {
        ++stale_;
        return std::nullopt;
    }
    ++heardFired_;
    return fire(heard);
}

void BeatScheduler::restate(const tracking::TempoState& state) noexcept {
    // Only the state *of the anchor's beat*: a newer count means a newer beat is on its way,
    // and it will be the anchor in a moment.
    if (!haveAnchor_ || state.beats != anchor_.beats) {
        return;
    }
    anchor_.event.bpm = state.bpm;
    anchor_.event.locked = state.locked;
    anchor_.event.confidence = state.confidence;
    anchor_.event.beatsPerBar = state.beatsPerBar;
    anchor_.event.beatInBar = state.beatInBar;
    anchor_.event.downbeat = state.beatInBar == 1;
    anchor_.bars = state.bars;
    holding_ = state.holding;
    period_ = beatSeconds(state, state.bpm);
}

std::optional<ScheduledBeat> BeatScheduler::due(double now, double lead, double staleAfter) {
    // Only from a locked tracker that believes what it is hearing: a hunting tempo flips
    // octaves and a held one is the last good number, and neither says where the next beat is.
    if (!haveAnchor_ || !anchor_.event.locked || holding_ || !(period_ > 0.0)) {
        return std::nullopt;
    }
    while (ahead_ < kMaxAhead) {
        const std::uint32_t steps = ahead_ + 1;
        const double moment = anchor_.moment + static_cast<double>(steps) * period_;
        if (firedAny_ && moment <= firedMoment_ + kSameBeat * period_) {
            // Fired from the anchor before this one.
            ahead_ = steps;
            continue;
        }
        if (now < moment + lead) {
            return std::nullopt;
        }
        ahead_ = steps;
        if (now - moment > staleAfter) {
            // Its moment has long gone — the output thread itself was held up — and a cue for
            // it now would be one of a burst.
            continue;
        }
        ++predicted_;
        return fire(stepped(steps));
    }
    return std::nullopt;
}

} // namespace takt4::output
