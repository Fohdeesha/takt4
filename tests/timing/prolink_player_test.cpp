#include "core/timing/prolink/virtual_player.hpp"

#include "support/prolink_capture.hpp"
#include "support/prolink_sim.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace takt4::timing::prolink;
using takt4::testing::Captured;
using takt4::testing::ipv4;
using takt4::testing::loadCapture;
namespace sim = takt4::testing::prolink_sim;

namespace {

using Channel = VirtualPlayer::Channel;
constexpr std::int64_t kMs = 1000;

constexpr std::uint32_t kOwnIp = ipv4(169, 254, 50, 50);
constexpr std::uint32_t kBroadcast = ipv4(169, 254, 255, 255);
const Mac kOwnMac{0x02, 0x74, 0x61, 0x6b, 0x74, 0x34};

VirtualPlayer::Identity identity() {
    VirtualPlayer::Identity id;
    id.ip = kOwnIp;
    id.broadcast = kBroadcast;
    id.mac = kOwnMac;
    return id;
}

/// A device on the network, as the simulator makes it: a number and an address.
struct Peer {
    std::uint8_t number;
    std::uint32_t ip;
    Mac mac;
};

Peer player(std::uint8_t number, unsigned host) {
    return {number, ipv4(169, 254, 1, host), Mac{0x74, 0x5e, 0x1c, 0x00, 0x00, number}};
}

struct Sent {
    std::int64_t at;
    VirtualPlayer::Datagram datagram;
    AnnouncePacket packet;
};

/// A virtual player on a clock the test moves, and everything it sends, with when.
struct Rig {
    VirtualPlayer player;
    std::int64_t now = 0;
    std::vector<Sent> sent;

    Rig() = default;
    explicit Rig(VirtualPlayer::Options options) : player(std::move(options)) {}

    void collect() {
        for (const VirtualPlayer::Datagram& d : player.outbox()) {
            Sent s{now, d, {}};
            REQUIRE(parseAnnounce(d.packet.view(), s.packet));
            sent.push_back(s);
        }
        player.outbox().clear();
    }
    /// Time moves on to `until`, a millisecond at a time.
    void runTo(std::int64_t until) {
        while (now < until) {
            now += kMs;
            player.advance(now);
            collect();
        }
    }
    void feed(Channel channel, const std::vector<std::uint8_t>& bytes, std::uint32_t from) {
        player.receive(channel, bytes, from, now);
        player.advance(now);
        collect();
    }
    void keepAlive(const Peer& peer) {
        feed(Channel::Announce, sim::keepAlive(peer.number, peer.ip, peer.mac), peer.ip);
    }
    std::vector<Sent> sentOf(Announce type) const {
        std::vector<Sent> out;
        for (const Sent& s : sent) {
            if (s.packet.type == type) {
                out.push_back(s);
            }
        }
        return out;
    }
    /// Joined, as player `number` is expected to be, with `peers` on the network first.
    void joinWith(const std::vector<Peer>& peers) {
        player.listen(now);
        for (const Peer& p : peers) {
            keepAlive(p);
        }
        player.join(identity(), now);
        runTo(now + 10'000 * kMs);
        REQUIRE(player.joined());
    }
};

/// Plays a capture into a player at the moments it was sent, moving its clock along.
void replay(VirtualPlayer& player, const std::vector<Captured>& datagrams,
            const std::function<void(const Captured&)>& after = {}) {
    for (const Captured& c : datagrams) {
        player.receive(c.channel(), c.bytes, c.from, c.micros());
        player.advance(c.micros());
        if (after) {
            after(c);
        }
    }
}

} // namespace

TEST_CASE("listening, the virtual player sends nothing whatever the network says, and lists the "
          "devices it hears",
          "[prolink]") {
    for (const char* file :
         {"joins-occupied.txt", "load-and-play.txt", "nexus-and-djm.txt", "master-handoff.txt"}) {
        INFO(file);
        VirtualPlayer player;
        player.listen(0);
        replay(player, loadCapture(file),
               [&](const Captured&) { REQUIRE(player.outbox().empty()); });
        CHECK(player.phase() == VirtualPlayer::Phase::Listening);
        CHECK(player.number() == 0);
        CHECK_FALSE(player.devices().empty());
    }
    VirtualPlayer player;
    player.listen(0);
    replay(player, loadCapture("load-and-play.txt"));
    REQUIRE(player.devices().size() == 2);
    for (const VirtualPlayer::Device& d : player.devices()) {
        CHECK((d.number == 1 || d.number == 2));
        CHECK(nameText(d.name) == "CDJ-2000nexus");
        CHECK(d.kind == DeviceKind::Player);
        CHECK(d.ip == (d.number == 1 ? ipv4(169, 254, 103, 172) : ipv4(169, 254, 202, 84)));
    }
}

TEST_CASE("joining, it watches four seconds from when it began listening, then says hello and "
          "claims player 7 in three stages 300 ms apart, then keeps alive every second and a half",
          "[prolink]") {
    Rig rig;
    rig.player.listen(0);
    const Peer one = player(1, 11);
    const Peer two = player(2, 12);
    rig.keepAlive(one);
    rig.keepAlive(two);
    rig.runTo(1000 * kMs);
    rig.player.join(identity(), rig.now);
    CHECK(rig.player.phase() == VirtualPlayer::Phase::Watching);
    rig.runTo(3999 * kMs);
    CHECK(rig.sent.empty());
    // Keep the two players alive while it claims.
    for (std::int64_t t = 2000; t <= 12000; t += 2000) {
        rig.runTo(t * kMs);
        rig.keepAlive(one);
        rig.keepAlive(two);
    }
    rig.runTo(12'000 * kMs);
    REQUIRE(rig.player.joined());
    CHECK(rig.player.number() == 7);

    const std::vector<Announce> claim{
        Announce::Hello,       Announce::Hello,      Announce::Hello,       Announce::FirstClaim,
        Announce::FirstClaim,  Announce::FirstClaim, Announce::SecondClaim, Announce::SecondClaim,
        Announce::SecondClaim, Announce::FinalClaim, Announce::FinalClaim,  Announce::FinalClaim};
    REQUIRE(rig.sent.size() > claim.size() + 2);
    for (std::size_t i = 0; i < claim.size(); ++i) {
        INFO("packet " << i);
        const Sent& s = rig.sent[i];
        CHECK(s.packet.type == claim[i]);
        CHECK(s.at == 4000 * kMs + static_cast<std::int64_t>(i) * 300 * kMs);
        CHECK(s.datagram.broadcast);
        CHECK(s.datagram.to == kBroadcast);
        CHECK(nameText(s.packet.name) == "takt4");
        if (claim[i] != Announce::Hello) {
            CHECK(s.packet.counter == i % 3 + 1);
        }
        if (claim[i] == Announce::FirstClaim) {
            CHECK(s.packet.mac == kOwnMac);
        }
        if (claim[i] == Announce::SecondClaim) {
            CHECK(s.packet.device == 7);
            CHECK(s.packet.ip == kOwnIp);
            CHECK(s.packet.mac == kOwnMac);
            CHECK(s.packet.autoAssign);
        }
        if (claim[i] == Announce::FinalClaim) {
            CHECK(s.packet.device == 7);
        }
    }
    const std::vector<Sent> keepAlives = rig.sentOf(Announce::KeepAlive);
    REQUIRE(keepAlives.size() >= 3);
    // The first a step after the last of the claim, as a player's is; then every 1.5 s.
    CHECK(keepAlives[0].at == 7600 * kMs);
    for (std::size_t i = 1; i < keepAlives.size(); ++i) {
        CHECK(keepAlives[i].at - keepAlives[i - 1].at == 1500 * kMs);
    }
    for (const Sent& k : keepAlives) {
        CHECK(k.packet.device == 7);
        CHECK(k.packet.ip == kOwnIp);
        CHECK(k.packet.mac == kOwnMac);
        CHECK(k.packet.peers == 3); // two players and itself
        CHECK(k.packet.kind == DeviceKind::Player);
    }
}

TEST_CASE("asked to join long after it began listening, it claims at once", "[prolink]") {
    Rig rig;
    rig.player.listen(0);
    rig.keepAlive(player(1, 11));
    rig.runTo(60'000 * kMs);
    rig.keepAlive(player(1, 11));
    rig.player.join(identity(), rig.now);
    rig.runTo(rig.now + 1 * kMs);
    REQUIRE_FALSE(rig.sent.empty());
    CHECK(rig.sent[0].packet.type == Announce::Hello);
    CHECK(rig.sent[0].at == 60'001 * kMs);
}

TEST_CASE("it claims the first number nothing is using, or is being claimed by", "[prolink]") {
    Rig rig;
    rig.player.listen(0);
    rig.keepAlive(player(7, 17)); // another virtual player
    rig.keepAlive(player(8, 18)); // and another
    rig.feed(Channel::Announce, sim::secondClaim(9, ipv4(169, 254, 1, 19), Mac{}),
             ipv4(169, 254, 1, 19));
    rig.player.join(identity(), rig.now);
    for (std::int64_t t = 2000; t <= 10000; t += 2000) {
        rig.runTo(t * kMs);
        rig.keepAlive(player(7, 17));
        rig.keepAlive(player(8, 18));
    }
    REQUIRE(rig.player.joined());
    CHECK(rig.player.number() == 10);
    for (const Sent& s : rig.sentOf(Announce::SecondClaim)) {
        CHECK(s.packet.device == 10);
    }
}

TEST_CASE("a conflict costs the number being claimed, and the claim starts again with the next",
          "[prolink]") {
    const std::uint32_t holder = ipv4(169, 254, 1, 99);
    for (const VirtualPlayer::Phase when :
         {VirtualPlayer::Phase::SecondClaim, VirtualPlayer::Phase::FinalClaim}) {
        INFO("in phase " << int(when));
        Rig rig;
        rig.player.listen(0);
        rig.keepAlive(player(1, 11));
        rig.player.join(identity(), rig.now);
        while (rig.player.phase() != when) {
            rig.runTo(rig.now + kMs);
        }
        const std::size_t before = rig.sent.size();
        rig.feed(Channel::Announce, sim::conflict(7, holder), holder);
        CHECK(rig.player.numbersLost() == 1);
        CHECK(rig.player.phase() == VirtualPlayer::Phase::FirstClaim);
        rig.runTo(rig.now + 10'000 * kMs);
        REQUIRE(rig.player.joined());
        CHECK(rig.player.number() == 8);
        // From the conflict on: three first-stage claims, then every second- and final-stage one
        // is for 8.
        std::vector<Sent> after(rig.sent.begin() + static_cast<std::ptrdiff_t>(before),
                                rig.sent.end());
        REQUIRE(after.size() >= 9);
        for (int i = 0; i < 3; ++i) {
            CHECK(after[static_cast<std::size_t>(i)].packet.type == Announce::FirstClaim);
        }
        for (const Sent& s : after) {
            if (s.packet.type == Announce::SecondClaim || s.packet.type == Announce::FinalClaim ||
                s.packet.type == Announce::KeepAlive) {
                CHECK(s.packet.device == 8);
            }
        }
    }
}

TEST_CASE("a player already on the network cuts the final stage short with assignment finished, "
          "as the real players' capture shows, and the keep-alives start at once",
          "[prolink]") {
    // The real "assignment finished" a CDJ-2000nexus sent the player joining beside it.
    std::vector<std::uint8_t> finished;
    for (const Captured& c : loadCapture("joins-occupied.txt")) {
        if (c.type() == 0x05) {
            finished = c.bytes;
        }
    }
    REQUIRE_FALSE(finished.empty());
    Rig rig;
    rig.player.listen(0);
    rig.keepAlive(player(2, 12));
    rig.player.join(identity(), rig.now);
    while (rig.sentOf(Announce::FinalClaim).empty()) {
        rig.runTo(rig.now + kMs);
    }
    rig.runTo(rig.now + 2 * kMs);
    rig.feed(Channel::Announce, finished, ipv4(169, 254, 202, 84));
    CHECK(rig.player.joined());
    rig.runTo(rig.now + 5000 * kMs);
    CHECK(rig.sentOf(Announce::FinalClaim).size() == 1);
    const std::vector<Sent> keepAlives = rig.sentOf(Announce::KeepAlive);
    REQUIRE_FALSE(keepAlives.empty());
    CHECK(keepAlives[0].at == rig.sentOf(Announce::FinalClaim)[0].at + 2 * kMs);
    CHECK(keepAlives[0].packet.device == 7);
}

TEST_CASE("a mixer that says it will assign the number is asked for one, and its number is "
          "taken",
          "[prolink]") {
    const std::uint32_t mixer = ipv4(169, 254, 1, 33);
    const Mac mixerMac{0x00, 0xe0, 0x36, 0x00, 0x00, 0x21};
    Rig rig;
    rig.player.listen(0);
    rig.feed(Channel::Announce, sim::keepAlive(33, mixer, mixerMac, 2), mixer);
    rig.player.join(identity(), rig.now);
    while (rig.sentOf(Announce::FirstClaim).empty()) {
        rig.runTo(rig.now + kMs);
    }
    rig.feed(Channel::Announce, sim::assignmentIntention(mixer, mixerMac), mixer);
    CHECK(rig.player.phase() == VirtualPlayer::Phase::AwaitingMixer);
    REQUIRE_FALSE(rig.sent.empty());
    const Sent& request = rig.sent.back();
    CHECK_FALSE(request.datagram.broadcast);
    CHECK(request.datagram.to == mixer);
    CHECK(request.packet.type == Announce::SecondClaim);
    CHECK(request.packet.fromMixerPort);
    CHECK(request.packet.ip == kOwnIp);
    CHECK(request.packet.mac == kOwnMac);

    // An assignment from anything but that mixer is not the mixer's.
    rig.feed(Channel::Announce, sim::assignment(5), ipv4(169, 254, 1, 77));
    CHECK(rig.player.phase() == VirtualPlayer::Phase::AwaitingMixer);

    rig.feed(Channel::Announce, sim::assignment(3), mixer);
    CHECK(rig.player.number() == 3);
    rig.runTo(rig.now + 1 * kMs);
    REQUIRE_FALSE(rig.sentOf(Announce::FinalClaim).empty());
    CHECK(rig.sentOf(Announce::FinalClaim)[0].packet.device == 3);
    CHECK(rig.sentOf(Announce::FinalClaim)[0].packet.counter == 1);
    rig.feed(Channel::Announce, sim::assignmentFinished(33), mixer);
    CHECK(rig.player.joined());
    rig.runTo(rig.now + 4000 * kMs);
    for (const Sent& k : rig.sentOf(Announce::KeepAlive)) {
        CHECK(k.packet.device == 3);
    }
    CHECK(rig.sentOf(Announce::FinalClaim).size() == 1);
}

TEST_CASE("a mixer that says it will assign a number and does not is waited for a second, then "
          "the number is claimed as anywhere else",
          "[prolink]") {
    const std::uint32_t mixer = ipv4(169, 254, 1, 33);
    Rig rig;
    rig.player.listen(0);
    rig.keepAlive(player(1, 11));
    rig.player.join(identity(), rig.now);
    while (rig.sentOf(Announce::FirstClaim).empty()) {
        rig.runTo(rig.now + kMs);
    }
    rig.feed(Channel::Announce, sim::assignmentIntention(mixer, Mac{}), mixer);
    const std::int64_t asked = rig.now;
    rig.runTo(rig.now + 999 * kMs);
    CHECK(rig.player.phase() == VirtualPlayer::Phase::AwaitingMixer);
    rig.runTo(rig.now + 10'000 * kMs);
    REQUIRE(rig.player.joined());
    CHECK(rig.player.number() == 7);
    const std::vector<Sent> second = rig.sentOf(Announce::SecondClaim);
    // The request to the mixer, then the claim for 7, from a second after it was asked.
    REQUIRE(second.size() == 4);
    CHECK(second[1].at == asked + 1000 * kMs);
    CHECK(second[1].packet.device == 7);
}

TEST_CASE("joined, it answers a device claiming its number with a conflict, sent to that device",
          "[prolink]") {
    Rig rig;
    rig.joinWith({player(1, 11)});
    REQUIRE(rig.player.number() == 7);
    for (const std::uint32_t claimer : {ipv4(169, 254, 1, 2), ipv4(169, 254, 200, 2)}) {
        for (const bool second : {true, false}) {
            const std::size_t before = rig.sent.size();
            rig.feed(Channel::Announce,
                     second ? sim::secondClaim(7, claimer, Mac{}) : sim::finalClaim(7), claimer);
            REQUIRE(rig.sent.size() == before + 1);
            const Sent& answer = rig.sent.back();
            CHECK(answer.packet.type == Announce::Conflict);
            CHECK_FALSE(answer.datagram.broadcast);
            CHECK(answer.datagram.to == claimer);
            CHECK(answer.packet.device == 7);
            CHECK(answer.packet.ip == kOwnIp);
            CHECK(rig.player.joined());
            CHECK(rig.player.number() == 7);
        }
    }
    // Claims for another number are none of its business.
    const std::size_t before = rig.sent.size();
    rig.feed(Channel::Announce, sim::secondClaim(8, ipv4(169, 254, 1, 2), Mac{}),
             ipv4(169, 254, 1, 2));
    CHECK(rig.sent.size() == before);
}

TEST_CASE("two holding one number part on their addresses: the lower keeps it", "[prolink]") {
    SECTION("a keep-alive for its number from a higher address is answered with a conflict") {
        Rig rig;
        rig.joinWith({player(1, 11)});
        const std::uint32_t higher = ipv4(169, 254, 99, 1);
        rig.feed(Channel::Announce, sim::keepAlive(7, higher, Mac{}), higher);
        CHECK(rig.sent.back().packet.type == Announce::Conflict);
        CHECK(rig.player.joined());
        CHECK(rig.player.number() == 7);
    }
    SECTION("one from a lower address keeps it, and this one claims another") {
        Rig rig;
        rig.joinWith({player(1, 11)});
        const std::uint32_t lower = ipv4(169, 254, 1, 1);
        rig.feed(Channel::Announce, sim::keepAlive(7, lower, Mac{}), lower);
        CHECK(rig.player.phase() == VirtualPlayer::Phase::FirstClaim);
        rig.runTo(rig.now + 10'000 * kMs);
        CHECK(rig.player.joined());
        CHECK(rig.player.number() == 8);
    }
    SECTION("claiming at the same moment as another, the higher gives way") {
        for (const bool otherLower : {true, false}) {
            Rig rig;
            rig.player.listen(0);
            rig.keepAlive(player(1, 11));
            rig.player.join(identity(), rig.now);
            while (rig.player.phase() != VirtualPlayer::Phase::SecondClaim) {
                rig.runTo(rig.now + kMs);
            }
            const std::uint32_t other = otherLower ? ipv4(169, 254, 1, 1) : ipv4(169, 254, 99, 1);
            rig.feed(Channel::Announce, sim::secondClaim(7, other, Mac{}), other);
            rig.runTo(rig.now + 10'000 * kMs);
            REQUIRE(rig.player.joined());
            CHECK(rig.player.number() == (otherLower ? 8 : 7));
        }
    }
}

TEST_CASE("its own packets coming back round are not a device and not a rival", "[prolink]") {
    Rig rig;
    rig.joinWith({player(1, 11)});
    const std::size_t devices = rig.player.devices().size();
    const std::size_t before = rig.sent.size();
    rig.feed(Channel::Announce, sim::keepAlive(7, kOwnIp, kOwnMac), kOwnIp);
    rig.feed(Channel::Announce, sim::secondClaim(7, kOwnIp, kOwnMac), kOwnIp);
    CHECK(rig.sent.size() == before);
    CHECK(rig.player.devices().size() == devices);
    CHECK(rig.player.joined());
}

TEST_CASE("a conflict while joined costs the number, and another is claimed", "[prolink]") {
    Rig rig;
    rig.joinWith({player(1, 11)});
    const std::uint32_t holder = ipv4(169, 254, 1, 70);
    rig.feed(Channel::Announce, sim::conflict(7, holder), holder);
    CHECK(rig.player.phase() == VirtualPlayer::Phase::FirstClaim);
    rig.runTo(rig.now + 10'000 * kMs);
    CHECK(rig.player.joined());
    CHECK(rig.player.number() == 8);
    CHECK(rig.player.numbersLost() == 1);
}

TEST_CASE("with every number from 7 to 15 taken it says so, and claims one when one comes free",
          "[prolink]") {
    Rig rig;
    rig.player.listen(0);
    const auto keepAll = [&](bool withNine) {
        for (std::uint8_t n = 7; n <= 15; ++n) {
            if (n != 9 || withNine) {
                rig.keepAlive(player(n, n));
            }
        }
    };
    keepAll(true);
    rig.player.join(identity(), rig.now);
    for (std::int64_t t = 1000; t <= 8000; t += 1000) {
        rig.runTo(t * kMs);
        keepAll(true);
    }
    CHECK(rig.player.phase() == VirtualPlayer::Phase::NoNumber);
    CHECK(rig.sentOf(Announce::SecondClaim).empty());
    // Player 9 goes quiet: gone after five seconds, and its number claimed.
    for (std::int64_t t = 9000; t <= 30'000; t += 1000) {
        rig.runTo(t * kMs);
        keepAll(false);
    }
    REQUIRE(rig.player.joined());
    CHECK(rig.player.number() == 9);
}

TEST_CASE("a device not heard from for five seconds is gone from the list", "[prolink]") {
    Rig rig;
    rig.player.listen(0);
    rig.keepAlive(player(1, 11));
    rig.runTo(4999 * kMs);
    CHECK(rig.player.devices().size() == 1);
    rig.runTo(5001 * kMs);
    CHECK(rig.player.devices().empty());
}

TEST_CASE("leaving stops the keep-alives, and it sends nothing more", "[prolink]") {
    Rig rig;
    rig.joinWith({player(1, 11)});
    rig.player.leave();
    CHECK(rig.player.phase() == VirtualPlayer::Phase::Listening);
    const std::size_t before = rig.sent.size();
    rig.runTo(rig.now + 20'000 * kMs);
    CHECK(rig.sent.size() == before);
    // And joins again from the start, the next time it is asked.
    rig.keepAlive(player(1, 11));
    rig.player.join(identity(), rig.now);
    rig.runTo(rig.now + 10'000 * kMs);
    CHECK(rig.player.joined());
}

TEST_CASE("the tempo master is the player or mixer whose status says so, and during a hand-off "
          "the one that took the role last, as the DJM-2000nexus capture hands it round",
          "[prolink]") {
    VirtualPlayer player;
    player.listen(0);
    std::vector<std::uint8_t> masters;
    replay(player, loadCapture("master-handoff.txt"), [&](const Captured& c) {
        const std::uint8_t now = player.master(c.micros());
        if (masters.empty() || masters.back() != now) {
            masters.push_back(now);
        }
    });
    // Nobody until player 3's first status (the capture opens on the mixer's), then each
    // hand-off in turn.
    const std::vector<std::uint8_t> expected{0, 3, 2, 3, 2, 33, 3};
    CHECK(masters == expected);
    // Player 3 is master here with a track that cannot lead (status Mm 02).
    bool sawMeaningless = false;
    for (const VirtualPlayer::Device& d : player.devices()) {
        if (d.number == 3 && d.status.masterMeaningful == 2) {
            sawMeaningless = true;
        }
    }
    CHECK(sawMeaningless);
}

TEST_CASE("a status gone stale says nothing about who is master", "[prolink]") {
    Rig rig;
    rig.player.listen(0);
    sim::PlayerState state;
    state.flags = sim::kPlaying | sim::kMaster;
    rig.feed(Channel::Status, sim::playerStatus(2, state), ipv4(169, 254, 1, 12));
    CHECK(rig.player.master(rig.now) == 2);
    rig.runTo(1500 * kMs);
    CHECK(rig.player.master(rig.now) == 2);
    rig.runTo(1501 * kMs);
    CHECK(rig.player.master(rig.now) == 0);
}

TEST_CASE("the master's beats are the ones heard, every one at the moment it came, and a cue "
          "press's are dropped, as two CDJ-2000nexus players send them",
          "[prolink]") {
    const std::vector<Captured> datagrams = loadCapture("load-and-play.txt");
    VirtualPlayer player;
    player.listen(0);
    std::vector<VirtualPlayer::Heard> heard;
    std::vector<std::int64_t> cueBeatTimes;
    std::uint8_t lastPlayMode = 0;
    replay(player, datagrams, [&](const Captured& c) {
        if (c.port == 50002 && c.bytes[0x21] == 1) {
            lastPlayMode = c.bytes[0x7b];
        }
        if (c.port == 50001 && lastPlayMode == 0x07) {
            cueBeatTimes.push_back(c.micros());
        }
        for (const VirtualPlayer::Heard& h : player.heard()) {
            heard.push_back(h);
            CHECK(h.at == c.micros()); // stamped with when it came, nothing else
        }
        player.heard().clear();
    });
    // Four beats came while the cue button was held over the paused track; every other is heard —
    // the very first too, which a CDJ-2000nexus sends 50 ms before its status says it is master.
    CHECK(cueBeatTimes.size() == 4);
    CHECK(player.cueBeatsDropped() == 4);
    CHECK(heard.size() == 93 - 4);
    for (const VirtualPlayer::Heard& h : heard) {
        CHECK(h.device == 1);
        CHECK(h.bpm > 127.98);
        CHECK(h.bpm < 127.99);
        CHECK(h.beatInBar >= 1);
        CHECK(h.beatInBar <= 4);
        for (const std::int64_t cue : cueBeatTimes) {
            CHECK(h.at != cue);
        }
    }
}

TEST_CASE("following one player, only that player's beats are heard, master or not", "[prolink]") {
    Rig rig;
    rig.player.listen(0);
    rig.player.setFollow(3);
    sim::PlayerState master;
    master.flags = sim::kPlaying | sim::kMaster;
    master.playMode = sim::kModePlaying;
    sim::PlayerState other = master;
    other.flags = sim::kPlaying | sim::kSynced;
    for (int i = 0; i < 8; ++i) {
        rig.feed(Channel::Status, sim::playerStatus(2, master), ipv4(169, 254, 1, 12));
        rig.feed(Channel::Status, sim::playerStatus(3, other), ipv4(169, 254, 1, 13));
        rig.feed(Channel::Beat, sim::beat(2, 128.0, static_cast<std::uint8_t>(i % 4 + 1)),
                 ipv4(169, 254, 1, 12));
        rig.feed(Channel::Beat, sim::beat(3, 128.0, static_cast<std::uint8_t>((i + 2) % 4 + 1)),
                 ipv4(169, 254, 1, 13));
        rig.runTo(rig.now + 469 * kMs);
    }
    CHECK(rig.player.master(rig.now) == 2);
    CHECK(rig.player.followed(rig.now) == 3);
    REQUIRE(rig.player.heard().size() == 8);
    for (const VirtualPlayer::Heard& h : rig.player.heard()) {
        CHECK(h.device == 3);
    }
    // Back to the master: its beats from here.
    rig.player.heard().clear();
    rig.player.setFollow(VirtualPlayer::kFollowMaster);
    rig.feed(Channel::Beat, sim::beat(2, 128.0, 1), ipv4(169, 254, 1, 12));
    rig.feed(Channel::Beat, sim::beat(3, 128.0, 3), ipv4(169, 254, 1, 13));
    REQUIRE(rig.player.heard().size() == 1);
    CHECK(rig.player.heard()[0].device == 2);
}

TEST_CASE("with no master and one player playing, its beats are followed; with two playing, "
          "nobody's",
          "[prolink]") {
    Rig rig;
    rig.player.listen(0);
    sim::PlayerState playing;
    playing.flags = sim::kPlaying;
    playing.playMode = sim::kModePlaying;
    rig.feed(Channel::Status, sim::playerStatus(2, playing), ipv4(169, 254, 1, 12));
    rig.feed(Channel::Beat, sim::beat(2, 128.0, 1), ipv4(169, 254, 1, 12));
    CHECK(rig.player.master(rig.now) == 0);
    CHECK(rig.player.leader(rig.now) == 2);
    CHECK(rig.player.heard().size() == 1);
    rig.feed(Channel::Status, sim::playerStatus(3, playing), ipv4(169, 254, 1, 13));
    CHECK(rig.player.leader(rig.now) == 0);
    rig.feed(Channel::Beat, sim::beat(2, 128.0, 2), ipv4(169, 254, 1, 12));
    CHECK(rig.player.heard().size() == 1);
    // A mixer playing is not a player playing.
    Rig mixer;
    mixer.player.listen(0);
    mixer.feed(Channel::Status, sim::mixerStatus(33, false), ipv4(169, 254, 1, 33));
    CHECK(mixer.player.leader(mixer.now) == 0);
}

TEST_CASE("during a hand-off, while the old master and the new both say they are master, the new "
          "one is followed, and the old one saying so again does not take it back",
          "[prolink]") {
    // As the DJM-2000nexus capture hands it round: the master names its successor in Mh, the
    // successor says it is master, and some 50 ms later the old one lets go.
    Rig rig;
    rig.player.listen(0);
    const std::uint32_t two = ipv4(169, 254, 1, 12);
    const std::uint32_t three = ipv4(169, 254, 1, 13);
    sim::PlayerState master;
    master.flags = sim::kPlaying | sim::kMaster;
    master.playMode = sim::kModePlaying;
    sim::PlayerState synced = master;
    synced.flags = sim::kPlaying | sim::kSynced;
    rig.feed(Channel::Status, sim::playerStatus(3, master), three);
    rig.feed(Channel::Status, sim::playerStatus(2, synced), two);
    CHECK(rig.player.master(rig.now) == 3);

    rig.runTo(100 * kMs);
    sim::PlayerState yielding = master;
    yielding.handingTo = 2;
    rig.feed(Channel::Status, sim::playerStatus(3, yielding), three);
    CHECK(rig.player.master(rig.now) == 3); // named, not yet taken
    rig.runTo(110 * kMs);
    sim::PlayerState taking = master;
    taking.flags = sim::kPlaying | sim::kMaster | sim::kSynced;
    rig.feed(Channel::Status, sim::playerStatus(2, taking), two);
    CHECK(rig.player.master(rig.now) == 2); // both say so: the new one
    rig.runTo(120 * kMs);
    rig.feed(Channel::Beat, sim::beat(2, 128.0, 1), two);
    rig.feed(Channel::Beat, sim::beat(3, 128.0, 3), three);
    REQUIRE(rig.player.heard().size() == 1);
    CHECK(rig.player.heard()[0].device == 2);
    rig.runTo(150 * kMs);
    rig.feed(Channel::Status, sim::playerStatus(3, yielding), three);
    CHECK(rig.player.master(rig.now) == 2); // the old one again, still letting go
    rig.runTo(160 * kMs);
    rig.feed(Channel::Status, sim::playerStatus(3, synced), three);
    CHECK(rig.player.master(rig.now) == 2);
}
