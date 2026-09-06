#include "core/output/output_target.hpp"

#include "core/engine/beat_engine.hpp"
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/rule.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using takt4::engine::BeatEngine;
using takt4::output::kAllOutputs;
using takt4::output::OutputCommand;
using takt4::output::OutputRunner;
using takt4::output::OutputTarget;
using takt4::output::Transports;
using takt4::testing::LoopbackReceiver;
using takt4::trigger::Rule;

namespace {

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

OutputTarget osc(std::string name, std::uint16_t port) {
    OutputTarget target;
    target.name = std::move(name);
    target.kind = OutputTarget::Kind::Osc;
    target.host = "127.0.0.1";
    target.port = port;
    return target;
}

/// One rule that fires on every beat, at `address`, routed to `outputs`.
Rule::Config firing(std::string id, std::string address, std::vector<std::string> outputs) {
    Rule::Config rule;
    rule.id = std::move(id);
    rule.trigger = takt4::trigger::Trigger::Beat;
    rule.address = std::move(address);
    rule.sendValue = false;
    rule.outputs = std::move(outputs);
    return rule;
}

/// Everything waiting on a receiver, as whole datagrams.
std::vector<std::string> drain(LoopbackReceiver& receiver) {
    std::vector<std::string> got;
    for (std::string datagram = receiver.receive(); !datagram.empty();
         datagram = receiver.receive()) {
        got.push_back(datagram);
    }
    return got;
}

bool sawAddress(const std::vector<std::string>& datagrams, std::string_view address) {
    return std::any_of(datagrams.begin(), datagrams.end(), [address](const std::string& d) {
        return d.find(address) != std::string::npos;
    });
}

} // namespace

TEST_CASE("a target's name survives being written down and read back", "[output][routing]") {
    const auto roundTrip = [](const OutputTarget& in) {
        OutputTarget out;
        const std::string text = takt4::output::formatOutputTarget(in);
        INFO(text);
        REQUIRE(takt4::output::parseOutputTarget(text, out));
        return out;
    };

    OutputTarget wall = osc("wall", 7000);
    wall.host = "192.168.1.40";
    CHECK(roundTrip(wall) == wall);

    OutputTarget lights;
    lights.name = "lights";
    lights.kind = OutputTarget::Kind::Midi;
    lights.device = "MOTU Pro Audio Midi Out 1";
    CHECK(roundTrip(lights) == lights);

    OutputTarget off = osc("spare", 9000);
    off.enabled = false;
    CHECK(roundTrip(off) == off);

    SECTION("and the format an operator already knew still works") {
        // The outputs field took one `host:port` per line before targets had names. A line
        // like that is still a target, named after its own address — which is what it was
        // always called on screen.
        OutputTarget bare;
        REQUIRE(takt4::output::parseOutputTarget("127.0.0.1:7000", bare));
        CHECK(bare.kind == OutputTarget::Kind::Osc);
        CHECK(bare.host == "127.0.0.1");
        CHECK(bare.port == 7000);
        CHECK(bare.name == "127.0.0.1:7000");
        CHECK(bare.enabled);
    }

    SECTION("a line that is not a target is refused rather than half-read") {
        OutputTarget out;
        for (const char* text : {"", "   ", "nonsense", "host:", ":7000", "a:0", "a:99999",
                                 "a:seven", "name =", "name = midi", "midi"}) {
            INFO(text);
            CHECK_FALSE(takt4::output::parseOutputTarget(text, out));
        }
    }
}

TEST_CASE("a rule's names become the bits its messages carry", "[output][routing]") {
    const std::vector<OutputTarget> targets{osc("main", 7000), osc("wall", 7001),
                                            osc("spare", 7002)};

    // §5.6's own default: a rule that names nothing goes everywhere. That is what every rule
    // meant before routing existed, so no preset changes behaviour by being loaded.
    CHECK(takt4::output::resolveOutputs({}, targets) == kAllOutputs);

    CHECK(takt4::output::resolveOutputs({"main"}, targets) == 0b001);
    CHECK(takt4::output::resolveOutputs({"spare"}, targets) == 0b100);
    CHECK(takt4::output::resolveOutputs({"main", "spare"}, targets) == 0b101);

    // A name this rig does not have contributes nothing and is not an error: a preset
    // written where there was a "lights" output should keep saying so.
    CHECK(takt4::output::resolveOutputs({"lights"}, targets) == 0);
    CHECK(takt4::output::resolveOutputs({"main", "lights"}, targets) == 0b001);
}

TEST_CASE("two rules, two targets, and each goes where it was sent", "[output][routing]") {
    // The whole point of the change, end to end and over real sockets: a rig with a media
    // server and a second machine on it, and a rule aimed at each.
    LoopbackReceiver deck;
    LoopbackReceiver wall;

    Transports::Config config;
    config.outputs = {osc("deck", deck.port()), osc("wall", wall.port())};

    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner(*engine, config);
    runner.post(OutputCommand::rules({firing("clips", "/deck/clip", {"deck"}),
                                      firing("cue", "/wall/cue", {"wall"}),
                                      firing("both", "/everywhere", {})}));

    // Fired through the [test] button rather than from audio: what is under test is where a
    // message goes, and the tracker's own beats are tested at length elsewhere.
    runner.post(OutputCommand::testRule("clips"));
    runner.post(OutputCommand::testRule("cue"));
    runner.post(OutputCommand::testRule("both"));

    const std::vector<std::string> atDeck = drain(deck);
    const std::vector<std::string> atWall = drain(wall);

    CHECK(sawAddress(atDeck, "/deck/clip"));
    CHECK_FALSE(sawAddress(atDeck, "/wall/cue"));

    CHECK(sawAddress(atWall, "/wall/cue"));
    CHECK_FALSE(sawAddress(atWall, "/deck/clip"));

    // And an unrouted rule reaches both, which is what "empty means everywhere" has to mean.
    CHECK(sawAddress(atDeck, "/everywhere"));
    CHECK(sawAddress(atWall, "/everywhere"));

    SECTION("a rule routed nowhere is undeliverable, not silently fine") {
        // "Your routing is wrong" and "your rule never fired" look identical from outside.
        const std::uint64_t before = runner.ruleSink().undeliverable();
        runner.post(OutputCommand::rules({firing("lost", "/nowhere", {"a-target-we-lack"})}));
        runner.post(OutputCommand::testRule("lost"));
        CHECK(runner.ruleSink().undeliverable() > before);
        CHECK_FALSE(sawAddress(drain(deck), "/nowhere"));
    }

    SECTION("switching a target off stops it receiving, and switching it back on resumes") {
        std::vector<OutputTarget> half = config.outputs;
        half[0].enabled = false;
        runner.post(OutputCommand::outputs(half));
        runner.post(OutputCommand::testRule("both"));
        CHECK_FALSE(sawAddress(drain(deck), "/everywhere"));
        CHECK(sawAddress(drain(wall), "/everywhere"));

        runner.post(OutputCommand::outputs(config.outputs));
        runner.post(OutputCommand::testRule("both"));
        CHECK(sawAddress(drain(deck), "/everywhere"));
    }

    SECTION("adding a target above one does not move what the rules below it reach") {
        // The reason routing travels as *names*: a bit moves the moment a target is inserted
        // ahead of it, and a rule that quietly started addressing its neighbour would be the
        // worst kind of bug — it would look like it was working.
        std::vector<OutputTarget> grown{osc("new", 7009), config.outputs[0], config.outputs[1]};
        runner.post(OutputCommand::outputs(grown));
        runner.post(OutputCommand::testRule("clips"));
        CHECK(sawAddress(drain(deck), "/deck/clip"));
        CHECK_FALSE(sawAddress(drain(wall), "/deck/clip"));
    }
}

TEST_CASE("the generic namespace goes to every target, routed or not", "[output][routing]") {
    // §5.6: "Always publish a generic namespace regardless of which host preset is active,
    // so anything can consume it with zero configuration." A *rule* chooses where it goes;
    // the app's own description of what the tempo is does not.
    LoopbackReceiver deck;
    LoopbackReceiver wall;

    Transports::Config config;
    config.outputs = {osc("deck", deck.port()), osc("wall", wall.port())};
    Transports transports(config);

    takt4::tracking::BeatEvent event;
    event.bpm = 128.0;
    event.beatInBar = 1;
    event.downbeat = true;
    event.beatsPerBar = 4;
    transports.publish(event, 0, 0.0);

    CHECK(sawAddress(drain(deck), "/takt4/bpm"));
    CHECK(sawAddress(drain(wall), "/takt4/bpm"));
}
