#include "core/audio/portaudio_session.hpp"

#include "core/audio/asio_scan.hpp"

#include <portaudio.h>

namespace takt4::audio {

namespace {

/// Before an initialise that builds PortAudio's device table: the ASIO drivers are asked in a
/// process of their own and PortAudio is handed the answers (see asio_scan.hpp). Not before a
/// nested one, which only counts and builds nothing.
void scanAsioFirst() {
    if (Pa_GetHostApiCount() != paNotInitialized) {
        return;
    }
    // No ASIO wanted is no scan, and so no problem left over from the last one.
    useAsioScan(asioWanted() ? scanAsio() : AsioScan{});
}

} // namespace

PortAudioError::PortAudioError(int code, const std::string& what)
    : std::runtime_error(what), code_(code) {}

PortAudioSession::PortAudioSession() {
    scanAsioFirst();
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
    scanAsioFirst();
    const PaError err = Pa_Initialize();
    if (err != paNoError) {
        throw PortAudioError(err, std::string("Pa_Initialize failed: ") + Pa_GetErrorText(err));
    }
    initialised_ = true;
}

} // namespace takt4::audio
