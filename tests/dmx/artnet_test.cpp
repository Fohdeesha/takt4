#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/artnet_publisher.hpp"
#include "core/dmx/artnet_sender.hpp"
#include "core/dmx/dmx_engine.hpp"
#include "core/dmx/fixture.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
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

    SECTION("a node is fed only the universes it carries") {
        engine.setPatch({rgbAt("a", 0, 1), rgbAt("b", 4, 1)});
        publisher.clearTargets();
        ArtNetPublisher::TargetConfig one;
        one.host = "127.0.0.1";
        one.port = receiver.port();
        one.universes = {4};
        publisher.addTarget(one);

        CHECK(publisher.publish(engine, 0.0) == 1);
        const std::string datagram = receiver.receive();
        CHECK(static_cast<std::uint8_t>(datagram[14]) == 4);
    }

    SECTION("an empty universe list means every universe the patch uses") {
        engine.setPatch({rgbAt("a", 0, 1), rgbAt("b", 4, 1)});
        CHECK(publisher.publish(engine, 0.0) == 2);
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
