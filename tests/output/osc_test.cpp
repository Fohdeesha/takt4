#include "core/net/resolver.hpp"
#include "core/net/udp.hpp"
#include "core/output/osc_message.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/output/osc_sender.hpp"
#include "core/output/output_target.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include "support/loopback_receiver.hpp"
#include "support/osc_messages.hpp"

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
        // resolver's timeout. Meanwhile nothing is sent and it is counted.
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

TEST_CASE("a datagram is stamped when it arrived, not when the test read it",
          "[output][osc][realtime]") {
    // What the [realtime] tests measure a cue's moment by: the kernel's stamp, so a reading
    // thread the machine held up does not make a cue on time read as late. Windows stamps
    // nothing, and there the receiver reads the time as it reads the datagram.
#if defined(_WIN32)
    SKIP("Windows gives a datagram no stamp of its arrival");
#else
    LoopbackReceiver receiver;
    OscSender sender("127.0.0.1", receiver.port());
    OscMessage message("/takt4/beat");
    message.addInt(1);
    const auto sent = std::chrono::steady_clock::now();
    REQUIRE(sender.send(message.packet()));
    std::this_thread::sleep_for(std::chrono::milliseconds(80)); // a reader held up
    const LoopbackReceiver::Stamped got = receiver.receiveStamped();
    const auto read = std::chrono::steady_clock::now();
    REQUIRE(got.datagram == bytesOf(message));
    const auto ms = [](std::chrono::steady_clock::duration span) {
        return std::chrono::duration<double, std::milli>(span).count();
    };
    INFO("arrived " << ms(got.at - sent) << " ms after the send, read " << ms(read - got.at)
                    << " ms after it arrived");
    CHECK(got.at >= sent - std::chrono::milliseconds(2)); // the clocks agree to a stamp's grain
    CHECK(got.at - sent < std::chrono::milliseconds(20));
    CHECK(read - got.at >= std::chrono::milliseconds(70));
#endif
}

TEST_CASE("an output aimed at a name follows the name to a new address", "[output][osc][net]") {
    // A name is looked up again every half minute (`net::AsyncAddress`), so a media server that
    // comes back from a restart on a new DHCP address is found there — but the sender stopped
    // asking once its socket was open, and went on sending to the old address with no error
    // until the output was edited. Here the name is the test's own, moved from one loopback
    // address to another on the same port, and looked up again a twentieth of a second apart.
    if (!LoopbackReceiver::canBind(0x7F000002u)) {
        SKIP("127.0.0.2 is not a loopback address on this machine (macOS has 127.0.0.1 alone)");
    }
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

namespace {

/// The IPv4 address an `AsyncAddress` has, in host order, or 0 with none (or an IPv6 one).
std::uint32_t ipv4Of(const std::optional<takt4::net::AsyncAddress::Address>& address) {
    if (!address || address->family != AF_INET) {
        return 0;
    }
    sockaddr_in v4{};
    std::memcpy(&v4, address->storage, sizeof v4);
    return ntohl(v4.sin_addr.s_addr);
}

} // namespace

TEST_CASE("a name with several addresses keeps the one it has while the name still answers it, "
          "and moves when the name does",
          "[output][osc][net]") {
    // A name server hands a name's addresses back in any order. Taking the first IPv4 one each
    // time moved a node from one address to another at every half-minute refresh.
    using takt4::net::AsyncAddress;
    AsyncAddress::answerForTests("two-cards.test", "127.0.0.1,127.0.0.2");
    AsyncAddress where("two-cards.test", 9000, 0.05);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!where.address() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    REQUIRE(ipv4Of(where.address()) == 0x7F000001u);

    // The same two, the other way round: refreshed many times, and kept.
    AsyncAddress::answerForTests("two-cards.test", "127.0.0.2,127.0.0.1");
    const std::uint64_t asked = where.lookUps();
    const auto watch = std::chrono::steady_clock::now() + std::chrono::milliseconds{600};
    while (std::chrono::steady_clock::now() < watch) {
        CHECK(ipv4Of(where.address()) == 0x7F000001u);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    CHECK(where.lookUps() >= asked + 3);

    // The name moved for real: followed.
    AsyncAddress::answerForTests("two-cards.test", "127.0.0.2");
    const auto moved = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (ipv4Of(where.address()) != 0x7F000002u && std::chrono::steady_clock::now() < moved) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    CHECK(ipv4Of(where.address()) == 0x7F000002u);
}

TEST_CASE("a name the name server does not answer for is said to be after a while, rather than "
          "shown as the ordinary wait every named output has at launch",
          "[output][osc][net]") {
    using takt4::net::AsyncAddress;
    AsyncAddress::answerForTests("slow-server.test", "127.0.0.1", 0.8);
    AsyncAddress where("slow-server.test", 9000, AsyncAddress::kRefreshSeconds, 0.2);
    CHECK(where.problem() == "looking up slow-server.test");
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
    const std::string slow = where.problem();
    INFO(slow);
    // Not "looking up …", which the outputs' list passes over as that ordinary wait.
    CHECK(slow.rfind("no answer about slow-server.test after ", 0) == 0);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!where.address() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    CHECK(where.problem().empty());
}

TEST_CASE("a sending socket never waits", "[output][osc]") {
    // an earlier review: "a single non-blocking sendto", said the senders, of sockets that were
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
#if defined(__APPLE__)
    // A permission refused would say "permission denied", which is what this is here to catch.
    if (!sent && sender.problem().find("no route") != std::string::npos) {
        SKIP("this Mac's network has no route for the limited broadcast: " + sender.problem());
    }
#endif
    CHECK(sent);
    CHECK(sender.failed() == 0);
    CHECK(sender.problem().empty());
}

TEST_CASE("the generic namespace is published as the design 5.6 specifies", "[output][osc]") {
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
    for (int i = 0; i < 8; ++i) {
        const std::string packet = receiver.receive();
        REQUIRE_FALSE(packet.empty());
        addresses.push_back(packet.substr(0, packet.find('\0')));
    }
    CHECK(addresses == std::vector<std::string>{"/takt4/bpm", "/takt4/confidence", "/takt4/locked",
                                                "/takt4/meter", "/takt4/beat", "/takt4/beat/bar",
                                                "/takt4/downbeat", "/takt4/beat/timing"});
    CHECK(publisher.messagesSent() == 8);
    CHECK(publisher.messagesFailed() == 0);

    SECTION("state that has not changed is not resent between beats") {
        takt4::tracking::TempoState state;
        state.bpm = 128.0;
        state.confidence = 0.75;
        state.locked = true;
        state.beatsPerBar = 4;
        publisher.publishState(state);
        CHECK(publisher.messagesSent() == 8);

        state.confidence = 0.20;
        publisher.publishState(state);
        CHECK(publisher.messagesSent() == 9);
        CHECK(receiver.receive().substr(0, 17) == "/takt4/confidence");

        state.locked = false;
        publisher.publishState(state);
        CHECK(publisher.messagesSent() == 10);
        CHECK(receiver.receive().substr(0, 13) == "/takt4/locked");
    }

    SECTION("a beat before any downbeat carries no bar position") {
        takt4::tracking::BeatEvent early = event;
        early.downbeat = false;
        early.beatInBar = 0;
        publisher.publishBeat(early);
        // Every beat repeats the state, so a consumer that started late is right again
        // within a beat: four state messages, the beat and its timing, and nothing else.
        CHECK(publisher.messagesSent() == 14);
        std::vector<std::string> after;
        for (int i = 0; i < 6; ++i) {
            const std::string packet = receiver.receive();
            REQUIRE_FALSE(packet.empty());
            after.push_back(packet.substr(0, packet.find('\0')));
        }
        CHECK(after == std::vector<std::string>{"/takt4/bpm", "/takt4/confidence", "/takt4/locked",
                                                "/takt4/meter", "/takt4/beat",
                                                "/takt4/beat/timing"});
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
    // the per-target latency, on real sockets. The thing this exists for is a rig whose
    // destinations do not share a lag — a media server a frame behind and a winch that has
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
    // can a target's not? It can — and since an earlier review it means what it says. The output
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
        CHECK(publisher.pending() == 8);
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
                                              "/takt4/downbeat", "/takt4/beat/timing"});
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
    // the slider and the per-target one are the same quantity measured from two places
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
    target.name = "winch";
    target.host = "10.0.0.7";
    target.port = 7000;
    target.delaySeconds = 0.352;

    const std::string text = takt4::output::formatOutputTarget(target);
    CHECK(text == "winch = 10.0.0.7:7000 +352ms");

    takt4::output::OutputTarget back;
    REQUIRE(takt4::output::parseOutputTarget(text, back));
    CHECK(back.name == "winch");
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
    // an earlier review. Every output edit cleared every sender and built them all again, which
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
    // The operator, 2026-10-01: a receiver logs "Unhandled OSC address: /takt4/bpm" for
    // what takt4 sends of its own accord. `OutputTarget::sendsNamespace` off keeps the namespace
    // from that target, and keeps nothing else from it.
    LoopbackReceiver deck;
    LoopbackReceiver winch;
    OscPublisher publisher;
    constexpr std::size_t kWinchBit = 1;
    const auto targets = [&](bool winchTakesNamespace, double winchDelay) {
        return std::vector<OscPublisher::TargetSpec>{
            {"o-000000a1", "127.0.0.1", deck.port(), 0, 0.0},
            {"o-000000b2", "127.0.0.1", winch.port(), kWinchBit, winchDelay, winchTakesNamespace}};
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
    // A rule sent everywhere, and one aimed at the winch alone.
    publisher.sendAddress("/everywhere");
    publisher.sendAddressTo(std::uint64_t{1} << kWinchBit, "/winch/cue", std::int32_t{1});

    const std::vector<std::string> atDeck = addresses(deck);
    const std::vector<std::string> atWinch = addresses(winch);
    INFO("deck: " << atDeck.size() << " datagrams, winch: " << atWinch.size());
    CHECK(namespaced(atDeck) == 10); // the beat's eight, the resync, the tempo that moved
    CHECK(std::find(atDeck.begin(), atDeck.end(), "/everywhere") != atDeck.end());
    CHECK(std::find(atDeck.begin(), atDeck.end(), "/winch/cue") == atDeck.end());
    CHECK(atWinch == std::vector<std::string>{"/everywhere", "/winch/cue"});

    SECTION("switched off with a beat held for its delay: the beat is dropped, its cue is not") {
        REQUIRE(publisher.setTargets(targets(true, 0.3)).empty());
        publisher.setNow(2.0);
        publisher.publishBeat(event);
        publisher.sendAddressTo(std::uint64_t{1} << kWinchBit, "/winch/cue", std::int32_t{1});
        REQUIRE(publisher.pending() == 9); // the beat's eight and the cue, all the winch's
        REQUIRE(publisher.setTargets(targets(false, 0.3)).empty());
        CHECK(publisher.pending() == 1);
        publisher.setNow(2.4);
        publisher.flushDue();
        CHECK(addresses(winch) == std::vector<std::string>{"/winch/cue"});
    }

    SECTION("switched back on, it is told the state at once, as a new target is") {
        // Nothing has moved since the state last said, so the deck is sent nothing — and the winch
        // has never been told any of it, so it is.
        publisher.setNow(3.0);
        publisher.publishState(state);
        CHECK(addresses(deck).empty());
        REQUIRE(publisher.setTargets(targets(true, 0.0)).empty());
        publisher.publishState(state);
        const std::vector<std::string> told = addresses(winch);
        CHECK(std::find(told.begin(), told.end(), "/takt4/bpm") != told.end());
        CHECK(std::find(told.begin(), told.end(), "/takt4/locked") != told.end());
    }
}

TEST_CASE("the tracker finding the beat again sends a resync", "[output][osc]") {
    // an earlier review: `/takt4/resync` was documented and never sent. It means "the tracker has
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
    publisher.publishState(state); // hunting: nothing at all goes out before a lock
    CHECK(received().empty());

    // Found for the first time: told, and no resync — nothing a receiver had was lost.
    state.locked = true;
    publisher.publishState(state);
    CHECK(resyncs(received()) == 0);
    publisher.publishState(state); // still locked: nothing new
    CHECK(received().empty());

    state.locked = false; // lost: /locked 0
    publisher.publishState(state);
    state.locked = true; // and found again
    publisher.publishState(state);
    CHECK(resyncs(received()) == 1);

    // A run stopped, which says /locked 0 too, and its next lock: a resync.
    publisher.publishState(state, /*running=*/false);
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

TEST_CASE("a beat's timing says how long ago it landed and how long until the next, as each "
          "output's copy is sent",
          "[output][osc][remote]") {
    // For a show controller that schedules from takt4's own phase and tempo, so a datagram that is
    // late, or lost, costs nothing. The instant is the beat's moment and the rig's latency — where
    // takt4's operator means it to land — so an output's own delay shows in it, with any lateness
    // (a show controller's answer of 2026-10-22).
    LoopbackReceiver prompt;
    LoopbackReceiver slow;
    OscPublisher publisher;
    publisher.addTarget("127.0.0.1", prompt.port(), 0);
    publisher.addTarget("127.0.0.1", slow.port(), 1, 0.2);
    takt4::tracking::BeatEvent event;
    event.bpm = 120.0;
    event.confidence = 0.9;
    event.locked = true;
    event.beatsPerBar = 4;
    event.beatInBar = 2;
    const auto timingIn = [](LoopbackReceiver& receiver) {
        std::optional<takt4::testing::OscMsg> timing;
        for (std::string datagram = receiver.receive(); !datagram.empty();
             datagram = receiver.receive()) {
            const std::vector<takt4::testing::OscMsg> messages =
                takt4::testing::oscMessagesIn(datagram);
            if (!messages.empty() && messages[0].address == "/takt4/beat/timing") {
                timing = messages[0];
            }
        }
        return timing;
    };

    // A beat at 10 s, fired 50 ms after it.
    publisher.setNow(10.05);
    publisher.publishBeat(event, 10.0, 41);
    const std::optional<takt4::testing::OscMsg> now = timingIn(prompt);
    REQUIRE(now.has_value());
    REQUIRE(now->args.size() == 3);
    CHECK(now->args[0].type == 'f');
    CHECK(now->args[0].f == Catch::Approx(50.0f).margin(0.01));
    CHECK(now->args[1].type == 'f');
    CHECK(now->args[1].f == Catch::Approx(450.0f).margin(0.01));
    CHECK(now->args[2].type == 'i');
    CHECK(now->args[2].i == 41);

    // The slow output's copy waits out its 200 ms and says what is true when it goes — worked out
    // then, not when the beat was fired.
    CHECK_FALSE(timingIn(slow).has_value());
    publisher.setNow(10.26);
    publisher.flushDue();
    const std::optional<takt4::testing::OscMsg> held = timingIn(slow);
    REQUIRE(held.has_value());
    CHECK(held->args[0].f == Catch::Approx(260.0f).margin(0.01));
    CHECK(held->args[1].f == Catch::Approx(240.0f).margin(0.01));
    CHECK(held->args[2].i == 41);

    SECTION("the rig's latency moves the instant the beat is meant to land") {
        publisher.setOffsetSeconds(0.1);
        publisher.setNow(20.15);
        publisher.publishBeat(event, 20.0, 42);
        const std::optional<takt4::testing::OscMsg> late = timingIn(prompt);
        REQUIRE(late.has_value());
        CHECK(late->args[0].f == Catch::Approx(50.0f).margin(0.01));
        CHECK(late->args[1].f == Catch::Approx(450.0f).margin(0.01));
        CHECK(late->args[2].i == 42);
    }

    SECTION("a beat with no tempo has no next beat to say, and goes without its timing") {
        event.bpm = 0.0;
        publisher.setNow(30.0);
        publisher.publishBeat(event, 30.0, 43);
        CHECK_FALSE(timingIn(prompt).has_value());
    }
}
