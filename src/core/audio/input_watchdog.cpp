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

/// How close a reading has to come to one of `kClockRates` to be read as that clock. A window
/// of 4096-frame buffers can be out by up to two percent — a look can land anywhere in a
/// buffer at each end of it — and 44.1 and 48 kHz are 8.8 % apart.
constexpr double kClockMatch = 0.025;

/// Timing slack on how far apart two looks may be without anything having gone missing.
constexpr double kPairSlackSeconds = 0.005;

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
    lastLookAt_ = now;
    lastCallbacks_ = 0;
    lastCallbackAt_ = now;
    anyCallback_ = false;
    samples_.clear();
}

InputWatchdog::Reading InputWatchdog::observe(const InputStreamCounters& counters, double now) {
    Reading reading;
    reading.inputOverflows = counters.inputOverflows;
    const double sinceLastLook = now - lastLookAt_;
    lastLookAt_ = now;

    bool advanced = false;
    if (counters.callbacks != lastCallbacks_) {
        lastCallbacks_ = counters.callbacks;
        lastCallbackAt_ = now;
        anyCallback_ = true;
        advanced = true;
    }

    if (!anyCallback_) {
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

    const double silentFor = now - lastCallbackAt_;
    if (silentFor >= options_.silentAfterSeconds) {
        reading.verdict = Verdict::Silent;
        reading.silentForSeconds = silentFor;
        // A rate measured across a gap is not a rate; start the window again when the audio
        // comes back.
        samples_.clear();
        return reading;
    }

    // **The limits follow the buffer** (the audit of 2026-09-25, L21). A callback comes every
    // `period` — the frames each one brings, over the rate — and a 2048-frame buffer at 44.1 kHz
    // is 46 ms, where fixed limits set for 256 frames (5.8 ms) left out nearly every pair of
    // looks: the window never filled, and a clock moved from 44.1 to 48 kHz went unnoticed.
    const double perCallback =
        counters.callbacks > 0
            ? static_cast<double>(counters.framesIn) / static_cast<double>(counters.callbacks)
            : 0.0;
    const double period = openedRate_ > 0.0 ? perCallback / openedRate_ : 0.0;
    // Between callbacks a look sees none for up to a period; twice that, allowing for a driver
    // whose callbacks are uneven, is a stall.
    const double stall = std::max(options_.stallSeconds, 2.0 * period);

    reading.verdict = Verdict::Healthy;
    if (silentFor > stall) {
        // A stall that has not yet become silence. The frames it lost would read as a slower
        // clock, so the measurement starts again once the callbacks come back.
        samples_.clear();
        return reading;
    }
    // Only at a look that saw the callbacks move, so every sample sits on delivered audio.
    if (advanced) {
        samples_.push_back(Sample{now, counters.framesIn, sinceLastLook});
    }
    if (samples_.empty()) {
        return reading;
    }
    // Keep a little over a window's worth of looks: stalls leave gaps that are not counted
    // below, so the samples have to reach further back than the window itself.
    while (samples_.size() > 2 &&
           now - samples_[1].at >= options_.rateWindowSeconds + 2.0 * stall) {
        samples_.pop_front();
    }
    // The rate over consecutive pairs of looks that were a normal distance apart, and only
    // those. A pair that straddles a dropout — frames the driver never delivered — is left
    // out, so a hiccup cannot read as a slower clock. A whole-window average was the first
    // version, and a single 80 ms dropout pulled it 4 % low (measured, by the test that found
    // it). Over the pairs kept, the counters' buffer-sized steps still average out to about a
    // tenth of a percent across three seconds with small buffers.
    //
    // **Normal is measured per pair.** The callback after the last one the earlier look saw
    // came within a period of it, and the later look found it within the time since the look
    // before — so a pair is normal up to a period (two, for an uneven driver) and that look's
    // spacing apart. A fixed limit also left out ordinary pairs of a slow redraw, and leaving
    // out only the long ones among ordinary pairs biases the rate high.
    double frames = 0.0;
    double seconds = 0.0;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        const double gap = samples_[i].at - samples_[i - 1].at;
        const double longest = 2.0 * period + samples_[i].sinceLastLook + kPairSlackSeconds;
        if (gap <= 0.0 || gap > longest || samples_[i].frames < samples_[i - 1].frames) {
            continue;
        }
        frames += static_cast<double>(samples_[i].frames - samples_[i - 1].frames);
        seconds += gap;
    }
    if (seconds >= options_.rateWindowSeconds) {
        reading.measuredRate = frames / seconds;
        // **Off, and on another clock.** A clock that moved runs at a clock's rate. Audio lost
        // inside a pair — a dropout shorter than a slow redraw's spacing, which no limit on the
        // pairs can see — reads as a rate between the clocks, and without this it reopened
        // streams that had already recovered: measured on a port of this arithmetic, with
        // redraws 20 to 160 ms apart and the dropout test's hiccups (L21).
        if (openedRate_ > 0.0 &&
            std::abs(reading.measuredRate / openedRate_ - 1.0) > options_.rateTolerance &&
            anotherClock(reading.measuredRate, openedRate_, options_.rateTolerance)) {
            reading.verdict = Verdict::RateChanged;
        }
    }
    return reading;
}

} // namespace takt4::audio
