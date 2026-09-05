#include "core/output/osc_message.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/output/osc_sender.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::output::OscMessage;
using takt4::output::OscPublisher;
using takt4::output::OscSender;
using takt4::testing::LoopbackReceiver;

namespace {

std::string bytesOf(OscMessage& message) {
    const auto packet = message.packet();
    return std::string(reinterpret_cast<const char*>(packet.data()), packet.size());
}

} // namespace

// The wire format, byte for byte. Every part is null-terminated and padded out to a
// multiple of four, and the numbers are big-endian whatever the host is.
TEST_CASE("an OSC message is encoded exactly as OSC 1.0 says", "[output][osc]") {
    SECTION("no arguments: address, then a bare comma") {
        OscMessage message("/takt4/resync");
        REQUIRE(message.valid());
        // "/takt4/resync" is 13 bytes, so it pads to 16; "," pads to 4.
        CHECK(bytesOf(message) == std::string("/takt4/resync\0\0\0,\0\0\0", 20));
        CHECK(message.argumentCount() == 0);
    }

    SECTION("one int32, big-endian") {
        OscMessage message("/takt4/beat");
        message.addInt(1);
        // "/takt4/beat" is 11 bytes and pads to 12; ",i" pads to 4; then four bytes.
        CHECK(bytesOf(message) == std::string("/takt4/beat\0,i\0\0\0\0\0\1", 20));
    }

    SECTION("one float32, the IEEE bit pattern big-endian") {
        OscMessage message("/takt4/bpm");
        message.addFloat(128.0f); // 0x43000000
        CHECK(bytesOf(message) == std::string("/takt4/bpm\0\0,f\0\0\x43\0\0\0", 20));
    }

    SECTION("an address of exactly four characters still takes eight bytes") {
        OscMessage message("/abc");
        CHECK(bytesOf(message) == std::string("/abc\0\0\0\0,\0\0\0", 12));
    }

    SECTION("several arguments in order, tags then values") {
        OscMessage message("/a");
        message.addInt(-2).addFloat(1.0f).addString("hi");
        CHECK(bytesOf(message) ==
              std::string("/a\0\0,ifs\0\0\0\0\xFF\xFF\xFF\xFE\x3F\x80\0\0hi\0\0", 24));
        CHECK(message.argumentCount() == 3);
    }

    SECTION("assembling twice gives the same bytes") {
        OscMessage message("/takt4/meter");
        message.addInt(4);
        const std::string first = bytesOf(message);
        CHECK(bytesOf(message) == first);
    }
}

TEST_CASE("an OSC message that cannot be legal goes invalid rather than wrong", "[output][osc]") {
    for (const std::string_view address : {"", "takt4/bpm", "/takt4 bpm", "/takt4/*", "/takt4/[1]",
                                           "/takt4/a,b", "/takt4/#x", "/takt4/{a}", "/takt4/?"}) {
        INFO("address " << address);
        OscMessage message(address);
        CHECK_FALSE(message.valid());
        CHECK(message.packet().empty());
    }

    SECTION("too many arguments") {
        OscMessage message("/x");
        for (std::size_t i = 0; i <= OscMessage::kMaxArguments; ++i) {
            message.addInt(0);
        }
        CHECK_FALSE(message.valid());
    }

    SECTION("a string argument that does not fit") {
        OscMessage message("/x");
        message.addString(std::string(OscMessage::kCapacity, 'x'));
        CHECK_FALSE(message.valid());
        CHECK(message.packet().empty());
    }

    SECTION("an invalid message stays invalid however it is used") {
        OscMessage message("/bad address");
        message.addInt(1).addFloat(2.0f);
        CHECK_FALSE(message.valid());
        CHECK(message.argumentCount() == 0);
    }
}

TEST_CASE("datagrams reach a socket that is really listening", "[output][osc]") {
    LoopbackReceiver receiver;
    OscSender sender("127.0.0.1", receiver.port());
    CHECK(sender.port() == receiver.port());
    CHECK(sender.resolved() == "127.0.0.1:" + std::to_string(receiver.port()));

    OscMessage message("/takt4/bpm");
    message.addFloat(128.0f);
    REQUIRE(sender.send(message.packet()));
    CHECK(sender.sent() == 1);
    CHECK(sender.failed() == 0);
    CHECK(receiver.receive() == bytesOf(message));

    SECTION("an empty packet is refused rather than sent") {
        CHECK_FALSE(sender.send({}));
        CHECK(sender.failed() == 1);
    }

    SECTION("a host that cannot be resolved is an error, not a silent no-op") {
        CHECK_THROWS_WITH(OscSender("no.such.host.takt4.invalid", 9000),
                          ContainsSubstring("cannot resolve"));
    }
}

TEST_CASE("the generic namespace is published as HANDOFF 5.6 specifies", "[output][osc]") {
    LoopbackReceiver receiver;
    OscPublisher publisher;
    publisher.addTarget("127.0.0.1", receiver.port());
    REQUIRE(publisher.targetCount() == 1);

    takt4::tracking::BeatEvent event;
    event.bpm = 128.0;
    event.confidence = 0.75;
    event.locked = true;
    event.beatsPerBar = 4;
    event.beatInBar = 1;
    event.downbeat = true;
    publisher.publishBeat(event);

    // Everything a consumer needs, on the first beat it hears: the state, then the beat.
    std::vector<std::string> addresses;
    for (int i = 0; i < 7; ++i) {
        const std::string packet = receiver.receive();
        REQUIRE_FALSE(packet.empty());
        addresses.push_back(packet.substr(0, packet.find('\0')));
    }
    CHECK(addresses == std::vector<std::string>{"/takt4/bpm", "/takt4/confidence", "/takt4/locked",
                                                "/takt4/meter", "/takt4/beat", "/takt4/beat/bar",
                                                "/takt4/downbeat"});
    CHECK(publisher.messagesSent() == 7);
    CHECK(publisher.messagesFailed() == 0);

    SECTION("state that has not changed is not resent between beats") {
        takt4::tracking::TempoState state;
        state.bpm = 128.0;
        state.confidence = 0.75;
        state.locked = true;
        state.beatsPerBar = 4;
        publisher.publishState(state);
        CHECK(publisher.messagesSent() == 7);

        state.confidence = 0.20;
        publisher.publishState(state);
        CHECK(publisher.messagesSent() == 8);
        CHECK(receiver.receive().substr(0, 17) == "/takt4/confidence");

        state.locked = false;
        publisher.publishState(state);
        CHECK(publisher.messagesSent() == 9);
        CHECK(receiver.receive().substr(0, 13) == "/takt4/locked");
    }

    SECTION("a beat before any downbeat carries no bar position") {
        takt4::tracking::BeatEvent early = event;
        early.downbeat = false;
        early.beatInBar = 0;
        publisher.publishBeat(early);
        // Every beat repeats the state, so a consumer that started late is right again
        // within a beat: four state messages and the beat, and nothing else.
        CHECK(publisher.messagesSent() == 12);
        std::vector<std::string> after;
        for (int i = 0; i < 5; ++i) {
            const std::string packet = receiver.receive();
            REQUIRE_FALSE(packet.empty());
            after.push_back(packet.substr(0, packet.find('\0')));
        }
        CHECK(after == std::vector<std::string>{"/takt4/bpm", "/takt4/confidence", "/takt4/locked",
                                                "/takt4/meter", "/takt4/beat"});
    }

    SECTION("resync is its own message") {
        publisher.publishResync();
        CHECK(receiver.receive().substr(0, 13) == "/takt4/resync");
    }
}

TEST_CASE("the namespace prefix is checked when the publisher is built", "[output][osc]") {
    CHECK_NOTHROW(OscPublisher("/other"));
    CHECK_THROWS_AS(OscPublisher(""), std::invalid_argument);
    CHECK_THROWS_AS(OscPublisher("takt4"), std::invalid_argument);
    CHECK_THROWS_AS(OscPublisher("/takt4/"), std::invalid_argument);
    CHECK_THROWS_AS(OscPublisher("/tak t4"), std::invalid_argument);
    CHECK_THROWS_AS(OscPublisher("/takt4/*"), std::invalid_argument);
}
