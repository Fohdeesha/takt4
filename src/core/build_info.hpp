#pragma once

#include <string>

namespace takt4 {

/// Versions of takt4 and of the libraries compiled into it.
struct BuildInfo {
    std::string version;
    std::string platform;
    std::string compiler;
    std::string portaudio;
    std::string r8brain;
    std::string link;
    std::string kohlhoffAsio; // the networking asio bundled with Link, not Steinberg's
    std::string rtmidi;
    std::string rtneuralRevision;
    std::string nlohmannJson;
    std::string slint; // empty when built without the UI
};

BuildInfo buildInfo();

/// Multi-line, human-readable rendering, as printed by `takt4 --version`.
std::string describe(const BuildInfo& info);

} // namespace takt4
