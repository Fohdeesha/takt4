#include "core/remote/remote.hpp"
#include "core/remote/status_messages.hpp"

#include "support/osc_messages.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using Catch::Approx;
using takt4::remote::CommandQueue;
using takt4::remote::Packet;
using takt4::remote::Status;
using takt4::testing::findOsc;
using takt4::testing::OscArg;
using takt4::testing::oscMessagesIn;
using takt4::testing::OscMsg;

namespace {

/// A status with something other than its default in every value.
Status everything() {
    Status s;
    s.running = true;
    s.locked = true;
    s.hunting = false;
    s.state = "locked";
    s.confidence = 0.8;
    s.bpm = 128.0;
    s.levelDb = -18.5;
    s.load = 0.25;
    s.dropouts = 3;
    s.source = "audio";
    s.sourceOk = true;
    s.sourceDetail = "-18 dB";
    s.panic = true;
    s.audioHost = "ASIO";
    s.audioDevice = "MOTU Pro Audio";
    s.audioChannel = 3;
    s.audioMono = true;
    s.audioRate = 48000;
    s.latencyMs = 47.0;
    s.version = "1.6.0";
    return s;
}

std::vector<OscMsg> readAll(const std::vector<Packet>& packets) {
    std::vector<OscMsg> out;
    for (const Packet& packet : packets) {
        for (OscMsg& message : oscMessagesIn(packet)) {
            out.push_back(std::move(message));
        }
    }
    return out;
}

} // namespace

TEST_CASE("remote setup's list of senders is dotted addresses, and anything else is said, never "
          "guessed at",
          "[remote]") {
    std::vector<std::string> rejected;
    CHECK(takt4::remote::senderList("192.168.1.70, 10.0.0.2;127.0.0.1\t192.168.1.70", &rejected) ==
          std::vector<std::uint32_t>{0xC0A80146u, 0x0A000002u, 0x7F000001u});
    CHECK(rejected.empty());

    // A name would be looked up, a port or a mask is not a sender, and a number past 255 is no
    // address: each said back as typed, and none of them taken as something near it.
    CHECK(takt4::remote::senderList("192.168.1.256 1.2.3 1.2.3.4.5 bridge.local 1.2.3.4:9000 "
                                    "192.168.1.0/24 0001.2.3.4 ..1 1.2.3.4.",
                                    &rejected)
              .empty());
    CHECK(rejected == std::vector<std::string>{"192.168.1.256", "1.2.3", "1.2.3.4.5",
                                               "bridge.local", "1.2.3.4:9000", "192.168.1.0/24",
                                               "0001.2.3.4", "..1", "1.2.3.4."});
    rejected.clear();
    CHECK(takt4::remote::senderList("", &rejected).empty());
    CHECK(takt4::remote::senderList(" , ;", &rejected).empty());
    CHECK(rejected.empty());
}

TEST_CASE("a remote command is stamped as it is queued, and the queue holds a bounded number",
          "[remote]") {
    CommandQueue queue;
    takt4::remote::Command command;
    command.address = "/takt4/ctl/audio/start";
    const auto before = std::chrono::steady_clock::now();
    REQUIRE(queue.push(command));
    const auto after = std::chrono::steady_clock::now();
    const std::vector<takt4::remote::Command> drained = queue.drain();
    REQUIRE(drained.size() == 1);
    CHECK(drained[0].queuedAt >= before);
    CHECK(drained[0].queuedAt <= after);
    CHECK(queue.drain().empty());

    // A window that is not draining it is not queued behind for ever: the sender is told busy.
    for (std::size_t i = 0; i < CommandQueue::kCapacity; ++i) {
        REQUIRE(queue.push(command));
    }
    CHECK_FALSE(queue.push(command));
    CHECK(queue.drain().size() == CommandQueue::kCapacity);
}

TEST_CASE("the status goes as one bundle of every value, and between snapshots only what moved",
          "[remote]") {
    const Status s = everything();
    const Packet bundle =
        takt4::remote::bundleOf(takt4::remote::statusMessages("/takt4", s, 3600, 12));
    // One datagram, and a bundle at "immediately": a snapshot read from it is one moment's.
    REQUIRE(bundle.size() > 16);
    CHECK(std::string(reinterpret_cast<const char*>(bundle.data()), 8) ==
          std::string("#bundle\0", 8));
    CHECK(static_cast<int>(bundle[15]) == 1);

    const std::vector<OscMsg> all = oscMessagesIn(bundle);
    std::set<std::string> addresses;
    for (const OscMsg& message : all) {
        addresses.insert(message.address);
    }
    CHECK(addresses.size() == all.size()); // each once
    CHECK(addresses ==
          std::set<std::string>{"/takt4/status/running",      "/takt4/status/locked",
                                "/takt4/status/hunting",      "/takt4/status/state",
                                "/takt4/status/confidence",   "/takt4/status/bpm",
                                "/takt4/status/level",        "/takt4/status/load",
                                "/takt4/status/dropouts",     "/takt4/status/source",
                                "/takt4/status/source_ok",    "/takt4/status/source_detail",
                                "/takt4/status/panic",        "/takt4/status/audio/host",
                                "/takt4/status/audio/device", "/takt4/status/audio/channel",
                                "/takt4/status/audio/mono",   "/takt4/status/audio/rate",
                                "/takt4/status/latency_ms",   "/takt4/status/version",
                                "/takt4/status/uptime_s",     "/takt4/status/ui_age_ms"});
    const auto integer = [&all](const char* address) {
        const OscMsg* message = findOsc(all, address);
        REQUIRE(message != nullptr);
        REQUIRE(message->args.size() == 1);
        REQUIRE(message->args[0].type == 'i');
        return message->args[0].i;
    };
    const auto number = [&all](const char* address) {
        const OscMsg* message = findOsc(all, address);
        REQUIRE(message != nullptr);
        REQUIRE(message->args.size() == 1);
        REQUIRE(message->args[0].type == 'f');
        return static_cast<double>(message->args[0].f);
    };
    const auto text = [&all](const char* address) {
        const OscMsg* message = findOsc(all, address);
        REQUIRE(message != nullptr);
        REQUIRE(message->args.size() == 1);
        REQUIRE(message->args[0].type == 's');
        return message->args[0].s;
    };
    CHECK(integer("/takt4/status/running") == 1);
    CHECK(integer("/takt4/status/locked") == 1);
    CHECK(integer("/takt4/status/hunting") == 0);
    CHECK(text("/takt4/status/state") == "locked");
    CHECK(number("/takt4/status/confidence") == Approx(0.8));
    CHECK(number("/takt4/status/bpm") == Approx(128.0));
    CHECK(number("/takt4/status/level") == Approx(-18.5));
    CHECK(number("/takt4/status/load") == Approx(0.25));
    CHECK(integer("/takt4/status/dropouts") == 3);
    CHECK(text("/takt4/status/source") == "audio");
    CHECK(integer("/takt4/status/source_ok") == 1);
    CHECK(text("/takt4/status/source_detail") == "-18 dB");
    CHECK(integer("/takt4/status/panic") == 1);
    CHECK(text("/takt4/status/audio/host") == "ASIO");
    CHECK(text("/takt4/status/audio/device") == "MOTU Pro Audio");
    CHECK(integer("/takt4/status/audio/channel") == 3);
    CHECK(integer("/takt4/status/audio/mono") == 1);
    CHECK(integer("/takt4/status/audio/rate") == 48000);
    CHECK(number("/takt4/status/latency_ms") == Approx(47.0));
    CHECK(text("/takt4/status/version") == "1.6.0");
    CHECK(integer("/takt4/status/uptime_s") == 3600);
    CHECK(integer("/takt4/status/ui_age_ms") == 12);

    // Between snapshots, loose: what moved, by more than it is worth a datagram — never the
    // level, which moves with every hop of music and goes once a second with the rest.
    Status moved = s;
    moved.bpm = 128.004;
    moved.confidence = 0.82;
    moved.levelDb = -6.0;
    moved.load = 0.9;
    moved.locked = false;
    moved.hunting = true;
    moved.state = "hunting";
    std::vector<std::string> changed;
    for (const OscMsg& message : readAll(takt4::remote::changedMessages("/takt4", s, moved))) {
        changed.push_back(message.address);
    }
    CHECK(changed == std::vector<std::string>{"/takt4/status/locked", "/takt4/status/hunting",
                                              "/takt4/status/state"});
    CHECK(takt4::remote::changedMessages("/takt4", s, s).empty());
    moved.bpm = 128.5;
    moved.dropouts = 4;
    const std::vector<OscMsg> again = readAll(takt4::remote::changedMessages("/takt4", s, moved));
    REQUIRE(findOsc(again, "/takt4/status/bpm") != nullptr);
    CHECK(findOsc(again, "/takt4/status/bpm")->args[0].f == Approx(128.5f));
    REQUIRE(findOsc(again, "/takt4/status/dropouts") != nullptr);
    CHECK(findOsc(again, "/takt4/status/dropouts")->args[0].i == 4);
    CHECK(findOsc(again, "/takt4/status/level") == nullptr);
    CHECK(findOsc(again, "/takt4/status/load") == nullptr);
}

TEST_CASE("a change of source_ok comes after the source_detail that says why", "[remote]") {
    // show controller judges each datagram as it comes, and source_ok went before the detail: a
    // source lost was a warning saying takt4 gave no reason, the reason following a moment later.
    const Status s = everything();
    const auto order = [](const Status& before, const Status& after) {
        std::vector<std::string> addresses;
        for (const OscMsg& message :
             readAll(takt4::remote::changedMessages("/takt4", before, after))) {
            addresses.push_back(message.address);
        }
        return addresses;
    };
    Status lost = s;
    lost.sourceOk = false;
    lost.sourceDetail = "no peers \xE2\x80\x94 coasting at 128.00 BPM";
    CHECK(order(s, lost) ==
          std::vector<std::string>{"/takt4/status/source_detail", "/takt4/status/source_ok"});
    // The detail came a look before, on its own: it goes again with the change.
    Status detailFirst = s;
    detailFirst.sourceDetail = lost.sourceDetail;
    CHECK(order(detailFirst, lost) ==
          std::vector<std::string>{"/takt4/status/source_detail", "/takt4/status/source_ok"});
    // A detail that moves on its own goes alone.
    Status moved = s;
    moved.sourceDetail = "-12 dB";
    CHECK(order(s, moved) == std::vector<std::string>{"/takt4/status/source_detail"});
}

TEST_CASE("the outputs and the devices are a message each, with how many after them, and an "
          "output says why it reaches nothing",
          "[remote]") {
    Status s;
    s.outputs = {{"o-00000001", "stage", "osc", "192.168.1.60", 19211, true, 0.0, true, ""},
                 {"o-00000002", "lights", "artnet", "10.0.0.5", 6454, false, 12.5, false,
                  "10.0.0.5 cannot be reached"}};
    s.devices = {{"ASIO", "MOTU Pro Audio", 28}, {"Windows WASAPI", "Microphone", 2}};

    const std::vector<OscMsg> outputs = readAll(takt4::remote::outputMessages("/takt4", s));
    REQUIRE(outputs.size() == 3);
    CHECK(outputs[0].address == "/takt4/status/output");
    REQUIRE(outputs[1].args.size() == 9);
    const std::vector<OscArg> lights = outputs[1].args;
    CHECK(lights[0].s == "o-00000002");
    CHECK(lights[1].s == "lights");
    CHECK(lights[2].s == "artnet");
    CHECK(lights[3].s == "10.0.0.5");
    CHECK(lights[4].i == 6454);
    CHECK(lights[5].i == 0);
    CHECK(lights[6].f == Approx(12.5f));
    CHECK(lights[7].i == 0);
    CHECK(lights[8].type == 's');
    CHECK(lights[8].s == "10.0.0.5 cannot be reached");
    CHECK(outputs[0].args[8].s.empty());
    CHECK(outputs[2].address == "/takt4/status/outputs");
    CHECK(outputs[2].args[0].i == 2);

    const std::vector<OscMsg> devices = readAll(takt4::remote::deviceMessages("/takt4", s));
    REQUIRE(devices.size() == 3);
    CHECK(devices[1].address == "/takt4/status/audio/device");
    CHECK(devices[1].args[0].s == "Windows WASAPI");
    CHECK(devices[1].args[1].s == "Microphone");
    CHECK(devices[1].args[2].i == 2);
    CHECK(devices[2].address == "/takt4/status/audio/devices");
    CHECK(devices[2].args[0].i == 2);
}

TEST_CASE("a reply carries the request number it answers, and a refusal without one says what "
          "it refused",
          "[remote]") {
    const std::vector<OscMsg> ok =
        oscMessagesIn(takt4::remote::replyMessage("/takt4", 41, true, "timing from Ableton Link"));
    REQUIRE(ok.size() == 1);
    CHECK(ok[0].address == "/takt4/reply");
    REQUIRE(ok[0].args.size() == 3);
    CHECK(ok[0].args[0].i == 41);
    CHECK(ok[0].args[1].s == "ok");
    CHECK(ok[0].args[2].s == "timing from Ableton Link");

    const std::vector<OscMsg> refused = oscMessagesIn(
        takt4::remote::refusedMessage("/takt4", "/takt4/ctl/audio/start", "remote setup is off"));
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].address == "/takt4/status/refused");
    CHECK(refused[0].args[0].s == "/takt4/ctl/audio/start");
    CHECK(refused[0].args[1].s == "remote setup is off");

    // A note longer than a message can hold is cut on a character, never inside one.
    std::string longNote;
    for (int i = 0; i < 100; ++i) {
        longNote += "\xE2\x82\xAC"; // the euro sign, three bytes, so the 160th is inside one
    }
    const std::vector<OscMsg> cut =
        oscMessagesIn(takt4::remote::replyMessage("/takt4", 1, false, longNote));
    REQUIRE(cut.size() == 1);
    const std::string& note = cut[0].args[2].s;
    CHECK(note.size() == 159);
    CHECK(note == longNote.substr(0, note.size()));

    const std::vector<OscMsg> line = oscMessagesIn(
        takt4::remote::logMessage("/takt4", {"warning", "No audio from MOTU Pro Audio"}));
    REQUIRE(line.size() == 1);
    CHECK(line[0].address == "/takt4/status/log");
    CHECK(line[0].args[0].s == "warning");
    CHECK(line[0].args[1].s == "No audio from MOTU Pro Audio");
}
