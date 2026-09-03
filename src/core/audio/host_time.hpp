#pragma once

#include <cstdint>

namespace takt4::audio {

/// Turns the monotonic sample clock into the output layer's host time (HANDOFF §4.3).
///
/// Link needs a host timestamp to align phase across peers, and it does *not* need the
/// driver's: `HostTimeFilter::sampleTimeToHostTime()` samples Link's own clock and runs a
/// 512-point linear regression against the sample time it is given. So the only thing
/// the audio side has to provide is a sample counter that goes up — which is what
/// `HopProcessor`'s hop index already is — and the only thing the output side has to
/// provide is this.
///
/// The interface exists so that nothing in audio/ or model/ has to include Link.
/// `output::LinkSession` implements it; a test can implement it with a straight line.
class HostTimeSource {
public:
    virtual ~HostTimeSource() = default;

    /// **Audio thread, one thread only.** `sampleTime` is a monotonically increasing
    /// count of samples at a fixed rate. Returns the host time of that sample, in
    /// microseconds on the host clock the output transports use.
    ///
    /// The regression behind this is stateful and not thread-safe: whoever installs a
    /// source promises to call it from exactly one thread.
    virtual std::int64_t hostMicrosForSample(double sampleTime) noexcept = 0;

protected:
    HostTimeSource() = default;
    HostTimeSource(const HostTimeSource&) = default;
    HostTimeSource& operator=(const HostTimeSource&) = default;
};

} // namespace takt4::audio
