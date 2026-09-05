#include "core/audio/rates.hpp"
#include "core/control/osc_control.hpp"
#include "core/control/osc_parse.hpp"
#include "core/control/osc_receiver.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/output/osc_message.hpp"
#include "core/output/osc_sender.hpp"
#include "core/tracking/state_space.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::control::OscControl;
using takt4::control::OscReceiver;
using takt4::engine::BeatEngine;
using takt4::output::OscMessage;
using takt4::output::OscSender;

namespace {

const std::filesystem::path kTestData{TAKT4_TEST_DATA_DIR};

const takt4::model::ModelWeights& weights() {
    static const takt4::model::ModelWeights loaded = takt4::model::ModelWeights::fromFile(
        std::filesystem::path(TAKT4_WEIGHTS_DIR) / "generic.bin");
    return loaded;
}

const takt4::tracking::StateSpaceModel& stateSpace() {
    static const takt4::tracking::StateSpaceModel loaded =
        takt4::tracking::StateSpaceModel::fromFile(std::filesystem::path(TAKT4_STATESPACE_DIR) /
                                                   "default.bin");
    return loaded;
}

std::unique_ptr<BeatEngine> makeEngine() {
    return std::make_unique<BeatEngine>(weights(), stateSpace());
}

/// Tracks the committed excerpt to a lock, so a halve has a real tempo to halve.
void trackUntilLocked(BeatEngine& engine) {
    static const std::vector<float> samples =
        takt4::io::readWavFile(kTestData / "features" / "synthetic.wav").samples;
    const std::size_t hops = samples.size() / takt4::audio::kHopSize;
    for (std::size_t hop = 0; hop < hops && !engine.state().locked; ++hop) {
        engine.processHop(samples.data() + hop * takt4::audio::kHopSize, hop);
        (void)engine.step();
    }
}

/// Port 0 asks the platform for a free one, which is the only way a test on a shared
/// runner cannot lose a race with whatever else is listening. `OscReceiver::port()` says
/// which it got.
constexpr std::uint16_t kAnyPort = 0;

OscControl::Config localConfig(std::uint16_t port) {
    OscControl::Config config;
    config.enabled = true;
    config.port = port;
    config.localOnly = true;
    return config;
}

} // namespace

TEST_CASE("the control addresses reach the tracker", "[control]") {
    // §5.7's table, dispatched without a socket: the address table on its own, held to what the
    // engine ends up saying rather than to the fact that a handler was called.
    auto engine = makeEngine();
    OscControl control(*engine, localConfig(kAnyPort));
    trackUntilLocked(*engine);
    REQUIRE(engine->state().locked);

    const double raw = engine->state().rawBpm;
    CHECK(control.dispatch("/takt4/ctl/tempo/halve", std::nullopt));
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(raw / 2.0, 1e-6));

    CHECK(control.dispatch("/takt4/ctl/tempo/double", std::nullopt));
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(raw, 1e-6));

    // A downbeat snap lands on the next beat the tracker calls; what that then means to
    // the bar is tests/engine/beat_engine_test.cpp's business.
    CHECK(control.dispatch("/takt4/ctl/downbeat", std::nullopt));
    (void)engine->step();

    // Three taps make a tempo. The first two say nothing, which is not the same as
    // failing: the address was understood either way.
    CHECK(control.dispatch("/takt4/ctl/tap", std::nullopt));
    CHECK(control.dispatch("/takt4/ctl/tap", std::nullopt));
    CHECK(control.dispatch("/takt4/ctl/tap", std::nullopt));
    (void)engine->step();
    CHECK(engine->commandsDropped() == 0);
}

TEST_CASE("an address for another app is not ours to act on", "[control]") {
    auto engine = makeEngine();
    OscControl control(*engine, localConfig(kAnyPort));

    CHECK_FALSE(control.dispatch("/other/ctl/tempo/halve", std::nullopt));
    CHECK_FALSE(control.dispatch("/ctl/tempo/halve", std::nullopt)); // no prefix at all
    CHECK_FALSE(control.dispatch("/takt4/ctl/", std::nullopt));      // no verb
    CHECK_FALSE(control.dispatch("/takt4/ctl/nonsense", std::nullopt));
    CHECK_FALSE(control.dispatch("/takt4/ctl/tempo/halveXX", std::nullopt));
    CHECK_FALSE(control.dispatch("", std::nullopt));

    // The ones Phase 6 and Q7 still owe, refused rather than half-done.
    CHECK_FALSE(control.dispatch("/takt4/ctl/panic", std::nullopt));
    CHECK_FALSE(control.dispatch("/takt4/ctl/preset", 1.0));
}

TEST_CASE("the lock address pins and releases, and insists on being told which", "[control]") {
    auto engine = makeEngine();
    OscControl control(*engine, localConfig(kAnyPort));
    trackUntilLocked(*engine);
    REQUIRE(engine->state().locked);
    REQUIRE_FALSE(engine->state().pinned);

    // §5.7 spells it `<0|1>`, and without one there is nothing to do. Read as a toggle it
    // would depend on a state the sender cannot see, so a control surface that missed one
    // datagram would be inverted for the rest of the set.
    CHECK_FALSE(control.dispatch("/takt4/ctl/lock", std::nullopt));
    (void)engine->step();
    CHECK_FALSE(engine->state().pinned);

    CHECK(control.dispatch("/takt4/ctl/lock", 1.0));
    (void)engine->step();
    CHECK(engine->state().pinned);
    CHECK(engine->state().locked);

    // Whatever a control surface spells "true" with: OSC booleans arrive as 1 and 0, and
    // a fader sending 1.0 means the same thing as an int 1.
    CHECK(control.dispatch("/takt4/ctl/lock", 0.0));
    (void)engine->step();
    CHECK_FALSE(engine->state().pinned);
    CHECK_FALSE(engine->state().locked); // released, so the tracker has it back

    CHECK(engine->commandsDropped() == 0);
}

TEST_CASE("a listening socket receives what a sender sends it", "[control][network]") {
    // The socket half, over real UDP on the loopback. `OscSender` is the other end, so
    // this also says the two agree about the wire.
    OscReceiver receiver(kAnyPort, /*localOnly=*/true);
    const std::uint16_t port = receiver.port();
    CHECK(port != 0);
    CHECK(receiver.datagrams() == 0);

    // Nothing sent: the wait ends on its own rather than blocking the test.
    const auto before = std::chrono::steady_clock::now();
    CHECK(receiver.receive(std::chrono::milliseconds{40}).empty());
    CHECK(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds{20});

    OscSender sender("127.0.0.1", port);
    OscMessage message("/takt4/ctl/tap");
    REQUIRE(sender.send(message.packet()));

    // A datagram can take a moment to come round the loopback.
    std::span<const std::byte> received;
    for (int attempt = 0; attempt < 20 && received.empty(); ++attempt) {
        received = receiver.receive(std::chrono::milliseconds{100});
    }
    REQUIRE_FALSE(received.empty());
    CHECK(receiver.datagrams() == 1);
    CHECK(receiver.lastSender().find("127.0.0.1") != std::string::npos);

    const auto view = takt4::control::parseOsc(received);
    REQUIRE(view.has_value());
    CHECK(view->address() == "/takt4/ctl/tap");
}

TEST_CASE("a port already in use is reported rather than silently dead", "[control][network]") {
    // A control surface that quietly does nothing is worse than one that will not start.
    const OscReceiver first(kAnyPort, /*localOnly=*/true);
    const std::uint16_t port = first.port();
    REQUIRE(port != 0);
    CHECK_THROWS(OscReceiver(port, /*localOnly=*/true));

    auto engine = makeEngine();
    OscControl control(*engine, localConfig(port));
    CHECK_THROWS(control.start());
    CHECK_FALSE(control.running());
}

TEST_CASE("the control thread acts on what arrives and counts what it cannot",
          "[control][network]") {
    auto engine = makeEngine();
    OscControl control(*engine, localConfig(kAnyPort));
    control.start();
    REQUIRE(control.running());
    const std::uint16_t port = control.port();
    REQUIRE(port != 0);

    OscSender sender("127.0.0.1", port);
    OscMessage halve("/takt4/ctl/tempo/halve");
    REQUIRE(sender.send(halve.packet()));

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < until && control.handled() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    CHECK(control.handled() == 1);
    CHECK(control.lastMessage().find("/takt4/ctl/tempo/halve") != std::string::npos);

    // Something that is not a message at all, and something addressed to nothing we know.
    const std::byte junk[8] = {};
    REQUIRE(sender.send(std::span<const std::byte>(junk, sizeof junk)));
    OscMessage stranger("/somebody/else/ctl/tap");
    REQUIRE(sender.send(stranger.packet()));

    const auto until2 = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < until2 && control.ignored() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    CHECK(control.ignored() == 2);
    CHECK(control.handled() == 1); // and neither one was acted on

    control.stop();
    CHECK_FALSE(control.running());
}

TEST_CASE("a control that was never enabled never opens a socket", "[control]") {
    // Off unless asked for: a listening socket is not something to open on somebody's
    // behalf, and two takt4s on one machine must not fight over a port neither wanted.
    auto engine = makeEngine();
    // A port we know is free, because we are holding it and letting it go.
    std::uint16_t free = 0;
    {
        const OscReceiver probe(kAnyPort, /*localOnly=*/true);
        free = probe.port();
    }
    OscControl::Config config;
    config.enabled = false;
    config.port = free;
    OscControl control(*engine, config);

    control.start();
    CHECK_FALSE(control.running());
    // Which is to say the port is still free.
    CHECK_NOTHROW(OscReceiver(free, /*localOnly=*/true));

    control.stop();
    CHECK_FALSE(control.running());
}

TEST_CASE("a control is safe to stop twice, and to never start", "[control]") {
    auto engine = makeEngine();
    {
        OscControl control(*engine, localConfig(kAnyPort));
        control.stop();
        control.start();
        control.stop();
        control.stop();
        CHECK_FALSE(control.running());
    }
    // And the destructor stops a running one, freeing the port with it.
    std::uint16_t held = 0;
    {
        OscControl control(*engine, localConfig(kAnyPort));
        control.start();
        CHECK(control.running());
        held = control.port();
        REQUIRE(held != 0);
    }
    CHECK_NOTHROW(OscReceiver(held, /*localOnly=*/true));
}
