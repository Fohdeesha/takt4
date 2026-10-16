#include "core/net/udp.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/output_target.hpp"
#include "core/output/transports.hpp"
#include "core/sandbox.hpp"
#include "core/timing/beat_grid.hpp"
#include "core/timing/external_beats.hpp"
#include "core/timing/prolink/network.hpp"
#include "core/timing/prolink/packets.hpp"
#include "core/timing/prolink_follower.hpp"
#include "core/trigger/rule.hpp"

#include "support/loopback_receiver.hpp"
#include "support/prolink_capture.hpp"
#include "support/prolink_peer.hpp"
#include "support/prolink_sim.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using takt4::testing::ipv4;
using takt4::testing::LoopbackReceiver;
using namespace takt4::timing::prolink;
namespace sim = takt4::testing::prolink_sim;
using namespace std::chrono_literals;

// The Pro DJ Link network on real sockets, against devices simulated on the loopback: a player
// and a mixer at 127.0.0.2, takt4 at 127.0.0.1. The sandbox lets takt4 bind only ports a test's
// own receiver was given and send only to such ports on the loopback, so these run in every
// suite; the one test on Pro DJ Link's own ports is [network].

using namespace takt4::testing::prolink_peer;

TEST_CASE("the CDJ network on sockets: listening lists the devices and sends nothing; joining "
          "claims a number and keeps alive; the followed player's beats arrive stamped on the "
          "clock; leaving stops the keep-alives",
          "[prolink][slow]") {
    if (!canUseSecondLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2");
    }
    SimulatedRig rig;
    const Network::Config config = testConfig(rig.announcePort(), steadyMicros);
    Network network(config);
    std::string problem;
    REQUIRE(network.open(problem));
    CHECK(problem.empty());
    CHECK(network.isOpen());
    rig.start(config);

    // Listening: both devices listed, and not one datagram sent to them.
    REQUIRE(eventually([&] { return network.snapshot().devices.size() == 2; }, 3s));
    const Network::Snapshot listening = network.snapshot();
    CHECK(listening.open);
    CHECK(listening.phase == VirtualPlayer::Phase::Listening);
    CHECK(listening.devices[0].number == 2);
    CHECK(listening.devices[0].kind == DeviceKind::Player);
    CHECK(listening.devices[0].name == "CDJ-3000");
    CHECK(listening.devices[0].ip == kDevices);
    CHECK(listening.devices[1].number == 33);
    CHECK(listening.devices[1].kind == DeviceKind::Mixer);
    CHECK_FALSE(rig.announceSocket().receive(400).has_value());

    // Joined: the claim and the keep-alives reach the devices, from takt4's address.
    network.join();
    std::vector<AnnouncePacket> heard;
    std::uint32_t from = 0;
    const auto until = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < until) {
        const auto datagram = rig.announceSocket().receive(100, &from);
        if (!datagram) {
            continue;
        }
        AnnouncePacket packet;
        REQUIRE(parseAnnounce(*datagram, packet));
        CHECK(from == kHere);
        heard.push_back(packet);
        if (packet.type == Announce::KeepAlive && heard.size() > 14) {
            break;
        }
    }
    REQUIRE(heard.size() >= 13);
    CHECK(heard[0].type == Announce::Hello);
    CHECK(heard[3].type == Announce::FirstClaim);
    CHECK(heard[6].type == Announce::SecondClaim);
    CHECK(heard[6].device == 7);
    CHECK(heard[6].ip == kHere);
    CHECK(heard[9].type == Announce::FinalClaim);
    CHECK(heard.back().type == Announce::KeepAlive);
    CHECK(heard.back().device == 7);
    CHECK(eventually([&] { return network.snapshot().phase == VirtualPlayer::Phase::Joined; }, 1s));
    CHECK(network.snapshot().number == 7);
    CHECK(network.snapshot().interfaceIp == kHere);

    // The player plays, and is master: its beats arrive, stamped as they came.
    rig.setPlaying(true);
    std::vector<VirtualPlayer::Heard> beats;
    const auto listenUntil = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < listenUntil) {
        VirtualPlayer::Heard beat;
        while (network.popBeat(beat)) {
            beats.push_back(beat);
        }
        std::this_thread::sleep_for(2ms);
    }
    rig.setPlaying(false);
    const std::vector<std::int64_t> sent = rig.beatsSent();
    REQUIRE(beats.size() >= 5);
    CHECK(network.snapshot().master == 2);
    CHECK(network.snapshot().followed == 2);
    // Each beat heard is one sent, a loopback hop and the thread's wake-up after it. The first
    // may have gone before a status said whom to follow, and one more after the listening ended.
    CHECK(sent.size() <= beats.size() + 2);
    for (const VirtualPlayer::Heard& beat : beats) {
        INFO("a beat heard at " << beat.at);
        std::size_t matches = 0;
        for (const std::int64_t at : sent) {
            const std::int64_t lag = beat.at - at;
            matches += lag >= 0 && lag < 15'000 ? 1u : 0u;
        }
        CHECK(matches == 1);
        CHECK(beat.device == 2);
        CHECK(beat.bpm > 127.9);
        CHECK(beat.bpm < 128.1);
    }
    CHECK(eventually([&] { return network.snapshot().beatsHeard >= beats.size(); }, 1s));

    // Left: the keep-alives stop.
    network.leave();
    std::this_thread::sleep_for(200ms);
    while (rig.announceSocket().receive(10).has_value()) {
    }
    CHECK_FALSE(rig.announceSocket().receive(600).has_value());
    CHECK(network.snapshot().phase == VirtualPlayer::Phase::Listening);

    rig.stop();
    network.close();
    CHECK_FALSE(network.isOpen());
    CHECK_FALSE(network.snapshot().open);
}

TEST_CASE("in the test sandbox the CDJs' own port is not listened on, and it says so",
          "[prolink]") {
    REQUIRE(takt4::sandbox::active());
    const std::uint64_t before = takt4::sandbox::refusals(takt4::sandbox::Refused::Bind);
    Network::Config config;
    config.clock = steadyMicros;
    Network network(config);
    std::string problem;
    CHECK_FALSE(network.open(problem));
    CHECK(problem.find("test sandbox") != std::string::npos);
    CHECK_FALSE(network.isOpen());
    CHECK(network.snapshot().problem == problem);
    CHECK(takt4::sandbox::refusals(takt4::sandbox::Refused::Bind) == before + 1);
}

TEST_CASE("a port another program holds is said to be in use, and rekordbox is named as the "
          "likely holder",
          "[prolink]") {
    UdpPeer holder(0); // every address, as rekordbox binds it
    Network::Config config = testConfig(1, steadyMicros);
    config.own.announce = holder.port();
    Network network(config);
    std::string problem;
    CHECK_FALSE(network.open(problem));
    INFO(problem);
    CHECK(problem.find("in use by another program") != std::string::npos);
    CHECK(problem.find("rekordbox") != std::string::npos);
}

TEST_CASE("with no card on the CDJs' network, joining says so, and with one picked that is not "
          "here, says that",
          "[prolink][slow]") {
    if (!canUseSecondLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2");
    }
    SimulatedRig rig;
    Network::Config config = testConfig(rig.announcePort(), steadyMicros);
    config.interfaces = [] {
        Interface card;
        card.name = "elsewhere";
        card.ip = ipv4(10, 1, 1, 1);
        card.mask = 0xffff0000u;
        card.broadcast = ipv4(10, 1, 255, 255);
        return std::vector<Interface>{card};
    };
    Network network(config);
    std::string problem;
    REQUIRE(network.open(problem));
    rig.start(config);
    REQUIRE(eventually([&] { return !network.snapshot().devices.empty(); }, 3s));
    network.join();
    CHECK(eventually(
        [&] {
            return network.snapshot().problem ==
                   "no network card here is on the CDJs' network (127.0.0.2)";
        },
        2s));
    CHECK(network.snapshot().phase == VirtualPlayer::Phase::Listening);
    CHECK_FALSE(rig.announceSocket().receive(300).has_value());

    // Left — STOP — and why it could not join is no longer said: it is not trying to.
    network.leave();
    CHECK(eventually([&] { return network.snapshot().problem.empty(); }, 1s));
    network.setInterface(ipv4(10, 9, 9, 9));
    network.join();
    CHECK(eventually(
        [&] {
            return network.snapshot().problem ==
                   "no network card here has the address 10.9.9.9 picked for the CDJs";
        },
        2s));
    rig.stop();
}

TEST_CASE("the CDJs followed through the whole chain on sockets: each beat a player sends fires a "
          "rule once, with its own place in the bar, on time",
          "[prolink][follow][slow]") {
    if (!canUseSecondLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2");
    }
    LoopbackReceiver media;
    takt4::output::Transports::Config outputs;
    takt4::output::OutputTarget server;
    server.name = "media";
    server.host = "127.0.0.1";
    server.port = media.port();
    outputs.outputs = {server};
    takt4::timing::ExternalBeats beats;
    takt4::output::OutputRunner runner(beats, outputs);
    takt4::trigger::Rule::Config rule;
    rule.id = "position";
    rule.trigger = takt4::trigger::Trigger::Beat;
    rule.address = "/cue";
    rule.value = takt4::trigger::Generator::Config{};
    rule.value.kind = takt4::trigger::GeneratorKind::Live;
    rule.value.source = takt4::trigger::LiveSource::BeatInBar;
    runner.post(takt4::output::OutputCommand::rules({rule}));
    REQUIRE(runner.triggers().rule(0).valid());
    const auto linkNow = [&runner] { return runner.transports().link().now().count(); };

    SimulatedRig rig;
    const Network::Config config = testConfig(rig.announcePort(), linkNow);
    Network network(config);
    std::string problem;
    REQUIRE(network.open(problem));
    takt4::timing::ProLinkFollower follower(network);
    struct StopFirst {
        takt4::timing::ExternalBeats& beats;
        ~StopFirst() { beats.stop(); }
    } stopFirst{beats};

    struct Cue {
        std::int64_t at = 0;
        int bar = 0;
    };
    std::mutex mutex;
    std::vector<Cue> cues;
    std::atomic<bool> listening{true};
    std::thread listener([&] {
        while (listening.load()) {
            const std::string datagram = media.receive();
            if (datagram.rfind("/cue", 0) == 0 && datagram.size() >= 4) {
                const auto byte = [&](std::size_t at) {
                    return static_cast<int>(static_cast<std::uint8_t>(datagram[at]));
                };
                const std::size_t at = datagram.size() - 4;
                const int value =
                    (byte(at) << 24) | (byte(at + 1) << 16) | (byte(at + 2) << 8) | byte(at + 3);
                const std::lock_guard<std::mutex> lock(mutex);
                cues.push_back({steadyMicros(), value});
            }
        }
    });

    rig.start(config);
    REQUIRE(eventually([&] { return network.snapshot().devices.size() == 2; }, 3s));
    runner.start();
    runner.setTracking(true);
    beats.start(follower, takt4::timing::BeatGrid::Options{4, 4.0});
    REQUIRE(
        eventually([&] { return network.snapshot().phase == VirtualPlayer::Phase::Joined; }, 3s));
    rig.setPlaying(true);
    std::this_thread::sleep_for(4s);
    rig.setPlaying(false);
    beats.stop();
    std::this_thread::sleep_for(200ms);
    runner.stop();
    listening.store(false);
    listener.join();
    rig.stop();

    const std::vector<std::int64_t> sent = rig.beatsSent();
    REQUIRE(sent.size() >= 7);
    REQUIRE(cues.size() >= 6);
    // Each cue on a beat the player sent, a hop and a round or two after it, with the bar the
    // player counted (1 2 3 4 from its first): no beat twice, none made up.
    for (const Cue& cue : cues) {
        INFO("a cue " << cue.bar << " at " << cue.at);
        std::size_t matches = 0;
        for (std::size_t i = 0; i < sent.size(); ++i) {
            const std::int64_t lag = cue.at - sent[i];
            if (lag >= 0 && lag < 25'000) {
                ++matches;
                CHECK(cue.bar == static_cast<int>(i % 4) + 1);
            }
        }
        CHECK(matches == 1);
    }
    CHECK(runner.errors() == 0);
    CHECK(follower.status().device == 0); // ended
}

TEST_CASE("the CDJ network on Pro DJ Link's own ports, 50000 to 50002: bound, heard, joined, and "
          "let go of",
          "[prolink][network]") {
    // Everything else here binds ports the sandbox handed out. This binds the real three — what a
    // firewall, or rekordbox running beside takt4, would get in the way of — with the devices
    // still on the loopback, so nothing reaches the network outside this machine.
    if (!canUseSecondLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2");
    }
    SimulatedRig rig;
    Network::Config config = testConfig(rig.announcePort(), steadyMicros);
    config.own = Network::Ports{};
    REQUIRE(config.own.announce == 50000);
    Network network(config);
    std::string problem;
    const bool opened = network.open(problem);
    INFO(problem);
    REQUIRE(opened);
    rig.start(config);
    REQUIRE(eventually([&] { return network.snapshot().devices.size() == 2; }, 3s));
    network.join();
    REQUIRE(
        eventually([&] { return network.snapshot().phase == VirtualPlayer::Phase::Joined; }, 4s));
    rig.setPlaying(true);
    REQUIRE(eventually(
        [&] {
            VirtualPlayer::Heard beat;
            return network.popBeat(beat) && beat.device == 2;
        },
        3s));
    rig.setPlaying(false);
    network.leave();
    rig.stop();
    network.close();
    // Let go of: a second network gets the same ports at once.
    Network again(config);
    CHECK(again.open(problem));
    again.close();
}
