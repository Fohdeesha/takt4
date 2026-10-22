#pragma once

#include "core/audio/host_time_fit.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace takt4::timing {

/// **Where a run of evenly spaced ticks is** — a MIDI clock's, 24 to the beat — as a straight line
/// through the moments they arrived, seen through the ones that arrived late.
///
/// The ticks are numbered as they come, and each is stamped when it arrives. A straight line
/// through the last `Options::window` of them — `audio::HostTimeFit`, the calculation that times
/// the audio input — says when any tick was or will be, and so the tempo. A plain line gets two
/// things wrong, both measured (takt4's own clock out through a loopback port, 2026-10-15):
///
///   * **Deliveries held up.** On a busy machine a tick can reach the driver's thread tens of
///     milliseconds late — 29 ms measured, sixteen ticks in a row — and never early, and a line
///     through those is dragged late by milliseconds. So a tick further off the line than
///     `Options::spreads` times the ticks' own spread (`Options::leastTolerance` at the least) is
///     held back, not taken.
///   * **A tempo that has moved** — a DJ's pitch fader, a DAW's new tempo. From then on every tick
///     is off the old line on one side, and a fit that held them all back would never move again.
///     But held-up deliveries are irregular (2, 20, 6, 28 ms late in a row, measured), and a new
///     tempo is a straight line. So ticks in a row off the line on one side, lying on a straight
///     line of their own (`Options::straightSpreads`) at a believable tempo
///     (`Options::widestChange`), are the new tempo, and the line starts again from them:
///     `Options::earlyRun` of them early — nothing held up arrives early — or `Options::lateRun`
///     late, since lateness that grows tick by tick is a line too (five ticks of it measured, as
///     a machine got busier). And `Options::giveUpAfter` in a row off the line on one side are
///     taken whatever they look like: no measured hold-up lasted that long, and a clock that
///     really has gone elsewhere is never left behind.
///
/// The tolerances scale with the sender's own spread: a clock sent on a millisecond grid sits a
/// third of a millisecond off its line, a jittery one further, and holding back what is only a
/// sender's own scatter would pull the line early.
///
/// Not thread-safe: one thread feeds it and reads it.
class TickFit {
public:
    struct Options {
        /// Ticks the line goes through: two beats of a MIDI clock. Four beats, replayed through the
        /// same measured deliveries, held the line closer through the held-up ones (0.8 ms off at
        /// worst against 1.6) and followed a 2 % pitch move made over a second half as well
        /// (2.8 ms off at worst against 1.1).
        std::size_t window = 48;
        /// Ticks taken whatever they say before any is held back: until then there is no spread
        /// to measure them by.
        std::size_t firstTaken = 6;
        /// How far off the line a tick may be and still be taken, in spreads — the spread being
        /// what a normal distribution with the ticks' median distance from the line would have.
        double spreads = 4.0;
        /// And never less than this, in microseconds: a clock sent on a millisecond grid.
        double leastTolerance = 1000.0;
        /// Ticks in a row early of the line that may be a new tempo, and late of it: late needs
        /// more, longer than the longest run of growing lateness measured (five).
        std::size_t earlyRun = 4;
        std::size_t lateRun = 8;
        /// How straight their own line has to be to be one, in spreads, and in microseconds at the
        /// least.
        double straightSpreads = 2.0;
        double leastStraight = 500.0;
        /// How far its tempo may be from the old one: within this factor of it, either way — a
        /// little past an octave. Further is not a tempo but ticks queued up and delivered in a
        /// bunch, as good as all at once.
        double widestChange = 2.5;
        /// Ticks in a row off the line on one side that are a new tempo however they look: a beat
        /// of a MIDI clock, half as long again as the longest run of held-up deliveries measured.
        std::size_t giveUpAfter = 24;
        /// **A burst, not a tempo**: while a run's line is still being started (`firstTaken`), a
        /// tick that arrives closer than this to the one before it — or than a quarter of the
        /// tempo the run was started at, when that is wider — was held up and handed over in a
        /// bunch with others, and says nothing about where the ticks are. The line starts again,
        /// and the run's line is made from ticks that arrive at their own pace. A quarter of the
        /// gap between two ticks at 400 BPM, the fastest clock followed: no clock that slow sends
        /// two this close (a delivery stall's 26 ticks, handed over 5 µs apart, were once fitted
        /// as 500 000 BPM).
        double leastGap = 60e6 / (400.0 * 24.0) / 4.0;
    };

    TickFit();
    explicit TickFit(Options options);

    /// Tick `index` arrived at `atMicros`. Ticks come in order of their numbers.
    void add(std::int64_t index, std::int64_t atMicros) noexcept;
    /// Forgets every tick, keeping the counts: a clock that stopped and started again is a new
    /// run of ticks, at whatever tempo and phase it starts with.
    void startAgain() noexcept;
    /// The same, for a clock taken to have started again at a tempo it has had before: until
    /// `Options::firstTaken` ticks have come to measure it by, the ticks are taken to be
    /// `microsPerTick` apart, and the line is there from the first of them.
    void startAgainAt(double microsPerTick) noexcept;
    /// Forgets everything, the counts too.
    void reset() noexcept;

    /// Whether there is a line: two ticks taken, or one at a tempo known from before.
    bool fitted() const noexcept {
        return line_.points() >= 2 || (assumed_ && line_.points() >= 1);
    }
    /// When tick `index` was or will be on the line, in microseconds. Fitted lines only.
    double momentOf(double index) const noexcept { return line_.at(index); }
    /// Which tick the line is at at `atMicros`, fractionally. Fitted lines only.
    double indexAt(std::int64_t atMicros) const noexcept;
    /// Microseconds from one tick to the next. Fitted lines only.
    double microsPerTick() const noexcept { return line_.slope(); }
    /// How many ticks the line goes through: its own measurement once this reaches two, or more
    /// than `Options::firstTaken` after a start at a tempo known from before.
    std::size_t taken() const noexcept { return line_.points(); }

    /// Ticks held back as deliveries held up — never taken.
    std::uint64_t heldBack() const noexcept { return heldBack_; }
    /// Ticks that arrived bunched as a run was starting, and started its line again (`leastGap`).
    std::uint64_t bunched() const noexcept { return bunched_; }
    /// How many times the line started again from a new tempo.
    std::uint64_t tempoChanges() const noexcept { return tempoChanges_; }

private:
    struct Tick {
        std::int64_t index = 0;
        std::int64_t at = 0;
    };

    void take(const Tick& tick) noexcept;
    /// Whether the last `count` ticks of the run held back are a tempo of their own rather than
    /// deliveries held up.
    bool isNewTempo(std::size_t count, double spread) noexcept;
    /// The line starts again from the last `count` ticks of the run.
    void startFromRun(std::size_t count) noexcept;

    Options options_;
    audio::HostTimeFit line_;
    /// A run's own line, to see whether it is straight.
    audio::HostTimeFit runLine_;
    /// The ticks held back in a row on one side, newest last, up to `giveUpAfter`.
    std::vector<Tick> run_;
    int runSide_ = 0;
    /// The line's slope is a tempo known from before until it has ticks to measure its own by.
    bool assumed_ = false;
    /// The tempo the run was started at (`startAgainAt`), in microseconds a tick, or zero — what a
    /// bunched tick starts the line again at.
    double runStartedAt_ = 0.0;
    /// The run's line is being started — `startAgain` or `startAgainAt`, and fewer than
    /// `Options::firstTaken` ticks taken since — which is when a bunched tick starts it again. Not
    /// after a tempo change within the run (`startFromRun`), whose line is already the ticks'.
    bool starting_ = true;
    std::int64_t lastIndex_ = 0;
    /// When the newest tick arrived, taken or not; and whether one has this run.
    std::int64_t lastArrival_ = 0;
    bool arrived_ = false;
    std::uint64_t heldBack_ = 0;
    std::uint64_t tempoChanges_ = 0;
    std::uint64_t bunched_ = 0;
};

} // namespace takt4::timing
