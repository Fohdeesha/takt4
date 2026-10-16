#pragma once

// Pro DJ Link datagrams captured from real players (tests/data/prolink, README there), read back
// for a test to play through takt4's parsers and its virtual player in the order and at the
// moments they were sent.

#include "core/timing/prolink/virtual_player.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef TAKT4_TEST_DATA_DIR
#error "TAKT4_TEST_DATA_DIR must be defined by the build"
#endif

namespace takt4::testing {

struct Captured {
    /// Since the excerpt's first datagram.
    double seconds = 0.0;
    std::uint16_t port = 0;
    /// Host order.
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    std::vector<std::uint8_t> bytes;

    std::int64_t micros() const { return static_cast<std::int64_t>(seconds * 1e6 + 0.5); }
    timing::prolink::VirtualPlayer::Channel channel() const {
        using Channel = timing::prolink::VirtualPlayer::Channel;
        return port == 50000 ? Channel::Announce : port == 50001 ? Channel::Beat : Channel::Status;
    }
    std::uint8_t type() const { return bytes.size() > 10 ? bytes[10] : std::uint8_t{0}; }
};

inline std::uint32_t parseDotted(const std::string& text) {
    std::uint32_t value = 0;
    std::istringstream in(text);
    for (int i = 0; i < 4; ++i) {
        unsigned part = 0;
        char dot = 0;
        in >> part;
        if (i < 3) {
            in >> dot;
        }
        value = (value << 8) | (part & 0xff);
    }
    return value;
}

/// The datagrams of `tests/data/prolink/<name>`, in order.
inline std::vector<Captured> loadCapture(const std::string& name) {
    const std::string path = std::string(TAKT4_TEST_DATA_DIR) + "/prolink/" + name;
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    std::vector<Captured> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        Captured captured;
        std::string from;
        std::string to;
        std::string hex;
        fields >> captured.seconds >> captured.port >> from >> to >> hex;
        captured.from = parseDotted(from);
        captured.to = parseDotted(to);
        if (hex.size() % 2 != 0) {
            throw std::runtime_error(path + ": odd hex in: " + line);
        }
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            captured.bytes.push_back(
                static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
        out.push_back(std::move(captured));
    }
    return out;
}

/// 169.254.103.172 and the like, in host order.
constexpr std::uint32_t ipv4(unsigned a, unsigned b, unsigned c, unsigned d) {
    return (a << 24) | (b << 16) | (c << 8) | d;
}

} // namespace takt4::testing
