#pragma once

#include "core/engine/beat_engine.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <cstdint>
#include <optional>

namespace takt4::output {

/// One beat for §5.8's rules and §5.6's namespace to act on, and when it is in the music.
struct ScheduledBeat {
    /// The beat as the namespace sends it: its tempo, its place in the bar, whether it is a
    /// downbeat, whether the tracker is locked.
    tracking::BeatEvent event;
    /// `TempoState::beats` and `bars` at it — what the beat- and bar-counting triggers count.
    std::uint64_t beats = 0;
    std::uint64_t bars = 0;
    /// When it is in the music, on the output thread's clock. Ahead of now for a beat fired on
    /// a prediction; behind it for one fired as it was heard.
    double moment = 0.0;
    /// Fired ahead of being heard, on a prediction.
    bool predicted = false;
};

/// **When a beat's messages are sent** — the audit's H4, and the operator's call of 2026-09-23
/// to schedule by prediction (Q5).
///
/// A beat reaches the output thread a pipeline's worth after it was in the music — tens of
/// milliseconds, the network and the decoder between them — and a message cannot be sent
/// before the thing it is about has been heard. So a negative offset, the direction an operator
/// actually needs, used to be honoured by holding every message until just before the *next*
/// beat: a bar-1 clip cue landed before beat 2, `/takt4/beat/bar 1` labelled the wrong beat, and
/// a message about nothing in particular waited most of a beat for no reason.
///
/// While the tracker is locked its tempo and phase say when the next beat will be, and that is
/// what this uses: each beat is fired **ahead of its moment by as much as the earliest output
/// needs** (`Transports::leadSeconds`), and every output is then held to its own offset from
/// that moment. When the beat is heard it is recognised as one already fired and nothing
/// happens twice. Hunting, there is nothing to predict with, and a beat fires the moment it is
/// heard — as early as it can be.
///
/// **What a prediction cannot know**, and what this does about it:
///
///   * A DOWNBEAT pressed on a beat already heard relabels that beat. `restate` folds the
///     tracker's newest state in between beats, so the beats predicted after it count from the
///     new bar.
///   * A DOWNBEAT pressed *before* a beat moves that beat, which may already have been fired
///     with its old label. It is not fired again — two fires of one beat would be worse — and
///     the bar is right from the beat after.
///   * The tracker misses a beat in a quiet passage. The prediction goes on for up to
///     `kMaxAhead` beats past the last one heard, which carries the outputs through the gap;
///     past that it waits to hear one.
///
/// Pure: times in, beats out, no clock and no thread of its own, so a test drives it exactly.
class BeatScheduler {
public:
    /// How many beats past the last one heard a prediction may run. Two: enough for an
    /// offset larger than a beat less the pipeline, and to carry the outputs over one missed
    /// beat, and no further — a tracker that has heard nothing for two beats is not one whose
    /// phase is worth betting a cue on.
    static constexpr std::uint32_t kMaxAhead = 2;
    /// The share of a beat within which a beat heard is the beat already fired. A beat's stamp
    /// jitters by a few milliseconds; a third of a beat is a hundred and fifty at 128 BPM.
    static constexpr double kSameBeat = 0.35;

    /// A beat the tracker called, which was in the music at `moment` on the output thread's
    /// clock. Returns it to fire now — or nothing when a prediction has already fired it, or
    /// when it is more than `staleAfter` seconds old, a backlog from a stalled worker whose
    /// cues would land in a burst for music long gone (the audit's M15).
    ///
    /// Either way it becomes what the next beats are predicted from.
    ///
    /// **No moment is no timeline**: a beat with no host-clock stamp — offline, where a file is
    /// worked through faster than it plays, and in tests that feed audio the same way — says
    /// nothing about when the next one will be, and two of them can arrive a millisecond apart.
    /// Every one is fired as it arrives, at now, and nothing is predicted from it.
    std::optional<ScheduledBeat> heard(const engine::EngineBeat& beat, std::optional<double> moment,
                                       double now, double staleAfter);

    /// The tracker's newest state, between beats. See the class note.
    void restate(const tracking::TempoState& state) noexcept;

    /// The next predicted beat whose time to fire — its moment plus `lead`, zero or negative —
    /// has come, or nothing. A predicted beat already more than `staleAfter` past its moment is
    /// passed over rather than fired, for the reason `heard` gives. Call until it returns
    /// nothing: a large lead can make two due in one round.
    std::optional<ScheduledBeat> due(double now, double lead, double staleAfter);

    /// Forgets everything, for a run that starts again from nothing.
    void reset() noexcept;

    /// Beats fired on a prediction, beats fired as heard, heard beats that a prediction had
    /// already fired, and heard beats dropped as stale.
    std::uint64_t predictedFires() const noexcept { return predicted_; }
    std::uint64_t heardFires() const noexcept { return heardFired_; }
    std::uint64_t matched() const noexcept { return matched_; }
    std::uint64_t stale() const noexcept { return stale_; }

private:
    /// What the anchor's beat is, `steps` beats on: its place in the bar and the bars counted.
    ScheduledBeat stepped(std::uint32_t steps) const noexcept;
    /// Records `beat` as fired, keeping the counts a rule sees from ever going backwards.
    ScheduledBeat fire(ScheduledBeat beat) noexcept;

    /// The last beat heard, which predictions are made from, and how far apart the beats it
    /// is on are — the published beat spacing, which is not always sixty over the published
    /// tempo: see `beatSeconds`.
    ScheduledBeat anchor_;
    double period_ = 0.0;
    bool haveAnchor_ = false;
    /// Whether the tracker's newest state is below the confidence gate. A held tempo is
    /// something to publish, not something to predict beats from.
    bool holding_ = false;
    /// Predictions fired from this anchor.
    std::uint32_t ahead_ = 0;
    /// The latest moment fired, and the counts it carried.
    double firedMoment_ = 0.0;
    bool firedAny_ = false;
    std::uint64_t firedBeats_ = 0;
    std::uint64_t firedBars_ = 0;
    /// The tracker's own count at the last beat heard, which is what tells a restart of it.
    std::uint64_t firedCount_ = 0;
    std::uint64_t predicted_ = 0;
    std::uint64_t heardFired_ = 0;
    std::uint64_t matched_ = 0;
    std::uint64_t stale_ = 0;
};

/// How far apart the beats the tracker publishes are, in seconds — which is sixty over the
/// published tempo **only when the beats are on the published grid**. A ×2 doubles the number
/// and cannot double the beats, and a fold the music has not yet backed halves the number and
/// leaves the beats alone (`TempoState::beatDivisor`); in both the beats are an octave from the
/// tempo. `TempoState::gridBpm` is the tracker's answer, and it is used whenever it is set: it
/// also covers a number left behind by the beats at some other ratio — a lock lost at 176 over a
/// record at 130 — which no octave of the number reaches. Without one (a state built by hand),
/// worked out from the rate the filter calls beats at and the divisor, snapped to the octave of
/// the published tempo so the refined tempo's precision is kept. Zero with no tempo.
double beatSeconds(const tracking::TempoState& state, double bpm) noexcept;

} // namespace takt4::output
