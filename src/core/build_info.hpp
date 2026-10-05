#pragma once

#include <string>

namespace takt4 {

/// Versions of takt4 and of the libraries compiled into it.
struct BuildInfo {
    std::string version;
    /// `git describe --tags --dirty --always` of the tree it was built from: "v0.9.7" for a
    /// release built from its tag, "v0.9.7-12-gfccd01d" for a commit after it, with "-dirty"
    /// when the tree had changes nobody had committed, and "unknown" with no git to ask.
    std::string commit;
    std::string platform;
    std::string compiler;
    std::string portaudio;
    std::string r8brain;
    std::string link;
    std::string kohlhoffAsio; // the networking asio bundled with Link, not Steinberg's
    std::string rtmidi;
    std::string rtneuralRevision;
    std::string kissfft;
    std::string nlohmannJson;
    std::string pugixml;
    std::string miniz;
    std::string slint; // empty when built without the UI
};

BuildInfo buildInfo();

/// The version as the window and the console show it: the bare version for a clean build of
/// its release tag, and the version with the commit after it for anything else — so a build
/// from a working tree can never be mistaken for the release it says it is.
std::string versionLabel(const BuildInfo& info);

/// Multi-line, human-readable rendering, as printed by `takt4 --version`.
std::string describe(const BuildInfo& info);

} // namespace takt4
