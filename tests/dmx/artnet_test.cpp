#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/artnet_publisher.hpp"
#include "core/dmx/artnet_sender.hpp"
#include "core/dmx/dmx_engine.hpp"
#include "core/dmx/fixture.hpp"
#include "core/net/resolver.hpp"

#include "support/artnet_nodes.hpp"
#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using takt4::dmx::ArtNetPublisher;
using takt4::dmx::ArtNetSender;
using takt4::dmx::DmxEngine;
using takt4::dmx::Fixture;
using takt4::dmx::PortAddress;
using takt4::dmx::Role;
using takt4::testing::LoopbackReceiver;

namespace {

std::vector<std::uint8_t> bytesOf(std::span<const std::byte> packet, std::size_t length) {
    std::vector<std::uint8_t> out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(static_cast<std::uint8_t>(packet[i]));
    }
    return out;
}

Fixture rgbAt(std::string name, PortAddress universe, std::uint16_t address) {
    Fixture fixture = takt4::dmx::fixtureFromMode(name, 1, universe, address); // RGB (3ch)
    return fixture;
}

/// Sets the patch's first fixture to this much red, at once.
void paint(DmxEngine& engine, std::uint8_t red, double now) {
    takt4::dmx::Payload payload;
    payload.kind = takt4::dmx::EffectKind::Color;
    payload.color = takt4::dmx::Color{red, 0, 0};
    engine.start(0b1, payload, now);
}

/// A publisher feeding each of `nodes` on the loopback, the i-th delayed by `delays[i]`.
void feed(ArtNetPublisher& publisher, const takt4::testing::ArtNetNodes& nodes,
          const std::vector<double>& delays) {
    for (std::size_t i = 0; i < delays.size(); ++i) {
        ArtNetPublisher::TargetConfig node;
        node.host = "127.0.0.1";
        node.port = nodes.port(i);
        node.delaySeconds = delays[i];
        node.bit = i;
        publisher.addTarget(node);
    }
}

} // namespace

// The wire format, byte for byte, against Art-Net 4 Protocol Release V1.4 §"ArtDmx packet
// definition". Field order and the two *different* endiannesses are the whole of what can go
// wrong here, and neither shows up as anything but a rig that does nothing.
TEST_CASE("an ArtDmx packet is encoded exactly as Art-Net 4 says", "[dmx][artnet]") {
    std::array<std::byte, takt4::dmx::kArtDmxMaxSize> buffer{};

    SECTION("the header, field by field") {
        const std::array<std::uint8_t, 4> levels{10, 20, 30, 40};
        const std::size_t length = takt4::dmx::writeArtDmx(buffer, 0, levels, 7);
        REQUIRE(length == takt4::dmx::kArtDmxHeaderSize + 4);
        const std::vector<std::uint8_t> bytes = bytesOf(buffer, length);

        // Field 1: "Art-Net" and a null.
        CHECK(std::string(reinterpret_cast<const char*>(bytes.data()), 7) == "Art-Net");
        CHECK(bytes[7] == 0);
        // Field 2: OpOutput, "transmitted low byte first".
        CHECK(bytes[8] == 0x00);
        CHECK(bytes[9] == 0x50);
        // Fields 3-4: protocol 0.14.
        CHECK(bytes[10] == 0);
        CHECK(bytes[11] == 14);
        // Field 5: the sequence we were given.
        CHECK(bytes[12] == 7);
        // Field 6: Physical. takt4 has no DMX inputs, so zero.
        CHECK(bytes[13] == 0);
        // Fields 7-8: SubUni then Net, low part first.
        CHECK(bytes[14] == 0);
        CHECK(bytes[15] == 0);
        // Fields 9-10: length, high byte FIRST — the opposite of the OpCode above.
        CHECK(bytes[16] == 0);
        CHECK(bytes[17] == 4);
        // Field 11: the data.
        CHECK(bytes[18] == 10);
        CHECK(bytes[19] == 20);
        CHECK(bytes[20] == 30);
        CHECK(bytes[21] == 40);
    }

    SECTION("a 15-bit Port-Address is split across SubUni and Net") {
        // 0x1234 = Net 0x12, Sub-Net 0x3, Universe 0x4.
        const std::array<std::uint8_t, 2> levels{1, 2};
        const std::size_t length = takt4::dmx::writeArtDmx(buffer, 0x1234, levels, 1);
        const std::vector<std::uint8_t> bytes = bytesOf(buffer, length);
        CHECK(bytes[14] == 0x34); // SubUni: the low byte
        CHECK(bytes[15] == 0x12); // Net: the top 7 bits
    }

    SECTION("the top bit of a Port-Address is never set on the wire") {
        // Net is seven bits. 0x7FFF is the largest legal address and its Net is 0x7F.
        const std::array<std::uint8_t, 2> levels{1, 2};
        const std::size_t length = takt4::dmx::writeArtDmx(buffer, 0x7FFF, levels, 1);
        const std::vector<std::uint8_t> bytes = bytesOf(buffer, length);
        CHECK(bytes[15] == 0x7F);
        CHECK((bytes[15] & 0x80) == 0);
    }

    SECTION("an odd channel count is rounded up, because the length must be even") {
        // "This value should be an even number in the range 2 to 512."
        const std::array<std::uint8_t, 3> levels{1, 2, 3};
        const std::size_t length = takt4::dmx::writeArtDmx(buffer, 0, levels, 1);
        REQUIRE(length == takt4::dmx::kArtDmxHeaderSize + 4);
        const std::vector<std::uint8_t> bytes = bytesOf(buffer, length);
        CHECK(bytes[17] == 4);
        CHECK(bytes[18 + 2] == 3);
        CHECK(bytes[18 + 3] == 0); // the padding channel, and it must be zero
    }

    SECTION("a full universe fits, and its length is written across two bytes") {
        const std::vector<std::uint8_t> levels(512, 255);
        const std::size_t length = takt4::dmx::writeArtDmx(buffer, 0, levels, 1);
        REQUIRE(length == takt4::dmx::kArtDmxMaxSize);
        const std::vector<std::uint8_t> bytes = bytesOf(buffer, length);
        CHECK(bytes[16] == 0x02); // 512 = 0x0200, high byte first
        CHECK(bytes[17] == 0x00);
    }

    SECTION("an empty or oversized frame is refused rather than truncated") {
        CHECK(takt4::dmx::writeArtDmx(buffer, 0, {}, 1) == 0);
        const std::vector<std::uint8_t> tooMany(513, 1);
        CHECK(takt4::dmx::writeArtDmx(buffer, 0, tooMany, 1) == 0);
    }

    SECTION("a buffer too small for the packet is refused") {
        std::array<std::byte, 20> small{};
        const std::vector<std::uint8_t> levels(512, 1);
        CHECK(takt4::dmx::writeArtDmx(small, 0, levels, 1) == 0);
    }
}

// The sequence counter is the one field with a rule that is easy to get subtly wrong: zero
// means "disabled", so it is not the number before one.
TEST_CASE("the ArtDmx sequence counter runs 1 to 255 and never lands on zero", "[dmx][artnet]") {
    std::uint8_t sequence = 0;
    sequence = takt4::dmx::nextSequence(sequence);
    CHECK(sequence == 1);
    for (int i = 0; i < 254; ++i) {
        sequence = takt4::dmx::nextSequence(sequence);
    }
    CHECK(sequence == 255);
    CHECK(takt4::dmx::nextSequence(255) == 1);

    // Every value it can produce, across two full laps.
    std::uint8_t value = 0;
    for (int i = 0; i < 600; ++i) {
        value = takt4::dmx::nextSequence(value);
        CHECK(value != 0);
    }
}

TEST_CASE("a Port-Address round-trips through the way a person writes it", "[dmx][artnet]") {
    PortAddress address = 0;

    SECTION("inside the first Net it is the flat number a node's front panel shows") {
        CHECK(takt4::dmx::describePortAddress(0) == "0");
        CHECK(takt4::dmx::describePortAddress(7) == "7");
        CHECK(takt4::dmx::describePortAddress(255) == "255");
        REQUIRE(takt4::dmx::parsePortAddress("7", address));
        CHECK(address == 7);
    }

    SECTION("past it, the three parts are spelled out") {
        CHECK(takt4::dmx::describePortAddress(0x1234) == "18:3:4");
        REQUIRE(takt4::dmx::parsePortAddress("18:3:4", address));
        CHECK(address == 0x1234);
    }

    SECTION("the three-part form is accepted inside the first Net too") {
        REQUIRE(takt4::dmx::parsePortAddress("0:0:5", address));
        CHECK(address == 5);
        REQUIRE(takt4::dmx::parsePortAddress(" 0 : 1 : 0 ", address));
        CHECK(address == 16);
    }

    SECTION("a part that overflows its own field is a typo, not a bigger number") {
        // 0:0:16 is not universe 16 written another way — it is a mistyped 0:1:0, and
        // carrying the overflow into the Sub-Net would point a rig at the wrong node.
        CHECK_FALSE(takt4::dmx::parsePortAddress("0:0:16", address));
        CHECK_FALSE(takt4::dmx::parsePortAddress("0:16:0", address));
        CHECK_FALSE(takt4::dmx::parsePortAddress("128:0:0", address));
    }

    SECTION("nonsense is refused and leaves the value alone") {
        address = 42;
        CHECK_FALSE(takt4::dmx::parsePortAddress("", address));
        CHECK_FALSE(takt4::dmx::parsePortAddress("universe 3", address));
        CHECK_FALSE(takt4::dmx::parsePortAddress("1:2", address));
        CHECK_FALSE(takt4::dmx::parsePortAddress("-1", address));
        CHECK_FALSE(takt4::dmx::parsePortAddress("32768", address));
        CHECK(address == 42);
    }
}

// Over a real socket: the claim is that what an operator configures reaches the wire, which
// building a packet in memory cannot make.
TEST_CASE("an Art-Net sender puts a real datagram on the loopback", "[dmx][artnet]") {
    LoopbackReceiver receiver;
    ArtNetSender sender("127.0.0.1", receiver.port());

    const std::array<std::uint8_t, 4> levels{1, 2, 3, 255};
    REQUIRE(sender.sendDmx(3, levels));
    const std::string datagram = receiver.receive();
    REQUIRE(datagram.size() == takt4::dmx::kArtDmxHeaderSize + 4);
    CHECK(datagram.compare(0, 7, "Art-Net") == 0);
    CHECK(static_cast<std::uint8_t>(datagram[14]) == 3);   // SubUni
    CHECK(static_cast<std::uint8_t>(datagram[12]) == 1);   // first sequence number
    CHECK(static_cast<std::uint8_t>(datagram[21]) == 255); // the last channel
    CHECK(sender.sent() == 1);
    CHECK(sender.failed() == 0);

    SECTION("the sequence counter advances per universe, not per socket") {
        // Two universes interleaved: each keeps its own count, because a receiver uses the
        // sequence to re-order one universe's stream and knows nothing of the other's.
        REQUIRE(sender.sendDmx(4, levels));
        CHECK(static_cast<std::uint8_t>(receiver.receive()[12]) == 1);
        REQUIRE(sender.sendDmx(3, levels));
        CHECK(static_cast<std::uint8_t>(receiver.receive()[12]) == 2);
        REQUIRE(sender.sendDmx(4, levels));
        CHECK(static_cast<std::uint8_t>(receiver.receive()[12]) == 2);
    }
}

TEST_CASE("a node aimed at a name follows the name to a new address", "[dmx][artnet][net]") {
    // As an OSC output: the name is looked up again, and the node is sent to where it now is
    // rather than where it was when the socket opened.
    LoopbackReceiver before(0x7F000001u, 0);
    LoopbackReceiver after(0x7F000002u, before.port());
    takt4::net::AsyncAddress::answerForTests("node.test", "127.0.0.1");
    ArtNetSender sender("node.test", before.port(), 0.05);
    const std::array<std::uint8_t, 512> levels{};
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!sender.ready() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    REQUIRE(sender.sendDmx(0, levels));
    CHECK_FALSE(before.receive().empty());

    takt4::net::AsyncAddress::answerForTests("node.test", "127.0.0.2");
    bool arrived = false;
    const auto by = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!arrived && std::chrono::steady_clock::now() < by) {
        sender.refresh();
        (void)sender.sendDmx(0, levels);
        arrived = after.ready(20);
    }
    CHECK(arrived);
}

TEST_CASE("an Art-Net node that cannot be resolved is reported, not swallowed", "[dmx][artnet]") {
    // Found out on a thread of its own rather than on the output thread — see
    // `net::AsyncAddress` — so building one does not wait on a name server.
    const auto opened = std::chrono::steady_clock::now();
    ArtNetSender unknown("no.such.host.takt4.invalid", 6454);
    CHECK(std::chrono::steady_clock::now() - opened < std::chrono::milliseconds(200));
    const std::array<std::uint8_t, 3> levels{255, 0, 0};
    CHECK_FALSE(unknown.sendDmx(0, levels));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (unknown.problem().rfind("cannot resolve", 0) != 0 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_THAT(unknown.problem(), ContainsSubstring("cannot resolve"));
}

TEST_CASE("an Art-Net node the network will not send to is reported", "[dmx][artnet]") {
    // The audit's T3 and M12: a network that is down, or a cable that is out, fails every
    // send — and nothing said so; the node simply went dark. See `kUnsendableHost` for what
    // stands in for that here.
    if constexpr (!takt4::testing::kUnsendableFails) {
        SKIP(takt4::testing::kUnsendableSkip);
    }
    ArtNetSender node(takt4::testing::kUnsendableHost, 6454);
    CHECK(node.problem().empty()); // nothing has been tried yet
    const std::array<std::uint8_t, 3> levels{255, 0, 0};
    CHECK_FALSE(node.sendDmx(0, levels));
    CHECK(node.failed() == 1);
    CHECK_THAT(node.problem(), ContainsSubstring("sends are failing"));
    CHECK_THAT(node.problem(), ContainsSubstring(takt4::testing::kUnsendableReason));
}

TEST_CASE("an Art-Net node at a broadcast address really sends", "[dmx][artnet][network]") {
    // Art-Net's spec forbids broadcasting ArtDmx, and some rigs do it anyway; the socket has to
    // be allowed to or every frame is refused. It goes through `net::prepareSender`, shared
    // with OSC since the audit's M20, so this is what notices if the node stops calling it.
    // One frame to the limited broadcast, on a port nothing uses.
    ArtNetSender node("255.255.255.255", 57092);
    const std::array<std::uint8_t, 3> levels{0, 0, 0};
    CHECK(node.sendDmx(0, levels));
    CHECK(node.failed() == 0);
    CHECK(node.problem().empty());
}

// The pacing rules are the whole of what this class does, and both of them are felt on a rig:
// too fast and a node drops frames mid-fade, too slow and a node decides takt4 has gone away.
TEST_CASE("frames are paced at 44 Hz and kept alive when nothing moves", "[dmx][artnet]") {
    LoopbackReceiver receiver;
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});

    ArtNetPublisher publisher;
    ArtNetPublisher::TargetConfig config;
    config.host = "127.0.0.1";
    config.port = receiver.port();
    publisher.addTarget(config);

    SECTION("the first frame goes out at once") {
        CHECK(publisher.publish(engine, 0.0) == 1);
        CHECK_FALSE(receiver.receive().empty());
    }

    SECTION("a change inside the 44 Hz window waits for the window, and is not lost") {
        REQUIRE(publisher.publish(engine, 0.0) == 1);
        (void)receiver.receive();

        takt4::dmx::Payload payload;
        payload.kind = takt4::dmx::EffectKind::Color;
        payload.color = takt4::dmx::Color{255, 0, 0};
        engine.start(0b1, payload, 0.001);

        // 1 ms later: changed, but 44 Hz is 22.7 ms, so nothing leaves yet.
        CHECK(publisher.publish(engine, 0.001) == 0);
        CHECK(publisher.publish(engine, 0.010) == 0);
        // Past the window, the *current* frame goes — the change was held, not dropped.
        CHECK(publisher.publish(engine, 0.030) == 1);
        const std::string datagram = receiver.receive();
        REQUIRE(datagram.size() == takt4::dmx::kArtDmxMaxSize);
        CHECK(static_cast<std::uint8_t>(datagram[18]) == 255); // red, channel 1
    }

    SECTION("an unchanged universe is re-sent on the keep-alive and not before") {
        REQUIRE(publisher.publish(engine, 0.0) == 1);
        (void)receiver.receive();
        CHECK(publisher.publish(engine, 0.5) == 0);
        CHECK(publisher.publish(engine, 0.89) == 0);
        CHECK(publisher.publish(engine, 0.95) == 1);
    }

    SECTION("every node is fed every universe the patch uses") {
        // Since 2026-09-25 a node has no universe list of its own: it sends on the universes
        // it is set up for and ignores the rest.
        engine.setPatch({rgbAt("a", 0, 1), rgbAt("b", 4, 1)});
        CHECK(publisher.publish(engine, 0.0) == 2);
        std::vector<int> universes;
        for (int i = 0; i < 2; ++i) {
            const std::string datagram = receiver.receive();
            REQUIRE(datagram.size() == takt4::dmx::kArtDmxMaxSize);
            universes.push_back(static_cast<std::uint8_t>(datagram[14]));
        }
        std::sort(universes.begin(), universes.end());
        CHECK(universes == std::vector<int>{0, 4});
    }

    SECTION("a node added mid-set is fed at once rather than waiting out the other's clock") {
        // The pacing is per target *and* universe, not per publisher. If it were shared, a
        // node plugged in half way through a keep-alive would sit dark until the next one.
        REQUIRE(publisher.publish(engine, 0.0) == 1);
        (void)receiver.receive();

        LoopbackReceiver second;
        ArtNetPublisher::TargetConfig other;
        other.host = "127.0.0.1";
        other.port = second.port();
        publisher.addTarget(other);

        // A millisecond later the first node is well inside its window and sends nothing;
        // the new one has never been sent to and gets its frame now.
        CHECK(publisher.publish(engine, 0.001) == 1);
        CHECK_FALSE(second.receive().empty());
    }
}

TEST_CASE("a delayed node's frame is looked up only when one can go, and only patched universes "
          "are kept",
          "[dmx][artnet]") {
    // The audit's L16: a delayed node's frame was looked up in the history — a walk back through
    // five hundred frames at a second's delay — every round, for every node and universe,
    // before the 44 Hz pacing said whether anything could be sent at all. And L17: the history
    // of a universe the patch had dropped was kept for as long as any node lagged.
    takt4::testing::ArtNetNodes nodes(1);
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    feed(publisher, nodes, {1.0});
    const auto round = [&](double now) { return publisher.publish(engine, now); };
    // A second of lighting that changes every 10 ms, so the history holds a hundred frames and
    // more, and then the node catches up with it.
    for (int ms = 0; ms < 1000; ms += 10) {
        paint(engine, static_cast<std::uint8_t>(ms / 10), ms / 1000.0);
        nodes.run(round, ms, ms + 10);
    }
    nodes.run(round, 1000, 1100);
    INFO(publisher.historyLookups() << " look-ups examined " << publisher.historySteps()
                                    << " frames");
    CHECK(publisher.historyLookups() > 0);
    CHECK(publisher.historyLookups() < 1100); // not in the 44 Hz period after a frame went
    // Halving: a dozen frames examined per look-up at most, where the walk back examined
    // hundreds.
    CHECK(publisher.historySteps() <= 12 * publisher.historyLookups());
    // And what went is still the lighting a second late.
    CHECK(nodes.first(0, 1) >= 1.0);
    CHECK(nodes.first(0, 1) < 1.05);
    CHECK(publisher.histories() == 1);

    // The fixture moved to another universe, twice: one history, the one still patched.
    engine.setPatch({rgbAt("par", 1, 1)});
    nodes.run(round, 1100, 1200);
    engine.setPatch({rgbAt("par", 2, 1)});
    nodes.run(round, 1200, 1300);
    CHECK(publisher.histories() == 1);
}

TEST_CASE("an Art-Net node set later is sent the lighting that much later", "[dmx][artnet]") {
    // The operator's call of 2026-09-25: every output has a delay, and a node's is honoured like
    // the rest. A universe is a stream of frames rather than a message to hold, so what is
    // delayed is the stream — the late node is sent the frame the patch had that long ago.
    using takt4::testing::kArtNetFrame;
    takt4::testing::ArtNetNodes nodes(2);
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    feed(publisher, nodes, {0.0, 0.100});
    CHECK(publisher.leadSeconds() == 0.0);
    CHECK_THAT(publisher.lagOf(0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(publisher.lagOf(1), WithinAbs(0.100, 1e-12));
    const auto round = [&](double now) { return publisher.publish(engine, now); };

    nodes.run(round, 0, 200);
    paint(engine, 255, 0.200);
    nodes.run(round, 200, 500);
    // The node on time has it as it is made, within one frame of its pacing; the late one not a
    // moment before its delay is up, and within a frame after.
    CHECK(nodes.first(0, 255) >= 0.200);
    CHECK(nodes.first(0, 255) < 0.200 + kArtNetFrame);
    CHECK(nodes.first(1, 255) >= 0.300);
    CHECK(nodes.first(1, 255) < 0.300 + kArtNetFrame);

    SECTION("a delay moved while the lighting runs is the new one from then on") {
        // What a dragged slider sends; the late node catches up with the lighting as it is.
        REQUIRE(publisher.setDelay(1, 0.0));
        CHECK_FALSE(publisher.setDelay(7, 0.0)); // no node on that bit
        nodes.run(round, 500, 600);
        paint(engine, 0, 0.600);
        nodes.run(round, 600, 800);
        for (std::size_t node = 0; node < 2; ++node) {
            INFO("node " << node);
            CHECK(nodes.first(node, 0, 0.5) >= 0.600);
            CHECK(nodes.first(node, 0, 0.5) < 0.600 + kArtNetFrame);
        }
    }

    SECTION("the last frame on the way out is the lighting as it is, to every node") {
        // `flush` is the last thing sent before the output thread stops. A delayed node would
        // otherwise be left on whatever it was a delay ago — a flash still lit after quitting.
        nodes.run(round, 500, 600);
        paint(engine, 0, 0.600);
        nodes.run(round, 600, 650);
        CHECK(nodes.first(0, 0, 0.5) >= 0.600);
        REQUIRE(nodes.first(1, 0, 0.5) < 0.0); // not due until 0.7
        nodes.run([&](double now) { return publisher.flush(engine, now); }, 650, 651);
        CHECK_THAT(nodes.first(1, 0, 0.5), WithinAbs(0.650, 1e-9));
    }
}

TEST_CASE("PANIC freezes a delayed Art-Net node when it freezes the rest", "[dmx][artnet]") {
    // The audit of 2026-09-25, M5. PANIC stopped the animation and froze the levels — and a node
    // set half a second late went on being sent the half-second before the freeze from the
    // history: a strobe that went on strobing, out of the operator's reach, beside a node that had
    // stopped.
    using takt4::testing::kArtNetFrame;
    takt4::testing::ArtNetNodes nodes(2);
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    feed(publisher, nodes, {0.0, 0.5});
    const auto round = [&](double now) { return publisher.publish(engine, now); };

    // A flash and another: full at 0.2, out at 0.4, full again at 0.6.
    nodes.run(round, 0, 200);
    paint(engine, 255, 0.200);
    nodes.run(round, 200, 400);
    paint(engine, 0, 0.400);
    nodes.run(round, 400, 600);
    paint(engine, 255, 0.600);
    nodes.run(round, 600, 650);
    // PANIC at 0.65, as the output runner does it: the effects cancelled, the levels held.
    engine.cancelAll();
    publisher.forgetHistory();
    nodes.run(round, 650, 1000);
    // The late node is sent the frozen frame now, and never the dark it was half a second before.
    CHECK(nodes.first(1, 255, 0.650) >= 0.650);
    CHECK(nodes.first(1, 255, 0.650) < 0.650 + kArtNetFrame);
    CHECK(nodes.first(1, 0, 0.650) < 0.0);

    // RELEASE: the late node is late again, from the frozen frame on — not from the animation
    // before it, which a history kept across the freeze would replay now.
    nodes.run(round, 1000, 1100);
    paint(engine, 0, 1.100);
    nodes.run(round, 1100, 1800);
    CHECK(nodes.first(0, 0, 1.0) >= 1.100);
    CHECK(nodes.first(0, 0, 1.0) < 1.100 + kArtNetFrame);
    CHECK(nodes.first(1, 0, 1.0) >= 1.600);
    CHECK(nodes.first(1, 0, 1.0) < 1.600 + kArtNetFrame);
}

TEST_CASE("an Art-Net node set earlier has the lighting as it is made, and the rest wait for it",
          "[dmx][artnet]") {
    // A node cannot be sent a frame nobody has made yet. So the earliest node is sent the lighting
    // as it is made, every other node that much later again — and `RuleSink` starts a beat's
    // effects early by the same lead, which is what puts the early node ahead of the beat (see
    // "each Art-Net node has a beat's lighting on the beat plus its own delay").
    using takt4::testing::kArtNetFrame;
    takt4::testing::ArtNetNodes nodes(3);
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    feed(publisher, nodes, {-0.100, 0.0, 0.050});
    CHECK_THAT(publisher.leadSeconds(), WithinAbs(0.100, 1e-12));
    CHECK_THAT(publisher.lagOf(0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(publisher.lagOf(1), WithinAbs(0.100, 1e-12));
    CHECK_THAT(publisher.lagOf(2), WithinAbs(0.150, 1e-12));
    const auto round = [&](double now) { return publisher.publish(engine, now); };

    nodes.run(round, 0, 200);
    paint(engine, 255, 0.200);
    nodes.run(round, 200, 500);
    const double expected[] = {0.200, 0.300, 0.350};
    for (std::size_t node = 0; node < 3; ++node) {
        INFO("node " << node);
        CHECK(nodes.first(node, 255) >= expected[node]);
        CHECK(nodes.first(node, 255) < expected[node] + kArtNetFrame);
    }
}

TEST_CASE("a delay raised from nothing holds a node where it was, then plays on that far behind",
          "[dmx][artnet]") {
    // The audit of 2026-09-25, coverage gap 9. With every node on time no history is kept — half
    // a megabyte a universe for nothing — so a delay raised mid-set has nothing to reach back
    // into. The node is held on the lighting as it was when the delay was raised until the history
    // reaches back that far, and from then on it is sent the lighting that long ago: a delay
    // introduced live has to hold somewhere, and it holds on what the node already shows.
    using takt4::testing::kArtNetFrame;
    takt4::testing::ArtNetNodes nodes(2);
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    feed(publisher, nodes, {0.0, 0.0});
    const auto round = [&](double now) { return publisher.publish(engine, now); };

    nodes.run(round, 0, 200);
    paint(engine, 255, 0.200);
    nodes.run(round, 200, 300);
    REQUIRE(nodes.first(1, 255) >= 0.200);
    REQUIRE(nodes.first(1, 255) < 0.200 + kArtNetFrame);

    REQUIRE(publisher.setDelay(1, 0.200)); // at 0.300
    nodes.run(round, 300, 350);
    paint(engine, 0, 0.350);
    nodes.run(round, 350, 800);

    CHECK(nodes.first(0, 0, 0.300) >= 0.350);
    CHECK(nodes.first(0, 0, 0.300) < 0.350 + kArtNetFrame);
    // Dark 200 ms after it went dark, and nothing in between but the lit frame it was holding.
    CHECK(nodes.first(1, 0, 0.300) >= 0.550);
    CHECK(nodes.first(1, 0, 0.300) < 0.550 + kArtNetFrame);
    for (const auto& heard : nodes.heard(1)) {
        if (heard.at >= 0.300 && heard.at < 0.550) {
            INFO("sent at " << heard.at);
            CHECK(heard.channel1 == 255);
        }
    }
}

TEST_CASE("a node's history goes round its ring and still sends what was that long ago",
          "[dmx][artnet]") {
    // Coverage gap 9. The history is a ring allocated once, 1033 frames kept at most one every
    // 2 ms, and a universe that changes every round fills it in two to three seconds (a frame is
    // kept only once 2 ms have passed, so the gaps come out 2 or 3 ms). So this runs a universe
    // that changes every millisecond for eight, and holds the delayed node, every frame it is
    // sent, to the level the universe had its delay ago — past six seconds reading frames written
    // over older ones.
    takt4::testing::ArtNetNodes nodes(2);
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    feed(publisher, nodes, {0.0, 1.5});
    // A level that climbs one step every 4 ms and wraps, never 0.
    const auto level = [](long ms) { return static_cast<std::uint8_t>((ms / 4) % 250 + 1); };
    const auto round = [&](double now) {
        paint(engine, level(std::lround(now * 1000.0)), now);
        return publisher.publish(engine, now);
    };
    nodes.run(round, 0, 8000);

    // Kept at most every 2 ms, so what was that long ago is a frame of up to a few ms before it.
    const auto wasAt = [&](long ms, std::uint8_t heard) {
        for (long back = 0; back <= 5; ++back) {
            if (ms - back >= 0 && level(ms - back) == heard) {
                return true;
            }
        }
        return false;
    };
    std::size_t checked = 0;
    std::size_t afterGoingRound = 0;
    for (const auto& heard : nodes.heard(1)) {
        if (heard.at < 1.6) {
            continue; // the delay's own first second and a half, held on the oldest frame
        }
        const long ms = std::lround(heard.at * 1000.0);
        INFO("sent at " << heard.at << ": " << int{heard.channel1} << ", which was "
                        << int{level(ms - 1500)} << " 1.5 s before");
        CHECK(wasAt(ms - 1500, heard.channel1));
        ++checked;
        // Sent from a frame kept after 4.5 s, when the ring had gone round at least once.
        afterGoingRound += heard.at >= 6.0 ? 1U : 0U;
    }
    CHECK(checked > 200);
    // A node is only sent a frame that moved, so a ring that stopped going round — handing back
    // one old frame from then on — would send it that frame once and then nothing more.
    CHECK(afterGoingRound > 60);
    // And the node on time is sent the universe as it is.
    for (const auto& heard : nodes.heard(0)) {
        const long ms = std::lround(heard.at * 1000.0);
        CHECK(wasAt(ms, heard.channel1));
    }
}

TEST_CASE("the same node given again keeps its sender, its sequence and its pacing",
          "[dmx][artnet]") {
    // Coverage gap 9, and the audit's H12 before it. Every edit of every output hands the
    // publisher the whole list of nodes again; a node whose id and address are unchanged has to
    // carry on as it was — its ArtDmx sequence numbers counting on, and its 44 Hz clock — or a
    // slider dragged on another row sends it frames faster than it takes them.
    LoopbackReceiver receiver;
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    ArtNetPublisher::TargetConfig node;
    node.host = "127.0.0.1";
    node.port = receiver.port();
    node.id = "o-node";
    node.bit = 1;
    REQUIRE(publisher.setTargets({node}).empty());
    // Byte 12 of an ArtDmx packet is its sequence number.
    const auto sequence = [&receiver] {
        const std::string datagram = receiver.receive();
        REQUIRE(datagram.size() == takt4::dmx::kArtDmxMaxSize);
        return static_cast<int>(static_cast<std::uint8_t>(datagram[12]));
    };

    REQUIRE(publisher.publish(engine, 0.0) == 1);
    CHECK(sequence() == 1);
    paint(engine, 100, 0.030);
    REQUIRE(publisher.publish(engine, 0.030) == 1);
    CHECK(sequence() == 2);

    // The same node again, a row further down the list — as another output being added above it
    // does — with the same id and the same address.
    node.bit = 2;
    REQUIRE(publisher.setTargets({node}).empty());
    CHECK(publisher.outputOf(0) == 2);
    paint(engine, 200, 0.031);
    // A millisecond after its last frame it is inside its 44 Hz window, as it was; a new sender
    // would have been fed at once.
    CHECK(publisher.publish(engine, 0.031) == 0);
    CHECK(publisher.publish(engine, 0.060) == 1);
    CHECK(sequence() == 3);

    SECTION("another address is another node, fed at once and counting from one") {
        LoopbackReceiver other;
        node.port = other.port();
        REQUIRE(publisher.setTargets({node}).empty());
        paint(engine, 50, 0.061);
        CHECK(publisher.publish(engine, 0.061) == 1);
        const std::string datagram = other.receive();
        REQUIRE(datagram.size() == takt4::dmx::kArtDmxMaxSize);
        CHECK(static_cast<std::uint8_t>(datagram[12]) == 1);
    }
}

TEST_CASE("a cue said to be coming goes out when it is due, through a fade that keeps its "
          "universe busy",
          "[dmx][artnet]") {
    // The 44 Hz ceiling was counted from wherever the last frame happened to go, so in a universe
    // a fade kept busy — a colour rule fading over a beat keeps it so for good — a cue waited
    // anything from nothing to 23 ms behind the fade's last frame, a different amount every beat.
    // A cue held for its moment is said to be coming, as `output::RuleSink` says it every round,
    // and no frame goes in the period before it: the cue's own frame goes the round it is due, and
    // never more than 44 a second. A node set 100 ms late is held clear for each cue the same way,
    // from when it started (`cueStarted`), and has it 100 ms after.
    using takt4::testing::kArtNetFrame;
    takt4::testing::ArtNetNodes nodes(2);
    DmxEngine engine;
    engine.setPatch({rgbAt("cue", 0, 1), rgbAt("wash", 0, 10)});
    ArtNetPublisher publisher;
    feed(publisher, nodes, {0.0, 0.100});
    // The wash round the colour wheel once a second, for a minute: a new frame every period.
    takt4::dmx::Payload sweep;
    sweep.kind = takt4::dmx::EffectKind::HueSweep;
    sweep.color = takt4::dmx::Color{255, 0, 0};
    sweep.durationSeconds = 60.0f;
    sweep.cycles = 60.0f;
    engine.start(0b10, sweep, 0.0);
    // A cue a beat at 128 BPM on the other fixture, lit and dark in turn, on whole milliseconds so
    // the round it is due in is the one it is due at.
    std::vector<double> cues;
    for (int k = 0; k < 24; ++k) {
        cues.push_back(std::round((0.5 + k * 60.0 / 128.0) * 1000.0) / 1000.0);
    }
    std::size_t next = 0;
    std::vector<std::pair<PortAddress, double>> upcoming;
    const auto round = [&](double now) {
        // What the output thread does each round: start what has come due, say what is still to
        // come, then make the frames.
        while (next < cues.size() && cues[next] <= now + 1e-9) {
            paint(engine, static_cast<std::uint8_t>(next % 2 == 0 ? 255 : 0), cues[next]);
            publisher.cueStarted(PortAddress{0}, cues[next]);
            ++next;
        }
        upcoming.clear();
        if (next < cues.size()) {
            upcoming.emplace_back(PortAddress{0}, cues[next]);
        }
        publisher.setUpcomingCues(upcoming);
        engine.tick(now);
        return publisher.publish(engine, now);
    };
    nodes.run(round, 0, static_cast<int>(cues.back() * 1000.0) + 200);
    for (std::size_t k = 0; k < cues.size(); ++k) {
        INFO("cue " << k << " due at " << cues[k]);
        const auto level = static_cast<std::uint8_t>(k % 2 == 0 ? 255 : 0);
        CHECK_THAT(nodes.first(0, level, cues[k] - 0.0005), WithinAbs(cues[k], 1e-9));
        // The late node's lighting is a history kept every couple of milliseconds.
        const double late = nodes.first(1, level, cues[k] + 0.100 - 0.0005);
        CHECK(late >= cues[k] + 0.100 - 1e-9);
        CHECK(late <= cues[k] + 0.100 + 0.0031);
    }
    // The wash went on being sent at the ceiling round them, and never faster.
    for (std::size_t node = 0; node < 2; ++node) {
        INFO("node " << node);
        const auto& heard = nodes.heard(node);
        CHECK(heard.size() > 400);
        for (std::size_t i = 1; i < heard.size(); ++i) {
            INFO("frames at " << heard[i - 1].at << " and " << heard[i].at);
            CHECK(heard[i].at - heard[i - 1].at >= kArtNetFrame - 1e-9);
        }
    }
}

TEST_CASE("a delay raised while the lighting runs never shows a node a flash twice",
          "[dmx][artnet]") {
    // A node's delay is a lag through the lighting's history. Taken at once, a delay dragged later
    // sent the node the stretch it had just been sent over again — a flash it had shown, shown a
    // second time. It grows no faster than time passes now: the node holds what it shows while it
    // does, and then plays on the new delay behind. Lowered, it is taken at once, which skips the
    // stretch between: that cannot be helped.
    using takt4::testing::kArtNetFrame;
    takt4::testing::ArtNetNodes nodes(2);
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    feed(publisher, nodes, {0.0, 0.100});
    const auto round = [&](double now) { return publisher.publish(engine, now); };
    // A flash at 0.2, gone at 0.25: the late node shows it from 0.3 to 0.35.
    nodes.run(round, 0, 200);
    paint(engine, 255, 0.200);
    nodes.run(round, 200, 250);
    paint(engine, 0, 0.250);
    nodes.run(round, 250, 400);
    REQUIRE(nodes.first(1, 255) >= 0.300);
    REQUIRE(nodes.first(1, 0, 0.300) >= 0.350);

    // The delay dragged from 100 ms to 400 at 0.4.
    REQUIRE(publisher.setDelay(1, 0.400));
    nodes.run(round, 400, 1000);
    for (const auto& heard : nodes.heard(1)) {
        if (heard.at >= 0.400) {
            INFO("sent at " << heard.at);
            CHECK(heard.channel1 == 0);
        }
    }
    // And from then on the lighting reaches it the new delay late.
    paint(engine, 255, 1.000);
    nodes.run(round, 1000, 1600);
    CHECK(nodes.first(0, 255, 1.0) >= 1.000);
    CHECK(nodes.first(0, 255, 1.0) < 1.000 + kArtNetFrame);
    CHECK(nodes.first(1, 255, 1.0) >= 1.400);
    CHECK(nodes.first(1, 255, 1.0) < 1.400 + kArtNetFrame);
}

TEST_CASE("a frame a node failed to take is sent again at the next frame, not the keep-alive",
          "[dmx][artnet]") {
    // A failed send was recorded as the frame sent, so a static look a node missed waited 0.9 s
    // for the keep-alive. A name the test binaries' sandbox lets nobody look up never has an
    // address, so every send to it fails, on every system. (0.0.0.1, used first, failed on
    // Windows only: Linux takes it as a destination, and the sandbox then said it was sent.)
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    ArtNetPublisher::TargetConfig config;
    config.host = "a-node-nobody-has.invalid";
    config.port = 6454;
    publisher.addTarget(config);
    paint(engine, 255, 0.0);
    for (int ms = 0; ms < 1000; ++ms) {
        (void)publisher.publish(engine, ms / 1000.0);
    }
    INFO(publisher.failed() << " attempts failed in a second");
    CHECK(publisher.sent() == 0);
    // At the 44 Hz pace, every frame period: 44 a second, give or take one.
    CHECK(publisher.failed() >= 40);
    CHECK(publisher.failed() <= 45);
}

TEST_CASE("a node taken back during its farewell is sent the lighting at the next frame",
          "[dmx][artnet]") {
    // Its farewell sends it zeros, and what it showed last was not the lighting — but its pacing
    // remembered the lighting as sent, and it waited up to 0.9 s dark for the keep-alive.
    using takt4::testing::kArtNetFrame;
    takt4::testing::ArtNetNodes nodes(1);
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    ArtNetPublisher::TargetConfig node;
    node.host = "127.0.0.1";
    node.port = nodes.port(0);
    node.id = "o-a0";
    REQUIRE(publisher.setTargets({node}).empty());
    const auto round = [&](double now) { return publisher.publish(engine, now); };
    paint(engine, 255, 0.0);
    nodes.run(round, 0, 100);
    REQUIRE(nodes.first(0, 255) >= 0.0);
    REQUIRE(publisher.setTargets({}).empty()); // switched off: its farewell begins
    nodes.run(round, 100, 200);
    REQUIRE(nodes.first(0, 0, 0.1) >= 0.1); // dark
    REQUIRE(publisher.setTargets({node}).empty()); // and back on, within the farewell
    nodes.run(round, 200, 1200);
    CHECK(nodes.first(0, 255, 0.2) >= 0.2);
    CHECK(nodes.first(0, 255, 0.2) < 0.2 + 2.0 * kArtNetFrame);
}

TEST_CASE("a farewell stops when a new node is given its address", "[dmx][artnet]") {
    // A node switched off is sent zeros for a moment; and an output added in its place — a new
    // one, with an id of its own, at the same address — was fought by those zeros, the lighting
    // and the dark in turn, until the farewell ran out.
    takt4::testing::ArtNetNodes nodes(1);
    DmxEngine engine;
    engine.setPatch({rgbAt("par", 0, 1)});
    ArtNetPublisher publisher;
    ArtNetPublisher::TargetConfig old;
    old.host = "127.0.0.1";
    old.port = nodes.port(0);
    old.id = "o-old";
    REQUIRE(publisher.setTargets({old}).empty());
    const auto round = [&](double now) { return publisher.publish(engine, now); };
    paint(engine, 255, 0.0);
    nodes.run(round, 0, 100);
    REQUIRE(publisher.setTargets({}).empty());
    nodes.run(round, 100, 150);
    REQUIRE(publisher.leaving() == 1);
    ArtNetPublisher::TargetConfig replacement = old;
    replacement.id = "o-new";
    REQUIRE(publisher.setTargets({replacement}).empty());
    CHECK(publisher.leaving() == 0);
    nodes.run(round, 150, 700);
    // From the first frame after the new one was given the address, only the lighting.
    const double lit = nodes.first(0, 255, 0.15);
    REQUIRE(lit >= 0.15);
    for (const auto& heard : nodes.heard(0)) {
        if (heard.at > lit) {
            INFO("sent at " << heard.at);
            CHECK(heard.channel1 == 255);
        }
    }
}
