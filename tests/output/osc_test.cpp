#include "core/output/osc_message.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/output/osc_sender.hpp"
#include "core/output/output_target.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_approx.hpp>
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
    publisher.addTarget("127.0.0.1", receiver.port(), 0);
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

TEST_CASE("a target's delay holds its datagrams and nobody else's", "[output][osc]") {
    // §5.6's per-target latency, on real sockets. The thing this exists for is a rig whose
    // destinations do not share a lag — a media server a frame behind and a robot that has
    // to physically move — so the claim under test is not "it is late" but "it is late *and
    // the other one is not*". See `OutputTarget::delaySeconds`.
    LoopbackReceiver prompt;
    LoopbackReceiver slow;
    OscPublisher publisher;
    publisher.addTarget("127.0.0.1", prompt.port(), 0);
    publisher.addTarget("127.0.0.1", slow.port(), 1, 0.2);

    publisher.setNow(10.0);
    publisher.sendAddress("/cue");

    // The undelayed target has it already; the delayed one has nothing at all yet.
    CHECK_FALSE(prompt.receive().empty());
    CHECK(slow.receive().empty());
    CHECK(publisher.pending() == 1);

    // Not yet: a delay is a deadline, not a "next round".
    publisher.setNow(10.199);
    publisher.flushDue();
    CHECK(slow.receive().empty());
    CHECK(publisher.pending() == 1);

    publisher.setNow(10.2);
    publisher.flushDue();
    const std::string held = slow.receive();
    REQUIRE_FALSE(held.empty());
    CHECK(held.substr(0, held.find('\0')) == "/cue");
    CHECK(publisher.pending() == 0);
    CHECK(publisher.dropped() == 0);

    SECTION("a target's own messages keep their order through the delay") {
        publisher.setNow(20.0);
        publisher.sendAddress("/first");
        publisher.setNow(20.05);
        publisher.sendAddress("/second");
        publisher.setNow(20.3);
        publisher.flushDue();
        std::vector<std::string> got;
        for (std::string packet = slow.receive(); !packet.empty(); packet = slow.receive()) {
            got.push_back(packet.substr(0, packet.find('\0')));
        }
        CHECK(got == std::vector<std::string>{"/first", "/second"});
    }

    SECTION("replacing the targets drops what was waiting for them") {
        publisher.setNow(30.0);
        publisher.sendAddress("/stale");
        REQUIRE(publisher.pending() == 1);
        publisher.clearTargets();
        CHECK(publisher.pending() == 0);
    }
}

TEST_CASE("a negative offset lands ahead of the next beat", "[output][osc]") {
    // The user's question of 2026-09-07: the whole-rig latency slider goes both ways, so why
    // can a target's not? It can. A message about a beat already heard cannot be sent before
    // that beat, so "earlier" is measured from the *next* one instead — the publisher holds
    // it for what is left of a beat after the offset. See `OutputTarget::delaySeconds`.
    LoopbackReceiver early;
    OscPublisher publisher;
    publisher.addTarget("127.0.0.1", early.port(), 0, -0.300);

    // 92 BPM: a beat is 652.17 ms, so 300 ms early is 352.17 ms late.
    publisher.setBeatSeconds(60.0 / 92.0);
    publisher.setNow(0.0);
    publisher.sendAddress("/cue");
    REQUIRE(publisher.pending() == 1);

    publisher.setNow(0.352);
    publisher.flushDue();
    CHECK(early.receive().empty()); // 352.17 ms, not 352
    publisher.setNow(0.3522);
    publisher.flushDue();
    CHECK_FALSE(early.receive().empty());

    SECTION("and it follows the tempo, which a fixed delay cannot") {
        // The same −300 ms at 128 BPM, where a beat is 468.75 ms: the hold is 168.75 ms. An
        // operator sets the lag of their device once and it stays right as the music moves.
        publisher.setBeatSeconds(60.0 / 128.0);
        publisher.setNow(10.0);
        publisher.sendAddress("/cue");
        publisher.setNow(10.168);
        publisher.flushDue();
        CHECK(early.receive().empty());
        publisher.setNow(10.169);
        publisher.flushDue();
        CHECK_FALSE(early.receive().empty());
    }

    SECTION("with no tempo yet it waits for nothing rather than inventing a beat") {
        publisher.setBeatSeconds(0.0);
        publisher.setNow(20.0);
        publisher.sendAddress("/cue");
        CHECK(publisher.pending() == 0);
        CHECK_FALSE(early.receive().empty());
    }

    SECTION("an offset longer than the beat is as early as it can go, not two beats early") {
        // 55 BPM is the slowest the state space tracks and its beat is 1.09 s, so this is
        // only reachable from a hand-edited file — but it must not send a cue a bar early.
        publisher.setBeatSeconds(60.0 / 240.0); // 250 ms
        publisher.setNow(30.0);
        publisher.sendAddress("/cue");
        CHECK(publisher.pending() == 0);
        CHECK_FALSE(early.receive().empty());
    }
}

TEST_CASE("the whole-rig offset and a target's own add up", "[output][osc]") {
    // §5.5's slider and §5.6's per-target one are the same quantity measured from two places
    // — "everything downstream of me is this late" and "this device is". They compose, and
    // the window shows each row's total for that reason.
    LoopbackReceiver target;
    OscPublisher publisher;
    publisher.addTarget("127.0.0.1", target.port(), 0, 0.100);
    publisher.setBeatSeconds(0.5);

    // +100 ms of its own, −40 ms for the rig: 60 ms. Straddled rather than landed on,
    // because 0.1 − 0.04 is not exactly 0.06 in a double and the deadline is a `>`.
    publisher.setOffsetSeconds(-0.040);
    publisher.setNow(0.0);
    publisher.sendAddress("/cue");
    publisher.setNow(0.0599);
    publisher.flushDue();
    CHECK(target.receive().empty());
    publisher.setNow(0.0601);
    publisher.flushDue();
    CHECK_FALSE(target.receive().empty());

    SECTION("and a total that comes out negative is measured from the next beat") {
        // +100 ms of its own against −250 ms for the rig is −150 ms, which on a 500 ms beat
        // is a 350 ms hold.
        publisher.setOffsetSeconds(-0.250);
        publisher.setNow(10.0);
        publisher.sendAddress("/cue");
        publisher.setNow(10.3499);
        publisher.flushDue();
        CHECK(target.receive().empty());
        publisher.setNow(10.3501);
        publisher.flushDue();
        CHECK_FALSE(target.receive().empty());
    }

    SECTION("and an undelayed target still feels the rig's offset") {
        // Which is the hole this closed: the slider moved Link and the MIDI clock and left
        // OSC — the thing a media server actually listens to — exactly where it was.
        LoopbackReceiver plain;
        OscPublisher rig;
        rig.addTarget("127.0.0.1", plain.port(), 0);
        rig.setBeatSeconds(0.5);
        rig.setOffsetSeconds(-0.150);
        rig.setNow(0.0);
        rig.sendAddress("/cue");
        CHECK(rig.pending() == 1);
        CHECK(plain.receive().empty());
        rig.setNow(0.350);
        rig.flushDue();
        CHECK_FALSE(plain.receive().empty());
    }
}

TEST_CASE("a delay round-trips through a target's written form", "[output][routing]") {
    takt4::output::OutputTarget target;
    target.name = "robot";
    target.host = "10.0.0.7";
    target.port = 7000;
    target.delaySeconds = 0.352;

    const std::string text = takt4::output::formatOutputTarget(target);
    CHECK(text == "robot = 10.0.0.7:7000 +352ms");

    takt4::output::OutputTarget back;
    REQUIRE(takt4::output::parseOutputTarget(text, back));
    CHECK(back.name == "robot");
    CHECK(back.host == "10.0.0.7");
    CHECK(back.port == 7000);
    CHECK(back.delaySeconds == Catch::Approx(0.352));

    SECTION("and so does a negative one, sign and all") {
        takt4::output::OutputTarget ahead;
        ahead.name = "resolume";
        ahead.host = "10.0.0.9";
        ahead.port = 7000;
        ahead.delaySeconds = -0.300;

        const std::string written = takt4::output::formatOutputTarget(ahead);
        CHECK(written == "resolume = 10.0.0.9:7000 -300ms");

        takt4::output::OutputTarget parsed;
        REQUIRE(takt4::output::parseOutputTarget(written, parsed));
        CHECK(parsed.host == "10.0.0.9");
        CHECK(parsed.delaySeconds == Catch::Approx(-0.300));
    }

    SECTION("an offset past either end is refused rather than half-applied") {
        // The address is then left exactly as typed, so it fails as an address instead of
        // silently becoming one with an offset nobody asked for.
        takt4::output::OutputTarget parsed;
        CHECK_FALSE(takt4::output::parseOutputTarget("a = 10.0.0.9:7000 -4000ms", parsed));
        CHECK_FALSE(takt4::output::parseOutputTarget("a = 10.0.0.9:7000 +4000ms", parsed));
    }

    SECTION("a MIDI device name is not eaten by the suffix, and keeps its spaces") {
        takt4::output::OutputTarget midi;
        midi.name = "lights";
        midi.kind = takt4::output::OutputTarget::Kind::Midi;
        midi.device = "MOTU Pro Audio Midi Out 1";
        midi.delaySeconds = 0.04;
        takt4::output::OutputTarget parsed;
        REQUIRE(takt4::output::parseOutputTarget(takt4::output::formatOutputTarget(midi), parsed));
        CHECK(parsed.device == "MOTU Pro Audio Midi Out 1");
        CHECK(parsed.delaySeconds == Catch::Approx(0.04));
    }

    SECTION("no delay writes no suffix, so nothing that already worked has changed") {
        takt4::output::OutputTarget plain;
        plain.name = "deck";
        CHECK(takt4::output::formatOutputTarget(plain) == "deck = 127.0.0.1:7000");
    }

    SECTION("an address that merely ends in a number is not a delay") {
        takt4::output::OutputTarget parsed;
        REQUIRE(takt4::output::parseOutputTarget("deck = 127.0.0.1:7000", parsed));
        CHECK(parsed.delaySeconds == 0.0);
        CHECK(parsed.port == 7000);
    }
}
