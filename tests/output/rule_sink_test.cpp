#include "core/dmx/fixture.hpp"
#include "core/output/midi_clock.hpp"
#include "core/output/rule_sink.hpp"
#include "core/output/transports.hpp"
#include "core/trigger/rule.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::output::RuleSink;
using takt4::output::Transports;
using takt4::testing::LoopbackReceiver;
using takt4::trigger::Message;
using takt4::trigger::Value;

namespace {

Transports::Config withTarget(std::uint16_t port) {
    Transports::Config config;
    config.outputs = takt4::output::oscOutputs({{"127.0.0.1", port}});
    return config;
}

Message oscTo(std::string address, Value argument) {
    Message message;
    message.kind = Message::Kind::Osc;
    message.address = std::move(address);
    message.argument = argument;
    return message;
}

} // namespace

TEST_CASE("a rule's message reaches a socket that really receives", "[output][trigger]") {
    // The whole point of the adapter: `trigger` builds messages and has no business owning
    // a socket, `Transports` sends 5.6's namespace and has no business knowing what a rule
    // is, and this is the one file that knows both. That a rule's address is *built*
    // correctly is tests/trigger's business; that it goes out is this one's.
    LoopbackReceiver receiver;
    Transports transports(withTarget(receiver.port()));
    RuleSink sink(transports);

    // 5.6's own Resolume example, with its templated segments already filled in.
    sink.send(oscTo("/composition/layers/2/clips/5/connect", Value::ofInt(1)));
    const std::string datagram = receiver.receive();
    REQUIRE_FALSE(datagram.empty());
    CHECK_THAT(datagram, ContainsSubstring("/composition/layers/2/clips/5/connect"));
    CHECK(sink.delivered() == 1);
    CHECK(sink.undeliverable() == 0);

    SECTION("the OSC prefix is not applied to it") {
        // It names this app inside the generic namespace. A rule's address belongs to
        // whatever is listening, and prefixing it would send Resolume's own address to
        // somewhere Resolume is not.
        CHECK_THAT(datagram, !ContainsSubstring(transports.oscPrefix()));
    }

    SECTION("the argument goes out as the type the generator produced") {
        // OSC is typed: a host expecting a float and given an int ignores the message
        // rather than guessing, so an operator who chose a float generator gets a float.
        sink.send(oscTo("/f", Value::ofFloat(0.25f)));
        CHECK_THAT(receiver.receive(), ContainsSubstring(",f"));
        sink.send(oscTo("/i", Value::ofInt(3)));
        CHECK_THAT(receiver.receive(), ContainsSubstring(",i"));
        sink.send(oscTo("/s", Value::ofText("intro")));
        const std::string text = receiver.receive();
        CHECK_THAT(text, ContainsSubstring(",s"));
        CHECK_THAT(text, ContainsSubstring("intro"));
        // A bool is carried as an int by OSC and by MIDI alike.
        sink.send(oscTo("/b", Value::ofBool(true)));
        CHECK_THAT(receiver.receive(), ContainsSubstring(",i"));
    }

    SECTION("an address that takes no argument sends none") {
        Message bare = oscTo("/composition/tempocontroller/resync", Value{});
        bare.hasArgument = false;
        sink.send(bare);
        const std::string only = receiver.receive();
        CHECK_THAT(only, ContainsSubstring("/composition/tempocontroller/resync"));
        // ",", padded to four bytes, and nothing after it.
        CHECK(only.size() == 40);
    }
}

TEST_CASE("a rule with nowhere to send is counted, not silent", "[output][trigger]") {
    // A rule firing into an app with no OSC target and no MIDI port sends nothing, and has
    // to say so: otherwise it looks exactly like a rule that never fired, and an operator
    // staring at a card reading "0 fires" has no way to tell them apart.
    Transports transports{Transports::Config{}};
    RuleSink sink(transports);
    REQUIRE(transports.osc().targetCount() == 0);
    REQUIRE(transports.midiPort() == nullptr);

    sink.send(oscTo("/anything", Value::ofInt(1)));
    Message note;
    note.kind = Message::Kind::MidiNote;
    sink.send(note);

    CHECK(sink.delivered() == 0);
    CHECK(sink.undeliverable() == 2);
}

TEST_CASE("a released note puts a real note off on the wire, not a note on of zero",
          "[output][trigger][midi]") {
    // The operator's laser controller (Liberation) holds its clip until 0x80
    // arrives. This file used to assert the opposite in a comment — "a note with velocity
    // zero is a note-off on every device made since 1983" — and the rig never released.
    CHECK(takt4::output::midiStatusFor(Message::Kind::MidiNote, 1) == 0x90);
    CHECK(takt4::output::midiStatusFor(Message::Kind::MidiNoteOff, 1) == 0x80);
    CHECK(takt4::output::midiStatusFor(Message::Kind::MidiCc, 1) == 0xB0);
    CHECK(takt4::output::midiStatusFor(Message::Kind::MidiProgramChange, 1) == 0xC0);
    CHECK(takt4::output::midiStatusFor(Message::Kind::MidiPitchBend, 1) == 0xE0);

    // The channel is the low nibble, counted from one by people and from zero on the wire.
    CHECK(takt4::output::midiStatusFor(Message::Kind::MidiNoteOff, 3) == 0x82);
    CHECK(takt4::output::midiStatusFor(Message::Kind::MidiNoteOff, 16) == 0x8F);
    // Out of range is clamped rather than wrapped into another channel's status.
    CHECK(takt4::output::midiStatusFor(Message::Kind::MidiNoteOff, 0) == 0x80);
    CHECK(takt4::output::midiStatusFor(Message::Kind::MidiNoteOff, 99) == 0x8F);

    // Program change is two bytes; there is nowhere to put a value.
    CHECK(takt4::output::midiLengthFor(Message::Kind::MidiProgramChange) == 2);
    CHECK(takt4::output::midiLengthFor(Message::Kind::MidiNote) == 3);
    CHECK(takt4::output::midiLengthFor(Message::Kind::MidiPitchBend) == 3);
}

TEST_CASE("a note rule's follow-up is a note off, carrying the same note and channel",
          "[trigger][midi]") {
    // §5.8's press-then-release, which is what turns a clip off. The follow-up must name the
    // same note on the same channel or it releases something else.
    takt4::trigger::Rule::Config config;
    config.id = "lasers";
    config.sendKind = Message::Kind::MidiNote;
    config.channel = 3;
    config.followUps.push_back(takt4::trigger::FollowUp{});
    takt4::trigger::Rule rule(config);

    Message fired;
    fired.kind = Message::Kind::MidiNote;
    fired.channel = 3;
    fired.number = 96;
    fired.value = 127;

    std::vector<std::pair<std::size_t, Message>> owed;
    rule.followUpsFor(takt4::trigger::Context{}, fired, owed);
    REQUIRE(owed.size() == 1);
    const Message& follow = owed[0].second;
    CHECK(follow.kind == Message::Kind::MidiNoteOff);
    CHECK(follow.number == 96);
    CHECK(follow.channel == 3);
    CHECK(follow.value == 0);
    // ...and that really is a note off on the cable, which is the half a kind alone does not
    // say. Channel 3 is wire channel 2, so 0x80 | 0x02.
    CHECK(takt4::output::midiStatusFor(follow.kind, follow.channel) == 0x82);
}

namespace {

/// A MIDI device that keeps every message it is sent, as bytes, under its own name.
struct Cable {
    std::vector<std::pair<std::string, std::vector<unsigned char>>> heard;
};

class CablePort final : public takt4::output::MidiPort {
public:
    CablePort(std::string name, std::shared_ptr<Cable> cable)
        : name_(std::move(name)), cable_(std::move(cable)) {}
    std::string open(std::string_view spec) override { return std::string(spec); }
    void close() noexcept override {}
    void send(std::span<const unsigned char> message) override {
        cable_->heard.emplace_back(name_, std::vector<unsigned char>(message.begin(), message.end()));
    }

private:
    std::string name_;
    std::shared_ptr<Cable> cable_;
};

takt4::output::OutputTarget midiOutput(const std::string& name, double delay = 0.0) {
    takt4::output::OutputTarget target;
    target.id = "o-" + name;
    target.name = name;
    target.kind = takt4::output::OutputTarget::Kind::Midi;
    target.device = name;
    target.delaySeconds = delay;
    return target;
}

Transports::Config midiRig(const std::vector<takt4::output::OutputTarget>& outputs,
                           const std::shared_ptr<Cable>& cable) {
    Transports::Config config;
    config.outputs = outputs;
    config.openMidi = [cable](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(name,
                                                           std::make_unique<CablePort>(name, cable));
    };
    return config;
}

Message midi(Message::Kind kind, int number, int value, std::uint64_t outputs = ~std::uint64_t{0}) {
    Message message;
    message.kind = kind;
    message.channel = 5;
    message.number = number;
    message.value = value;
    message.outputs = outputs;
    return message;
}

} // namespace

TEST_CASE("pitch bend and program change go on the wire as MIDI spells them", "[output][trigger][midi]") {
    // The audit of 2026-09-25, coverage gap 10: the bytes themselves, which nothing read. Pitch
    // bend is fourteen bits sent low seven first; program change is two bytes and no third.
    const auto cable = std::make_shared<Cable>();
    Transports transports(midiRig({midiOutput("desk")}, cable));
    RuleSink sink(transports);

    sink.send(midi(Message::Kind::MidiPitchBend, 0, 0x1234)); // 4660: low 0x34, high 0x24
    sink.send(midi(Message::Kind::MidiPitchBend, 0, 8192));   // centre
    sink.send(midi(Message::Kind::MidiPitchBend, 0, 20000));  // past the top: the top
    sink.send(midi(Message::Kind::MidiPitchBend, 0, -5));     // below the bottom: the bottom
    sink.send(midi(Message::Kind::MidiProgramChange, 42, 99));
    REQUIRE(cable->heard.size() == 5);
    using Bytes = std::vector<unsigned char>;
    CHECK(cable->heard[0].second == Bytes{0xE4, 0x34, 0x24});
    CHECK(cable->heard[1].second == Bytes{0xE4, 0x00, 0x40});
    CHECK(cable->heard[2].second == Bytes{0xE4, 0x7F, 0x7F});
    CHECK(cable->heard[3].second == Bytes{0xE4, 0x00, 0x00});
    CHECK(cable->heard[4].second == Bytes{0xC4, 42});
    CHECK(sink.delivered() == 5);
}

TEST_CASE("held MIDI follows its output through an edit of the outputs, and goes with it",
          "[output][trigger][midi]") {
    // Coverage gap 10. A note held for an output's delay carries the output's place in the list,
    // and the list can change before it goes — a row added above, one removed. `remap` moves each
    // held message to where its output now is, and drops one whose output is gone, rather than
    // sending it to whatever took its place.
    const auto cable = std::make_shared<Cable>();
    Transports transports(midiRig(
        {midiOutput("a", 0.5), midiOutput("b", 0.5), midiOutput("c", 0.5)}, cable));
    RuleSink sink(transports);
    sink.setNow(10.0);
    // A different note to each, so where each one lands shows.
    for (int i = 0; i < 3; ++i) {
        Message note = midi(Message::Kind::MidiNote, 60 + i, 100, std::uint64_t{1} << i);
        note.moment = 10.0;
        sink.send(note);
    }
    REQUIRE(sink.queued() == 3);
    REQUIRE(cable->heard.empty());

    // "a" removed, and the other two the other way round.
    transports.setOutputs({midiOutput("c", 0.5), midiOutput("b", 0.5)});
    sink.remap({-1, 1, 0}, {});
    CHECK(sink.queued() == 2);
    sink.releaseDue(10.6);
    CHECK(sink.queued() == 0);
    std::vector<std::pair<std::string, int>> landed;
    for (const auto& [device, bytes] : cable->heard) {
        landed.emplace_back(device, bytes.at(1));
    }
    std::sort(landed.begin(), landed.end());
    CHECK(landed == std::vector<std::pair<std::string, int>>{{"b", 61}, {"c", 62}});
}

TEST_CASE("a held lighting effect follows its fixture through a re-patch", "[output][trigger][dmx]") {
    // Coverage gap 10's other half. An effect waiting for its moment names fixtures by their place
    // in the patch; remove the one before it and it has to start on the fixture it was meant for,
    // which now sits one place up.
    Transports::Config config;
    takt4::dmx::Fixture first = takt4::dmx::fixtureFromMode("first", 1, 0, 1);
    first.id = "first";
    takt4::dmx::Fixture second = takt4::dmx::fixtureFromMode("second", 1, 0, 4);
    second.id = "second";
    config.patch = {first, second};
    Transports transports(config);
    RuleSink sink(transports);
    sink.setNow(10.0);
    Message red;
    red.kind = Message::Kind::Dmx;
    red.fixtures = 0b10; // the second
    red.payload.kind = takt4::dmx::EffectKind::Color;
    red.payload.color = takt4::dmx::Color{255, 0, 0};
    red.moment = 10.5;
    sink.send(red);
    REQUIRE(sink.queued() == 1);

    transports.setPatch({second});
    sink.remap({}, {-1, 0});
    sink.releaseDue(10.6);
    CHECK(sink.queued() == 0);
    // "second" starts at channel 4 and is now the patch's first fixture.
    const auto levels = transports.dmx().levels(0);
    REQUIRE(levels.size() >= 6);
    CHECK(levels[3] == 255);
    CHECK(sink.delivered() == 1);
}

TEST_CASE("held messages stop at the cap, the newest dropped and counted", "[output][trigger][midi]") {
    // Coverage gap 10. A second of delay on a rule firing every 32nd note at 215 BPM is 115 held
    // messages; past `kMaxQueued` something upstream is wrong, and the newest are dropped with a
    // count rather than a queue growing on the thread that runs the MIDI clock. What was kept goes
    // out, in the order it came.
    const auto cable = std::make_shared<Cable>();
    Transports transports(midiRig({midiOutput("desk", 1.0)}, cable));
    RuleSink sink(transports);
    sink.setNow(0.0);
    const std::size_t sent = RuleSink::kMaxQueued + 88;
    for (std::size_t i = 0; i < sent; ++i) {
        Message cc = midi(Message::Kind::MidiCc, static_cast<int>(i % 128), static_cast<int>(i / 128));
        sink.send(cc);
    }
    CHECK(sink.queued() == RuleSink::kMaxQueued);
    CHECK(sink.dropped() == 88);
    sink.releaseDue(1.0);
    REQUIRE(cable->heard.size() == RuleSink::kMaxQueued);
    for (std::size_t i = 0; i < cable->heard.size(); ++i) {
        INFO("message " << i);
        CHECK(cable->heard[i].second.at(1) == i % 128);
        CHECK(cable->heard[i].second.at(2) == i / 128);
    }

    SECTION("and the lighting's queue has the same cap") {
        Transports::Config config;
        takt4::dmx::Fixture par = takt4::dmx::fixtureFromMode("par", 1, 0, 1);
        par.id = "par";
        config.patch = {par};
        Transports lit(config);
        RuleSink lighting(lit);
        lighting.setNow(0.0);
        for (std::size_t i = 0; i < sent; ++i) {
            Message red;
            red.kind = Message::Kind::Dmx;
            red.fixtures = 0b1;
            red.payload.kind = takt4::dmx::EffectKind::Color;
            red.payload.color = takt4::dmx::Color{static_cast<std::uint8_t>(i % 256), 0, 0};
            red.moment = 1.0;
            lighting.send(red);
        }
        CHECK(lighting.queued() == RuleSink::kMaxQueued);
        CHECK(lighting.dropped() == 88);
    }
}

TEST_CASE("a CC follow-up stays a CC, because a controller has no off message", "[trigger][midi]") {
    takt4::trigger::Rule::Config config;
    config.id = "dimmer";
    config.sendKind = Message::Kind::MidiCc;
    config.followUps.push_back(takt4::trigger::FollowUp{});
    takt4::trigger::Rule rule(config);

    Message fired;
    fired.kind = Message::Kind::MidiCc;
    fired.number = 21;
    fired.value = 127;

    std::vector<std::pair<std::size_t, Message>> owed;
    rule.followUpsFor(takt4::trigger::Context{}, fired, owed);
    REQUIRE(owed.size() == 1);
    CHECK(owed[0].second.kind == Message::Kind::MidiCc);
    CHECK(owed[0].second.value == 0);
}
