#pragma once

// Other devices' Pro DJ Link packets, built for a test: the players and the mixer a virtual
// player shares a network with. Laid out as Deep Symmetry's DJ Link analysis gives them — the
// same layouts the captured fixtures (tests/data/prolink) are real examples of, and the
// packets test checks these against those.

#include "core/timing/prolink/packets.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace takt4::testing::prolink_sim {

using Bytes = std::vector<std::uint8_t>;
using timing::prolink::Mac;

inline void put16(Bytes& b, std::size_t at, std::uint32_t value) {
    b[at] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    b[at + 1] = static_cast<std::uint8_t>(value & 0xff);
}

inline void put32(Bytes& b, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        b[at + i] = static_cast<std::uint8_t>((value >> (24 - 8 * i)) & 0xff);
    }
}

inline void putName(Bytes& b, std::size_t at, const std::string& name) {
    for (std::size_t i = 0; i < 20 && i < name.size(); ++i) {
        b[at + i] = static_cast<std::uint8_t>(name[i]);
    }
}

/// A port-50000 packet's first 0x24 bytes: magic, type, 00, name at 0c, 01, subtype, lenp.
inline Bytes announceHeader(std::uint8_t type, std::size_t length, const std::string& name,
                            std::uint8_t subtype = 0x02) {
    Bytes b(length, 0);
    for (std::size_t i = 0; i < timing::prolink::kMagic.size(); ++i) {
        b[i] = timing::prolink::kMagic[i];
    }
    b[0x0a] = type;
    putName(b, 0x0c, name);
    b[0x20] = 0x01;
    b[0x21] = subtype;
    put16(b, 0x22, static_cast<std::uint32_t>(length));
    return b;
}

/// A port-50001 or -50002 packet's first 0x24 bytes: magic, type, name at 0b, 01, subtype, D,
/// lenr (the bytes after it).
inline Bytes deviceHeader(std::uint8_t type, std::size_t length, const std::string& name,
                          std::uint8_t device, std::uint8_t subtype = 0x00) {
    Bytes b(length, 0);
    for (std::size_t i = 0; i < timing::prolink::kMagic.size(); ++i) {
        b[i] = timing::prolink::kMagic[i];
    }
    b[0x0a] = type;
    putName(b, 0x0b, name);
    b[0x1f] = 0x01;
    b[0x20] = subtype;
    b[0x21] = device;
    put16(b, 0x22, static_cast<std::uint32_t>(length - 0x24));
    return b;
}

inline void putMac(Bytes& b, std::size_t at, const Mac& mac) {
    for (std::size_t i = 0; i < mac.size(); ++i) {
        b[at + i] = mac[i];
    }
}

/// `kind` 1 a player, 2 a mixer.
inline Bytes keepAlive(std::uint8_t device, std::uint32_t ip, const Mac& mac, std::uint8_t kind = 1,
                       std::uint8_t peers = 2, const std::string& name = "CDJ-3000") {
    Bytes b = announceHeader(0x06, 0x36, name);
    b[0x24] = device;
    b[0x25] = 0x01;
    putMac(b, 0x26, mac);
    put32(b, 0x2c, ip);
    b[0x30] = peers;
    b[0x34] = kind;
    return b;
}

inline Bytes secondClaim(std::uint8_t device, std::uint32_t ip, const Mac& mac,
                         std::uint8_t counter = 1, const std::string& name = "CDJ-3000") {
    Bytes b = announceHeader(0x02, 0x32, name);
    put32(b, 0x24, ip);
    putMac(b, 0x28, mac);
    b[0x2e] = device;
    b[0x2f] = counter;
    b[0x30] = 0x01;
    b[0x31] = 0x01;
    return b;
}

inline Bytes finalClaim(std::uint8_t device, std::uint8_t counter = 1,
                        const std::string& name = "CDJ-3000") {
    Bytes b = announceHeader(0x04, 0x26, name);
    b[0x24] = device;
    b[0x25] = counter;
    return b;
}

inline Bytes conflict(std::uint8_t device, std::uint32_t ip, const std::string& name = "CDJ-3000") {
    Bytes b = announceHeader(0x08, 0x29, name);
    b[0x24] = device;
    put32(b, 0x25, ip);
    return b;
}

inline Bytes assignmentFinished(std::uint8_t device, const std::string& name = "CDJ-3000") {
    Bytes b = announceHeader(0x05, 0x26, name);
    b[0x24] = device;
    b[0x25] = 0x01;
    return b;
}

inline Bytes assignmentIntention(std::uint32_t ip, const Mac& mac,
                                 const std::string& name = "DJM-900NXS2") {
    Bytes b = announceHeader(0x01, 0x2f, name);
    put32(b, 0x24, ip);
    putMac(b, 0x28, mac);
    b[0x2e] = 0x01;
    return b;
}

inline Bytes assignment(std::uint8_t device, const std::string& name = "DJM-900NXS2") {
    Bytes b = announceHeader(0x03, 0x27, name);
    b[0x0b] = 0x01;
    b[0x24] = device;
    b[0x25] = 0x01;
    return b;
}

inline std::uint32_t pitchOf(double factor) {
    return static_cast<std::uint32_t>(std::lround(factor * 1048576.0));
}

/// A beat at `bpm` playing (pitch included): the track's tempo `bpm / pitch` and the pitch.
inline Bytes beat(std::uint8_t device, double bpm, std::uint8_t beatInBar, double pitch = 1.0,
                  const std::string& name = "CDJ-3000") {
    Bytes b = deviceHeader(0x28, 0x60, name, device);
    const double trackBpm = bpm / pitch;
    put32(b, 0x24, static_cast<std::uint32_t>(std::lround(60000.0 / trackBpm)));
    put32(b, 0x28, static_cast<std::uint32_t>(std::lround(120000.0 / trackBpm)));
    for (std::size_t at = 0x3c; at < 0x54; ++at) {
        b[at] = 0xff;
    }
    put32(b, 0x54, pitchOf(pitch));
    put16(b, 0x5a, static_cast<std::uint32_t>(std::lround(trackBpm * 100.0)));
    b[0x5c] = beatInBar;
    b[0x5f] = device;
    return b;
}

/// Flag bits of a status's F byte.
inline constexpr std::uint8_t kPlaying = 0x40;
inline constexpr std::uint8_t kMaster = 0x20;
inline constexpr std::uint8_t kSynced = 0x10;
inline constexpr std::uint8_t kOnAir = 0x08;

/// P1 play modes.
inline constexpr std::uint8_t kModePlaying = 0x03;
inline constexpr std::uint8_t kModePaused = 0x05;
inline constexpr std::uint8_t kModeCuePlay = 0x07;

struct PlayerState {
    std::uint8_t flags = 0x84;
    std::uint8_t playMode = kModePaused;
    double bpm = 128.0; // playing, pitch included
    double pitch = 1.0;
    std::uint8_t masterMeaningful = 0;
    std::uint8_t handingTo = 0xff;
    std::uint32_t beat = 1;
    std::uint8_t beatInBar = 1;
};

/// A CDJ's status, 0x11c bytes as a CDJ-2000nexus on firmware 1.44 sends it.
inline Bytes playerStatus(std::uint8_t device, const PlayerState& s,
                          const std::string& name = "CDJ-3000", std::size_t length = 0x11c) {
    Bytes b = deviceHeader(0x0a, length, name, device, 0x03);
    b[0x24] = device;
    b[0x26] = 0x01;
    b[0x27] = static_cast<std::uint8_t>((s.flags & kPlaying) != 0 ? 0x01 : 0x00);
    b[0x7b] = s.playMode;
    b[0x89] = static_cast<std::uint8_t>(s.flags | 0x84);
    b[0x8a] = 0xff;
    put32(b, 0x8c, pitchOf(s.pitch));
    put16(b, 0x90, 0x8000);
    put16(b, 0x92, static_cast<std::uint32_t>(std::lround(s.bpm / s.pitch * 100.0)));
    b[0x9e] = static_cast<std::uint8_t>(
        (s.flags & kMaster) != 0 ? (s.masterMeaningful != 0 ? s.masterMeaningful : 0x01) : 0x00);
    b[0x9f] = s.handingTo;
    put32(b, 0xa0, s.beat);
    b[0xa6] = s.beatInBar;
    return b;
}

/// A mixer's status, 0x38 bytes: F f0 as master, d0 otherwise.
inline Bytes mixerStatus(std::uint8_t device, bool master, double bpm = 120.0,
                         std::uint8_t handingTo = 0xff, const std::string& name = "DJM-900NXS2") {
    Bytes b = deviceHeader(0x29, 0x38, name, device);
    b[0x24] = device;
    b[0x27] = static_cast<std::uint8_t>(master ? 0xf0 : 0xd0);
    put32(b, 0x28, 0x00100000);
    b[0x2c] = 0x80;
    put16(b, 0x2e, static_cast<std::uint32_t>(std::lround(bpm * 100.0)));
    put32(b, 0x30, 0x00100000);
    b[0x35] = 0x09;
    b[0x36] = handingTo;
    return b;
}

} // namespace takt4::testing::prolink_sim
