#include "core/audio/portaudio_session.hpp"

#include <portaudio.h>

namespace takt4::audio {

PortAudioError::PortAudioError(int code, const std::string& what)
    : std::runtime_error(what), code_(code) {}

PortAudioSession::PortAudioSession() {
    const PaError err = Pa_Initialize();
    if (err != paNoError) {
        throw PortAudioError(err, std::string("Pa_Initialize failed: ") + Pa_GetErrorText(err));
    }
    initialised_ = true;
}

PortAudioSession::~PortAudioSession() {
    // Pa_Terminate only fails if PortAudio was never initialised; nothing useful can be done
    // with the result in a destructor. Not called at all after a restart that failed, which
    // would take away another session's count.
    if (initialised_) {
        Pa_Terminate();
    }
}

void PortAudioSession::restart() {
    if (initialised_) {
        Pa_Terminate();
        initialised_ = false;
    }
    const PaError err = Pa_Initialize();
    if (err != paNoError) {
        throw PortAudioError(err, std::string("Pa_Initialize failed: ") + Pa_GetErrorText(err));
    }
    initialised_ = true;
}

} // namespace takt4::audio
