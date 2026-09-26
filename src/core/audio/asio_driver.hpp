#pragma once

#include <cstdint>

namespace takt4::audio {

/// What an ASIO driver has told takt4 since the last time it was asked — the messages
/// PortAudio's ASIO host acknowledges and drops, kept by the configure-time patch in
/// cmake/pa_asio_patch.cmake (the audit's C4, option (a) of its Q9).
///
/// Each is answered the same way — by closing and reopening the stream — because each means
/// the stream as opened no longer describes the hardware: the buffers were resized, the clock
/// moved, or the driver wants to be torn down and built again (which is what a MOTU does when
/// its control panel changes anything, and when it is power-cycled and comes back).
struct AsioDriverEvents {
    bool resetRequest = false;
    bool bufferSizeChange = false;
    bool sampleRateChange = false;
    /// A resync is the driver reporting a moment of lost data, not a change; counted, and shown
    /// with the input's other trouble — not a reason to reopen on its own.
    bool resync = false;
    /// The rate the last sample-rate message named, when there was one; zero otherwise. Drivers
    /// send that message for things that are not a new rate — the SDK names S/PDIF status — and
    /// one naming the rate the stream already runs at is not a reason to reopen (the audit of
    /// 2026-09-25, L22).
    double reportedRate = 0.0;

    bool needsReopen() const noexcept { return resetRequest || bufferSizeChange || sampleRateChange; }
    bool any() const noexcept { return needsReopen() || resync; }
};

/// Everything the driver has said since the last call, and clears it. Safe from any thread;
/// always empty on a platform without ASIO, unless a test posted something.
AsioDriverEvents takeAsioDriverEvents() noexcept;

/// For the tests: what the next `takeAsioDriverEvents` finds, as though the driver had said it —
/// no test can make a real driver speak. Adds to whatever the driver has said.
void postAsioDriverEvents(const AsioDriverEvents& events) noexcept;

/// The rate the last refused open found the interface running at, or the rate the driver
/// last said it changed to — zero when neither has happened. See the patch's second part:
/// PortAudio refuses to open an ASIO stream at a rate other than the interface's own rather
/// than re-clocking it, and this is the rate to open at instead.
double asioClockedRate() noexcept;

/// Clears what `asioClockedRate` reports, so a value read after the next open can only have
/// come from that open. Called just before one.
void forgetAsioClockedRate() noexcept;

} // namespace takt4::audio
