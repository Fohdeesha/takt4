#include "core/output/link_peers.hpp"
#include "core/output/link_session.hpp"

#include "support/loopback_receiver.hpp"
#include "support/this_machine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::output::LinkAnnouncement;
using takt4::output::LinkPeer;
using takt4::output::LinkPeerWatch;

// SHOW PEERS: what each Link peer announces on the network, read off the multicast group Link
// finds its peers on. The datagrams here are built byte for byte the way Link's own
// `discovery::v1::aliveMessage` writes them (third_party/link, `discovery/v1/Messages.hpp` and
// the payload entries), and the last test holds the parser to what a real Link session sends.

namespace {

using Bytes = std::vector<std::uint8_t>;
using Id = LinkAnnouncement::Id;

void put32(Bytes& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void put64(Bytes& out, std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
}

Id id(std::uint8_t last) {
    return Id{1, 2, 3, 4, 5, 6, 7, last};
}

/// One announcement as Link writes it: header, then 'tmln', 'sess' and 'stst'.
Bytes announcement(std::uint8_t type, Id node, double bpm, Id session,
                   std::optional<bool> playing = std::nullopt, std::uint8_t ttl = 5) {
    Bytes out{'_', 'a', 's', 'd', 'p', '_', 'v', 1, type, ttl, 0, 0};
    out.insert(out.end(), node.begin(), node.end());
    if (type == 3) {
        return out; // a bye-bye carries nothing
    }
    put32(out, 0x746d6c6e); // 'tmln'
    put32(out, 24);
    put64(out, static_cast<std::int64_t>(60.0e6 / bpm)); // microseconds a beat
    put64(out, 1234567);                                 // the beat origin, in micro-beats
    put64(out, 987654321);                               // the time origin
    put32(out, 0x73657373);                              // 'sess'
    put32(out, 8);
    out.insert(out.end(), session.begin(), session.end());
    put32(out, 0x73747374); // 'stst'
    put32(out, 17);
    out.push_back(playing.value_or(false) ? 1 : 0);
    put64(out, 0);
    put64(out, playing ? 5555 : 0); // never stamped: nobody shares start and stop
    return out;
}

} // namespace

TEST_CASE("a Link announcement is read as Link writes it", "[output][link]") {
    LinkAnnouncement read;
    REQUIRE(takt4::output::parseLinkAnnouncement(announcement(1, id(9), 128.0, id(1), true), read));
    CHECK(read.type == LinkAnnouncement::Type::Alive);
    CHECK(read.ttl == 5);
    CHECK(read.node == id(9));
    REQUIRE(read.bpm.has_value());
    CHECK_THAT(*read.bpm, WithinAbs(128.0, 0.001));
    CHECK(read.session == std::optional<Id>(id(1)));
    CHECK(read.playing == std::optional<bool>(true));

    SECTION("a session nobody shares start and stop in says nothing about playing") {
        REQUIRE(takt4::output::parseLinkAnnouncement(announcement(1, id(9), 90.0, id(1)), read));
        CHECK_FALSE(read.playing.has_value());
    }

    SECTION("an entry it does not know is stepped over") {
        Bytes bytes = announcement(1, id(9), 128.0, id(1));
        Bytes extra{};
        put32(extra, 0x6d657034); // 'mep4', an endpoint: four bytes of address and a port
        put32(extra, 6);
        extra.insert(extra.end(), {192, 168, 1, 20, 0x51, 0x50});
        bytes.insert(bytes.begin() + 20, extra.begin(), extra.end());
        REQUIRE(takt4::output::parseLinkAnnouncement(bytes, read));
        CHECK_THAT(*read.bpm, WithinAbs(128.0, 0.001));
    }

    SECTION("anything else sent to that port is refused, never half read") {
        const Bytes good = announcement(1, id(9), 128.0, id(1));
        Bytes wrongProtocol = good;
        wrongProtocol[7] = 2;
        Bytes wrongType = good;
        wrongType[8] = 7;
        Bytes overrun = good;
        overrun[27] = 200; // the first entry claims more bytes than there are
        Bytes halfHeader = good;
        halfHeader.resize(good.size() + 3, 0); // three bytes of a next entry
        for (const Bytes& bad : {wrongProtocol, wrongType, overrun, halfHeader,
                                 Bytes(good.begin(), good.begin() + 15), Bytes{}}) {
            CHECK_FALSE(takt4::output::parseLinkAnnouncement(bad, read));
        }
    }

    // The audit of 2026-09-25, coverage gap 12: the three edges nothing tried.
    const auto header = [](Id node) {
        Bytes out{'_', 'a', 's', 'd', 'p', '_', 'v', 1, 1, 5, 0, 0};
        out.insert(out.end(), node.begin(), node.end());
        return out;
    };

    SECTION("an entry Link knows, at a size Link never writes, is stepped over and not half read") {
        Bytes bytes = header(id(9));
        put32(bytes, 0x746d6c6e); // 'tmln', sixteen bytes rather than twenty-four
        put32(bytes, 16);
        put64(bytes, static_cast<std::int64_t>(60.0e6 / 128.0));
        put64(bytes, 1234567);
        put32(bytes, 0x73657373); // 'sess', four rather than eight
        put32(bytes, 4);
        bytes.insert(bytes.end(), {1, 2, 3, 4});
        put32(bytes, 0x73747374); // 'stst', nine rather than seventeen
        put32(bytes, 9);
        bytes.push_back(1);
        put64(bytes, 5555);
        // A real 'sess' after them, which is still read: the wrong-sized ones were stepped over
        // by the size they gave, not by the size they should have had.
        put32(bytes, 0x73657373);
        put32(bytes, 8);
        const Id session = id(4);
        bytes.insert(bytes.end(), session.begin(), session.end());
        REQUIRE(takt4::output::parseLinkAnnouncement(bytes, read));
        CHECK_FALSE(read.bpm.has_value());
        CHECK_FALSE(read.playing.has_value());
        CHECK(read.session == std::optional<Id>(id(4)));
    }

    SECTION("a beat that lasts no time, or less, is no tempo") {
        for (const std::int64_t micros : {std::int64_t{0}, std::int64_t{-468750}}) {
            INFO(micros << " microseconds a beat");
            Bytes bytes = header(id(9));
            put32(bytes, 0x746d6c6e);
            put32(bytes, 24);
            put64(bytes, micros);
            put64(bytes, 0);
            put64(bytes, 0);
            REQUIRE(takt4::output::parseLinkAnnouncement(bytes, read));
            CHECK_FALSE(read.bpm.has_value());
        }
    }

    SECTION("a datagram longer than Link ever sends is refused, and one at the limit is read") {
        // Link's `kMaxMessageSize` is 512. An entry nobody knows pads the announcement out to it.
        const auto padded = [&](std::size_t total) {
            Bytes bytes = announcement(1, id(9), 128.0, id(1));
            REQUIRE(bytes.size() + 8 <= total);
            put32(bytes, 0x70616464); // 'padd'
            put32(bytes, static_cast<std::uint32_t>(total - bytes.size() - 4));
            bytes.resize(total, 0);
            return bytes;
        };
        CHECK(takt4::output::parseLinkAnnouncement(padded(512), read));
        CHECK_THAT(*read.bpm, WithinAbs(128.0, 0.001));
        CHECK_FALSE(takt4::output::parseLinkAnnouncement(padded(513), read));
    }
}

TEST_CASE("a peer is this process only when it sends from this machine as well as this port",
          "[output][link]") {
    // The audit's L19: this process's own Link was told apart by the port its announcements
    // left from alone, so a peer on another machine that happened to send from a port number
    // one of takt4's sockets has here was hidden from the list. Asked of the operating system,
    // with a port this process really holds.
    takt4::testing::LoopbackReceiver socket;
    const std::uint16_t ours = socket.port();
    REQUIRE(takt4::output::portOwnedHere(ours));
    CHECK(takt4::output::addressIsHere("127.0.0.1"));
    CHECK_FALSE(takt4::output::addressIsHere("203.0.113.9")); // TEST-NET-3: never a machine's
    CHECK(takt4::output::sentFromHere("127.0.0.1", ours));
    CHECK_FALSE(takt4::output::sentFromHere("203.0.113.9", ours));

    LinkPeerWatch watch; // the operating system's answer
    watch.take(announcement(1, id(7), 128.0, id(7)), "203.0.113.9", ours, 0.0);
    watch.take(announcement(1, id(8), 128.0, id(8)), "127.0.0.1", ours, 0.0);
    const std::vector<LinkPeer> peers = watch.peers();
    REQUIRE(peers.size() == 1); // the one elsewhere; the one from here is this process
    CHECK(peers[0].addresses == std::vector<std::string>{"203.0.113.9"});
}

TEST_CASE("the peers list is every Link peer but this process's own", "[output][link]") {
    // This process's Link announces from port 50000 of this machine; everything else is a peer.
    LinkPeerWatch watch([](const std::string& address, std::uint16_t port) {
        return address == "192.168.1.10" && port == 50000;
    });
    watch.take(announcement(1, id(1), 128.0, id(1)), "192.168.1.10", 50000, 0.0); // takt4 itself
    watch.take(announcement(1, id(2), 128.0, id(1), true), "192.168.1.20", 61000, 0.0);
    watch.take(announcement(1, id(3), 120.0, id(3)), "192.168.1.35", 62000, 0.1);
    watch.take(Bytes{'n', 'o', 'i', 's', 'e'}, "192.168.1.99", 9, 0.1); // not Link: ignored

    CHECK(watch.ownSession() == std::optional<Id>(id(1)));
    std::vector<LinkPeer> peers = watch.peers();
    REQUIRE(peers.size() == 2);
    CHECK(peers[0].addresses == std::vector<std::string>{"192.168.1.20"});
    CHECK_THAT(peers[0].bpm, WithinAbs(128.0, 0.001));
    CHECK(peers[0].sameSession); // following takt4's timeline
    CHECK(peers[0].playing == std::optional<bool>(true));
    CHECK(peers[1].addresses == std::vector<std::string>{"192.168.1.35"});
    CHECK_THAT(peers[1].bpm, WithinAbs(120.0, 0.001));
    CHECK_FALSE(peers[1].sameSession); // a session of its own
    CHECK_FALSE(peers[1].playing.has_value());

    SECTION("a peer on two networks is one peer at two addresses") {
        watch.take(announcement(1, id(2), 128.0, id(1), true), "10.0.0.20", 61001, 0.2);
        peers = watch.peers();
        REQUIRE(peers.size() == 2);
        CHECK(peers[0].addresses == std::vector<std::string>{"192.168.1.20", "10.0.0.20"});
    }

    SECTION("a peer that joins takt4's session is shown in it, and a new tempo is shown") {
        watch.take(announcement(1, id(3), 128.0, id(1)), "192.168.1.35", 62000, 0.3);
        peers = watch.peers();
        CHECK(peers[1].sameSession);
        CHECK_THAT(peers[1].bpm, WithinAbs(128.0, 0.001));
    }

    SECTION("a peer that says goodbye is gone at once") {
        watch.take(announcement(3, id(2), 0.0, id(0)), "192.168.1.20", 61000, 0.5);
        peers = watch.peers();
        REQUIRE(peers.size() == 1);
        CHECK(peers[0].addresses.front() == "192.168.1.35");
    }

    SECTION("a peer that falls silent is gone when its ttl runs out, and not before") {
        watch.poll(4.9);
        CHECK(watch.peers().size() == 2);
        watch.take(announcement(1, id(3), 120.0, id(3)), "192.168.1.35", 62000, 4.9); // still here
        watch.poll(5.05);
        peers = watch.peers();
        REQUIRE(peers.size() == 1);
        CHECK(peers[0].addresses.front() == "192.168.1.35");
    }

    SECTION("closing it forgets everything") {
        watch.close();
        CHECK(watch.peers().empty());
        CHECK_FALSE(watch.ownSession().has_value());
    }
}

TEST_CASE("a port this process holds is known to be its own", "[output][link]") {
    // What tells takt4's own Link apart from a peer: the socket an announcement leaves from is
    // one this process opened. Asked of a socket this test opens, and again once it is closed.
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    std::uint16_t port = 0;
    {
        takt4::testing::LoopbackReceiver socket;
        port = socket.port();
        CHECK(takt4::output::portOwnedHere(port));
    }
    CHECK_FALSE(takt4::output::portOwnedHere(port));
#else
    SKIP("no table of sockets by owner here");
#endif
}

TEST_CASE("a real Link session's announcements are read, and takt4's own is not a peer",
          "[output][link][network]") {
    // Two Link sessions in this process, announcing on the network as any peer does. Seen with
    // nothing taken for this process's own, both are listed at their tempos — so what the parser
    // reads is what Link really sends. Seen as takt4 sees them, both are this process's, and the
    // list is empty: takt4's own Link is never shown as a peer of itself.
    takt4::output::LinkSession first(123.0);
    takt4::output::LinkSession second(97.0);
    LinkPeerWatch everyone([](const std::string&, std::uint16_t) { return false; });
    LinkPeerWatch asTakt4;
    std::string problem;
    REQUIRE(everyone.open(problem));
    INFO(problem);
    REQUIRE(asTakt4.open(problem));
    first.enable(true);
    second.enable(true);

    const auto start = std::chrono::steady_clock::now();
    const auto seconds = [&start] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    // Only the sessions on this machine: the network may have Link sessions of its own, which
    // both watches hear, correctly (support/this_machine.hpp).
    const std::vector<std::string> ours = takt4::testing::thisMachinesAddresses();
    const auto here = [&ours](const std::vector<LinkPeer>& peers) {
        std::vector<LinkPeer> mine;
        for (const LinkPeer& peer : peers) {
            if (takt4::testing::onThisMachine(peer.addresses, ours)) {
                mine.push_back(peer);
            }
        }
        return mine;
    };
    while (seconds() < 10.0 && (here(everyone.peers()).size() < 2 || !asTakt4.ownSession())) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        everyone.poll(seconds());
        asTakt4.poll(seconds());
    }
    const std::vector<LinkPeer> heard = here(everyone.peers());
    REQUIRE(heard.size() >= 2);
    // Link settles two sessions on one tempo once they meet; either is what a peer announces.
    for (const LinkPeer& peer : heard) {
        INFO("a peer at " << peer.bpm << " BPM");
        CHECK((std::abs(peer.bpm - 123.0) < 0.01 || std::abs(peer.bpm - 97.0) < 0.01));
        CHECK_FALSE(peer.addresses.empty());
    }
    CHECK(asTakt4.ownSession().has_value());
    std::string listed;
    for (const LinkPeer& peer : asTakt4.peers()) {
        listed += (peer.addresses.empty() ? std::string("?") : peer.addresses.front()) + " at " +
                  std::to_string(peer.bpm) + " BPM; ";
    }
    INFO("listed as peers: " << listed);
    CHECK(here(asTakt4.peers()).empty());

    first.enable(false);
    second.enable(false);
}
