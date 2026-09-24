#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::audio {

/// Every ASIO driver asked what it is in a process of its own, rather than in takt4's.
///
/// **Why.** Asking an ASIO driver what it is means loading it, initialising it and letting it
/// go again, and PortAudio does that to every installed driver each time it starts: at launch,
/// at RESCAN, and every ten seconds while a lost input is being looked for. The MOTU's driver
/// falls over as it is let go, about one time in 840 — a fast fail inside its `ASIOExit`
/// (caught on 2026-09-23), which no handler in the process can catch. In takt4's own process
/// that was the rig going dark with no report; the operator's call on 2026-09-24 was to move the
/// asking out.
///
/// So this executable is started again with `kAsioScanOption`. It lets PortAudio ask every
/// driver, writes down what PortAudio found, and exits without going on to `main`. takt4 reads
/// that and hands it to PortAudio (`asio_probe.h`, the patch's fifth part), which then lists the
/// ASIO devices without loading a single driver. A scanning process that falls over is started
/// once more; if that one falls over too, the ASIO devices are left out of the list and
/// `asioScanProblem` says why and what to do — takt4 carries on either way.
///
/// **What is left.** Opening a stream still loads its driver here, because the audio has to
/// arrive in this process, and closing one still lets it go. Those two are the only times an
/// ASIO driver is in takt4's process now.
struct AsioScannedDevice {
    std::string name;
    long inputChannels = 0;
    long outputChannels = 0;
    long minBufferSize = 0;
    long maxBufferSize = 0;
    long preferredBufferSize = 0;
    long bufferGranularity = 0;
    double defaultSampleRate = 0.0;
    double defaultLowInputLatency = 0.0;
    double defaultLowOutputLatency = 0.0;
    double defaultHighInputLatency = 0.0;
    double defaultHighOutputLatency = 0.0;
    /// The inputs' names, then the outputs'.
    std::vector<std::string> channelNames;

    bool operator==(const AsioScannedDevice&) const = default;
};

struct AsioScan {
    std::vector<AsioScannedDevice> devices;
    /// Empty when the scan finished. Otherwise why it did not, as a sentence for the status
    /// line — and then `devices` is empty, and no ASIO device is offered.
    std::string problem;
};

/// The last argument of the command line that makes a takt4 executable scan instead of
/// starting. Every executable that links PortAudio answers it, before `main`.
inline constexpr std::wstring_view kAsioScanOption = L"--takt4-asio-scan";

/// How long a scanning process may take before it is ended and ASIO is left out. A driver
/// takes well under a second to answer; this is for one that never does, which in takt4's own
/// process used to hang the window for good.
inline constexpr std::chrono::milliseconds kAsioScanLimit{20000};

/// Scans in a process of its own — twice if the first falls over — and waits for it.
AsioScan scanAsio(std::chrono::milliseconds limit = kAsioScanLimit);

/// What the scanning process writes, and reading it back. A text that does not end the way
/// `writeAsioScan` ends one is a scan that did not finish, and says so in `problem`.
std::string writeAsioScan(const AsioScan& scan);
AsioScan readAsioScan(std::string_view text);

/// Has the next `Pa_Initialize` take its ASIO devices from `scan` instead of loading drivers.
/// `PortAudioSession` calls it with a fresh scan before each initialise that builds the table.
void useAsioScan(AsioScan scan);

/// Why the last scan left the ASIO devices out; empty when it did not, or when there was none.
std::string asioScanProblem();

/// Whether this process lists ASIO devices at all: a platform that has ASIO, and no
/// TAKT4_NO_ASIO in the environment (the test binaries set it; cmake/pa_asio_patch.cmake).
bool asioWanted();

} // namespace takt4::audio
