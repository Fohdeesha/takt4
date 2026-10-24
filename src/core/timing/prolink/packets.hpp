#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/// **Pro DJ Link's packets**, read and written — the UDP protocol Pioneer's players and mixers
/// speak to each other, as Deep Symmetry's DJ Link packet analysis describes it
/// (djl-analysis.deepsymmetry.org: "Packet Types", "Mixer and CDJ Startup", "Tracking BPM, Beats,
/// and Position", "Detailed Device Status", "Sync and Tempo Master"). Written from that
/// description alone; the offsets below are the analysis's own, in hex, as it gives them.
///
/// Every packet starts with the ten bytes "Qspt1WmJOL" and a type byte, read together with the
/// port it came to. Nothing here has a socket, a clock or a thread: bytes in, fields out, and
/// fields in, bytes out — so every byte is tested against packets real players sent
/// (tests/data/prolink).
namespace takt4::timing::prolink {

/// "Qspt1WmJOL".
inline constexpr std::array<std::uint8_t, 10> kMagic{0x51, 0x73, 0x70, 0x74, 0x31,
                                                     0x57, 0x6d, 0x4a, 0x4f, 0x4c};

/// Device announcements and the claiming of device numbers.
inline constexpr std::uint16_t kAnnouncePort = 50000;
/// Beats, broadcast by every player as it plays and by the mixer always.
inline constexpr std::uint16_t kBeatPort = 50001;
/// Device status, sent only to a device that has announced itself.
inline constexpr std::uint16_t kStatusPort = 50002;

/// The 20 bytes a device's name is given, padded with zeros.
inline constexpr std::size_t kNameLength = 0x14;

using Mac = std::array<std::uint8_t, 6>;
/// A device's name as it sent it — ASCII, at most 20 characters — with a terminator.
using Name = std::array<char, kNameLength + 1>;

/// The types of port 50000's packets.
enum class Announce : std::uint8_t {
    /// First of the three stages of claiming a device number, sent three times.
    FirstClaim = 0x00,
    /// A mixer telling a device plugged into one of its channel's ports that it will be given
    /// that channel's number.
    AssignmentIntention = 0x01,
    /// Second stage: the number wanted, with the device's address. With byte 0b at 01, the
    /// request a device sends a mixer that has said it will assign the number.
    SecondClaim = 0x02,
    /// The number a mixer assigns.
    Assignment = 0x03,
    /// Final stage of the claim.
    FinalClaim = 0x04,
    /// "Done": sent to a device claiming a number by one already on the network, which cuts its
    /// final stage short.
    AssignmentFinished = 0x05,
    /// "Still here", every second and a half (a mixer) to two seconds (a player).
    KeepAlive = 0x06,
    /// "That number is mine", sent to a device claiming one already in use.
    Conflict = 0x08,
    /// The first thing a device starting up says, three times.
    Hello = 0x0a,
};

/// What a device is, as its keep-alive says — byte 34 is 01 for a player and 02 for a mixer.
enum class DeviceKind : std::uint8_t { Unknown, Player, Mixer };

/// A port 50000 packet. Only the fields its type carries are filled in; the rest stay zero.
struct AnnouncePacket {
    Announce type = Announce::Hello;
    Name name{};
    /// D: the device number claimed, assigned, kept or defended.
    std::uint8_t device = 0;
    /// N: which of a stage's three packets this is, 1 to 3.
    std::uint8_t counter = 0;
    /// The sender's address, as the packet itself says it, in host order.
    std::uint32_t ip = 0;
    Mac mac{};
    /// A keep-alive's p: the devices its sender sees, itself included.
    std::uint8_t peers = 0;
    DeviceKind kind = DeviceKind::Unknown;
    /// A second-stage claim's a: the number is being picked automatically, not asked for.
    bool autoAssign = false;
    /// Byte 0b at 01: the packets between a mixer and a device plugged into its channel's port.
    bool fromMixerPort = false;
};

/// Reads one port 50000 packet of a type above. False for anything else, or a packet too short
/// for its type. Never throws.
bool parseAnnounce(std::span<const std::uint8_t> datagram, AnnouncePacket& out) noexcept;

/// A beat packet (port 50001, type 28): sent by a player on every beat while it plays a track
/// rekordbox has analysed, and by the mixer on every beat always, as a backup metronome.
struct BeatPacket {
    Name name{};
    std::uint8_t device = 0;
    /// Milliseconds to the next beat, as if at 0 % pitch; 0xffffffff where the track ends first.
    std::uint32_t nextBeatMs = 0;
    /// The pitch: 0x00100000 is 0 %, 0 is stopped, 0x00200000 double speed.
    std::uint32_t pitch = 0;
    /// The track's tempo at this point, in hundredths of a BPM, at 0 % pitch.
    std::uint16_t bpm100 = 0;
    /// Bb: the beat in the bar, 1 to 4 — the bar is the DJ's beat grid's.
    std::uint8_t beatInBar = 0;

    /// The tempo playing, pitch and all, in BPM; 0 when the packet carries none.
    double bpm() const noexcept;
};

bool parseBeat(std::span<const std::uint8_t> datagram, BeatPacket& out) noexcept;

/// A device's status (port 50002): a player's (type 0a) or a mixer's (type 29), sent about
/// five times a second to each device that has announced itself.
struct StatusPacket {
    bool mixer = false;
    Name name{};
    std::uint8_t device = 0;
    /// F: bit 6 playing, bit 5 tempo master, bit 4 synced, bit 3 on the air. A mixer's is f0 as
    /// master and d0 otherwise; a player older than the nexus sends 00 here always.
    std::uint8_t flags = 0;
    /// P1, a player's play mode: 03 playing, 05 and 06 paused, 07 cue play (the cue button held),
    /// 08 cue scratch, 09 searching. A mixer has none, and reads 0.
    std::uint8_t playMode = 0;
    std::uint32_t pitch = 0;
    /// The track's tempo at the play position, in hundredths of a BPM, at 0 % pitch; 0xffff with
    /// no track loaded.
    std::uint16_t bpm100 = 0;
    /// Mm, a player's: 01 tempo master with a rekordbox track, 02 master with a track that
    /// cannot lead, 00 not master. A mixer has none.
    std::uint8_t masterMeaningful = 0;
    /// Mh: ff, or the device the master role is being handed to.
    std::uint8_t handingTo = 0xff;
    /// A player's beat counter in its track, or 0xffffffff with none. A mixer has none.
    std::uint32_t beat = 0;
    std::uint8_t beatInBar = 0;

    bool playing() const noexcept { return (flags & 0x40) != 0; }
    bool master() const noexcept { return (flags & 0x20) != 0; }
    bool synced() const noexcept { return (flags & 0x10) != 0; }
    /// Its channel can be heard in the mix. Only a mixer that tells the players so sets it (a DJM
    /// with Pro DJ Link): with none, every player reads it clear; with one, a player reads it set
    /// whenever its fader is up, playing or not (both seen in the captures).
    bool onAir() const noexcept { return (flags & 0x08) != 0; }
    /// A player with a track rekordbox has analysed, which is what it sends beats for. The beat
    /// counter reads ffffffff with no track, a track not analysed or a CD, and always on a player
    /// older than the nexus.
    bool beatGrid() const noexcept { return !mixer && beat != 0xffffffffu; }
    /// Playing with the cue button held, scratching at the cue, or searching: what a player says
    /// while its beats are a preview in the DJ's headphones, not the music.
    bool cueing() const noexcept { return !mixer && playMode >= 0x07 && playMode <= 0x09; }
    /// The tempo playing, pitch and all, in BPM; 0 with no track.
    double bpm() const noexcept;
};

bool parseStatus(std::span<const std::uint8_t> datagram, StatusPacket& out) noexcept;

/// The longest packet takt4 sends: a keep-alive, 0x36 bytes.
inline constexpr std::size_t kMaxSent = 0x40;

/// A packet to send, built in place.
struct Built {
    std::array<std::uint8_t, kMaxSent> bytes{};
    std::size_t size = 0;
    std::span<const std::uint8_t> view() const noexcept { return {bytes.data(), size}; }
};

/// The packets a virtual player sends, each in the form that lets it share a network with
/// CDJ-3000s on numbers 5 and 6 — the analysis's "CDJ-3000 compatible" templates, which the
/// older players take too. `name` is cut to 20 ASCII characters.
Built buildHello(const std::string& name) noexcept;
Built buildFirstClaim(const std::string& name, std::uint8_t counter, const Mac& mac) noexcept;
Built buildSecondClaim(const std::string& name, std::uint8_t counter, std::uint32_t ip,
                       const Mac& mac, std::uint8_t device, bool autoAssign) noexcept;
Built buildFinalClaim(const std::string& name, std::uint8_t counter, std::uint8_t device) noexcept;
/// `firstHere` is byte 25: 02 when this device found nobody else on the network as it started,
/// 01 when it did.
Built buildKeepAlive(const std::string& name, std::uint8_t device, const Mac& mac, std::uint32_t ip,
                     std::uint8_t peers, bool firstHere) noexcept;
/// "Device `device` is mine, at `ip`" — sent to a device claiming it.
Built buildConflict(const std::string& name, std::uint8_t device, std::uint32_t ip) noexcept;
/// The answer to a mixer's assignment intention: ready to be given a number.
Built buildNumberRequest(const std::string& name, std::uint32_t ip, const Mac& mac) noexcept;

/// The name as text, for a screen: the printable ASCII of it, the zeros dropped.
std::string nameText(const Name& name);

/// A hardware address as the settings file keeps it: "74:5e:1c:00:00:02", lower case.
std::string macText(const Mac& mac);
/// The other way: six pairs of hex digits, each pair after the first behind a ':' or a '-', in
/// either case. Nothing for anything else.
std::optional<Mac> parseMac(std::string_view text);

} // namespace takt4::timing::prolink
