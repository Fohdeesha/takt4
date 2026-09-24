#pragma once

#include <stdexcept>
#include <string>

namespace takt4::audio {

/// A PortAudio call failed. `code` is the PaError.
class PortAudioError : public std::runtime_error {
public:
    PortAudioError(int code, const std::string& what);
    int code() const noexcept { return code_; }

private:
    int code_;
};

/// Owns one Pa_Initialize()/Pa_Terminate() pair.
///
/// PortAudio reference-counts initialisation, so sessions may nest. A session must
/// outlive every stream opened while it was alive.
///
/// The initialise that builds the device table — the first, and each `restart` — is preceded
/// by a scan of the ASIO drivers in a process of its own (`asio_scan.hpp`), so no ASIO driver
/// is loaded here until a stream is opened on one.
class PortAudioSession {
public:
    PortAudioSession(); // throws PortAudioError if Pa_Initialize() fails
    ~PortAudioSession();

    PortAudioSession(const PortAudioSession&) = delete;
    PortAudioSession& operator=(const PortAudioSession&) = delete;

    /// Terminates and initialises again, which is the only thing that makes PortAudio look at
    /// the machine's devices a second time: it builds its device table once, in
    /// `Pa_Initialize`, and a nested initialise only counts. An interface switched on after
    /// takt4 started, or one unplugged and plugged back in, is invisible until this runs.
    ///
    /// Every device index and every stream from before is meaningless afterwards, so nothing
    /// may be open. And it only re-enumerates if this is the last session alive — another one
    /// holds PortAudio's count above zero, and then this terminates nothing. Throws
    /// PortAudioError if the new initialise fails, leaving the session uninitialised.
    void restart();

private:
    bool initialised_ = false;
};

} // namespace takt4::audio
