#include "core/output/rule_sink.hpp"
#include "core/output/transports.hpp"
#include "core/trigger/rule.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <string>
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
    // The operator's laser controller (Pangolin Liberation) holds its clip until 0x80
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
    rule.followUpsFor(fired, owed);
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
    rule.followUpsFor(fired, owed);
    REQUIRE(owed.size() == 1);
    CHECK(owed[0].second.kind == Message::Kind::MidiCc);
    CHECK(owed[0].second.value == 0);
}
