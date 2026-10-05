#include "core/build_info.hpp"

#include "core/audio/resampler.hpp"
#include "core/build_config.hpp"
#include "core/git_describe.hpp"

#include <RtMidi.h>
#include <asio/version.hpp>
#include <nlohmann/json_fwd.hpp>
#include <portaudio.h>
#include <pugixml.hpp>

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

/// pugixml says its own version, as major × 1000 + minor × 10 (1.16 is 1160). Read from the
/// header rather than the pin in cmake/deps.cmake, so the two disagreeing is a failing test.
std::string pugixmlVersion() {
    return std::to_string(PUGIXML_VERSION / 1000) + '.' +
           std::to_string(PUGIXML_VERSION % 1000 / 10);
}

} // namespace

BuildInfo buildInfo() {
    BuildInfo info;
    info.version = TAKT4_VERSION;
    info.commit = TAKT4_GIT_DESCRIBE;
    info.platform = TAKT4_PLATFORM;
    info.compiler = TAKT4_COMPILER;
    info.portaudio = Pa_GetVersionInfo()->versionText;
    info.r8brain = audio::Resampler::libraryVersion();
    info.link = TAKT4_LINK_VERSION;
    info.kohlhoffAsio = kohlhoffAsioVersion();
    info.rtmidi = RtMidi::getVersion();
    info.rtneuralRevision = TAKT4_RTNEURAL_REV;
    info.kissfft = TAKT4_KISSFFT_VERSION;
    info.nlohmannJson = nlohmannJsonVersion();
    info.pugixml = pugixmlVersion();
    // miniz's header carries the version of the zlib it imitates (MZ_VERSION, "11.3.2"), not
    // its own release; the release is the pin.
    info.miniz = TAKT4_MINIZ_VERSION;
    info.slint = TAKT4_SLINT_VERSION;
    return info;
}

std::string versionLabel(const BuildInfo& info) {
    if (info.commit == "v" + info.version) {
        return info.version;
    }
    return info.version + " (" + info.commit + ")";
}

std::string describe(const BuildInfo& info) {
    std::ostringstream out;
    out << "takt4 " << info.version << '\n'
        << "  commit:        " << info.commit << '\n'
        << "  platform:      " << info.platform << '\n'
        << "  compiler:      " << info.compiler << '\n'
        << "  PortAudio:     " << info.portaudio << '\n'
        << "  r8brain-free:  " << info.r8brain << '\n'
        << "  Ableton Link:  " << info.link << " (asio " << info.kohlhoffAsio << ")\n"
        << "  RtMidi:        " << info.rtmidi << '\n'
        << "  RTNeural:      " << info.rtneuralRevision << '\n'
        << "  KissFFT:       " << info.kissfft << '\n'
        << "  nlohmann/json: " << info.nlohmannJson << '\n'
        << "  pugixml:       " << info.pugixml << '\n'
        << "  miniz:         " << info.miniz << '\n'
        << "  Slint:         " << (info.slint.empty() ? "not built" : info.slint) << '\n';
    return out.str();
}

} // namespace takt4
