#pragma once

#include <ostream>
#include <string_view>
#include <vector>

namespace takt4::cli {

/// The `midi-in` command's block of the usage text.
void printMidiInUsage(std::ostream& out);

/// `takt4-cli midi-in [PORT] [...]`: list the MIDI inputs, or listen to one and say how steady
/// the clock arriving on it is — every message stamped on arrival with Link's clock, as the
/// application stamps what it follows.
int runMidiIn(const std::vector<std::string_view>& args);

} // namespace takt4::cli
