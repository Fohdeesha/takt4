#pragma once

#include <cstdint>

namespace takt4::audio {

/// Turns the monotonic sample clock into the output layer's host time (HANDOFF §4.3).
///
/// Link needs a host timestamp to align phase across peers, and so does every output held to a
/// beat's moment. The audio side supplies two things: **one observation per device buffer** —
/// where the buffer's first sample is on the sample clock, and when that sample was at the input
/// (`observe`) — and then a sample time for each hop, whose host time is read off the line the
/// observations draw (`hostMicrosForSample`). `output::LinkSession` fits the line
/// (`HostTimeFit`); a test can implement this with a straight line and no observations at all.
///
/// The interface exists so that nothing in audio/ or model/ has to include Link.
class HostTimeSource {
public:
    virtual ~HostTimeSource() = default;

    /// **Audio thread, one thread only.** A buffer of input has arrived: its first sample is
    /// at `sampleTime` on the sample clock — internal-rate samples since the stream started, with
    /// any audio the device lost counted in (`LostTime`) — and was at the input at `steadyMicros`,
    /// microseconds on `std::chrono::steady_clock`. The default ignores it, for a source whose
    /// line is known already.
    virtual void observe(double sampleTime, std::int64_t steadyMicros) noexcept {
        (void)sampleTime;
        (void)steadyMicros;
    }

    /// **Audio thread, one thread only.** The host time of the sample at `sampleTime`, in
    /// microseconds on the clock the output transports use, as the observations so far say.
    /// Zero before there are any.
    virtual std::int64_t hostMicrosForSample(double sampleTime) noexcept = 0;

    /// Forgets whatever was learned from the sample clock so far.
    ///
    /// A restarted stream counts from zero again, and a line fitted to the previous run maps that
    /// to a time long past — so whoever opens a stream calls this first. `engine::LiveTracker::
    /// start` does. The default does nothing, for a source with no state to forget.
    virtual void resetHostTimeFilter() noexcept {}

protected:
    HostTimeSource() = default;
    HostTimeSource(const HostTimeSource&) = default;
    HostTimeSource& operator=(const HostTimeSource&) = default;
};

} // namespace takt4::audio
