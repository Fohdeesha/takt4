#include "core/dmx/artnet_packet.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace takt4::dmx {
namespace {

/// The digits of one decimal field of a "net:sub:uni" address, or nothing.
bool readNumber(std::string_view text, int& out) noexcept {
    if (text.empty()) {
        return false;
    }
    int value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value < 0) {
        return false;
    }
    out = value;
    return true;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

std::string describePortAddress(PortAddress address) {
    const int net = netOf(address);
    if (net == 0) {
        // Everything inside the first Net is spelled as the flat number, because that is
        // what a node's "universe" box holds on a rig that never leaves it — which is every
        // rig with fewer than 256 universes on it, which is every rig.
        return std::to_string(address);
    }
    const int sub = (address >> 4) & 0x0F;
    const int uni = address & 0x0F;
    return std::to_string(net) + ":" + std::to_string(sub) + ":" + std::to_string(uni);
}

bool parsePortAddress(std::string_view text, PortAddress& out) noexcept {
    text = trim(text);
    if (text.empty()) {
        return false;
    }

    const std::size_t firstColon = text.find(':');
    if (firstColon == std::string_view::npos) {
        int flat = 0;
        if (!readNumber(text, flat) || flat > kMaxPortAddress) {
            return false;
        }
        out = static_cast<PortAddress>(flat);
        return true;
    }

    const std::size_t secondColon = text.find(':', firstColon + 1);
    if (secondColon == std::string_view::npos) {
        return false;
    }
    int net = 0;
    int sub = 0;
    int uni = 0;
    if (!readNumber(trim(text.substr(0, firstColon)), net) ||
        !readNumber(trim(text.substr(firstColon + 1, secondColon - firstColon - 1)), sub) ||
        !readNumber(trim(text.substr(secondColon + 1)), uni)) {
        return false;
    }
    // Each part has to fit its own field. 0:0:16 is not universe 16 written another way —
    // it is a typo for 0:1:0, and quietly carrying the overflow into the Sub-Net would point
    // a rig at the wrong node.
    if (net > 127 || sub > 15 || uni > 15) {
        return false;
    }
    out = static_cast<PortAddress>((net << 8) | (sub << 4) | uni);
    return true;
}

std::size_t writeArtDmx(std::span<std::byte> out, PortAddress address,
                        std::span<const std::uint8_t> levels, std::uint8_t sequence,
                        std::uint8_t physical) noexcept {
    if (levels.empty() || levels.size() > kChannelsPerUniverse ||
        out.size() < kArtDmxHeaderSize + levels.size()) {
        return 0;
    }
    // "This value should be an even number in the range 2 to 512." A patch whose highest
    // channel is odd is sent one channel wider rather than one channel short.
    const std::size_t length = levels.size() + (levels.size() % 2);
    if (out.size() < kArtDmxHeaderSize + length) {
        return 0;
    }

    auto* const bytes = reinterpret_cast<std::uint8_t*>(out.data());
    std::memcpy(bytes, kArtNetId, sizeof kArtNetId);
    // Field 2, "transmitted low byte first".
    bytes[8] = static_cast<std::uint8_t>(kOpDmx & 0xFF);
    bytes[9] = static_cast<std::uint8_t>((kOpDmx >> 8) & 0xFF);
    bytes[10] = kProtocolVersionHi;
    bytes[11] = kProtocolVersionLo;
    bytes[12] = sequence;
    bytes[13] = physical;
    bytes[14] = subUniOf(address);
    bytes[15] = netOf(address);
    // Fields 9 and 10, high byte **first** — the opposite of the OpCode above, and the
    // specification's own asymmetry rather than an accident here.
    bytes[16] = static_cast<std::uint8_t>((length >> 8) & 0xFF);
    bytes[17] = static_cast<std::uint8_t>(length & 0xFF);

    std::memcpy(bytes + kArtDmxHeaderSize, levels.data(), levels.size());
    if (length != levels.size()) {
        bytes[kArtDmxHeaderSize + levels.size()] = 0;
    }
    return kArtDmxHeaderSize + length;
}

} // namespace takt4::dmx
