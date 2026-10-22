#pragma once

#include <algorithm>
#include <cstdint>

namespace takt4::audio {

/// **Audio the device never delivered, as time** — so the sample clock can be moved past it.
///
/// The input's sample clock counts the frames that arrive, and a dropout, a skipped buffer or a
/// paused WASAPI loopback delivers none: the clock stands still while the room's time goes on.
/// Stamped through a line fitted to the two (`HostTimeFit`), that put every beat early by the
/// whole gap at once and then late by a third of it seven seconds on, for the ten seconds the
/// line took to forget it — a 40 ms dropout was 40 ms early, then 13 ms late (simulated
/// 2026-10-07); a loopback paused for thirty seconds stamped the next beats thirty seconds early,
/// and Link was snapped to a phase that meant nothing. Counted into the sample time the line is
/// given, the gap is a stretch of samples nobody heard, and the line runs straight through it.
///
/// Each buffer is measured against the one before: it should have arrived one buffer's length
/// after it. **A buffer later than that** by more than `kSlackSeconds` — or three quarters of a
/// buffer, for a driver whose buffers are long — **is not yet time lost**: a driver that held the
/// audio while the thread was held up hands it over late and complete, then the buffers that
/// waited behind it one straight after another. Only the buffers after it can tell the two
/// apart. Their lateness is added up until one comes on time again: held, and the catch-up has
/// made it all up; lost, and what is left is the gap, counted from the late buffer's first frame
/// on. Counted at once, as it was, a held buffer was stamped as if the gap were real — WASAPI's
/// 10 ms buffers after a 25 ms stall were stamped 25 and 15 ms late, and 5 ms of it stayed
/// counted for seconds.
///
/// So the late buffer itself is stamped as if nothing were lost. When something was, that is
/// wrong for the hops that begin in it and are finished by it — none, while a hop is longer
/// than a buffer — and right for every hop after.
///
/// The callback's side only: one writer, the audio thread. Allocation-free and lock-free.
class LostTime {
public:
    /// How far a buffer's arrival may stray from when it was due before that is anything but
    /// a thread woken a little late — on a rig whose output thread runs at MMCSS "Pro Audio",
    /// well inside this.
    static constexpr double kSlackSeconds = 0.004;
    /// How much sooner than due a buffer has to come, after a late one, to be one that waited
    /// behind it — or a quarter of a buffer, for a driver whose buffers are shorter than a few
    /// milliseconds. Handed over as soon as the one before was, it is nearly a whole buffer early,
    /// and the last of them anything up to that. A thread woken a little early is inside it.
    static constexpr double kEarlySeconds = 0.001;

    struct Reading {
        /// Seconds lost before this buffer, all told, since the stream started: what to add to
        /// its sample time.
        double seconds = 0.0;
        /// The frame, counted among those delivered, that the last of it was lost before. Audio
        /// before it was heard before the gap; from it on, after.
        std::uint64_t sinceFrame = 0;
        /// This buffer came late, or in the run after a late one before the buffers were on time
        /// again: when it came does not say when it was heard.
        bool delayed = false;
    };

    /// A buffer of `frames` at `rate` arrived at `nowNanos` on the steady clock.
    Reading onBuffer(std::uint32_t frames, double rate, std::int64_t nowNanos) noexcept {
        if (frames == 0) {
            // Says nothing about time, and the next buffer is measured from the one before it.
            return Reading{lost_, sinceFrame_, running_};
        }
        if (lastNanos_ != 0 && rate > 0.0) {
            // This buffer filled up in the gap before it, so its own length is what the gap
            // should have been — `CallbackClock`'s reasoning, for the same callbacks.
            const double length = static_cast<double>(frames) / rate;
            const double came = static_cast<double>(nowNanos - lastNanos_) / 1e9;
            const double late = came - length;
            const double band = std::max(kSlackSeconds, 0.75 * length);
            const double early = std::min(kEarlySeconds, 0.25 * length);
            if (late > band) {
                if (!running_) {
                    running_ = true;
                    owed_ = 0.0;
                    runFrame_ = frames_;
                }
                owed_ += late;
            } else if (running_ && late < -early) {
                // Each makes up more than `early` of it, so a run is over within owed ÷ early
                // buffers whatever comes — and a crystal off by a few per cent is never early.
                owed_ += late;
                if (owed_ < early) {
                    running_ = false; // all of it made up: held, not lost
                }
            } else if (running_) {
                // On time again: whatever the run did not make up is the gap.
                const double gap = owed_ + late;
                if (gap > band) {
                    lost_ += gap;
                    sinceFrame_ = runFrame_;
                }
                running_ = false;
            }
        }
        lastNanos_ = nowNanos;
        frames_ += frames;
        return Reading{lost_, sinceFrame_, running_};
    }

    double seconds() const noexcept { return lost_; }

private:
    std::int64_t lastNanos_ = 0;
    /// Frames delivered before the next buffer.
    std::uint64_t frames_ = 0;
    double lost_ = 0.0;
    std::uint64_t sinceFrame_ = 0;
    /// A late buffer and the ones after it, until they are on time again: how late they have
    /// come between them, and where the first began.
    bool running_ = false;
    double owed_ = 0.0;
    std::uint64_t runFrame_ = 0;
};

} // namespace takt4::audio
