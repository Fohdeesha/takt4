#pragma once

#include "core/timing/prolink/network.hpp"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::cli {

/// The `prolink-watch` command's block of the usage text.
void printProLinkWatchUsage(std::ostream& out);

/// `takt4-cli prolink-watch [--join] [...]`: list the Pro DJ Link devices on this network as they
/// are heard — sending nothing — or, with `--join`, announce takt4 as a player and print each
/// device's status and the followed device's beats.
int runProLinkWatch(const std::vector<std::string_view>& args);

/// `master`, or a player's number 1 to 15, as `--follow` takes it.
std::uint8_t parseProLinkFollow(std::string_view text);

/// What the CDJ network is doing, in a line: the devices heard, the number held, who is
/// followed — for `follow --prolink` and `prolink-watch` alike.
std::string proLinkStatus(const timing::prolink::Network::Snapshot& snapshot);

/// One device, in a line: its number, name, kind, address, and — once status comes — playing,
/// master, synced, on the air, and its tempo.
std::string proLinkDevice(const timing::prolink::Network::DeviceView& device);

} // namespace takt4::cli
