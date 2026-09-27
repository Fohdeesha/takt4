#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace takt4::dmx {

/// Art-Net 4 on the wire — the ArtDmx packet and the 15-bit Port-Address it carries.
///
/// Written against *Art-Net 4 Protocol Release V1.4, Document Revision 1.4dp (23/10/2025)*,
/// published by Artistic Licence at https://art-net.org.uk/downloads/art-net.pdf. Every
/// constant below is quoted from it, and the byte layout was checked a second time against
/// an independent implementation before it was written down, because a field in the wrong
/// place here is a rig that does nothing and gives no reason.
///
/// **Art-Net™ Designed by and Copyright Artistic Licence** — the credit the specification
/// requires of anything that implements it. It is also in the README, which is where a user
/// will see it.
///
/// What this file is not: a Node. takt4 is a *Controller* in Art-Net's terms — it generates
/// levels and sends them — so ArtPoll, ArtPollReply, ArtAddress and RDM are all somebody
/// else's half of the protocol and none of them are here. See `ArtNetSender` for the one
/// consequence of that choice that an operator can actually see (takt4 has to be told where
/// to send, because it does not discover).

/// The only UDP port Art-Net uses, for source and destination alike: *"Art-Net uses only one
/// port of 0x1936."*
inline constexpr std::uint16_t kArtNetPort = 0x1936; // 6454

/// The eight bytes every Art-Net packet opens with, null terminator included.
inline constexpr char kArtNetId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', '\0'};

/// `OpOutput` / `OpDmx` — *"zero start code DMX512 information for a single Universe"*.
/// Transmitted **low byte first**, unlike the length below; see `writeArtDmx`.
inline constexpr std::uint16_t kOpDmx = 0x5000;

/// `ProtVerHi` and `ProtVerLo`. *"Current value 14"*, and *"controllers should ignore
/// communication with nodes using a protocol version lower than 14"*.
inline constexpr std::uint8_t kProtocolVersionHi = 0;
inline constexpr std::uint8_t kProtocolVersionLo = 14;

/// One DMX512 frame. The number is the protocol's and the standard's alike, and is the
/// reason a fixture patch is addressed 1 to 512 rather than from zero.
inline constexpr std::size_t kChannelsPerUniverse = 512;

/// Fields 1 to 10 of the packet — everything before `Data`.
inline constexpr std::size_t kArtDmxHeaderSize = 18;

/// The largest ArtDmx datagram: the header and a full universe.
inline constexpr std::size_t kArtDmxMaxSize = kArtDmxHeaderSize + kChannelsPerUniverse;

/// How fast a DMX512 gateway can be driven: *"For a gateway outputting DMX512, this will
/// always be the maximum rate of 44Hz."*
///
/// It is a ceiling and not a target. A node handed frames faster than it can pass them on
/// drops them, and dropping them is the *good* case — some drop the wrong ones.
/// `ArtNetPublisher` paces every universe of every node against this, and nothing in takt4 may
/// send faster.
inline constexpr double kMaxRefreshHz = 44.0;

/// How long a universe may go unchanged before it is sent again anyway.
///
/// The specification's own words are *"an input that is active but not changing, will
/// re-transmit the last valid ArtDmx packet at approximately 4-second intervals"*, followed
/// immediately by the note that supersedes it: *"In order to converge the needs of Art-Net
/// and sACN it is recommended that Art-Net devices actually use a re-transmit time of 800mS
/// to 1000mS"*. The recommendation is what is implemented; 900 ms sits in the middle of it.
///
/// This is what tells a node the difference between "nothing is changing" and "the
/// controller has gone away", and a node that decides the latter may hold, fade or release
/// depending on how it is configured. A keep-alive is therefore not an optimisation to be
/// switched off: it is the thing that keeps the lights on between drops.
inline constexpr double kKeepAliveSeconds = 0.9;

/// A 15-bit Port-Address: *"one of the 32,768 possible addresses to which a DMX frame can be
/// directed ... composed of Net+Sub-Net+Universe"*.
///
/// | Bit 15 | Bits 14-8 | Bits 7-4 | Bits 3-0 |
/// |---|---|---|---|
/// | 0 | Net | Sub-Net | Universe |
///
/// **Held as the one flat number rather than the three parts**, because that is how every
/// lighting desk, media server and node front panel an operator has already used spells it:
/// a box that says "universe" and takes 0, 1, 2. Splitting it into three boxes would be
/// showing the operator the packet layout instead of their rig. `netOf` and `subUniOf` put
/// it back into the two bytes the wire wants, and are the only place the split appears.
using PortAddress = std::uint16_t;

/// The largest Port-Address. *"The Port-Address valid range is 1 to 32,767."*
inline constexpr PortAddress kMaxPortAddress = 32767;

/// Field 8: *"the top 7 bits of the 15 bit Port-Address"*.
constexpr std::uint8_t netOf(PortAddress address) noexcept {
    return static_cast<std::uint8_t>((address >> 8) & 0x7F);
}

/// Field 7: *"the low byte of the 15 bit Port-Address"* — Sub-Net in the high nibble, the
/// universe within the node in the low one.
constexpr std::uint8_t subUniOf(PortAddress address) noexcept {
    return static_cast<std::uint8_t>(address & 0xFF);
}

/// Anything past the 15 bits, folded back in. A settings file hand-edited to 70000 gets a
/// universe rather than a packet addressed to a node that cannot exist — `settings::load` is
/// documented never to fail, so this clamps like everything else it reads.
constexpr PortAddress clampPortAddress(int universe) noexcept {
    if (universe < 0) {
        return 0;
    }
    return universe > kMaxPortAddress ? kMaxPortAddress : static_cast<PortAddress>(universe);
}

/// "0" — and "4:2:1" for anything past the first Net, which is how a node's front panel
/// spells it and the only way to tell universe 513 from universe 1 at a glance.
std::string describePortAddress(PortAddress address);

/// The inverse, forgiving: "1", "0:0:1" and "4:2:1" all parse, and so does whitespace
/// around them. False for anything else, leaving `out` untouched.
bool parsePortAddress(std::string_view text, PortAddress& out) noexcept;

/// Builds one ArtDmx packet into `out`, and says how many bytes of it are the packet.
///
/// `levels` is the universe's channel data, **1 to 512 bytes**; `out` must be at least
/// `kArtDmxMaxSize`. The returned length is `kArtDmxHeaderSize` plus the rounded-up data
/// length, or zero when `levels` is empty or longer than a universe — a refusal rather than
/// a truncation, because a short frame silently sent is a fixture at the top of a universe
/// that stops responding for reasons nothing explains.
///
/// **The two multi-byte fields have opposite endianness, and that is the specification's,
/// not a mistake here.** `OpCode` is *"transmitted low byte first"*; `Length` is written
/// `LengthHi` then `Length`, high byte first. An implementation that picks one order for
/// both works against exactly half of the fields.
///
/// The data length is rounded up to an even number: *"this value should be an even number in
/// the range 2 to 512"*. A patch that only reaches channel 7 is sent as 8.
///
/// `sequence` is field 5: *"incremented in the range 0x01 to 0xff to allow the receiving node
/// to re-sequence packets ... set to 0x00 to disable this feature"*. `nextSequence` below
/// counts it; `ArtNetSender::sendDmx` keeps one counter per universe, and says why.
///
/// `physical` is field 6, which exists so a node can tell two of its *own* DMX inputs apart
/// when merging. takt4 has no DMX inputs, so it sends zero, which is what every controller
/// that is not a gateway sends.
std::size_t writeArtDmx(std::span<std::byte> out, PortAddress address,
                        std::span<const std::uint8_t> levels, std::uint8_t sequence,
                        std::uint8_t physical = 0) noexcept;

/// The next value of the sequence counter: 1 to 255, skipping 0 because 0 means *"disabled"*
/// rather than *"the packet before 1"*. A node that has been told sequencing is on and then
/// sees a zero has to decide what that means, and they do not all decide the same thing.
constexpr std::uint8_t nextSequence(std::uint8_t current) noexcept {
    return current == 255 || current == 0 ? 1 : static_cast<std::uint8_t>(current + 1);
}

} // namespace takt4::dmx
