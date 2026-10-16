#include "core/timing/prolink/packets.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace takt4::timing::prolink {

namespace {

/// The type byte, after the magic.
constexpr std::size_t kTypeAt = 0x0a;
/// Port 50000's packets name the device at 0c; every other port's at 0b, the byte after the type
/// (the analysis's "first packet structure variant around the device name").
constexpr std::size_t kAnnounceNameAt = 0x0c;
constexpr std::size_t kNameAt = 0x0b;

/// Each type's length as the analysis gives it — the shortest a packet of it may be read from.
/// A real one is never shorter; a longer one (a newer firmware's) is read for the fields here.
constexpr std::size_t kHelloLength = 0x25;
constexpr std::size_t kFirstClaimLength = 0x2c;
constexpr std::size_t kSecondClaimLength = 0x32;
constexpr std::size_t kAssignmentLength = 0x27;
constexpr std::size_t kFinalClaimLength = 0x26;
constexpr std::size_t kFinishedLength = 0x26;
constexpr std::size_t kKeepAliveLength = 0x36;
constexpr std::size_t kConflictLength = 0x29;
constexpr std::size_t kIntentionLength = 0x2f;
constexpr std::size_t kBeatLength = 0x60;
constexpr std::size_t kMixerStatusLength = 0x38;
/// The shortest player status there is: a pre-nexus player's, 0xd0 bytes. A nexus sends 0xd4, a
/// nexus 2 or newer firmware 0x11c or 0x124, a CDJ-3000 0x200 — and the length says nothing of
/// the generation (two CDJ-2000nexus players on firmware 1.44 send 0x11c).
constexpr std::size_t kPlayerStatusLength = 0xd0;

constexpr std::uint8_t kBeatType = 0x28;
constexpr std::uint8_t kPlayerStatusType = 0x0a;
constexpr std::uint8_t kMixerStatusType = 0x29;

bool hasMagic(std::span<const std::uint8_t> datagram) noexcept {
    return datagram.size() > kTypeAt && std::equal(kMagic.begin(), kMagic.end(), datagram.begin());
}

std::uint16_t read16(std::span<const std::uint8_t> d, std::size_t at) noexcept {
    return static_cast<std::uint16_t>((d[at] << 8) | d[at + 1]);
}

std::uint32_t read32(std::span<const std::uint8_t> d, std::size_t at) noexcept {
    return (std::uint32_t{d[at]} << 24) | (std::uint32_t{d[at + 1]} << 16) |
           (std::uint32_t{d[at + 2]} << 8) | std::uint32_t{d[at + 3]};
}

Name readName(std::span<const std::uint8_t> d, std::size_t at) noexcept {
    Name name{};
    for (std::size_t i = 0; i < kNameLength; ++i) {
        name[i] = static_cast<char>(d[at + i]);
    }
    name[kNameLength] = '\0';
    return name;
}

Mac readMac(std::span<const std::uint8_t> d, std::size_t at) noexcept {
    Mac mac{};
    std::copy_n(d.begin() + static_cast<std::ptrdiff_t>(at), mac.size(), mac.begin());
    return mac;
}

DeviceKind kindOf(std::uint8_t byte) noexcept {
    return byte == 1 ? DeviceKind::Player : byte == 2 ? DeviceKind::Mixer : DeviceKind::Unknown;
}

/// A pitch and a tempo in hundredths, as the packets carry them, as the tempo playing.
double playingBpm(std::uint32_t pitch, std::uint16_t bpm100) noexcept {
    if (bpm100 == 0xffff || bpm100 == 0) {
        return 0.0;
    }
    return static_cast<double>(bpm100) / 100.0 * static_cast<double>(pitch) / 1048576.0;
}

/// A packet's first bytes: the magic, the type, the byte after it, and — on port 50000 — the
/// name, 01, the subtype and the whole packet's length (lenp).
class Writer {
public:
    Writer(std::uint8_t type, std::uint8_t second, const std::string& name, std::uint8_t subtype,
           std::size_t length) noexcept {
        built_.size = length;
        std::copy(kMagic.begin(), kMagic.end(), built_.bytes.begin());
        built_.bytes[kTypeAt] = type;
        built_.bytes[kTypeAt + 1] = second;
        // ASCII only, and no more than the field holds: anything else is a space.
        for (std::size_t i = 0; i < kNameLength && i < name.size(); ++i) {
            const auto c = static_cast<unsigned char>(name[i]);
            built_.bytes[kAnnounceNameAt + i] =
                static_cast<std::uint8_t>(c >= 0x20 && c < 0x7f ? c : ' ');
        }
        built_.bytes[0x20] = 0x01;
        built_.bytes[0x21] = subtype;
        put16(0x22, static_cast<std::uint16_t>(length));
    }
    void put(std::size_t at, std::uint8_t value) noexcept { built_.bytes[at] = value; }
    void put16(std::size_t at, std::uint16_t value) noexcept {
        built_.bytes[at] = static_cast<std::uint8_t>(value >> 8);
        built_.bytes[at + 1] = static_cast<std::uint8_t>(value & 0xff);
    }
    void put32(std::size_t at, std::uint32_t value) noexcept {
        for (std::size_t i = 0; i < 4; ++i) {
            built_.bytes[at + i] = static_cast<std::uint8_t>((value >> (24 - 8 * i)) & 0xff);
        }
    }
    void putMac(std::size_t at, const Mac& mac) noexcept {
        std::copy(mac.begin(), mac.end(), built_.bytes.begin() + static_cast<std::ptrdiff_t>(at));
    }
    Built done() const noexcept { return built_; }

private:
    Built built_;
};

} // namespace

double BeatPacket::bpm() const noexcept {
    return playingBpm(pitch, bpm100);
}

double StatusPacket::bpm() const noexcept {
    return playingBpm(pitch, bpm100);
}

bool parseAnnounce(std::span<const std::uint8_t> d, AnnouncePacket& out) noexcept {
    if (!hasMagic(d) || d.size() < kHelloLength) {
        return false;
    }
    AnnouncePacket packet;
    packet.name = readName(d, kAnnounceNameAt);
    packet.fromMixerPort = d[0x0b] == 0x01;
    switch (d[kTypeAt]) {
    case 0x0a:
        packet.type = Announce::Hello;
        packet.kind = kindOf(d[0x24]);
        break;
    case 0x00:
        if (d.size() < kFirstClaimLength) {
            return false;
        }
        packet.type = Announce::FirstClaim;
        packet.counter = d[0x24];
        packet.kind = kindOf(d[0x25]);
        packet.mac = readMac(d, 0x26);
        break;
    case 0x01:
        if (d.size() < kIntentionLength) {
            return false;
        }
        packet.type = Announce::AssignmentIntention;
        packet.ip = read32(d, 0x24);
        packet.mac = readMac(d, 0x28);
        break;
    case 0x02:
        if (d.size() < kSecondClaimLength) {
            return false;
        }
        packet.type = Announce::SecondClaim;
        packet.ip = read32(d, 0x24);
        packet.mac = readMac(d, 0x28);
        packet.device = d[0x2e];
        packet.counter = d[0x2f];
        packet.kind = kindOf(d[0x30]);
        packet.autoAssign = d[0x31] == 0x01;
        break;
    case 0x03:
        if (d.size() < kAssignmentLength) {
            return false;
        }
        packet.type = Announce::Assignment;
        packet.device = d[0x24];
        packet.counter = d[0x25];
        break;
    case 0x04:
        if (d.size() < kFinalClaimLength) {
            return false;
        }
        packet.type = Announce::FinalClaim;
        packet.device = d[0x24];
        packet.counter = d[0x25];
        break;
    case 0x05:
        if (d.size() < kFinishedLength) {
            return false;
        }
        packet.type = Announce::AssignmentFinished;
        packet.device = d[0x24];
        break;
    case 0x06:
        if (d.size() < kKeepAliveLength) {
            return false;
        }
        packet.type = Announce::KeepAlive;
        packet.device = d[0x24];
        packet.mac = readMac(d, 0x26);
        packet.ip = read32(d, 0x2c);
        packet.peers = d[0x30];
        packet.kind = kindOf(d[0x34]);
        break;
    case 0x08:
        if (d.size() < kConflictLength) {
            return false;
        }
        packet.type = Announce::Conflict;
        packet.device = d[0x24];
        packet.ip = read32(d, 0x25);
        break;
    default:
        return false;
    }
    out = packet;
    return true;
}

bool parseBeat(std::span<const std::uint8_t> d, BeatPacket& out) noexcept {
    if (!hasMagic(d) || d[kTypeAt] != kBeatType || d.size() < kBeatLength) {
        return false;
    }
    BeatPacket beat;
    beat.name = readName(d, kNameAt);
    beat.device = d[0x21];
    beat.nextBeatMs = read32(d, 0x24);
    beat.pitch = read32(d, 0x54);
    beat.bpm100 = read16(d, 0x5a);
    beat.beatInBar = d[0x5c];
    out = beat;
    return true;
}

bool parseStatus(std::span<const std::uint8_t> d, StatusPacket& out) noexcept {
    if (!hasMagic(d)) {
        return false;
    }
    StatusPacket status;
    if (d[kTypeAt] == kPlayerStatusType && d.size() >= kPlayerStatusLength) {
        status.name = readName(d, kNameAt);
        status.device = d[0x21];
        status.playMode = d[0x7b];
        status.flags = d[0x89];
        status.pitch = read32(d, 0x8c);
        status.bpm100 = read16(d, 0x92);
        status.masterMeaningful = d[0x9e];
        status.handingTo = d[0x9f];
        status.beat = read32(d, 0xa0);
        status.beatInBar = d[0xa6];
    } else if (d[kTypeAt] == kMixerStatusType && d.size() >= kMixerStatusLength) {
        status.mixer = true;
        status.name = readName(d, kNameAt);
        status.device = d[0x21];
        status.flags = d[0x27];
        status.pitch = read32(d, 0x28);
        status.bpm100 = read16(d, 0x2e);
        status.handingTo = d[0x36];
        status.beatInBar = d[0x37];
    } else {
        return false;
    }
    out = status;
    return true;
}

Built buildHello(const std::string& name) noexcept {
    // The CDJ-3000 form: subtype 04 and one byte longer than the older players' 0x25.
    Writer w(0x0a, 0x00, name, 0x04, 0x26);
    w.put(0x24, 0x01);
    w.put(0x25, 0x40);
    return w.done();
}

Built buildFirstClaim(const std::string& name, std::uint8_t counter, const Mac& mac) noexcept {
    Writer w(0x00, 0x00, name, 0x03, kFirstClaimLength);
    w.put(0x24, counter);
    w.put(0x25, 0x01);
    w.putMac(0x26, mac);
    return w.done();
}

Built buildSecondClaim(const std::string& name, std::uint8_t counter, std::uint32_t ip,
                       const Mac& mac, std::uint8_t device, bool autoAssign) noexcept {
    Writer w(0x02, 0x00, name, 0x03, kSecondClaimLength);
    w.put32(0x24, ip);
    w.putMac(0x28, mac);
    w.put(0x2e, device);
    w.put(0x2f, counter);
    w.put(0x30, 0x01);
    w.put(0x31, static_cast<std::uint8_t>(autoAssign ? 0x01 : 0x02));
    return w.done();
}

Built buildFinalClaim(const std::string& name, std::uint8_t counter, std::uint8_t device) noexcept {
    Writer w(0x04, 0x00, name, 0x03, kFinalClaimLength);
    w.put(0x24, device);
    w.put(0x25, counter);
    return w.done();
}

Built buildKeepAlive(const std::string& name, std::uint8_t device, const Mac& mac, std::uint32_t ip,
                     std::uint8_t peers, bool firstHere) noexcept {
    Writer w(0x06, 0x00, name, 0x02, kKeepAliveLength);
    w.put(0x24, device);
    w.put(0x25, static_cast<std::uint8_t>(firstHere ? 0x02 : 0x01));
    w.putMac(0x26, mac);
    w.put32(0x2c, ip);
    w.put(0x30, peers);
    w.put(0x34, 0x01);
    // 64, not the older players' 00: the wrong value here can make a CDJ-3000 on number 5 or 6
    // keep throwing itself off the network.
    w.put(0x35, 0x64);
    return w.done();
}

Built buildConflict(const std::string& name, std::uint8_t device, std::uint32_t ip) noexcept {
    Writer w(0x08, 0x00, name, 0x02, kConflictLength);
    w.put(0x24, device);
    w.put32(0x25, ip);
    return w.done();
}

Built buildNumberRequest(const std::string& name, std::uint32_t ip, const Mac& mac) noexcept {
    // A second-stage claim with 01 after the type, sent to the mixer alone, naming no number.
    Writer w(0x02, 0x01, name, 0x02, kSecondClaimLength);
    w.put32(0x24, ip);
    w.putMac(0x28, mac);
    w.put(0x2e, 0x00);
    w.put(0x2f, 0x01);
    w.put(0x30, 0x01);
    w.put(0x31, 0x01);
    return w.done();
}

std::string nameText(const Name& name) {
    std::string text;
    for (const char c : name) {
        if (c == '\0') {
            break;
        }
        const auto byte = static_cast<unsigned char>(c);
        text.push_back(byte >= 0x20 && byte < 0x7f ? c : '?');
    }
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

} // namespace takt4::timing::prolink
