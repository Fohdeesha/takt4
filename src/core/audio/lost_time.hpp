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
/// after it. Arriving later than that by more than `kSlackSeconds` — or three quarters of a buffer,
/// for a driver whose buffers are long — is time lost, and is counted. Arriving earlier by as much
/// gives back what was counted, up to all of it: a buffer held up and then handed over with the
/// next one, nothing lost, nets out to nothing.
///
/// The callback's side only: one writer, the audio thread. Allocation-free and lock-free.
class LostTime {
public:
    /// How far a buffer's arrival may stray from when it was due before that is anything but
    /// a thread woken a little late — on a rig whose output thread runs at MMCSS "Pro Audio",
    /// well inside this.
    static constexpr double kSlackSeconds = 0.004;

    /// A buffer of `frames` at `rate` arrived at `nowNanos` on the steady clock. Returns the
    /// seconds lost before it, all told, since the stream started: what to add to its sample time.
    double onBuffer(std::uint32_t frames, double rate, std::int64_t nowNanos) noexcept {
        if (lastNanos_ != 0 && rate > 0.0) {
            // This buffer filled up in the gap before it, so its own length is what the gap
            // should have been — `CallbackClock`'s reasoning, for the same callbacks.
            const double length = static_cast<double>(frames) / rate;
            const double came = static_cast<double>(nowNanos - lastNanos_) / 1e9;
            const double late = came - length;
            const double band = std::max(kSlackSeconds, 0.75 * length);
            if (late > band) {
                lost_ += late;
            } else if (late < -band && lost_ > 0.0) {
                lost_ = std::max(0.0, lost_ + late);
            }
        }
        lastNanos_ = nowNanos;
        return lost_;
    }

    double seconds() const noexcept { return lost_; }

private:
    std::int64_t lastNanos_ = 0;
    double lost_ = 0.0;
};

} // namespace takt4::audio
