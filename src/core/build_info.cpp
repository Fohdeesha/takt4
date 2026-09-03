#include "core/build_info.hpp"

#include "core/audio/resampler.hpp"
#include "core/build_config.hpp"

#include <RtMidi.h>
#include <asio/version.hpp>
#include <nlohmann/json_fwd.hpp>
#include <portaudio.h>

#include <sstream>
#include <string>

namespace takt4 {

namespace {

std::string kohlhoffAsioVersion() {
    // ASIO_VERSION is MMmmmss (1.36.0 -> 103600).
    return std::to_string(ASIO_VERSION / 100000) + '.' + std::to_string(ASIO_VERSION / 100 % 1000) +
           '.' + std::to_string(ASIO_VERSION % 100);
}

std::string nlohmannJsonVersion() {
    return std::to_string(NLOHMANN_JSON_VERSION_MAJOR) + '.' +
           std::to_string(NLOHMANN_JSON_VERSION_MINOR) + '.' +
           std::to_string(NLOHMANN_JSON_VERSION_PATCH);
}

} // namespace

BuildInfo buildInfo() {
    BuildInfo info;
    info.version = TAKT4_VERSION;
    info.platform = TAKT4_PLATFORM;
    info.compiler = TAKT4_COMPILER;
    info.portaudio = Pa_GetVersionInfo()->versionText;
    info.r8brain = audio::Resampler::libraryVersion();
    info.link = TAKT4_LINK_VERSION;
    info.kohlhoffAsio = kohlhoffAsioVersion();
    info.rtmidi = RtMidi::getVersion();
    info.rtneuralRevision = TAKT4_RTNEURAL_REV;
    info.nlohmannJson = nlohmannJsonVersion();
    info.slint = TAKT4_SLINT_VERSION;
    return info;
}

std::string describe(const BuildInfo& info) {
    std::ostringstream out;
    out << "takt4 " << info.version << '\n'
        << "  platform:      " << info.platform << '\n'
        << "  compiler:      " << info.compiler << '\n'
        << "  PortAudio:     " << info.portaudio << '\n'
        << "  r8brain-free:  " << info.r8brain << '\n'
        << "  Ableton Link:  " << info.link << " (asio " << info.kohlhoffAsio << ")\n"
        << "  RtMidi:        " << info.rtmidi << '\n'
        << "  RTNeural:      " << info.rtneuralRevision << '\n'
        << "  nlohmann/json: " << info.nlohmannJson << '\n'
        << "  Slint:         " << (info.slint.empty() ? "not built" : info.slint) << '\n';
    return out.str();
}

} // namespace takt4
