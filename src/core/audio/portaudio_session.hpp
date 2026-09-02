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
class PortAudioSession {
public:
    PortAudioSession(); // throws PortAudioError if Pa_Initialize() fails
    ~PortAudioSession();

    PortAudioSession(const PortAudioSession&) = delete;
    PortAudioSession& operator=(const PortAudioSession&) = delete;
};

} // namespace takt4::audio
