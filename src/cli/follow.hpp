#pragma once

#include <ostream>
#include <string_view>
#include <vector>

namespace takt4::cli {

/// The `follow` command's block of the usage text.
void printFollowUsage(std::ostream& out);

/// `takt4-cli follow --link [...]`: take the beats from somewhere else rather than from audio —
/// the house DJ's Ableton Link session — and send them on through the application's own outputs,
/// printing each one.
int runFollow(const std::vector<std::string_view>& args);

} // namespace takt4::cli
