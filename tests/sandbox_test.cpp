// The test binaries' sandbox (src/core/sandbox.hpp): what it refuses, and that it refuses it by
// the effect — a datagram that does not arrive, a port that is not bound, a session not joined —
// rather than by a flag. Untagged, so it runs where the sandbox is on; every other untagged test
// is relying on what this one proves.

#include "core/audio/devices.hpp"
#include "core/audio/input_stream.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/control/osc_receiver.hpp"
#include "core/dmx/artnet_sender.hpp"
#include "core/net/resolver.hpp"
#include "core/output/link_peers.hpp"
#include "core/output/link_session.hpp"
#include "core/output/midi_clock.hpp"
#include "core/output/midi_ports.hpp"
#include "core/output/osc_sender.hpp"
#include "core/sandbox.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using takt4::testing::LoopbackReceiver;

std::array<std::byte, 16> datagram() {
    std::array<std::byte, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(i + 1);
    }
    return bytes;
}

} // namespace

TEST_CASE("the sandbox is on for a test that is not tagged for the rig", "[sandbox]") {
    // Switched on before main and kept on by tests/support/rig_sandbox.cpp for an untagged test.
    // If this fails, every other untagged test may be reaching the rig.
    CHECK(takt4::sandbox::active());
}

TEST_CASE("the sandbox lets a datagram reach a test's own receiver and nothing else",
          "[sandbox][output]") {
    REQUIRE(takt4::sandbox::active());
    const auto bytes = datagram();

    SECTION("a receiver the test made is sent to") {
        LoopbackReceiver receiver;
        takt4::output::OscSender sender("127.0.0.1", receiver.port());
        CHECK(sender.send(bytes));
        CHECK(receiver.receive().size() == bytes.size());
    }

    SECTION("a port on this machine no test was given is not") {
        // A real socket, bound and listening, that the sandbox was not told about: what the
        // operator's own receiver on 9000 is, to a test.
        LoopbackReceiver stranger(false);
        const std::uint64_t before = takt4::sandbox::refusals();
        takt4::output::OscSender sender("127.0.0.1", stranger.port());
        // Reported as sent, so the code under test behaves as it would with the datagram lost.
        CHECK(sender.send(bytes));
        CHECK(sender.problem().empty());
        CHECK(stranger.receive().empty());
        CHECK(takt4::sandbox::refusals() == before + 1);
        CHECK(takt4::sandbox::lastRefused() == takt4::sandbox::Refused::Send);
        CHECK(takt4::sandbox::lastRefusedPort() == stranger.port());
    }

    SECTION("Art-Net is held to the same") {
        LoopbackReceiver node;
        LoopbackReceiver stranger(false);
        const std::vector<std::uint8_t> levels(512, 255);
        takt4::dmx::ArtNetSender allowed("127.0.0.1", node.port());
        takt4::dmx::ArtNetSender refused("127.0.0.1", stranger.port());
        CHECK(allowed.sendDmx(0, levels));
        CHECK(refused.sendDmx(0, levels));
        CHECK_FALSE(node.receive().empty());
        CHECK(stranger.receive().empty());
    }

    SECTION("an address off this machine is not") {
        const std::uint64_t before = takt4::sandbox::refusals();
        takt4::output::OscSender sender("192.0.2.1", 9000);
        CHECK(sender.send(bytes));
        CHECK(takt4::sandbox::refusals() == before + 1);
    }

    SECTION("a network that is down still fails the way a real one does") {
        // 0.0.0.0/8 is let through for the tests of a send that fails, because no system sends
        // there: the failure is the operating system's own.
        takt4::output::OscSender sender(takt4::testing::kUnsendableHost, 9000);
        CHECK_FALSE(sender.send(bytes));
        CHECK_FALSE(sender.problem().empty());
    }
}

TEST_CASE("the sandbox binds nothing a program on the rig may be listening on",
          "[sandbox][control]") {
    REQUIRE(takt4::sandbox::active());

    SECTION("takt4's own control port is refused") {
        const std::uint64_t before = takt4::sandbox::refusals();
        CHECK_THROWS_AS(takt4::control::OscReceiver(7001, true), std::runtime_error);
        CHECK(takt4::sandbox::refusals() == before + 1);
        CHECK(takt4::sandbox::lastRefused() == takt4::sandbox::Refused::Bind);
        // And the port really is free afterwards: a raw bind to it on the loopback succeeds —
        // unless the show's own takt4 is running, which is the case the refusal is for.
    }

    SECTION("any free port is taken, and it may then be sent to") {
        takt4::control::OscReceiver receiver(0, true);
        REQUIRE(receiver.port() != 0);
        const auto bytes = datagram();
        takt4::output::OscSender sender("127.0.0.1", receiver.port());
        CHECK(sender.send(bytes));
        CHECK(receiver.receive(std::chrono::milliseconds(500)).size() == bytes.size());
    }

    SECTION("a port a test's receiver was given may be listened on once it is free") {
        std::uint16_t free = 0;
        {
            const LoopbackReceiver probe;
            free = probe.port();
        }
        takt4::control::OscReceiver receiver(free, true);
        CHECK(receiver.port() == free);
    }
}

TEST_CASE("the sandbox joins no Link session and listens for none", "[sandbox][link]") {
    REQUIRE(takt4::sandbox::active());
    takt4::output::LinkSession link(120.0);
    link.enable(true);
    CHECK_FALSE(link.enabled());

    takt4::output::LinkPeerWatch watch;
    std::string problem;
    CHECK_FALSE(watch.open(problem));
    CHECK_FALSE(watch.isOpen());
    CHECK(problem.find("sandbox") != std::string::npos);
}

TEST_CASE("the sandbox opens no MIDI port, and still says when one is not there",
          "[sandbox][midi]") {
    REQUIRE(takt4::sandbox::active());
    // A port that is not there is reported in the operator's words, as before.
    try {
        takt4::output::MidiOutput missing("takt4 test - no such MIDI port exists");
        FAIL("a port that is not there opened");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()).find("no port matching") != std::string::npos);
    }

    const std::vector<std::string> ports = takt4::output::listMidiOutputPorts();
    if (ports.empty()) {
        SKIP("no MIDI output on this machine to be refused");
    }
    // Windows lists its GS Wavetable Synth on every machine, and it plays through the speakers.
    const std::uint64_t before = takt4::sandbox::refusals();
    try {
        takt4::output::MidiOutput real(ports.front());
        FAIL("a real MIDI port was opened in the sandbox: " << ports.front());
    } catch (const std::runtime_error& error) {
        INFO(error.what());
        CHECK(std::string(error.what()).find("sandbox") != std::string::npos);
    }
    CHECK(takt4::sandbox::refusals() == before + 1);
    CHECK(takt4::sandbox::lastRefused() == takt4::sandbox::Refused::Midi);
}

TEST_CASE("the sandbox opens no audio device", "[sandbox][audio]") {
    REQUIRE(takt4::sandbox::active());
    const takt4::audio::PortAudioSession session;
    const std::vector<takt4::audio::InputDevice> devices = takt4::audio::listInputDevices(session);
    if (devices.empty()) {
        SKIP("no audio input on this machine to be refused");
    }
    struct Nothing final : takt4::audio::HopProcessor {
        void processHop(const float*, std::uint64_t) noexcept override {}
    } nothing;
    const std::uint64_t before = takt4::sandbox::refusals();
    try {
        takt4::audio::InputStream stream(session, devices.front(), {}, nothing, {});
        FAIL("an audio device was opened in the sandbox: " << devices.front().name);
    } catch (const std::exception& error) {
        INFO(error.what());
        CHECK(std::string(error.what()).find("sandbox") != std::string::npos);
    }
    CHECK(takt4::sandbox::refusals() == before + 1);
    CHECK(takt4::sandbox::lastRefused() == takt4::sandbox::Refused::Audio);
}

TEST_CASE("the sandbox looks up no name but localhost", "[sandbox][net]") {
    // A host typed as a name was looked up by the tests that check an unknown one is reported
    // — a DNS query out of the rig's network card on every run (the audit of 2026-09-25, T4).
    // It fails here the way an unknown name fails, so those tests still see "cannot resolve".
    REQUIRE(takt4::sandbox::active());
    const std::uint64_t before = takt4::sandbox::refusals(takt4::sandbox::Refused::Lookup);
    takt4::net::AsyncAddress name("no.such.host.takt4.invalid", 9000);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (name.problem().rfind("cannot resolve", 0) != 0 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(name.problem().rfind("cannot resolve", 0) == 0);
    CHECK_FALSE(name.address().has_value());
    CHECK(takt4::sandbox::refusals(takt4::sandbox::Refused::Lookup) == before + 1);

    // localhost is the system's own answer and asks nobody, so it is let through.
    takt4::net::AsyncAddress local("LocalHost", 9000);
    const auto by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!local.address() && std::chrono::steady_clock::now() < by) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(local.address().has_value());
    CHECK(takt4::sandbox::refusals(takt4::sandbox::Refused::Lookup) == before + 1);

    // And a literal never needed looking up.
    takt4::net::AsyncAddress literal("127.0.0.1", 9000);
    CHECK(literal.address().has_value());
}
