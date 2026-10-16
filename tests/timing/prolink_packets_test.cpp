#include "core/timing/prolink/packets.hpp"

#include "support/prolink_capture.hpp"
#include "support/prolink_sim.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace takt4::timing::prolink;
using takt4::testing::Captured;
using takt4::testing::ipv4;
using takt4::testing::loadCapture;

namespace {

std::span<const std::uint8_t> bytesOf(const Captured& c) {
    return {c.bytes.data(), c.bytes.size()};
}

/// Port 50001 and 50002 types takt4 has no use for, which its parsers must refuse: on-air flags,
/// fader start, master hand-off requests, sync control, media queries and answers.
bool otherKnownType(const Captured& c) {
    if (c.port == 50001) {
        return c.type() == 0x02 || c.type() == 0x03 || c.type() == 0x26 || c.type() == 0x27 ||
               c.type() == 0x2a || c.type() == 0x0b;
    }
    if (c.port == 50002) {
        return c.type() == 0x05 || c.type() == 0x06;
    }
    return false;
}

const Mac kDeckA{0x74, 0x5e, 0x1c, 0x56, 0x67, 0xac};
constexpr std::uint32_t kDeckAIp = ipv4(169, 254, 103, 172);
constexpr std::uint32_t kDeckBIp = ipv4(169, 254, 202, 84);

} // namespace

TEST_CASE("every datagram real players sent parses as the packet its port and type say, or is "
          "one takt4 leaves alone",
          "[prolink]") {
    for (const char* file : {"cold-boot.txt", "joins-occupied.txt", "load-and-play.txt",
                             "nexus-and-djm.txt", "master-handoff.txt"}) {
        INFO(file);
        const std::vector<Captured> datagrams = loadCapture(file);
        REQUIRE(datagrams.size() > 10);
        for (const Captured& c : datagrams) {
            INFO("at " << c.seconds << " port " << c.port << " type " << int(c.type()));
            REQUIRE(c.bytes.size() > 10);
            REQUIRE(std::equal(kMagic.begin(), kMagic.end(), c.bytes.begin()));
            AnnouncePacket announce;
            BeatPacket beat;
            StatusPacket status;
            const bool parsed = c.port == 50000   ? parseAnnounce(bytesOf(c), announce)
                                : c.port == 50001 ? parseBeat(bytesOf(c), beat)
                                                  : parseStatus(bytesOf(c), status);
            CHECK(parsed != otherKnownType(c));
        }
    }
}

TEST_CASE("a CDJ-2000nexus starting up alone says hello, claims player 1 in three stages and "
          "keeps alive, and every field reads as it sent it",
          "[prolink]") {
    const std::vector<Captured> boot = loadCapture("cold-boot.txt");
    std::vector<AnnouncePacket> packets;
    for (const Captured& c : boot) {
        AnnouncePacket p;
        REQUIRE(parseAnnounce(bytesOf(c), p));
        CHECK(nameText(p.name) == "CDJ-2000nexus");
        CHECK(c.from == kDeckAIp);
        CHECK(c.to == ipv4(169, 254, 255, 255));
        packets.push_back(p);
    }
    REQUIRE(packets.size() == 20);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(packets[i].type == Announce::Hello);
        CHECK(packets[i].kind == DeviceKind::Player);
        CHECK(packets[3 + i].type == Announce::FirstClaim);
        CHECK(packets[3 + i].counter == i + 1);
        CHECK(packets[3 + i].mac == kDeckA);
        CHECK(packets[6 + i].type == Announce::SecondClaim);
        CHECK(packets[6 + i].counter == i + 1);
        CHECK(packets[6 + i].device == 1);
        CHECK(packets[6 + i].ip == kDeckAIp);
        CHECK(packets[6 + i].mac == kDeckA);
        CHECK_FALSE(packets[6 + i].autoAssign); // set to player 1 by hand
        CHECK(packets[9 + i].type == Announce::FinalClaim);
        CHECK(packets[9 + i].device == 1);
        CHECK(packets[9 + i].counter == i + 1);
    }
    for (std::size_t i = 12; i < packets.size(); ++i) {
        CHECK(packets[i].type == Announce::KeepAlive);
        CHECK(packets[i].device == 1);
        CHECK(packets[i].ip == kDeckAIp);
        CHECK(packets[i].mac == kDeckA);
        CHECK(packets[i].kind == DeviceKind::Player);
        CHECK(packets[i].peers == 1);
    }
    // 300 ms between the claim's packets, and a keep-alive every two seconds once settled.
    for (std::size_t i = 1; i < 12; ++i) {
        CHECK(boot[i].seconds - boot[i - 1].seconds == Catch::Approx(0.3).margin(0.01));
    }
    for (std::size_t i = 14; i < boot.size(); ++i) {
        CHECK(boot[i].seconds - boot[i - 1].seconds == Catch::Approx(2.0).margin(0.01));
    }
}

TEST_CASE("a player starting onto a network another holds is told assignment finished by it, "
          "after one final-stage claim",
          "[prolink]") {
    const std::vector<Captured> datagrams = loadCapture("joins-occupied.txt");
    bool autoClaimSeen = false;
    int finalClaimsFromA = 0;
    bool finishedSeen = false;
    for (const Captured& c : datagrams) {
        AnnouncePacket p;
        REQUIRE(parseAnnounce(bytesOf(c), p));
        if (p.type == Announce::SecondClaim && c.from == kDeckBIp) {
            CHECK(p.autoAssign); // player 2 is on AUTO here
            CHECK(p.device == 2);
            autoClaimSeen = true;
        }
        if (p.type == Announce::FinalClaim && c.from == kDeckAIp && !finishedSeen) {
            ++finalClaimsFromA;
        }
        if (p.type == Announce::AssignmentFinished) {
            CHECK(c.from == kDeckBIp);
            CHECK(c.to == kDeckAIp); // straight to the player claiming, not broadcast
            CHECK(p.device == 2);    // the sender's own number
            finishedSeen = true;
        }
    }
    CHECK(autoClaimSeen);
    CHECK(finishedSeen);
    CHECK(finalClaimsFromA == 1);
}

TEST_CASE("a CDJ's beats and status read as the track, pitch, bar and play state it was in",
          "[prolink]") {
    const std::vector<Captured> datagrams = loadCapture("load-and-play.txt");
    std::vector<BeatPacket> beats;
    std::set<std::uint8_t> playModes;
    bool sawPlayingMaster = false;
    bool sawCuePlay = false;
    for (const Captured& c : datagrams) {
        if (c.port == 50001) {
            BeatPacket b;
            REQUIRE(parseBeat(bytesOf(c), b));
            beats.push_back(b);
        } else if (c.port == 50002) {
            StatusPacket s;
            REQUIRE(parseStatus(bytesOf(c), s));
            CHECK_FALSE(s.mixer);
            CHECK(nameText(s.name) == "CDJ-2000nexus");
            CHECK(c.bytes.size() == 0x11c); // a nexus on firmware 1.44: the length is no guide
            if (s.device == 1) {
                playModes.insert(s.playMode);
                if (s.playing() && s.master()) {
                    sawPlayingMaster = true;
                    CHECK(s.flags == 0xe4);
                    CHECK(s.masterMeaningful == 1);
                    CHECK(s.handingTo == 0xff);
                    CHECK(s.bpm() == Catch::Approx(127.984).margin(0.001));
                    CHECK(s.beatInBar >= 1);
                    CHECK(s.beatInBar <= 4);
                }
                if (s.playMode == 0x07) {
                    sawCuePlay = true;
                    CHECK(s.cueing());
                    CHECK_FALSE(s.playing());
                }
            }
        }
    }
    CHECK(sawPlayingMaster);
    CHECK(sawCuePlay);
    CHECK(playModes.count(0x03) == 1); // playing
    CHECK(playModes.count(0x05) == 1); // paused
    CHECK(playModes.count(0x06) == 1); // paused at the cue
    REQUIRE(beats.size() == 93);
    for (const BeatPacket& b : beats) {
        CHECK(b.device == 1);
        CHECK(nameText(b.name) == "CDJ-2000nexus");
        CHECK(b.bpm100 == 13201);
        CHECK(b.pitch == 0x0f8312u);
        // 132.01 BPM at -3 %: the tempo playing.
        CHECK(b.bpm() == Catch::Approx(127.984).margin(0.001));
        CHECK((b.nextBeatMs == 454 || b.nextBeatMs == 455));
        CHECK(b.beatInBar >= 1);
        CHECK(b.beatInBar <= 4);
    }
    // While it plays straight through, the bar counts 1 2 3 4.
    for (std::size_t i = 1; i < 60; ++i) {
        CHECK(beats[i].beatInBar == beats[i - 1].beatInBar % 4 + 1);
    }
}

TEST_CASE("a DJM-2000nexus's status and beats, and a nexus's 0xd4-byte status, read as sent",
          "[prolink]") {
    const std::vector<Captured> datagrams = loadCapture("nexus-and-djm.txt");
    int mixerStatus = 0;
    int mixerBeats = 0;
    int playerStatus = 0;
    for (const Captured& c : datagrams) {
        if (c.port == 50002 && c.type() == 0x29) {
            StatusPacket s;
            REQUIRE(parseStatus(bytesOf(c), s));
            CHECK(s.mixer);
            CHECK(s.device == 33);
            CHECK(nameText(s.name) == "DJM-2000nexus");
            CHECK_FALSE(s.master());
            CHECK(s.flags == 0xd0);
            CHECK(s.bpm() == Catch::Approx(120.0));
            CHECK_FALSE(s.cueing());
            ++mixerStatus;
        } else if (c.port == 50002 && c.type() == 0x0a) {
            StatusPacket s;
            REQUIRE(parseStatus(bytesOf(c), s));
            CHECK_FALSE(s.mixer);
            CHECK((s.device == 2 || s.device == 3));
            CHECK(c.bytes.size() == 0xd4);
            ++playerStatus;
        } else if (c.port == 50001 && c.type() == 0x28) {
            BeatPacket b;
            REQUIRE(parseBeat(bytesOf(c), b));
            CHECK(b.device == 33);
            CHECK(b.bpm() == Catch::Approx(120.0));
            ++mixerBeats;
        }
    }
    CHECK(mixerStatus == 35);
    CHECK(playerStatus == 70);
    CHECK(mixerBeats == 14);
}

TEST_CASE("the tempo master is handed from player to player and to the mixer, as status says",
          "[prolink]") {
    // Each change of who says they are master, in order: the hand-offs the DJM was asked for.
    std::vector<std::pair<std::uint8_t, bool>> changes;
    std::map<std::uint8_t, bool> master;
    std::vector<std::pair<std::uint8_t, std::uint8_t>> handOffs;
    for (const Captured& c : loadCapture("master-handoff.txt")) {
        if (c.port != 50002) {
            continue;
        }
        StatusPacket s;
        if (!parseStatus(bytesOf(c), s)) {
            continue;
        }
        if (master[s.device] != s.master()) {
            master[s.device] = s.master();
            changes.emplace_back(s.device, s.master());
        }
        if (s.handingTo != 0xff && s.handingTo != 0x00 &&
            (handOffs.empty() || handOffs.back() != std::make_pair(s.device, s.handingTo))) {
            handOffs.emplace_back(s.device, s.handingTo);
        }
    }
    // Player 3 starts as master, without a track that can lead.
    REQUIRE(changes.size() >= 9);
    CHECK(changes[0] == std::pair<std::uint8_t, bool>{3, true});
    const std::vector<std::pair<std::uint8_t, std::uint8_t>> expected{
        {3, 2}, {2, 3}, {3, 2}, {2, 33}, {33, 3}};
    CHECK(handOffs == expected);
    CHECK(master[33] == false); // ...and the mixer handed it back to 3 at the end
}

TEST_CASE("a status of a player older than the nexus, 0xd0 bytes, is read; one shorter is not",
          "[prolink]") {
    takt4::testing::prolink_sim::PlayerState state;
    state.flags = 0;
    const auto old = takt4::testing::prolink_sim::playerStatus(4, state, "CDJ-900", 0xd0);
    StatusPacket s;
    CHECK(parseStatus(old, s));
    CHECK(s.device == 4);
    CHECK_FALSE(s.master());
    const std::vector<std::uint8_t> shorter(old.begin(), old.end() - 1);
    CHECK_FALSE(parseStatus(shorter, s));
}

TEST_CASE("every packet cut short by a byte, and one with the wrong magic, is refused",
          "[prolink]") {
    for (const char* file : {"cold-boot.txt", "joins-occupied.txt", "nexus-and-djm.txt"}) {
        for (const Captured& c : loadCapture(file)) {
            if (otherKnownType(c)) {
                continue;
            }
            const std::span<const std::uint8_t> whole = bytesOf(c);
            // Each type's documented length, and the longer packets a newer firmware sends, are
            // read; one byte short of the documented length never is.
            std::vector<std::uint8_t> bad(whole.begin(), whole.end());
            bad[3] ^= 0x01;
            AnnouncePacket a;
            BeatPacket b;
            StatusPacket s;
            CHECK_FALSE(parseAnnounce(bad, a));
            CHECK_FALSE(parseBeat(bad, b));
            CHECK_FALSE(parseStatus(bad, s));
            if (c.port == 50000 && c.type() != 0x0a) {
                CHECK_FALSE(parseAnnounce(whole.first(whole.size() - 1), a));
            } else if (c.port == 50001) {
                CHECK_FALSE(parseBeat(whole.first(whole.size() - 1), b));
            }
        }
    }
}

TEST_CASE("bytes off the network that are nothing (empty, short, random) are refused without "
          "reading past their end",
          "[prolink]") {
    std::mt19937 random(20261016);
    std::uniform_int_distribution<int> byte(0, 255);
    for (int trial = 0; trial < 20000; ++trial) {
        const std::size_t length = static_cast<std::size_t>(trial % 700);
        // A heap block of exactly the length, so AddressSanitizer catches one byte read too far.
        std::vector<std::uint8_t> data(length);
        for (auto& b : data) {
            b = static_cast<std::uint8_t>(byte(random));
        }
        if (length >= 11 && trial % 2 == 0) {
            std::copy(kMagic.begin(), kMagic.end(), data.begin());
            static constexpr std::uint8_t kTypes[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                                      0x06, 0x08, 0x0a, 0x28, 0x29, 0x0b};
            data[10] = kTypes[static_cast<std::size_t>(trial / 2) % std::size(kTypes)];
        }
        AnnouncePacket a;
        BeatPacket b;
        StatusPacket s;
        (void)parseAnnounce(data, a);
        (void)parseBeat(data, b);
        (void)parseStatus(data, s);
        (void)nameText(a.name);
    }
    SUCCEED();
}

TEST_CASE("takt4's own packets are a real player's byte for byte, where they say the same thing",
          "[prolink]") {
    const std::vector<Captured> boot = loadCapture("cold-boot.txt");
    const auto same = [](const Built& built, const Captured& real, std::set<std::size_t> differ) {
        REQUIRE(built.size == real.bytes.size());
        for (std::size_t i = 0; i < built.size; ++i) {
            if (differ.count(i) == 0) {
                INFO("byte " << i);
                CHECK(built.bytes[i] == real.bytes[i]);
            }
        }
    };
    // Byte 21 is the CDJ-3000 form's subtype (03, where the nexus sends 02); byte 31 says the
    // number is picked automatically, which the nexus, set to player 1, did not.
    same(buildFirstClaim("CDJ-2000nexus", 1, kDeckA), boot[3], {0x21});
    same(buildSecondClaim("CDJ-2000nexus", 2, kDeckAIp, kDeckA, 1, false), boot[7], {0x21});
    same(buildFinalClaim("CDJ-2000nexus", 3, 1), boot[11], {0x21});
    // The keep-alive: 25 is "first on the network" (this nexus was), 35 the CDJ-3000 form's 64.
    same(buildKeepAlive("CDJ-2000nexus", 1, kDeckA, kDeckAIp, 1, true), boot[12], {0x35});
    CHECK(buildKeepAlive("x", 1, kDeckA, kDeckAIp, 1, true).bytes[0x35] == 0x64);
    // The CDJ-3000 hello is one byte longer than the nexus's, with 04 at 21 and 40 at the end.
    const Built hello = buildHello("CDJ-2000nexus");
    REQUIRE(hello.size == 0x26);
    for (std::size_t i = 0; i < 0x21; ++i) {
        CHECK(hello.bytes[i] == boot[0].bytes[i]);
    }
    CHECK(hello.bytes[0x21] == 0x04);
    CHECK(hello.bytes[0x25] == 0x40);
}

TEST_CASE("every packet takt4 sends parses back as what it was built to say", "[prolink]") {
    const Mac mac{0x02, 0x00, 0x5e, 0x10, 0x20, 0x30};
    const std::uint32_t ip = ipv4(169, 254, 7, 7);
    AnnouncePacket p;

    REQUIRE(parseAnnounce(buildHello("takt4").view(), p));
    CHECK(p.type == Announce::Hello);
    CHECK(nameText(p.name) == "takt4");

    REQUIRE(parseAnnounce(buildFirstClaim("takt4", 2, mac).view(), p));
    CHECK(p.type == Announce::FirstClaim);
    CHECK(p.counter == 2);
    CHECK(p.mac == mac);

    REQUIRE(parseAnnounce(buildSecondClaim("takt4", 3, ip, mac, 9, true).view(), p));
    CHECK(p.type == Announce::SecondClaim);
    CHECK(p.counter == 3);
    CHECK(p.ip == ip);
    CHECK(p.mac == mac);
    CHECK(p.device == 9);
    CHECK(p.autoAssign);
    CHECK_FALSE(p.fromMixerPort);

    REQUIRE(parseAnnounce(buildFinalClaim("takt4", 1, 9).view(), p));
    CHECK(p.type == Announce::FinalClaim);
    CHECK(p.device == 9);
    CHECK(p.counter == 1);

    REQUIRE(parseAnnounce(buildKeepAlive("takt4", 9, mac, ip, 4, false).view(), p));
    CHECK(p.type == Announce::KeepAlive);
    CHECK(p.device == 9);
    CHECK(p.ip == ip);
    CHECK(p.mac == mac);
    CHECK(p.peers == 4);
    CHECK(p.kind == DeviceKind::Player);

    REQUIRE(parseAnnounce(buildConflict("takt4", 9, ip).view(), p));
    CHECK(p.type == Announce::Conflict);
    CHECK(p.device == 9);
    CHECK(p.ip == ip);

    REQUIRE(parseAnnounce(buildNumberRequest("takt4", ip, mac).view(), p));
    CHECK(p.type == Announce::SecondClaim);
    CHECK(p.fromMixerPort);
    CHECK(p.device == 0);
    CHECK(p.ip == ip);
}

TEST_CASE("a name is cut to twenty characters, and anything but ASCII in it is sent as a space",
          "[prolink]") {
    AnnouncePacket p;
    REQUIRE(parseAnnounce(buildHello("takt4 \xC3\xA9 a very long name indeed").view(), p));
    CHECK(nameText(p.name) == "takt4    a very long");
    CHECK(nameText(p.name).size() == 20);
}

TEST_CASE("the test simulator's packets parse as the fields it was given", "[prolink]") {
    namespace sim = takt4::testing::prolink_sim;
    BeatPacket b;
    REQUIRE(parseBeat(sim::beat(2, 126.0, 3, 1.05), b));
    CHECK(b.device == 2);
    CHECK(b.beatInBar == 3);
    CHECK(b.bpm() == Catch::Approx(126.0).margin(0.01));

    sim::PlayerState state;
    state.flags = sim::kPlaying | sim::kMaster;
    state.playMode = sim::kModePlaying;
    state.bpm = 124.5;
    StatusPacket s;
    REQUIRE(parseStatus(sim::playerStatus(3, state), s));
    CHECK(s.device == 3);
    CHECK(s.playing());
    CHECK(s.master());
    CHECK(s.masterMeaningful == 1);
    CHECK(s.bpm() == Catch::Approx(124.5).margin(0.01));

    REQUIRE(parseStatus(sim::mixerStatus(33, true, 122.0, 2), s));
    CHECK(s.mixer);
    CHECK(s.master());
    CHECK(s.handingTo == 2);
    CHECK(s.bpm() == Catch::Approx(122.0));
}
