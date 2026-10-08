#include "core/net/resolver.hpp"
#include "core/net/udp.hpp"
#include "core/output/osc_message.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/output/osc_sender.hpp"
#include "core/output/output_target.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include "support/loopback_receiver.hpp"

#if !defined(_WIN32)
#include <sys/time.h>
#endif

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

    SECTION("a host that cannot be resolved is said, not a silent no-op") {
        // Found out on a thread of its own, not here — opening one used to resolve on the
        // output thread, and a name with the DNS down stopped the MIDI clock for the
        // resolver's timeout (the audit's H12). Meanwhile nothing is sent and it is counted.
        const auto opened = std::chrono::steady_clock::now();
        OscSender unknown("no.such.host.takt4.invalid", 9000);
        CHECK(std::chrono::steady_clock::now() - opened < std::chrono::milliseconds(200));
        OscMessage probe("/takt4/bpm");
        CHECK_FALSE(unknown.send(probe.packet()));
        CHECK(unknown.failed() == 1);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (unknown.problem().rfind("cannot resolve", 0) != 0 &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK_THAT(unknown.problem(), ContainsSubstring("cannot resolve"));
        CHECK_FALSE(unknown.ready());
    }

    SECTION("a literal address has its socket at once, and no problem to report") {
        CHECK(sender.problem().empty());
        CHECK(sender.ready());
    }
}

TEST_CASE("an output aimed at a name follows the name to a new address", "[output][osc][net]") {
    // A name is looked up again every half minute (`net::AsyncAddress`), so a media server that
    // comes back from a restart on a new DHCP address is found there — but the sender stopped
    // asking once its socket was open, and went on sending to the old address with no error
    // until the output was edited. Here the name is the test's own, moved from one loopback
    // address to another on the same port, and looked up again a twentieth of a second apart.
    LoopbackReceiver before(0x7F000001u, 0);
    LoopbackReceiver after(0x7F000002u, before.port());
    takt4::net::AsyncAddress::answerForTests("media-server.test", "127.0.0.1");
    OscSender sender("media-server.test", before.port(), 0.05);
    OscMessage message("/clip");
    message.addInt(1);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!sender.ready() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    REQUIRE(sender.send(message.packet()));
    CHECK_FALSE(before.receive().empty());

    takt4::net::AsyncAddress::answerForTests("media-server.test", "127.0.0.2");
    bool arrived = false;
    const auto by = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!arrived && std::chrono::steady_clock::now() < by) {
        sender.refresh();
        (void)sender.send(message.packet());
        arrived = after.ready(20);
    }
    CHECK(arrived);
    CHECK(sender.resolved().find("127.0.0.2") != std::string::npos);
}

TEST_CASE("a sending socket never waits", "[output][osc]") {
    // The audit's M20: "a single non-blocking sendto", said the senders, of sockets that were
    // blocking — so a full send buffer would have held the output thread, and the MIDI clock
    // and every rule with it. A full send buffer cannot be made on demand, so this asks the
    // socket the question it answers the same way: with nothing to read and a two-second
    // receive timeout, a blocking socket waits the two seconds and a non-blocking one says
    // "would block" at once.
    namespace net = takt4::net;
    [[maybe_unused]] const net::WinsockGuard winsock; // an empty struct off Windows
    const net::Socket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(socket != net::kInvalidSocket);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    REQUIRE(::bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof local) == 0);
#if defined(_WIN32)
    const DWORD timeout = 2000;
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof timeout);
    constexpr int kWouldBlock = WSAEWOULDBLOCK;
#else
    const timeval timeout{2, 0};
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    constexpr int kWouldBlock = EAGAIN;
#endif

    net::prepareSender(socket);

    char byte = 0;
    const auto asked = std::chrono::steady_clock::now();
    const auto got = ::recvfrom(socket, &byte, 1, 0, nullptr, nullptr);
    const int error = net::lastSocketError();
    const auto waited = std::chrono::steady_clock::now() - asked;
    net::closeSocket(socket);
    CHECK(got < 0);
    CHECK(error == kWouldBlock);
    CHECK(waited < std::chrono::milliseconds(500));
}

TEST_CASE("an OSC target at a broadcast address really sends", "[output][osc][network]") {
    // The other half of M20: the OSC socket was never allowed to broadcast, so a rig that
    // aims OSC at its subnet's broadcast address got WSAEACCES on every send while Art-Net,
    // which did ask, worked. One datagram to the limited broadcast, on a port nothing uses —
    // [network], because it does leave the machine.
    OscSender sender("255.255.255.255", 57091);
    OscMessage message("/takt4/bpm");
    message.addFloat(128.0f);
    const bool sent = sender.send(message.packet());
    INFO(sender.problem());
    CHECK(sent);
    CHECK(sender.failed() == 0);
    CHECK(sender.problem().empty());
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

TEST_CASE("a negative offset lands ahead of the beat it belongs to", "[output][osc]") {
    // The user's question of 2026-09-07: the whole-rig latency slider goes both ways, so why
    // can a target's not? It can — and since the audit (H4) it means what it says. The output
    // thread fires a locked beat ahead of time, and a message *about that beat* carries its
    // moment: each target has it at the moment plus its offset. See `OutputTarget::delaySeconds`.
    LoopbackReceiver early;
    OscPublisher publisher;
    publisher.addTarget("127.0.0.1", early.port(), 0, -0.300);

    // A beat in the music at 1.0, fired at 0.5 on a prediction: 300 ms early is 0.7.
    publisher.setNow(0.5);
    publisher.sendAddressTo(takt4::output::kAllOutputs, "/cue", std::optional<double>{1.0});
    REQUIRE(publisher.pending() == 1);
    CHECK(early.receive().empty());

    publisher.setNow(0.6999);
    publisher.flushDue();
    CHECK(early.receive().empty());
    publisher.setNow(0.7001);
    publisher.flushDue();
    CHECK_FALSE(early.receive().empty());
    CHECK(publisher.pending() == 0);

    SECTION("a message about now goes now, not most of a beat later") {
        // Which is what every message used to do under a negative offset — held for what was
        // left of a beat, so a manual cue or a lock change arrived nearly a beat late.
        publisher.setNow(20.0);
        publisher.sendAddress("/cue");
        CHECK(publisher.pending() == 0);
        CHECK_FALSE(early.receive().empty());
    }

    SECTION("a beat heard after its time is sent at once") {
        // Hunting, nothing is predicted and a beat fires as it is heard: its moment is gone
        // and earlier than now is not a time anything can be sent at.
        publisher.setNow(30.0);
        publisher.sendAddressTo(takt4::output::kAllOutputs, "/cue", std::optional<double>{29.95});
        CHECK(publisher.pending() == 0);
        CHECK_FALSE(early.receive().empty());
    }

    SECTION("the namespace keeps a beat's state ahead of the beat, both at its moment") {
        takt4::tracking::BeatEvent event;
        event.bpm = 128.0;
        event.confidence = 0.9;
        event.locked = true;
        event.beatsPerBar = 4;
        event.beatInBar = 1;
        event.downbeat = true;
        publisher.setNow(40.0);
        publisher.publishBeat(event, 40.5);
        CHECK(publisher.pending() == 7);
        publisher.setNow(40.1999);
        publisher.flushDue();
        CHECK(early.receive().empty());
        publisher.setNow(40.2001);
        publisher.flushDue();
        std::vector<std::string> got;
        for (std::string packet = early.receive(); !packet.empty(); packet = early.receive()) {
            got.push_back(packet.substr(0, packet.find('\0')));
        }
        CHECK(got == std::vector<std::string>{"/takt4/bpm", "/takt4/confidence", "/takt4/locked",
                                              "/takt4/meter", "/takt4/beat", "/takt4/beat/bar",
                                              "/takt4/downbeat"});
    }

    SECTION("flushing sends everything held, whatever it was waiting for") {
        publisher.setNow(50.0);
        publisher.sendAddressTo(takt4::output::kAllOutputs, "/held", std::optional<double>{51.0});
        REQUIRE(publisher.pending() == 1);
        publisher.flushAll();
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

    SECTION("and a total that comes out negative is measured from the beat's own moment") {
        // +100 ms of its own against −250 ms for the rig is −150 ms: a beat at 10.5, fired at
        // 10.0, is 10.35.
        publisher.setOffsetSeconds(-0.250);
        publisher.setNow(10.0);
        publisher.sendAddressTo(takt4::output::kAllOutputs, "/cue", std::optional<double>{10.5});
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
        rig.setOffsetSeconds(-0.150);
        rig.setNow(0.0);
        rig.sendAddressTo(takt4::output::kAllOutputs, "/cue", std::optional<double>{0.5});
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

TEST_CASE("an edit to the targets keeps what is queued for the ones that stay", "[output][osc]") {
    // The audit's H12. Every output edit cleared every sender and built them all again, which
    // dropped whatever was queued for a delayed target — a release among it, so a Resolume clip
    // was left latched by somebody adding an output on another row.
    LoopbackReceiver media;
    LoopbackReceiver other;
    OscPublisher publisher;
    publisher.setTargets({{"o-000000a1", "127.0.0.1", media.port(), 0, 0.5}});
    publisher.setNow(0.0);
    publisher.sendAddressTo(takt4::output::kAllOutputs, "/held", std::int32_t{0}, 0.0);
    REQUIRE(publisher.pending() == 1);
    const OscSender* const sender = &publisher.target(0);

    SECTION("another output added above it, and this one untouched") {
        publisher.setTargets({{"o-000000b2", "127.0.0.1", other.port(), 0, 0.0},
                              {"o-000000a1", "127.0.0.1", media.port(), 1, 0.5}});
        CHECK(publisher.pending() == 1);
        // The very same sender, not a new one on the same address.
        CHECK(&publisher.target(1) == sender);
        publisher.setNow(0.6);
        publisher.flushDue();
        CHECK(media.receive().find("/held") != std::string::npos);
        CHECK(other.receive().empty());
    }

    SECTION("its own address edited: a new sender, and what is queued goes to where it is now") {
        publisher.setTargets({{"o-000000a1", "127.0.0.1", other.port(), 0, 0.5}});
        CHECK(publisher.pending() == 1);
        publisher.setNow(0.6);
        publisher.flushDue();
        CHECK(other.receive().find("/held") != std::string::npos);
    }

    SECTION("deleted, or switched off: what was queued for it goes nowhere") {
        publisher.setTargets({{"o-000000b2", "127.0.0.1", other.port(), 0, 0.0}});
        CHECK(publisher.pending() == 0);
        publisher.setNow(0.6);
        publisher.flushDue();
        CHECK(media.receive().empty());
        CHECK(other.receive().empty());
    }
}

TEST_CASE("a target that takes only its rules is sent nothing of the namespace", "[output][osc]") {
    // The operator, 2026-10-01: a robot's EGM bridge logs "Unhandled OSC address: /takt4/bpm" for
    // what takt4 sends of its own accord. `OutputTarget::sendsNamespace` off keeps the namespace
    // from that target, and keeps nothing else from it.
    LoopbackReceiver deck;
    LoopbackReceiver robot;
    OscPublisher publisher;
    constexpr std::size_t kRobotBit = 1;
    const auto targets = [&](bool robotTakesNamespace, double robotDelay) {
        return std::vector<OscPublisher::TargetSpec>{
            {"o-000000a1", "127.0.0.1", deck.port(), 0, 0.0},
            {"o-000000b2", "127.0.0.1", robot.port(), kRobotBit, robotDelay, robotTakesNamespace}};
    };
    const auto addresses = [](LoopbackReceiver& receiver) {
        std::vector<std::string> got;
        for (std::string datagram = receiver.receive(); !datagram.empty();
             datagram = receiver.receive()) {
            got.push_back(datagram.substr(0, datagram.find('\0')));
        }
        return got;
    };
    const auto namespaced = [](const std::vector<std::string>& got) {
        return std::count_if(got.begin(), got.end(),
                             [](const std::string& a) { return a.starts_with("/takt4"); });
    };
    REQUIRE(publisher.setTargets(targets(false, 0.0)).empty());

    takt4::tracking::BeatEvent event;
    event.bpm = 128.0;
    event.confidence = 0.75;
    event.locked = true;
    event.beatsPerBar = 4;
    event.beatInBar = 1;
    event.downbeat = true;
    takt4::tracking::TempoState state; // what the beat said, as the next round has it
    state.bpm = event.bpm;
    state.confidence = event.confidence;
    state.locked = event.locked;
    state.beatsPerBar = event.beatsPerBar;

    publisher.setNow(1.0);
    publisher.publishBeat(event);
    publisher.publishResync();
    state.bpm = 131.0;
    publisher.publishState(state); // and `state` is what was last said from here on
    // A rule sent everywhere, and one aimed at the robot alone.
    publisher.sendAddress("/everywhere");
    publisher.sendAddressTo(std::uint64_t{1} << kRobotBit, "/robot/cue", std::int32_t{1});

    const std::vector<std::string> atDeck = addresses(deck);
    const std::vector<std::string> atRobot = addresses(robot);
    INFO("deck: " << atDeck.size() << " datagrams, robot: " << atRobot.size());
    CHECK(namespaced(atDeck) == 9); // the beat's seven, the resync, the tempo that moved
    CHECK(std::find(atDeck.begin(), atDeck.end(), "/everywhere") != atDeck.end());
    CHECK(std::find(atDeck.begin(), atDeck.end(), "/robot/cue") == atDeck.end());
    CHECK(atRobot == std::vector<std::string>{"/everywhere", "/robot/cue"});

    SECTION("switched off with a beat held for its delay: the beat is dropped, its cue is not") {
        REQUIRE(publisher.setTargets(targets(true, 0.3)).empty());
        publisher.setNow(2.0);
        publisher.publishBeat(event);
        publisher.sendAddressTo(std::uint64_t{1} << kRobotBit, "/robot/cue", std::int32_t{1});
        REQUIRE(publisher.pending() == 8); // the beat's seven and the cue, all the robot's
        REQUIRE(publisher.setTargets(targets(false, 0.3)).empty());
        CHECK(publisher.pending() == 1);
        publisher.setNow(2.4);
        publisher.flushDue();
        CHECK(addresses(robot) == std::vector<std::string>{"/robot/cue"});
    }

    SECTION("switched back on, it is told the state at once, as a new target is") {
        // Nothing has moved since the state last said, so the deck is sent nothing — and the robot
        // has never been told any of it, so it is.
        publisher.setNow(3.0);
        publisher.publishState(state);
        CHECK(addresses(deck).empty());
        REQUIRE(publisher.setTargets(targets(true, 0.0)).empty());
        publisher.publishState(state);
        const std::vector<std::string> told = addresses(robot);
        CHECK(std::find(told.begin(), told.end(), "/takt4/bpm") != told.end());
        CHECK(std::find(told.begin(), told.end(), "/takt4/locked") != told.end());
    }
}

TEST_CASE("the tracker finding the beat again sends a resync", "[output][osc]") {
    // The audit's M8: `/takt4/resync` was documented and never sent. It means "the tracker has
    // just re-found itself", which is the published lock going from off back to on.
    LoopbackReceiver receiver;
    OscPublisher publisher;
    publisher.addTarget("127.0.0.1", receiver.port(), 0);
    const auto received = [&receiver] {
        std::vector<std::string> got;
        for (std::string datagram = receiver.receive(); !datagram.empty();
             datagram = receiver.receive()) {
            got.push_back(datagram);
        }
        return got;
    };
    const auto resyncs = [](const std::vector<std::string>& datagrams) {
        return std::count_if(datagrams.begin(), datagrams.end(), [](const std::string& d) {
            return d.find("/takt4/resync") != std::string::npos;
        });
    };
    takt4::tracking::TempoState state;
    state.bpm = 128.0;
    state.locked = false;
    publisher.publishState(state);
    CHECK(resyncs(received()) == 0);

    state.locked = true; // found
    publisher.publishState(state);
    CHECK(resyncs(received()) == 1);
    publisher.publishState(state); // still locked: nothing new
    CHECK(resyncs(received()) == 0);

    state.locked = false; // lost
    publisher.publishState(state);
    state.locked = true; // and found again
    publisher.publishState(state);
    CHECK(resyncs(received()) == 1);

    SECTION("a target added mid-lock is told the state, not a resync") {
        publisher.clearTargets();
        publisher.addTarget("127.0.0.1", receiver.port(), 0);
        publisher.publishState(state);
        CHECK(resyncs(received()) == 0);
    }
}

TEST_CASE("an OSC target past the 64th is sent what goes everywhere and nothing routed",
          "[output][osc]") {
    // A rule's routing is a bit per output, 64 of them. A target past the 64th was given every
    // bit, so every rule routed to any output reached it too.
    LoopbackReceiver third;
    LoopbackReceiver past;
    OscPublisher publisher;
    publisher.addTarget("127.0.0.1", third.port(), 3, 0.0);
    publisher.addTarget("127.0.0.1", past.port(), 64, 0.0);
    publisher.sendAddressTo(std::uint64_t{1} << 3, "/routed", 1);
    CHECK_FALSE(third.receive().empty());
    CHECK(past.receive().empty());
    publisher.sendAddressTo(takt4::output::kAllOutputs, "/everywhere", 1);
    CHECK_FALSE(third.receive().empty());
    CHECK_FALSE(past.receive().empty());
}
