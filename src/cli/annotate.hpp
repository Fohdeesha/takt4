#pragma once

#include <ostream>
#include <string_view>
#include <vector>

namespace takt4::cli {

/// The `annotate` command's block of the usage text.
void printAnnotateUsage(std::ostream& out);

/// `takt4-cli annotate IN.wav [...]`: play the track and tap along to it, and write the
/// taps as a beat annotation in the Ballroom layout.
int runAnnotate(const std::vector<std::string_view>& args);

} // namespace takt4::cli
