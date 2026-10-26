#pragma once

#include "core/engine/beat_source.hpp"
#include "core/timing/grid_source.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace takt4::timing {

/// **A followed beat grid turned into beats**: what `ExternalBeats` does each round, with no
/// thread, no clock and no ring of its own, so a test drives it one reading at a time.
///
/// The source says where it is on its grid at each instant (`GridReading`). A beat is sent when
/// a reading has passed one, **stamped with the beat's own moment** on Link's clock — worked back
/// from the reading by the tempo, never the instant it was noticed — and sent then, not before:
/// the output thread fires a locked beat ahead of its moment on a prediction where an output
/// needs it early (`output::BeatScheduler`), and an exact grid makes every prediction exact.
///
/// **The published grid is the source's, moved by what the operator asked for**:
///
///   * **The bar** is held as an *anchor*: a downbeat's position on the source's own grid. A
///     source that says where its bars are puts it at 0. DOWNBEAT moves it to the published beat
///     nearest the press — and, for a source with no phase of its own, to the press itself — until
///     the source places its bar again (`GridReading::placements`: a MIDI Start), which puts it
///     back. The beat a placement puts there is sent even when the reading that first sees the
///     placed grid is already a little past it (`GridReading::placedAt`): a Start read together
///     with its first tick.
///   * **÷2 and ×2** scale the grid about the anchor: the published position is
///     `(beat - anchor) * 2^shift`, and a beat is every whole number of it. So ÷2 keeps every
///     second beat counted from a downbeat, and the downbeat survives; ×2 adds one half-way.
///     The beats really are on that grid, so the clocks follow them (`gridBpm` is the published
///     tempo, `clockOctaves` zero) — unlike the tracker's ×2, which cannot double the beats it
///     hears.
///
/// **A source that goes is coasted** at its last tempo and phase for `Options::coastSeconds` —
/// beats go on, the lock stays — and then the beats stop, the lock goes and `noSignal` is
/// raised, exactly as a deck going silent does to the tracker; the output thread lets go of the
/// rig on that edge. When the source comes back it is a new acquisition. A source that says to
/// stop (an obeyed MIDI Stop) is not coasted.
///
/// `beats` only ever goes up within a run, as the output thread's scheduler needs: a grid that
/// jumps — a peer forcing a beat, a phase moved — is followed from wherever it lands, no beat is
/// sent within `kSameBeat` of a beat of the last, no beat the grid has already sent is sent again
/// when it moves back across it, and no two beats are sent closer than `kLeastBeatMicros`.
///
/// **A reading at a tempo no source has is no reading** (`kMinBpm`, `kMaxBpm`), whatever the
/// source says: a MIDI clock's delivery stall once came out as hundreds of thousands of BPM, and a
/// beat went out every millisecond.
class BeatGrid {
public:
    struct Options {
        /// Beats to the bar. Neither Link nor a MIDI clock carries a meter, so it is the
        /// operator's setting, and it is Link's quantum too.
        std::uint32_t beatsPerBar = 4;
        /// How long a source that has gone is run on before the beats stop — the tracker's
        /// `BeatEngine::Options::noSignalSeconds`, so a lost source and a silent deck end alike.
        double coastSeconds = 4.0;
    };

    /// The operator's ÷2 and ×2 reach this far and no further either way: a quarter or four
    /// times the source's tempo, as the tracker's shift is bounded.
    static constexpr std::int32_t kMaxOctaveShift = 2;
    /// How far, in published beats, the grid may move in one reading beyond what its tempo
    /// carries it before that is a jump — a peer forcing a beat, a phase that moved — rather than
    /// time going by. A beat is not sent across a jump; the grid is followed from where it lands.
    static constexpr double kJumpBeats = 0.25;
    /// No beat is sent within this share of a beat of the last: a grid moved back a little — by
    /// no more than `kJumpBeats` — crosses the beat just sent again, and that is the same beat.
    /// Short of half, because ÷2 pressed just after a beat the halved grid does not keep puts the
    /// next beat it does keep exactly half a beat after it, and that one is wanted. A grid moved
    /// back further, but by less than a beat, between nothing the operator did, is caught by the
    /// beat's own number: the one just sent is not sent again. One moved back a beat or more
    /// is a new grid, followed from where it lands.
    static constexpr double kSameBeat = 0.4;
    /// How many gaps between the beats sent `TempoState::beatsBpm` is the median of — the
    /// tracker's own count.
    static constexpr std::size_t kBeatRateGaps = 8;
    /// The tempi a reading may have: Link's own range, the widest any source followed carries.
    static constexpr double kMinBpm = 20.0;
    static constexpr double kMaxBpm = 999.0;
    /// No two beats closer than this: 1000 BPM, past any grid an operator's ×2 makes of a tempo a
    /// source has.
    static constexpr std::int64_t kLeastBeatMicros = 60'000;
    /// A tempo that moves by more than this share between two readings — a DJ halving the Link
    /// session — starts the beats' own rate (`TempoState::beatsBpm`) again: the gaps before it are
    /// the old tempo's, and the window would read them as beats going out at the wrong rate.
    static constexpr double kTempoJump = 0.12;

    BeatGrid();
    explicit BeatGrid(Options options);

    /// A new run: nothing heard and nothing counted, no octave shift, the bar wherever the source
    /// says. Options are taken here and fixed for the run.
    void reset(Options options) noexcept;
    const Options& options() const noexcept { return options_; }

    /// The operator's ÷2 and ×2 — at once, from the reading after. Bounded at `kMaxOctaveShift`.
    void halve() noexcept;
    void redouble() noexcept;
    /// DOWNBEAT: the published beat nearest the press starts the bar, taken at the next reading.
    /// The beat just sent if it is less than half a beat behind — then the bar it began is
    /// declared at once (`TempoState::barsDeclared`), since that beat went out as another — or
    /// the one still to come. A source with no phase of its own takes its phase from the press
    /// too: the press is the beat. Either way the next beat sent carries `BeatEvent::snapped`. A
    /// press with no source to follow points at no beat, and is dropped.
    void snapDownbeat() noexcept;

    /// One reading of the source: the beat it has passed, if it has, or nothing.
    std::optional<engine::EngineBeat> advance(const GridReading& reading) noexcept;

    /// Everything a reader of the source sees, as of the last reading: see `ExternalBeats` for
    /// what each field means here.
    const tracking::TempoState& state() const noexcept { return state_; }
    /// Running on what the source last said, the source having gone.
    bool coasting() const noexcept { return coasting_; }

private:
    /// The grid a reading leaves to follow: the source's, or what it last said run on.
    struct Grid {
        double bpm = 0.0;
        double beat = 0.0;
        bool anchored = false;
        std::uint32_t placements = 0;
        double placedAt = 0.0;
    };

    double scale() const noexcept;
    /// The source's grid at this reading, or nothing to follow — and the lock with it.
    std::optional<Grid> follow(const GridReading& reading) noexcept;
    /// The beats stop and the lock goes, with `noSignal` raised: the source has stopped, or been
    /// gone longer than the coast.
    void goQuiet() noexcept;
    /// Moves the bar's anchor, keeping where the published grid has got to — `lastP_` is on it.
    void moveAnchor(double anchor) noexcept;
    /// The DOWNBEAT pressed, at `at` on this grid. Moves the anchor and, where the press names the
    /// beat just sent, declares its bar; a press that is itself a beat is sent by the reading's
    /// own crossing of it.
    void snap(std::int64_t at, const Grid& grid) noexcept;
    /// The bar the beat just sent began, declared after it went out as another beat.
    void declareBar() noexcept;
    /// The beat at published position `k` and `moment`, after `crossed` whole beats since the
    /// last reading — more than one only when a reading came late — every one counted.
    engine::EngineBeat send(std::int64_t k, std::int64_t moment, std::int64_t crossed) noexcept;
    std::uint32_t beatInBarOf(std::int64_t k) const noexcept;
    /// The fields that move between beats: the lock, the confidence, the beats' own rate.
    void publish(std::int64_t now) noexcept;
    /// Forgets the gaps the beats' own rate is measured over: the grid they were sent on is gone.
    void forgetRate() noexcept;

    Options options_;
    tracking::TempoState state_;

    /// When the run's first reading was, for `BeatEvent::time`.
    bool started_ = false;
    std::int64_t origin_ = 0;
    /// The last reading the source was there for, which a coast runs on from.
    GridReading good_;
    bool haveGood_ = false;
    bool coasting_ = false;
    /// Whether the last grid followed was anchored — a source that gains an anchor (a MIDI Start)
    /// takes its own bar back.
    bool wasAnchored_ = false;
    /// And how many times it had placed its bar (`GridReading::placements`).
    std::uint32_t placements_ = 0;
    /// The source has placed its bar and no whole beat of it has been crossed since: the first
    /// is the bar's downbeat however soon it comes after the beat sent before.
    bool placedUncrossed_ = false;

    /// See the class: the bar's downbeat on the source's grid, and whether anything has said
    /// where it is — the source, or a DOWNBEAT. Until then `beatInBar` is 0 and no bar is counted.
    double anchor_ = 0.0;
    bool barSet_ = false;
    std::int32_t octaveShift_ = 0;

    /// The published position at the last reading, and when it was.
    bool haveLast_ = false;
    double lastP_ = 0.0;
    std::int64_t lastAt_ = 0;

    /// The last beat sent: when, and where on the source's grid.
    bool sent_ = false;
    std::int64_t lastSentAt_ = 0;
    double lastSentBeat_ = 0.0;
    /// And its whole published position — while nothing has moved the grid under it since: ÷2, ×2,
    /// DOWNBEAT, a placement and going quiet each renumber the published beats, and forget it.
    std::int64_t lastSentIndex_ = 0;
    bool indexKnown_ = false;
    /// The source's tempo at the last reading followed, for `kTempoJump`.
    double lastSourceBpm_ = 0.0;

    bool snapPending_ = false;
    /// A snap whose new bar has not reached the outputs yet: the next beat sent carries it.
    bool snapUnsent_ = false;

    /// When the last `kBeatRateGaps + 1` beats were sent, oldest first.
    std::array<std::int64_t, kBeatRateGaps + 1> sentAt_{};
    std::size_t sentCount_ = 0;
};

} // namespace takt4::timing
