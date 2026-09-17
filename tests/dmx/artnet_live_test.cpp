// **The whole path, end to end: audio in, ArtDmx on a socket.**
//
// Everything below this file has tests that hold one layer still and check the next — the
// packet encoder against the specification, the engine against a fade, the rule against the
// engine. None of them says that a rig plugged in and pointed at takt4 sees its lights move,
// because each of them replaces the layer underneath with a stub.
//
// This one replaces nothing. The committed excerpt goes through the real tracker, the real
// output thread drains the real beats, a real rule fires a real fade, the real Art-Net
// publisher paces it and a real UDP socket receives the datagrams — which are then decoded
// back and checked against what the fade should have been doing at that moment.

#include "core/audio/rates.hpp"
#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/fixture.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/rule.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using takt4::audio::kHopSize;
using takt4::dmx::Fixture;
using takt4::engine::BeatEngine;
using takt4::output::OutputRunner;
using takt4::output::Transports;
using takt4::testing::LoopbackReceiver;
using takt4::trigger::Rule;

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

const std::vector<float>& excerpt() {
    static const std::vector<float> samples =
        takt4::io::readWavFile(kTestData / "features" / "synthetic.wav").samples;
    return samples;
}

/// The particle filter, like the other output-thread tests: they want a beat stream that
/// never moves, and that one is held to `tools/pf_reference.py` frame for frame.
BeatEngine::Options particleOptions() {
    BeatEngine::Options options;
    options.decoder = takt4::tracking::Decoder::ParticleFilter;
    return options;
}

void feedExcerpt(BeatEngine& engine) {
    const std::vector<float>& samples = excerpt();
    const std::size_t hops = samples.size() / kHopSize;
    for (std::size_t hop = 0; hop < hops; ++hop) {
        engine.processHop(samples.data() + hop * kHopSize, hop);
        (void)engine.step();
    }
}

/// One ArtDmx datagram, taken apart again. The decoder is written *here* rather than reused
/// from `writeArtDmx`'s own constants on purpose: a test that encoded and decoded with the
/// same table would agree with itself about a field in the wrong place.
struct Frame {
    bool valid = false;
    int sequence = 0;
    int physical = 0;
    int subUni = 0;
    int net = 0;
    int length = 0;
    std::vector<std::uint8_t> levels;
};

Frame decode(const std::string& datagram) {
    Frame frame;
    if (datagram.size() < 18 || datagram.compare(0, 8, std::string("Art-Net\0", 8)) != 0) {
        return frame;
    }
    const auto byte = [&datagram](std::size_t at) {
        return static_cast<int>(static_cast<std::uint8_t>(datagram[at]));
    };
    // OpOutput, low byte first; protocol 0.14.
    if (byte(8) != 0x00 || byte(9) != 0x50 || byte(10) != 0 || byte(11) != 14) {
        return frame;
    }
    frame.sequence = byte(12);
    frame.physical = byte(13);
    frame.subUni = byte(14);
    frame.net = byte(15);
    frame.length = byte(16) * 256 + byte(17); // high byte first, unlike the OpCode
    if (datagram.size() < 18 + static_cast<std::size_t>(frame.length)) {
        return frame;
    }
    for (int i = 0; i < frame.length; ++i) {
        frame.levels.push_back(
            static_cast<std::uint8_t>(datagram[18 + static_cast<std::size_t>(i)]));
    }
    frame.valid = true;
    return frame;
}

Transports::Config rigWithNode(std::uint16_t port) {
    Transports::Config config;
    takt4::output::OutputTarget node;
    node.name = "truss";
    node.kind = takt4::output::OutputTarget::Kind::ArtNet;
    node.host = "127.0.0.1";
    node.port = port;
    config.outputs.push_back(node);

    // One RGB par at 1 and one dimmer at 10, so the frame has something at two addresses and
    // a channel between them that must stay where its parked level put it.
    Fixture par = takt4::dmx::fixtureFromMode("par", 1, 0, 1);
    par.group = "washes";
    Fixture dim = takt4::dmx::fixtureFromMode("blinder", 0, 0, 10);
    config.patch = {par, dim};
    return config;
}

/// A rule that snaps the washes to a color on every downbeat.
Rule::Config downbeatColor() {
    Rule::Config rule;
    rule.id = "wash-hit";
    rule.name = "wash on the downbeat";
    rule.trigger = takt4::trigger::Trigger::Downbeat;
    rule.sendKind = takt4::trigger::Message::Kind::Dmx;
    rule.dmx.fixtures = {"washes"};
    rule.dmx.effect = takt4::dmx::EffectKind::Color;
    rule.dmx.color.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.dmx.color.fixed = takt4::trigger::Value::ofText("#ff2040");
    // A snap, so what lands on the wire is one known frame rather than a moment of a ramp.
    rule.dmx.unit = takt4::trigger::DelayUnit::Milliseconds;
    rule.dmx.durationSeconds = 0.0;
    return rule;
}

} // namespace

TEST_CASE("a beat in the audio becomes an ArtDmx packet on the network", "[dmx][artnet][slow]") {
    LoopbackReceiver node;
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, rigWithNode(node.port()));
    runner.post(takt4::output::OutputCommand::rules({downbeatColor()}));
    runner.start();

    // The first frame goes out as soon as the output thread has a round, before any beat:
    // a node has to be told what the universe is at, not only what it becomes. Everything is
    // parked, so this is the rig dark — which is the state a fade has to start from.
    std::string datagram = node.receive();
    REQUIRE_FALSE(datagram.empty());
    Frame first = decode(datagram);
    REQUIRE(first.valid);
    CHECK(first.length == 512);
    CHECK(first.subUni == 0);
    CHECK(first.net == 0);
    CHECK(first.physical == 0); // takt4 has no DMX inputs
    CHECK(first.sequence == 1); // the counter starts at 1, because 0 means "disabled"
    CHECK(first.levels[0] == 0);
    CHECK(first.levels[1] == 0);
    CHECK(first.levels[2] == 0);

    feedExcerpt(*engine);

    // Then the color, which only a fired rule can have put there. Read until a frame shows
    // it or the socket goes quiet: the publisher paces at 44 Hz and keeps alive under a
    // second, so several frames of the old state may still be in flight.
    bool lit = false;
    int lastSequence = first.sequence;
    for (int attempt = 0; attempt < 200 && !lit; ++attempt) {
        datagram = node.receive();
        if (datagram.empty()) {
            break;
        }
        const Frame frame = decode(datagram);
        REQUIRE(frame.valid);
        // Every frame is one universe's worth, whatever is in it.
        CHECK(frame.length == 512);
        // And the sequence advances by one each time, wrapping 255 to 1 rather than to 0.
        const int expected = lastSequence == 255 ? 1 : lastSequence + 1;
        CHECK(frame.sequence == expected);
        lastSequence = frame.sequence;

        if (frame.levels[0] == 0xff && frame.levels[1] == 0x20 && frame.levels[2] == 0x40) {
            lit = true;
            // The channel the par does not own is still at its parked level — the rule wrote
            // three channels and not the whole universe.
            CHECK(frame.levels[3] == 0);
            CHECK(frame.levels[9] == 0);
        }
    }
    CHECK(lit);

    runner.stop();
    CHECK(runner.transports().artnet().sent() > 0);
    CHECK(runner.transports().artnet().failed() == 0);
}

TEST_CASE("a fade really is a stream of frames, not one", "[dmx][artnet][slow]") {
    // The claim a packet test cannot make: that a *duration* turns into a sequence of
    // different frames on the wire. If the engine wrote one level and stopped, or if the
    // publisher sent the same frame each time, everything else would still pass.
    LoopbackReceiver node;
    auto engine = std::make_unique<BeatEngine>(weights(), stateSpace(), particleOptions());
    OutputRunner runner(*engine, rigWithNode(node.port()));

    Rule::Config fade = downbeatColor();
    fade.id = "wash-fade";
    fade.dmx.effect = takt4::dmx::EffectKind::Level;
    fade.dmx.role = takt4::dmx::Role::Red;
    fade.dmx.level.kind = takt4::trigger::GeneratorKind::Fixed;
    fade.dmx.level.fixed = takt4::trigger::Value::ofInt(255);
    fade.dmx.curve = takt4::dmx::Curve::Linear;
    fade.dmx.unit = takt4::trigger::DelayUnit::Milliseconds;
    fade.dmx.durationSeconds = 0.4; // long enough to be several frames at 44 Hz
    runner.post(takt4::output::OutputCommand::rules({fade}));
    runner.start();

    (void)node.receive(); // the opening frame, everything parked
    feedExcerpt(*engine);

    // How many *distinct* values channel 1 was seen at. A snap would show two — off, then on.
    // A fade sent as a stream shows a staircase.
    std::vector<int> seen;
    for (int attempt = 0; attempt < 400; ++attempt) {
        const std::string datagram = node.receive();
        if (datagram.empty()) {
            break;
        }
        const Frame frame = decode(datagram);
        REQUIRE(frame.valid);
        const int red = frame.levels[0];
        if (seen.empty() || seen.back() != red) {
            seen.push_back(red);
        }
    }
    runner.stop();

    // At 44 Hz a 400 ms fade is about eighteen frames. Four distinct levels is a low bar that
    // a snap cannot clear and a stream clears easily, and it does not depend on the scheduler
    // giving the output thread any particular slice.
    INFO("distinct levels seen on channel 1: " << seen.size());
    CHECK(seen.size() >= 4);
    // And it arrived: whatever the middle looked like, the fade finished at full.
    CHECK(seen.back() == 255);
}
