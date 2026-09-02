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
}

PortAudioSession::~PortAudioSession() {
    // Pa_Terminate only fails if PortAudio was never initialised, which the constructor
    // guarantees against; nothing useful can be done with the result in a destructor.
    Pa_Terminate();
}

} // namespace takt4::audio
