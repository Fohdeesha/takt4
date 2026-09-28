#pragma once

#include "core/audio/input_stream.hpp"

#include <cstdint>
#include <deque>

namespace takt4::audio {

/// Notices the two ways an open input fails **without saying so**: its callbacks stop, or
/// its clock changes rate underneath it.
///
/// Both leave PortAudio reporting a stream that is running. An interface that is unplugged,
/// power-cycled or reset by its driver simply stops calling back — ASIO has no message for
/// "the hardware went away", and PortAudio's ASIO host ignores the messages it does have —
/// and until this the window went on saying LOCKED at the last tempo, with Link and the MIDI
/// clock carrying that tempo on, and nothing anywhere saying why the rules had gone quiet.
/// And a clock moved by another client (Live switching the MOTU from 44.1 to 48 kHz) keeps
/// the callbacks coming at the new rate while the resampler converts at the old ratio, so
/// every tempo is wrong by that ratio: 128 reads as 117.6, and that goes to Link. The audit's
/// C4.
///
/// Pure arithmetic over `InputStream::counters()`, so a test can drive it with any story it
/// likes. The window calls `observe` on its redraw timer and decides what to do about the
/// answer; this decides nothing but the reading.
///
/// **Measured over the callbacks' own account of their clock** (`CallbackClock`), not over when
/// the window happened to look: silence is the time since the last callback came, and the rate
/// is frames over seconds of the callbacks that came a normal distance after the one before. It
/// used to be the frame counter as each look found it, and a pair of looks had to be allowed to
/// be as far apart as a busy redraw — up to 160 ms — so audio a driver dropped inside that span
/// read as a slower clock, and with 2048- to 4096-frame buffers could land on another standard
/// rate and reopen a stream that had recovered (the audit of 2026-09-25's watchdog residue).
/// Now how often the window looks changes nothing but how soon it knows.
class InputWatchdog {
public:
    struct Options {
        /// No callback for this long is a dead input. A buffer is milliseconds; half a second
        /// with nothing is not a slow driver.
        double silentAfterSeconds = 0.5;
        /// How long a stream is given to deliver its first callback before silence counts —
        /// opening an ASIO driver and starting it can take a moment on a busy machine.
        double startupGraceSeconds = 2.0;
        /// The span the device's real rate is measured over. The counters move a buffer at a
        /// time (a few milliseconds), so over three seconds the measurement is good to about
        /// a tenth of a percent — far finer than the smallest real change, 44.1 to 48 kHz,
        /// which is 8.8 %.
        double rateWindowSeconds = 3.0;
        /// How far the measured rate may stray from the one the stream was opened at before
        /// it counts as a different clock — and it must then also be another clock's rate
        /// (`observe`). The smallest real change, 44.1 to 48 kHz or back, is 8 %; a crystal
        /// drifts by parts per million. What sets the floor is a dropout the driver recovers
        /// from on its own: the callback clock keeps a gap of up to two buffers as normal, so a
        /// dropout of one buffer loses that much audio from the window — 3 % of three seconds
        /// at 4096 frames — and must not read as a new clock.
        double rateTolerance = 0.04;
    };

    enum class Verdict : std::uint8_t {
        /// Opened, and not yet delivered anything — within the grace period.
        Starting,
        Healthy,
        /// No callback for `silentAfterSeconds` (or none at all past the grace period).
        Silent,
        /// Callbacks are arriving, at a rate that is not the one the stream was opened at.
        RateChanged,
    };

    struct Reading {
        Verdict verdict = Verdict::Starting;
        /// How long since the last callback, while `Silent`.
        double silentForSeconds = 0.0;
        /// The device's rate as measured over the last window, once there is a window's worth;
        /// zero before that.
        double measuredRate = 0.0;
        /// The input's own complaints, cumulative: callbacks the driver flagged as having
        /// dropped input. Worth showing — nothing else would say the device is struggling.
        std::uint32_t inputOverflows = 0;
    };

    InputWatchdog();
    explicit InputWatchdog(Options options);

    /// Starts watching a stream opened at `openedRate`, from `now` (seconds, any monotonic
    /// origin). Forgets everything about the one before.
    void reset(double openedRate, double now);

    /// One look. `now` on the same clock `reset` was given.
    Reading observe(const InputStreamCounters& counters, double now);

    /// The rate the stream being watched was opened at, as `reset` was told.
    double openedRate() const noexcept { return openedRate_; }

private:
    /// The callback clock's totals as one look found them.
    struct Sample {
        double normalSeconds = 0.0;
        std::uint64_t normalFrames = 0;
    };

    Options options_;
    double openedRate_ = 0.0;
    double openedAt_ = 0.0;
    /// A little over the last window's worth of looks, oldest first.
    std::deque<Sample> samples_;
};

} // namespace takt4::audio
