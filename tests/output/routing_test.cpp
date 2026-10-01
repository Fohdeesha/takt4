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
#include <cstdint>
#include <cstdio>
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

/// A stable id for a test target, from its name — the shape `newOutputId` makes, so it also
/// survives the text line. What a rule in these tests is routed by.
std::string idFor(std::string_view name) {
    std::uint32_t hash = 2166136261u;
    for (const char c : name) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619u;
    }
    char text[16] = {};
    std::snprintf(text, sizeof text, "o-%08x", static_cast<unsigned int>(hash));
    return text;
}

OutputTarget osc(std::string name, std::uint16_t port) {
    OutputTarget target;
    target.id = idFor(name);
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

TEST_CASE("a target's name and id survive being written down and read back", "[output][routing]") {
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
    lights.id = idFor("lights");
    lights.name = "lights";
    lights.kind = OutputTarget::Kind::Midi;
    lights.device = "MOTU Pro Audio Midi Out 1";
    CHECK(roundTrip(lights) == lights);

    SECTION("a # in a device's name is the device's, in a line written before ids") {
        OutputTarget pad;
        REQUIRE(takt4::output::parseOutputTarget("pads = midi Launchpad #2", pad));
        CHECK(pad.device == "Launchpad #2");
        CHECK(pad.id.empty());
        pad.id = idFor("pads");
        CHECK(roundTrip(pad) == pad);
    }

    OutputTarget off = osc("spare", 9000);
    off.enabled = false;
    CHECK(roundTrip(off) == off);

    SECTION("an OSC target that takes only its rules says so, and keeps saying so") {
        OutputTarget robot = osc("robot", 9000);
        robot.sendsNamespace = false;
        robot.delaySeconds = 0.150;
        CHECK(takt4::output::formatOutputTarget(robot) ==
              "robot = 127.0.0.1:9000 rules-only +150ms #" + robot.id);
        CHECK(roundTrip(robot) == robot);
        robot.enabled = false;
        CHECK(roundTrip(robot) == robot);
        // On is said by saying nothing, so every line written before reads as it did.
        CHECK(takt4::output::formatOutputTarget(wall).find("rules-only") == std::string::npos);
        OutputTarget old;
        REQUIRE(takt4::output::parseOutputTarget("deck = 127.0.0.1:7000 +40ms #o-1a2b", old));
        CHECK(old.sendsNamespace);
        // Unnamed, the address is the name, without the word.
        REQUIRE(takt4::output::parseOutputTarget("127.0.0.1:9000 rules-only", old));
        CHECK_FALSE(old.sendsNamespace);
        CHECK(old.name == "127.0.0.1:9000");
        CHECK(old.port == 9000);
        // Only OSC has the namespace: nothing else writes the word, and a MIDI device whose name
        // ends in it keeps it.
        OutputTarget node = lights;
        node.sendsNamespace = false;
        CHECK(takt4::output::formatOutputTarget(node).find("rules-only") == std::string::npos);
        REQUIRE(takt4::output::parseOutputTarget("pads = midi Pad rules-only", old));
        CHECK(old.device == "Pad rules-only");
        CHECK(old.sendsNamespace);
    }

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
        CHECK(bare.id.empty()); // until `ensureOutputIds` gives it one
    }

    SECTION("a line that is not a target is refused rather than half-read") {
        OutputTarget out;
        for (const char* text : {"", "   ", "nonsense", "host:", ":7000", "a:0", "a:99999",
                                 "a:seven", "name =", "name = midi", "midi", "\"unclosed = a:1",
                                 "\"closed\" a:1"}) {
            INFO(text);
            CHECK_FALSE(takt4::output::parseOutputTarget(text, out));
        }
    }

    SECTION("any name at all, however it would read unquoted") {
        // The audit of 2026-09-25, L28, and the operator's answer to its Q5: "off stage" came
        // back switched off and called "stage", and "a=b" came back mangled. A name like that
        // is written in quotes; every other name is written as it always was.
        for (const bool enabled : {true, false}) {
            for (const char* name : {"off stage", "off", "a=b", " spaced ", "say \"hi\"",
                                     "back\\slash", "comma, here", "\"quoted\"", "line\nbreak"}) {
                OutputTarget odd = osc(name, 7000);
                odd.enabled = enabled;
                INFO("name [" << name << "], " << (enabled ? "on" : "off"));
                CHECK(roundTrip(odd) == odd);
            }
        }
        OutputTarget plain = osc("wall", 7000);
        CHECK(takt4::output::formatOutputTarget(plain).starts_with("wall = "));
        OutputTarget stage = osc("off stage", 7000);
        CHECK(takt4::output::formatOutputTarget(stage).starts_with("\"off stage\" = "));
    }

    SECTION("a file written before quoting reads as it always did") {
        OutputTarget out;
        REQUIRE(takt4::output::parseOutputTarget("off deck = 127.0.0.1:7000", out));
        CHECK_FALSE(out.enabled);
        CHECK(out.name == "deck");
        REQUIRE(takt4::output::parseOutputTarget("deck = 127.0.0.1:7000 +40ms #o-1a2b", out));
        CHECK(out.name == "deck");
        CHECK(out.id == "o-1a2b");
    }
}

TEST_CASE("a rule's output ids become the bits its messages carry", "[output][routing]") {
    std::vector<OutputTarget> targets{osc("main", 7000), osc("wall", 7001), osc("spare", 7002)};

    // §5.6's own default: a rule that names nothing goes everywhere. That is what every rule
    // meant before routing existed, so no preset changes behaviour by being loaded.
    CHECK(takt4::output::resolveOutputs({}, targets) == kAllOutputs);

    CHECK(takt4::output::resolveOutputs({idFor("main")}, targets) == 0b001);
    CHECK(takt4::output::resolveOutputs({idFor("spare")}, targets) == 0b100);
    CHECK(takt4::output::resolveOutputs({idFor("main"), idFor("spare")}, targets) == 0b101);

    // An id this rig does not have contributes nothing and is not an error.
    CHECK(takt4::output::resolveOutputs({idFor("lights")}, targets) == 0);
    CHECK(takt4::output::resolveOutputs({idFor("main"), idFor("lights")}, targets) == 0b001);

    SECTION("a name reaches nothing, so renaming an output moves no rule") {
        // The operator's report of 2026-09-23: renaming an OSC target broke every rule routed
        // to it, and each had to be routed again.
        CHECK(takt4::output::resolveOutputs({"main"}, targets) == 0);
        targets[0].name = "front of house";
        CHECK(takt4::output::resolveOutputs({idFor("main")}, targets) == 0b001);
    }

    SECTION("routing written by name is re-pointed at the ids of those outputs") {
        std::vector<std::string> routing{"wall", "lights", idFor("spare")};
        takt4::output::routeByIds(routing, targets);
        CHECK(routing == std::vector<std::string>{idFor("wall"), "lights", idFor("spare")});
    }

    SECTION("every output is given an id, and a copied one its own") {
        std::vector<OutputTarget> typed{targets[0], targets[0]};
        typed[0].id.clear();
        takt4::output::ensureOutputIds(typed);
        CHECK_FALSE(typed[0].id.empty());
        CHECK(typed[1].id == idFor("main"));
        CHECK(typed[0].id != typed[1].id);
    }
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
    runner.post(OutputCommand::rules({firing("clips", "/deck/clip", {idFor("deck")}),
                                      firing("cue", "/wall/cue", {idFor("wall")}),
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
        // The reason routing travels as *ids*: a bit moves the moment a target is inserted
        // ahead of it, and a rule that quietly started addressing its neighbour would be the
        // worst kind of bug — it would look like it was working.
        std::vector<OutputTarget> grown{osc("new", 57009), config.outputs[0], config.outputs[1]};
        runner.post(OutputCommand::outputs(grown));
        runner.post(OutputCommand::testRule("clips"));
        CHECK(sawAddress(drain(deck), "/deck/clip"));
        CHECK_FALSE(sawAddress(drain(wall), "/deck/clip"));
    }

    SECTION("renaming a target leaves the rules routed to it reaching it") {
        std::vector<OutputTarget> renamed = config.outputs;
        renamed[0].name = "media server";
        runner.post(OutputCommand::outputs(renamed));
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
    // ...except one told to take only its rules (`OutputTarget::sendsNamespace`).
    LoopbackReceiver robot;

    Transports::Config config;
    config.outputs = {osc("deck", deck.port()), osc("wall", wall.port()),
                      osc("robot", robot.port())};
    config.outputs[2].sendsNamespace = false;
    Transports transports(config);

    takt4::tracking::BeatEvent event;
    event.bpm = 128.0;
    event.beatInBar = 1;
    event.downbeat = true;
    event.beatsPerBar = 4;
    transports.publish(event, 0, 0.0);

    CHECK(sawAddress(drain(deck), "/takt4/bpm"));
    CHECK(sawAddress(drain(wall), "/takt4/bpm"));
    CHECK(drain(robot).empty());
}

TEST_CASE("a MIDI clock and the Link output survive their text line too", "[output][routing]") {
    // Both were settings of their own until 2026-09-25; now they are lines among the outputs.
    const auto roundTrip = [](const OutputTarget& in) {
        OutputTarget out;
        const std::string text = takt4::output::formatOutputTarget(in);
        INFO(text);
        REQUIRE(takt4::output::parseOutputTarget(text, out));
        return out;
    };

    OutputTarget drums;
    drums.id = idFor("drums");
    drums.name = "drums";
    drums.kind = OutputTarget::Kind::MidiClock;
    drums.device = "TR-8S";
    drums.delaySeconds = -0.012;
    CHECK(takt4::output::formatOutputTarget(drums) == "drums = midiclock TR-8S -12ms #" + drums.id);
    CHECK(roundTrip(drums) == drums);
    // Without a name, the device is the name, as a MIDI output's is.
    OutputTarget unnamed;
    REQUIRE(takt4::output::parseOutputTarget("midiclock MOTU Pro Audio Midi Out 1", unnamed));
    CHECK(unnamed.kind == OutputTarget::Kind::MidiClock);
    CHECK(unnamed.name == "MOTU Pro Audio Midi Out 1");

    std::vector<OutputTarget> outputs;
    REQUIRE(takt4::output::ensureLinkOutput(outputs, true));
    OutputTarget& link = outputs.front();
    link.delaySeconds = 0.025;
    CHECK(takt4::output::formatOutputTarget(link) == "Link = link +25ms #" + link.id);
    CHECK(roundTrip(link) == link);
    link.enabled = false;
    CHECK(roundTrip(link) == link);

    OutputTarget out;
    for (const char* text : {"midiclock", "clock = midiclock ", "clock = midiclock   #o-12"}) {
        INFO(text);
        CHECK_FALSE(takt4::output::parseOutputTarget(text, out));
    }
}

TEST_CASE("every set of outputs has exactly one Link output, and it is first",
          "[output][routing]") {
    std::vector<OutputTarget> outputs{osc("wall", 7000), osc("deck", 7001)};
    CHECK(takt4::output::ensureLinkOutput(outputs, false));
    REQUIRE(outputs.size() == 3);
    CHECK(outputs[0].kind == OutputTarget::Kind::Link);
    CHECK(outputs[0].name == "Link");
    CHECK_FALSE(outputs[0].enabled);
    CHECK_FALSE(outputs[0].id.empty());
    CHECK(outputs[1].name == "wall");
    CHECK(outputs[2].name == "deck");

    // Already right, nothing changes — and the switch it has is its own, whatever is asked.
    const std::vector<OutputTarget> settled = outputs;
    CHECK_FALSE(takt4::output::ensureLinkOutput(outputs, true));
    CHECK(outputs == settled);

    SECTION("one that is not first is moved there, and a second one is dropped") {
        OutputTarget link = settled[0];
        link.enabled = true;
        link.delaySeconds = 0.030;
        OutputTarget second = link;
        second.id = idFor("second");
        second.delaySeconds = -0.5;
        std::vector<OutputTarget> shuffled{osc("wall", 7000), link, osc("deck", 7001), second};
        CHECK(takt4::output::ensureLinkOutput(shuffled, false));
        REQUIRE(shuffled.size() == 3);
        CHECK(shuffled[0] == link); // its switch, its delay and its id
        CHECK(shuffled[1].name == "wall");
        CHECK(shuffled[2].name == "deck");
    }
}
