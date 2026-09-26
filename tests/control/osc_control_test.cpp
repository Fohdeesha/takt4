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

#include "support/recording_rules.hpp"
#include "support/tap_rhythm.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Catch::Approx;
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

    // Half of what is showing, and back (the audit of 2026-09-25, H5).
    const double shown = engine->state().bpm;
    CHECK(control.dispatch("/takt4/ctl/tempo/halve", std::nullopt));
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(shown / 2.0, 1e-9));

    CHECK(control.dispatch("/takt4/ctl/tempo/double", std::nullopt));
    (void)engine->step();
    CHECK_THAT(engine->state().bpm, WithinAbs(shown, 1e-9));

    // A downbeat snap lands on the beat nearest the message; what that then means to the
    // bar is tests/engine/beat_engine_test.cpp's business.
    CHECK(control.dispatch("/takt4/ctl/downbeat", std::nullopt));
    (void)engine->step();

    // Three taps at machine speed are one tap and two bounces, which is no tempo at all —
    // where they used to be thousands of BPM (the audit's H1). The address was understood
    // each time, which is not the same as acting on it. "taps over OSC make the tempo they
    // were tapped at" is the rhythm a hand gives.
    const auto before = engine->tempoOptions();
    CHECK(control.dispatch("/takt4/ctl/tap", std::nullopt));
    CHECK(control.dispatch("/takt4/ctl/tap", std::nullopt));
    CHECK(control.dispatch("/takt4/ctl/tap", std::nullopt));
    (void)engine->step();
    CHECK(engine->commandsDropped() == 0);
    // A tap tempo moves the tempo window onto itself; nothing moved it.
    CHECK_THAT(engine->tempoOptions().minBpm, WithinAbs(before.minBpm, 1e-9));
    CHECK_THAT(engine->tempoOptions().maxBpm, WithinAbs(before.maxBpm, 1e-9));
}

TEST_CASE("taps over OSC make the tempo they were tapped at", "[control]") {
    // The tap tests above used to stop at "the address was understood", which is how a tap
    // button that tapped twice per press went unseen (the audit's H1 and T1). So: a rhythm
    // played in real time, from a TouchOSC button that chatters — every press followed 5 ms
    // later by a second — and where the tracker's tempo window ends up. A tap is a seed, not
    // an override (§7): it centres the window on the tempo tapped, an octave wide, and the
    // decoder weighs that as evidence. Without the bounce guard the chatter halves the median
    // gap, and the window is centred on twice the tempo tapped, or not moved at all.
    auto engine = makeEngine();
    OscControl control(*engine, localConfig(kAnyPort));

    const takt4::testing::TappedRhythm rhythm = takt4::testing::tapWithBounces(
        [&control] { CHECK(control.dispatch("/takt4/ctl/tap", std::nullopt)); }, 3,
        std::chrono::milliseconds{400});
    const double heard = rhythm.tempoOfThree();
    INFO("tapped at " << heard << " BPM (" << rhythm.slowestOfThree() << " to "
                      << rhythm.fastestOfThree() << " as the surface could have stamped it); "
                      << "the slowest bounce came " << rhythm.longestBounce()
                      << " s after its press");
    REQUIRE(rhythm.longestBounce() < 0.1);
    REQUIRE(heard == Approx(150.0).margin(8.0)); // the sleeps did roughly what was asked

    (void)engine->step();
    CHECK(engine->commandsDropped() == 0);
    // Centred on the tempo tapped: within what the surface's own stamps allow, which is wider
    // than a plain margin under a sanitizer — see `TappedRhythm::slowestOfThree`.
    const auto window = engine->tempoOptions();
    const double centre = std::sqrt(window.minBpm * window.maxBpm);
    CHECK(centre >= rhythm.slowestOfThree() - 0.5);
    CHECK(centre <= rhythm.fastestOfThree() + 0.5);
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

    // Q7's, still refused rather than half-done: nothing names or stores a preset yet.
    CHECK_FALSE(control.dispatch("/takt4/ctl/preset", 1.0));

    // And the two that are understood but have nowhere to go, because this control was
    // given no rules. Refused rather than accepted and dropped — an operator whose panic
    // button is not wired has to be able to find that out before the set, not during it.
    CHECK_FALSE(control.dispatch("/takt4/ctl/panic", std::nullopt));
    CHECK_FALSE(control.dispatch("/takt4/ctl/rule/intro/enable", 1.0));
}

TEST_CASE("the two addresses that reach the rules rather than the tracker", "[control]") {
    // §5.7's `panic` and `rule/<id>/enable`. The rule engine has carried both since Phase
    // 6's first half; this is the *route*, and the route is the whole of what is checked
    // here — what the rules then do with it is tests/output/output_runner_test.cpp's.
    auto engine = makeEngine();
    takt4::testing::RecordingRules rules;
    OscControl control(*engine, localConfig(kAnyPort), &rules);

    SECTION("manual fires the manual rules, as a button") {
        // The audit's M7: a trigger called "manual hotkey" that nothing could fire. A press
        // fires; a pad's release, sent as a zero, is understood and does nothing.
        CHECK(control.dispatch("/takt4/ctl/manual", std::nullopt));
        CHECK(rules.manuals() == 1);
        CHECK(control.dispatch("/takt4/ctl/manual", 1.0));
        CHECK(control.dispatch("/takt4/ctl/manual", 0.0));
        CHECK(rules.manuals() == 2);
    }

    SECTION("a bare panic engages, because a panic button panics") {
        // §5.7 writes `lock <0|1>` and `rule/<id>/enable <0|1>` with an argument and writes
        // `panic` bare. A message with no argument is the whole gesture.
        CHECK(control.dispatch("/takt4/ctl/panic", std::nullopt));
        CHECK(rules.panics() == std::vector<bool>{true});
    }

    SECTION("every panic engages, and panic/release lets go of it") {
        // The audit of 2026-09-25, L15, and the operator's answer to its Q1. `panic 0` used to
        // let go, so a TouchOSC push button — 1 on the press, 0 on the release — held the halt
        // only while the finger was down. Every panic message engages now, and the same
        // surface lets go with an address of its own: §5.8 makes panic a latch, and §5.7
        // exists so the laptop need not be touched.
        CHECK(control.dispatch("/takt4/ctl/panic", 1.0));
        CHECK(control.dispatch("/takt4/ctl/panic", 0.0));
        CHECK(rules.panics() == std::vector<bool>{true, true});
        CHECK(control.dispatch("/takt4/ctl/panic/release", std::nullopt));
        CHECK(rules.panics() == std::vector<bool>{true, true, false});
        // A push button bound to release sends 1 and then 0, and the 0 is its release.
        CHECK(control.dispatch("/takt4/ctl/panic/release", 1.0));
        CHECK(control.dispatch("/takt4/ctl/panic/release", 0.0));
        CHECK(rules.panics() == std::vector<bool>{true, true, false, false});
    }

    SECTION("a rule is named in the middle of its own address") {
        CHECK(control.dispatch("/takt4/ctl/rule/intro/enable", 0.0));
        CHECK(control.dispatch("/takt4/ctl/rule/drop/enable", 1.0));
        const std::vector<std::pair<std::string, bool>> expected{{"intro", false}, {"drop", true}};
        CHECK(rules.enables() == expected);
    }

    SECTION("enabling insists on being told which, exactly as the lock does") {
        // Read as a toggle it would depend on a state the sender cannot see, so a surface
        // that missed one message would be inverted for the rest of the set.
        CHECK_FALSE(control.dispatch("/takt4/ctl/rule/intro/enable", std::nullopt));
        CHECK(rules.enables().empty());
    }

    SECTION("a value that is not a number is refused, not read as on") {
        // The audit's M1. Every state here is "anything but zero is on", and a NaN is not zero:
        // `lock nan` pinned the lock, `enable nan` armed a rule, and `rate nan` gave a rule an
        // interval that was undefined. An OSC float can carry a NaN or an infinity; no control
        // surface means one.
        for (const double junk : {std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity()}) {
            CHECK_FALSE(control.dispatch("/takt4/ctl/rule/intro/enable", junk));
            CHECK_FALSE(control.dispatch("/takt4/ctl/rule/intro/mute", junk));
            CHECK_FALSE(control.dispatch("/takt4/ctl/rule/intro/rate", junk));
            CHECK_FALSE(control.dispatch("/takt4/ctl/panic", junk));
            CHECK_FALSE(control.dispatch("/takt4/ctl/lock", junk));
        }
        CHECK(rules.enables().empty());
        CHECK(rules.mutes().empty());
        CHECK(rules.rates().empty());
        CHECK(rules.panics().empty());
        (void)engine->step();
        CHECK_FALSE(engine->state().pinned);
        // And the same addresses with a number still work.
        CHECK(control.dispatch("/takt4/ctl/rule/intro/rate", 2.0));
        REQUIRE(rules.rates().size() == 1);
        CHECK(rules.rates()[0].factor == 2.0);
    }

    SECTION("mute is its own verb, because it is its own state") {
        // Not a second spelling of enable: a disabled rule stops running and restarts when it
        // comes back, a muted one keeps running and stops sending. Dropping a layer out for
        // eight bars is the second; taking a rule out of the show is the first.
        CHECK(control.dispatch("/takt4/ctl/rule/stabs/mute", 1.0));
        CHECK(control.dispatch("/takt4/ctl/rule/stabs/mute", 0.0));
        const std::vector<std::pair<std::string, bool>> expected{{"stabs", true}, {"stabs", false}};
        CHECK(rules.mutes() == expected);
        CHECK(rules.enables().empty()); // and it did not reach the other one
    }

    SECTION("double and halve are bare, relative buttons") {
        // Relative, so a Stream Deck button can be pressed twice and mean four times the
        // interval. Bare, because a button is not a state anybody can hold.
        CHECK(control.dispatch("/takt4/ctl/rule/stabs/double", std::nullopt));
        CHECK(control.dispatch("/takt4/ctl/rule/stabs/halve", std::nullopt));
        const auto rates = rules.rates();
        REQUIRE(rates.size() == 2);
        CHECK(rates[0].id == "stabs");
        CHECK(rates[0].factor == 2.0);
        CHECK(rates[0].relative);
        CHECK(rates[1].factor == 0.5);
        CHECK(rates[1].relative);
    }

    SECTION("rate is absolute, so a fader that repeats itself does not compound") {
        CHECK(control.dispatch("/takt4/ctl/rule/stabs/rate", 4.0));
        const auto rates = rules.rates();
        REQUIRE(rates.size() == 1);
        CHECK(rates[0].factor == 4.0);
        CHECK_FALSE(rates[0].relative);

        SECTION("and it insists on being told what to, like every other state verb") {
            CHECK_FALSE(control.dispatch("/takt4/ctl/rule/stabs/rate", std::nullopt));
        }
    }

    SECTION("reset is rate 1, spelled the way an operator would reach for it") {
        CHECK(control.dispatch("/takt4/ctl/rule/stabs/reset", std::nullopt));
        const auto rates = rules.rates();
        REQUIRE(rates.size() == 1);
        CHECK(rates[0].factor == 1.0);
        CHECK_FALSE(rates[0].relative);
    }

    SECTION("\"all\" is passed through as an id, for the implementation to expand") {
        // The surface does not know what rules exist — only the thing behind `RuleControl`
        // does — so "all" travels as a name and `OutputRunner` is where it means every rule.
        CHECK(control.dispatch("/takt4/ctl/rule/all/mute", 1.0));
        const std::vector<std::pair<std::string, bool>> expected{{"all", true}};
        CHECK(rules.mutes() == expected);
    }

    SECTION("an address that is nearly the rule address is not one") {
        CHECK_FALSE(control.dispatch("/takt4/ctl/rule", 1.0));
        CHECK_FALSE(control.dispatch("/takt4/ctl/rule/intro", 1.0));
        CHECK_FALSE(control.dispatch("/takt4/ctl/rule/enable", 1.0));
        CHECK_FALSE(control.dispatch("/takt4/ctl/rule//enable", 1.0));
        // Both ends match here and the id would be "x/enable", which no rule can be called.
        CHECK_FALSE(control.dispatch("/takt4/ctl/rule/x/enable/enable", 1.0));
        CHECK_FALSE(control.dispatch("/takt4/ctl/rule/intro/disable", 1.0));
        CHECK(rules.enables().empty());
    }

    SECTION("one surface, two destinations, in one sequence") {
        // The point of the whole change: a Stream Deck sends §5.7's addresses down one
        // socket and does not know that five of them are the tracker's and two are the
        // rules'. Interleaved here so that a route which only worked from a cold surface —
        // or only before a tracker command — would show up.
        trackUntilLocked(*engine);
        REQUIRE(engine->state().locked);
        const double shown = engine->state().bpm;

        CHECK(control.dispatch("/takt4/ctl/panic", std::nullopt));
        CHECK(control.dispatch("/takt4/ctl/tempo/halve", std::nullopt));
        CHECK(control.dispatch("/takt4/ctl/rule/intro/enable", 0.0));
        (void)engine->step();

        CHECK_THAT(engine->state().bpm, WithinAbs(shown / 2.0, 1e-9));
        CHECK(rules.panics() == std::vector<bool>{true});
        const std::vector<std::pair<std::string, bool>> expected{{"intro", false}};
        CHECK(rules.enables() == expected);
    }
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

TEST_CASE("a listening socket receives what a sender sends it", "[control]") {
    // The socket half, over real UDP on the loopback. `OscSender` is the other end, so
    // this also says the two agree about the wire. Not [network]: a port the system picks, on
    // the loopback, reaches nothing on the rig — and tagged, the receive thread never ran in
    // the default or the ASan preset (the audit of 2026-09-25, T5).
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

TEST_CASE("a port already in use is reported rather than silently dead", "[control]") {
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

TEST_CASE("the control thread acts on what arrives and counts what it cannot", "[control]") {
    auto engine = makeEngine();
    OscControl control(*engine, localConfig(kAnyPort));
    control.start();
    REQUIRE(control.running());
    const std::uint16_t port = control.port();
    REQUIRE(port != 0);

    OscSender sender("127.0.0.1", port);
    OscMessage halve("/takt4/ctl/tempo/halve");
    REQUIRE(sender.send(halve.packet()));

    // Wait for **both** things asserted below, not for the counter and then assume the
    // message. `OscControl::run` increments `handled_` and only then takes the lock to
    // write `last_`, so a reader that stops as soon as the count moves can catch the
    // message still empty — a preemption between two statements on a busy runner is all
    // it takes. The sibling of this cost a red macOS run at `66173912`.
    const auto arrived = [&control] {
        return control.handled() != 0 &&
               control.lastMessage().find("/takt4/ctl/tempo/halve") != std::string::npos;
    };
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < until && !arrived()) {
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

TEST_CASE("a new config while listening closes the old socket, and the next start is the new one",
          "[control]") {
    // The audit of 2026-09-25, coverage gap 14: `setConfig` on a control that is running — which
    // is what the window does when the operator changes the port, the prefix or "allow other
    // machines" with the listener on. It has to stop the thread that reads the old config before
    // the config changes under it, give the old port back, and start as it now says.
    auto engine = makeEngine();
    OscControl control(*engine, localConfig(kAnyPort));
    control.start();
    REQUIRE(control.running());
    const std::uint16_t before = control.port();
    REQUIRE(before != 0);

    const auto waitFor = [&control](std::uint64_t handled, std::uint64_t ignored) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < until &&
               (control.handled() < handled || control.ignored() < ignored)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    };
    OscSender first("127.0.0.1", before);
    REQUIRE(first.send(OscMessage("/takt4/ctl/tap").packet()));
    waitFor(1, 0);
    REQUIRE(control.handled() == 1);

    OscControl::Config renamed = localConfig(kAnyPort);
    renamed.prefix = "/show";
    const auto asked = std::chrono::steady_clock::now();
    control.setConfig(renamed);
    // Stopped, within the thread's own poll — not left reading with the old prefix.
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::seconds{2});
    CHECK_FALSE(control.running());
    CHECK(control.port() == 0);
    CHECK(control.config().prefix == "/show");
    // The old port is free again.
    CHECK_NOTHROW(OscReceiver(before, /*localOnly=*/true));

    control.start();
    REQUIRE(control.running());
    const std::uint16_t after = control.port();
    REQUIRE(after != 0);
    OscSender second("127.0.0.1", after);
    // The old prefix is somebody else's now; the new one is ours.
    REQUIRE(second.send(OscMessage("/takt4/ctl/tap").packet()));
    REQUIRE(second.send(OscMessage("/show/ctl/tap").packet()));
    waitFor(2, 1);
    CHECK(control.handled() == 2);
    CHECK(control.ignored() == 1);
    CHECK(control.lastMessage().find("/show/ctl/tap") != std::string::npos);
    control.stop();
}

TEST_CASE("a control that was never enabled never opens a socket", "[control]") {
    // Off unless asked for: a listening socket is not something to open on somebody's
    // behalf, and two takt4s on one machine must not fight over a port neither wanted.
    //
    // Asked of the control itself: whether it is listening, and on what. This used to bind a
    // port, let it go, and bind it again to show it was still free — and a port let go is one
    // any program on the rig may take in between, whose loopback datagrams a second bind would
    // then take from it (the audit of 2026-09-25, T4).
    auto engine = makeEngine();
    OscControl::Config config;
    config.enabled = false;
    config.port = 0; // any: were it to open, it would say which
    OscControl control(*engine, config);

    control.start();
    CHECK_FALSE(control.running());
    CHECK(control.port() == 0); // nothing listening

    control.stop();
    CHECK_FALSE(control.running());
    CHECK(control.port() == 0);
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
