#include "core/audio/input_watchdog.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace takt4::audio {

namespace {

/// The rates an audio clock runs at. A clock that has really moved has moved to one of these;
/// a reading between them is audio that went missing (see `observe`).
constexpr std::array<double, 15> kClockRates{8000.0,   11025.0,  16000.0,  22050.0,  24000.0,
                                             32000.0,  44100.0,  48000.0,  64000.0,  88200.0,
                                             96000.0,  176400.0, 192000.0, 352800.0, 384000.0};

/// How close a reading has to come to one of `kClockRates` to be read as that clock.
///
/// 2.5 % while the window was measured between looks: a look could land anywhere in a buffer at
/// each end of it, which at 4096 frames put a real clock up to two percent off. Between the
/// callbacks' own times a clock that has moved reads as the clock it moved to, to a tenth of a
/// percent, once the window has passed the move — so the match can be closer, and **the closer it
/// is, the more audio a run of dropouts has to lose before it can pass for a clock**: gaps of up
/// to two buffers count as normal (`CallbackClock`), so each dropout can hide up to a buffer of
/// lost audio in the window, and at 48 kHz in 4096-frame buffers two of them within three seconds
/// lose 6 % — 2.3 % off 44.1 kHz, inside the old match, and outside this one.
constexpr double kClockMatch = 0.015;

/// Whether `measured` is a clock rate other than the one the stream was opened at.
bool anotherClock(double measured, double opened, double tolerance) {
    return std::any_of(kClockRates.begin(), kClockRates.end(), [&](double rate) {
        return std::abs(measured / rate - 1.0) <= kClockMatch &&
               std::abs(rate / opened - 1.0) > tolerance;
    });
}

} // namespace

InputWatchdog::InputWatchdog() : InputWatchdog(Options{}) {}

InputWatchdog::InputWatchdog(Options options) : options_(options) {}

void InputWatchdog::reset(double openedRate, double now) {
    openedRate_ = openedRate;
    openedAt_ = now;
    samples_.clear();
}

InputWatchdog::Reading InputWatchdog::observe(const InputStreamCounters& counters, double now) {
    Reading reading;
    reading.inputOverflows = counters.inputOverflows;

    if (counters.callbacks == 0) {
        // Nothing yet. Given its grace period, and then it is as dead as one that stopped.
        if (now - openedAt_ < options_.startupGraceSeconds) {
            reading.verdict = Verdict::Starting;
            return reading;
        }
        reading.verdict = Verdict::Silent;
        reading.silentForSeconds = now - openedAt_;
        samples_.clear();
        return reading;
    }

    // Since the last callback came, as the callback itself timed it — not since the look that
    // happened to find it, which a busy redraw put up to 160 ms later.
    const double silentFor = counters.sinceLastCallbackSeconds;
    if (silentFor >= options_.silentAfterSeconds) {
        reading.verdict = Verdict::Silent;
        reading.silentForSeconds = silentFor;
        // The window starts again when the audio comes back.
        samples_.clear();
        return reading;
    }

    reading.verdict = Verdict::Healthy;
    // **The rate is the callbacks' own**: frames over seconds of the callbacks that came a normal
    // distance after the one before (`CallbackClock`). A dropout is left out with the time it
    // lasted, however far apart the looks were — so a stall no longer has to start the window
    // again, and a slow redraw cannot hide one. Only a look that found the totals moved adds a
    // sample.
    if (samples_.empty() || counters.normalSeconds > samples_.back().normalSeconds) {
        samples_.push_back(Sample{counters.normalSeconds, counters.normalFrames});
    }
    // A little over a window's worth, so there is always a sample a whole window back.
    while (samples_.size() > 2 &&
           samples_.back().normalSeconds - samples_[1].normalSeconds >= options_.rateWindowSeconds) {
        samples_.pop_front();
    }
    const Sample& first = samples_.front();
    const Sample& last = samples_.back();
    const double seconds = last.normalSeconds - first.normalSeconds;
    if (seconds >= options_.rateWindowSeconds && last.normalFrames >= first.normalFrames) {
        reading.measuredRate = static_cast<double>(last.normalFrames - first.normalFrames) / seconds;
        // **Off, and on another clock.** A clock that moved runs at a clock's rate. A reading
        // between the clocks is audio lost inside a gap short enough to count as normal — up to
        // two buffers — and is not a reason to reopen a stream that has recovered.
        if (openedRate_ > 0.0 &&
            std::abs(reading.measuredRate / openedRate_ - 1.0) > options_.rateTolerance &&
            anotherClock(reading.measuredRate, openedRate_, options_.rateTolerance)) {
            reading.verdict = Verdict::RateChanged;
        }
    }
    return reading;
}

} // namespace takt4::audio
