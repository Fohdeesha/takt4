#include "core/settings/settings.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using Catch::Matchers::WithinAbs;
using takt4::settings::Settings;
using takt4::test::TempDir;

namespace {

Settings roundTrip(const Settings& in) {
    return takt4::settings::fromJson(takt4::settings::toJson(in));
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

TEST_CASE("settings survive a round trip through the file's text", "[settings]") {
    Settings in;
    in.machine.deviceName = "MOTU Pro Audio";
    in.machine.hostApiName = "ASIO";
    in.machine.channel = 6;
    in.machine.midiClockPort = "Microsoft GS Wavetable Synth 0";
    in.machine.midiControlPort = "MOTU Pro Audio Midi In 1";
    in.machine.midiBindings = {"note 36 ch 10 -> tap", "cc 64 ch 1 -> lock",
                               "cc 21 ch 1 -> rule/drop/enable"};
    in.machine.oscControlEnabled = true;
    in.machine.oscControlPort = 7005;
    in.machine.oscControlLocalOnly = false;
    in.preset.tempo.minBpm = 88.0;
    in.preset.tempo.maxBpm = 176.0;
    in.preset.tempo.octaveFold = false;
    in.preset.decoder = takt4::tracking::Decoder::ParticleFilter;
    in.preset.meters = {3, 4, 0, 0};
    in.preset.tempo.confidenceThreshold = 0.25;
    in.preset.tempo.latencyOffsetSeconds = -0.030;
    in.preset.link = true;
    in.preset.oscPrefix = "/vj";
    in.preset.outputs = takt4::output::oscOutputs({{"192.168.1.40", 7000}, {"127.0.0.1", 7001}});
    // A named one, and a MIDI one, and one switched off — the three things a target can be
    // that an address pair could not say.
    in.preset.outputs.front().name = "wall";
    takt4::output::OutputTarget lights;
    lights.name = "lights";
    lights.kind = takt4::output::OutputTarget::Kind::Midi;
    lights.device = "MOTU Pro Audio Midi Out 1";
    in.preset.outputs.push_back(lights);
    takt4::output::OutputTarget spare;
    spare.name = "spare";
    spare.host = "10.0.0.9";
    spare.port = 9000;
    spare.enabled = false;
    in.preset.outputs.push_back(spare);

    const Settings out = roundTrip(in);
    CHECK(out.machine.deviceName == in.machine.deviceName);
    CHECK(out.machine.hostApiName == in.machine.hostApiName);
    CHECK(out.machine.channel == 6);
    CHECK(out.machine.midiClockPort == in.machine.midiClockPort);
    CHECK(out.machine.midiControlPort == in.machine.midiControlPort);
    CHECK(out.machine.midiBindings == in.machine.midiBindings);
    CHECK(out.machine.oscControlEnabled);
    CHECK(out.machine.oscControlPort == 7005);
    CHECK_FALSE(out.machine.oscControlLocalOnly);
    CHECK_THAT(out.preset.tempo.minBpm, WithinAbs(88.0, 1e-9));
    CHECK_THAT(out.preset.tempo.maxBpm, WithinAbs(176.0, 1e-9));
    CHECK_FALSE(out.preset.tempo.octaveFold);
    CHECK(out.preset.decoder == takt4::tracking::Decoder::ParticleFilter);
    // And the default is the forward filter, whatever a file says that is not "particle".
    CHECK(Settings{}.preset.decoder == takt4::tracking::Decoder::Forward);
    CHECK(takt4::settings::fromJson(R"({"preset": {"decoder": "banana"}})").preset.decoder ==
          takt4::tracking::Decoder::Forward);
    CHECK(takt4::settings::fromJson(R"({"preset": {"decoder": "forward"}})").preset.decoder ==
          takt4::tracking::Decoder::Forward);
    // The meters travel as a list, and the default is a bar of four alone.
    CHECK(out.preset.meters == std::array<std::uint8_t, 4>{3, 4, 0, 0});
    CHECK(Settings{}.preset.meters == std::array<std::uint8_t, 4>{4, 0, 0, 0});
    CHECK(takt4::settings::fromJson(R"({"preset": {"meters": [3, 4]}})").preset.meters ==
          std::array<std::uint8_t, 4>{3, 4, 0, 0});
    // A list the decoder could not be built from leaves the default standing.
    CHECK(takt4::settings::fromJson(R"({"preset": {"meters": []}})").preset.meters ==
          std::array<std::uint8_t, 4>{4, 0, 0, 0});
    CHECK(takt4::settings::fromJson(R"({"preset": {"meters": [0, 4]}})").preset.meters ==
          std::array<std::uint8_t, 4>{4, 0, 0, 0});
    CHECK(takt4::settings::fromJson(R"({"preset": {"meters": "4"}})").preset.meters ==
          std::array<std::uint8_t, 4>{4, 0, 0, 0});
    CHECK(takt4::settings::fromJson(R"({"preset": {"meters": [2, 3, 4, 6, 8]}})").preset.meters ==
          std::array<std::uint8_t, 4>{4, 0, 0, 0});
    CHECK_THAT(out.preset.tempo.confidenceThreshold, WithinAbs(0.25, 1e-9));
    CHECK_THAT(out.preset.tempo.latencyOffsetSeconds, WithinAbs(-0.030, 1e-9));
    CHECK(out.preset.link);
    CHECK(out.preset.oscPrefix == "/vj");
    REQUIRE(out.preset.outputs.size() == 4);
    CHECK(out.preset.outputs[0].name == "wall");
    CHECK(out.preset.outputs[0].host == "192.168.1.40");
    CHECK(out.preset.outputs[0].port == 7000);
    CHECK(out.preset.outputs[1].port == 7001);
    CHECK(out.preset.outputs[2].kind == takt4::output::OutputTarget::Kind::Midi);
    CHECK(out.preset.outputs[2].device == "MOTU Pro Audio Midi Out 1");
    CHECK(out.preset.outputs[3].name == "spare");
    CHECK_FALSE(out.preset.outputs[3].enabled);
}

TEST_CASE("the two layers stay apart in the file", "[settings]") {
    // Q7's whole point: the portable half has to be liftable into a preset file without a
    // migration, which means it cannot be mixed in with the machine's own business.
    const std::string text = takt4::settings::toJson(Settings{});
    const std::size_t machine = text.find("\"machine\"");
    const std::size_t preset = text.find("\"preset\"");
    REQUIRE(machine != std::string::npos);
    REQUIRE(preset != std::string::npos);
    // Nothing device-shaped inside the portable half.
    CHECK(text.find("deviceName", preset) == std::string::npos);
    CHECK(text.find("midiClockPort", preset) == std::string::npos);
    // Nor the control surface: "note 36 is tap" is a fact about the box of buttons on
    // this desk, and carrying it to a machine with a different controller is worse than
    // carrying nothing.
    CHECK(text.find("midiControlPort", preset) == std::string::npos);
    CHECK(text.find("midiBindings", preset) == std::string::npos);
    // Nor the listening socket, and for a sharper reason than the rest: a preset carrying
    // "listen on 0.0.0.0:7001" to somebody else's laptop would open a port they never
    // asked for.
    CHECK(text.find("oscControlEnabled", preset) == std::string::npos);
    CHECK(text.find("oscControlPort", preset) == std::string::npos);
    CHECK(text.find("oscControlLocalOnly", preset) == std::string::npos);
    CHECK(text.find("version") != std::string::npos);
}

TEST_CASE("a file that cannot be understood gives the defaults", "[settings]") {
    // Settings that will not parse must never be the reason an app does not open.
    const Settings defaults;
    for (const char* text : {"", "   ", "not json at all", "[1, 2, 3]", "{", "null",
                             "{\"machine\": 7}", "{\"preset\": {\"tempo\": \"nope\"}}"}) {
        INFO("input: " << text);
        const Settings out = takt4::settings::fromJson(text);
        CHECK(out.machine.deviceName.empty());
        CHECK(out.machine.channel == 0);
        CHECK_THAT(out.preset.tempo.minBpm, WithinAbs(defaults.preset.tempo.minBpm, 1e-9));
        CHECK_THAT(out.preset.tempo.maxBpm, WithinAbs(defaults.preset.tempo.maxBpm, 1e-9));
    }
}

TEST_CASE("a field the file does not mention keeps its default", "[settings]") {
    // What makes a file written by an older build still load.
    const Settings defaults;
    const Settings out = takt4::settings::fromJson(R"({"machine": {"channel": 3}})");
    CHECK(out.machine.channel == 3);
    CHECK(out.machine.deviceName.empty());
    CHECK_THAT(out.preset.tempo.minBpm, WithinAbs(defaults.preset.tempo.minBpm, 1e-9));
    CHECK(out.preset.oscPrefix == defaults.preset.oscPrefix);
    CHECK(out.preset.outputs.empty());
    // The listening socket most of all: a build that did not have this field must not
    // load as one that opens a port.
    CHECK_FALSE(out.machine.oscControlEnabled);
    CHECK(out.machine.oscControlPort == defaults.machine.oscControlPort);
    CHECK(out.machine.oscControlLocalOnly);
}

TEST_CASE("a control port a file could not have meant is refused", "[settings]") {
    // A hand-edited file is exactly where "port 99999" comes from, and reading it into a
    // uint16_t would take the low bits — 33465, a port nobody meant and nobody can guess.
    const auto portIn = [](const std::string& value) {
        return takt4::settings::fromJson(R"({"machine": {"oscControlPort": )" + value + "}}")
            .machine.oscControlPort;
    };

    for (const char* value : {"99999", "-1", "\"7001\"", "1.5", "null"}) {
        INFO("port: " << value);
        CHECK(portIn(value) == 7001); // the default, kept
    }

    // 0 is not out of range: it asks the platform for a free port, which `OscReceiver`
    // supports and the window reports back once it has one.
    CHECK(portIn("0") == 0);
    CHECK(portIn("65535") == 65535);
}

TEST_CASE("a setting a tracker could not honour is refused, not passed on", "[settings]") {
    // `TempoTracker::setOptions` is noexcept and trusts its caller, and an inverted fold
    // window makes `foldInto` return the tempo unfolded — which looks like the fold
    // quietly not working rather than like a file being wrong. A file is not a caller
    // worth trusting.
    const Settings defaults;

    const Settings inverted =
        takt4::settings::fromJson(R"({"preset": {"tempo": {"minBpm": 200, "maxBpm": 100}}})");
    CHECK_THAT(inverted.preset.tempo.minBpm, WithinAbs(defaults.preset.tempo.minBpm, 1e-9));
    CHECK_THAT(inverted.preset.tempo.maxBpm, WithinAbs(defaults.preset.tempo.maxBpm, 1e-9));

    const Settings absurd =
        takt4::settings::fromJson(R"({"preset": {"tempo": {"minBpm": 0, "maxBpm": 99999}}})");
    CHECK_THAT(absurd.preset.tempo.minBpm, WithinAbs(defaults.preset.tempo.minBpm, 1e-9));

    const Settings gate =
        takt4::settings::fromJson(R"({"preset": {"tempo": {"confidenceThreshold": 5}}})");
    CHECK_THAT(gate.preset.tempo.confidenceThreshold,
               WithinAbs(defaults.preset.tempo.confidenceThreshold, 1e-9));

    const Settings latency =
        takt4::settings::fromJson(R"({"preset": {"tempo": {"latencyOffsetSeconds": 90}}})");
    CHECK_THAT(latency.preset.tempo.latencyOffsetSeconds, WithinAbs(0.0, 1e-9));

    const Settings channel = takt4::settings::fromJson(R"({"machine": {"channel": -4}})");
    CHECK(channel.machine.channel == 0);

    // A target with no host, or a port outside the range, is left out rather than sent to.
    const Settings targets = takt4::settings::fromJson(
        R"({"preset": {"outputs": [" = :7000", "a:0", "b:99999", "good = 10.0.0.1:7000"]}})");
    REQUIRE(targets.preset.outputs.size() == 1);
    CHECK(targets.preset.outputs[0].name == "good");
    CHECK(targets.preset.outputs[0].host == "10.0.0.1");
}

TEST_CASE("a settings file written before targets had names still loads", "[settings]") {
    // The shape older builds wrote: an array of {host, port} objects under `oscTargets`.
    // Dropping it silently would cost an operator every target they had typed, which is the
    // one upgrade failure they would notice and could not diagnose.
    const Settings out = takt4::settings::fromJson(
        R"({"preset": {"oscTargets": [{"host": "192.168.1.40", "port": 7000},
                                      {"host": "", "port": 7000},
                                      {"host": "127.0.0.1", "port": 7001}]}})");
    REQUIRE(out.preset.outputs.size() == 2);
    CHECK(out.preset.outputs[0].host == "192.168.1.40");
    CHECK(out.preset.outputs[0].kind == takt4::output::OutputTarget::Kind::Osc);
    // Named after its own address, which is what an unnamed target has always been called.
    CHECK(out.preset.outputs[0].name == "192.168.1.40:7000");
    CHECK(out.preset.outputs[1].port == 7001);
}

TEST_CASE("settings write to a file and read back from it", "[settings]") {
    const TempDir dir;
    const std::filesystem::path path = dir.path() / "nested" / "settings.json";

    Settings in;
    in.machine.deviceName = "MOTU Pro Audio";
    in.machine.channel = 6;
    in.preset.link = true;
    // The directory does not exist yet; saving makes it.
    REQUIRE(takt4::settings::save(in, path));
    REQUIRE(std::filesystem::exists(path));

    const Settings out = takt4::settings::load(path);
    CHECK(out.machine.deviceName == "MOTU Pro Audio");
    CHECK(out.machine.channel == 6);
    CHECK(out.preset.link);
}

TEST_CASE("a missing or unreadable settings file is not an error", "[settings]") {
    const TempDir dir;
    const Settings missing = takt4::settings::load(dir.path() / "there-is-no-file.json");
    CHECK(missing.machine.deviceName.empty());

    // A directory where a file should be: openable as neither.
    CHECK(takt4::settings::load(dir.path()).machine.deviceName.empty());
    CHECK(takt4::settings::load({}).machine.deviceName.empty());
    CHECK_FALSE(takt4::settings::save(Settings{}, {}));

    // Half a file, as a crash mid-write would leave.
    const std::filesystem::path truncated = dir.path() / "half.json";
    write(truncated, R"({"machine": {"deviceName": "MOTU)");
    CHECK(takt4::settings::load(truncated).machine.deviceName.empty());
}

TEST_CASE("the settings file lives beside the program", "[settings]") {
    // What makes takt4 something an operator can copy onto a stick: the settings travel
    // with the executable rather than staying in a profile on one machine.
    const std::filesystem::path file = takt4::settings::settingsFile();
    if (file.empty()) {
        SKIP("this environment names neither an executable nor a config directory");
    }
    CHECK(file.is_absolute());
    CHECK(file.filename() == "settings.json");
    CHECK(file.parent_path() == takt4::settings::settingsDirectory());

    // This test binary is the program, so its own directory is the answer — and finding an
    // executable in there is what shows the path came from the running program rather than
    // from an environment variable, which is the whole change.
    std::error_code code;
    const std::filesystem::path directory = takt4::settings::settingsDirectory();
    REQUIRE(std::filesystem::is_directory(directory, code));
    bool foundAnExecutable = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        // Named rather than extension-matched, so this reads the same on a platform where an
        // executable has no extension at all.
        if (entry.path().stem().string().starts_with("takt4")) {
            foundAnExecutable = true;
            break;
        }
    }
    CHECK(foundAnExecutable);
}

TEST_CASE("settings left by an older build are still read", "[settings]") {
    // The per-user location is where these used to be kept. A rig that has one there must
    // not lose its outputs, its device and its MIDI bindings just because the file moved,
    // so it is read until a save writes one beside the executable.
    const std::filesystem::path beside = takt4::settings::settingsFile();
    const std::filesystem::path user = takt4::settings::userSettingsDirectory();
    if (beside.empty()) {
        SKIP("this environment names no executable");
    }

    std::error_code code;
    const bool haveNew = std::filesystem::exists(beside, code);
    const bool haveOld = !user.empty() && std::filesystem::exists(user / "settings.json", code);

    // Whichever exists, `existingSettingsFile` names one that can be read; and the new
    // location wins whenever both are there, so the move only ever happens once.
    if (haveNew) {
        CHECK(takt4::settings::existingSettingsFile() == beside);
    } else if (haveOld) {
        CHECK(takt4::settings::existingSettingsFile() == user / "settings.json");
    } else {
        // Neither: it still names where one would be written, and loading it gives defaults
        // rather than failing.
        CHECK(takt4::settings::existingSettingsFile() == beside);
        CHECK(takt4::settings::load(takt4::settings::existingSettingsFile())
                  .machine.deviceName.empty());
    }
}

TEST_CASE("a name this program did not choose cannot stop the file being written", "[settings]") {
    // **The whole of a crash on Stop.** nlohmann refuses to write a string that is not valid
    // UTF-8 by throwing, and half the strings in this file are names takt4 was handed rather
    // than names it made: PortAudio gives a device name as the driver spelled it, RtMidi a
    // port name as the platform did, and neither promises UTF-8. The throw came out of
    // `settings::save` into `ui::run`'s save on the way out, where nothing catches it — so
    // the process died and the operator's whole configuration went with it.
    //
    // `error_handler_t::replace` is the fix: the byte nothing can decode becomes U+FFFD and
    // every other setting in the file survives. 0xB5 is 'µ' in Latin-1 and is not a legal
    // UTF-8 byte on its own; a driver with it in its name is all it took.
    Settings in;
    in.machine.deviceName = "Focusrite \xB5 Interface";
    in.machine.midiClockPort = "Bad \xFF Port";
    in.preset.oscPrefix = "/takt4";

    std::string json;
    REQUIRE_NOTHROW(json = takt4::settings::toJson(in));
    CHECK_FALSE(json.empty());

    // And it is still a settings file: what round-trips is everything but the bytes that
    // could not be decoded.
    const Settings back = takt4::settings::fromJson(json);
    CHECK(back.machine.deviceName.find("Focusrite") != std::string::npos);
    CHECK(back.machine.deviceName.find("Interface") != std::string::npos);
    CHECK(back.preset.oscPrefix == "/takt4");

    SECTION("and it really reaches the disk") {
        const TempDir dir;
        const std::filesystem::path path = dir.path() / "settings.json";
        CHECK(takt4::settings::save(in, path));
        CHECK(std::filesystem::exists(path));
    }

    SECTION("the same for a rule carrying one") {
        // A text value is cut to `Value::kTextCapacity`, and a cut through the middle of a
        // character used to make the same throw from the other end. `Value::ofText` cuts on
        // a boundary now; this holds the two halves of that together.
        Settings withRule;
        takt4::trigger::Rule::Config rule;
        rule.id = "r";
        rule.address = "/deck/{name}";
        takt4::trigger::Generator::Config segment;
        segment.kind = takt4::trigger::GeneratorKind::Fixed;
        segment.fixed = takt4::trigger::Value::ofText(std::string(46, 'a') + "\xC3\xA9");
        rule.segments.push_back(segment);
        withRule.preset.rules.push_back(rule);

        std::string text;
        REQUIRE_NOTHROW(text = takt4::settings::toJson(withRule));
        const Settings read = takt4::settings::fromJson(text);
        REQUIRE(read.preset.rules.size() == 1);
        REQUIRE(read.preset.rules.front().segments.size() == 1);
        // The accented character was the one thing that did not fit, so what comes back is
        // the 46 that did — not a replacement character, because nothing was ever malformed.
        CHECK(read.preset.rules.front().segments.front().fixed.text() == std::string(46, 'a'));
    }
}

TEST_CASE("save reports a failure rather than raising one", "[settings]") {
    // The header promises "false when it could not be written", and both callers depend on
    // it literally: `ui::run` saves after the event loop has returned, where an exception
    // has nowhere to go, and `WindowController::saveNow` is a Slint callback, where one
    // crossing back into the toolkit takes the process with it.
    const Settings in;
    CHECK_FALSE(takt4::settings::save(in, {}));
    // A directory where a file should be: `create_directories` makes the parent, the open
    // then fails, and nothing is raised.
    const TempDir dir;
    const std::filesystem::path occupied = dir.path() / "settings.json";
    std::filesystem::create_directories(occupied);
    CHECK_FALSE(takt4::settings::save(in, occupied));
}

TEST_CASE("the lighting patch survives the round trip", "[settings][dmx]") {
    Settings in;
    takt4::dmx::Fixture wash = takt4::dmx::fixtureFromMode("wash L", 1, 0, 1);
    wash.group = "washes";
    takt4::dmx::Fixture head = takt4::dmx::fixtureFromMode("head 1", 6, 4, 100);
    head.group = "heads";
    // Aimed at the floor and away from the audience, which is the whole reason the window
    // exists and the one part of a patch that must not be lost in a file.
    head.panMin = 0.2;
    head.panMax = 0.8;
    head.tiltMin = 0.45;
    head.tiltMax = 0.65;
    head.enabled = false;
    in.preset.fixtures = {wash, head};

    const Settings out = roundTrip(in);
    REQUIRE(out.preset.fixtures.size() == 2);
    CHECK(out.preset.fixtures[0] == wash);
    CHECK(out.preset.fixtures[1] == head);

    SECTION("an unknown role keeps its channel rather than re-addressing everything below it") {
        // Dropping it would move every channel after it up one, silently re-patching the
        // fixture. `Unused` keeps the footprint honest.
        const std::string text = R"({"preset":{"fixtures":[
            {"name":"x","universe":0,"address":1,
             "channels":["red","iris-from-the-future","blue"]}]}})";
        const Settings read = takt4::settings::fromJson(text);
        REQUIRE(read.preset.fixtures.size() == 1);
        REQUIRE(read.preset.fixtures[0].channels.size() == 3);
        CHECK(read.preset.fixtures[0].channels[1] == takt4::dmx::Role::Unused);
        CHECK(takt4::dmx::channelOf(read.preset.fixtures[0], takt4::dmx::Role::Blue) == 3);
    }

    SECTION("a hand-edited address out of range is clamped, not a file that will not open") {
        const std::string text =
            R"({"preset":{"fixtures":[{"name":"x","universe":99999,"address":900,
                "channels":["red"]}]}})";
        const Settings read = takt4::settings::fromJson(text);
        REQUIRE(read.preset.fixtures.size() == 1);
        CHECK(read.preset.fixtures[0].address == 512);
        CHECK(read.preset.fixtures[0].universe == 32767);
    }
}

TEST_CASE("a DMX rule survives the round trip", "[settings][dmx]") {
    Settings in;
    takt4::trigger::Rule::Config rule;
    rule.id = "drop-swing";
    rule.name = "heads on the drop";
    rule.sendKind = takt4::trigger::Message::Kind::Dmx;
    rule.dmx.fixtures = {"heads", "wash L"};
    rule.dmx.effect = takt4::dmx::EffectKind::Position;
    rule.dmx.curve = takt4::dmx::Curve::EaseInOut;
    rule.dmx.unit = takt4::trigger::DelayUnit::Bars;
    rule.dmx.durationBeats = 4.0;
    rule.dmx.pan.kind = takt4::trigger::GeneratorKind::Random;
    rule.dmx.pan.low = 20;
    rule.dmx.pan.high = 80;
    rule.dmx.tilt.kind = takt4::trigger::GeneratorKind::Fixed;
    rule.dmx.tilt.fixed = takt4::trigger::Value::ofInt(40);

    // A fade out two bars later, which is the operator's own first example.
    takt4::trigger::FollowUp down;
    down.unit = takt4::trigger::DelayUnit::Bars;
    down.delayBeats = 2.0;
    takt4::trigger::DmxFollow fade;
    fade.effect = takt4::dmx::EffectKind::Blackout;
    fade.color = takt4::dmx::Color{16, 32, 48};
    fade.unit = takt4::trigger::DelayUnit::Beats;
    fade.durationBeats = 2.0;
    down.dmx = fade;
    rule.followUps.push_back(down);
    in.preset.rules.push_back(rule);

    const Settings out = roundTrip(in);
    REQUIRE(out.preset.rules.size() == 1);
    const takt4::trigger::Rule::Config& back = out.preset.rules[0];
    CHECK(back.sendKind == takt4::trigger::Message::Kind::Dmx);
    CHECK(back.dmx.fixtures == rule.dmx.fixtures);
    CHECK(back.dmx.effect == takt4::dmx::EffectKind::Position);
    CHECK(back.dmx.curve == takt4::dmx::Curve::EaseInOut);
    CHECK(back.dmx.unit == takt4::trigger::DelayUnit::Bars);
    CHECK(back.dmx.durationBeats == 4.0);
    CHECK(back.dmx.pan.kind == takt4::trigger::GeneratorKind::Random);
    CHECK(back.dmx.pan.low == 20);
    CHECK(back.dmx.pan.high == 80);
    CHECK(back.dmx.tilt.fixed.asInt() == 40);
    REQUIRE(back.followUps.size() == 1);
    REQUIRE(back.followUps[0].dmx.has_value());
    CHECK(back.followUps[0].dmx->effect == takt4::dmx::EffectKind::Blackout);
    CHECK(back.followUps[0].dmx->color == fade.color);
    CHECK(back.followUps[0].dmx->durationBeats == 2.0);

    SECTION("a color palette is a shuffle over a list of hex strings, and nothing more") {
        // The point of reusing the generator machinery: "a random color from my palette,
        // never the same one twice" needed no code of its own, and the file says so.
        Settings palette;
        takt4::trigger::Rule::Config colorful;
        colorful.id = "colors";
        colorful.sendKind = takt4::trigger::Message::Kind::Dmx;
        colorful.dmx.fixtures = {"washes"};
        colorful.dmx.effect = takt4::dmx::EffectKind::Color;
        colorful.dmx.color.kind = takt4::trigger::GeneratorKind::Shuffle;
        colorful.dmx.color.pool = takt4::trigger::Pool::List;
        colorful.dmx.color.values = {takt4::trigger::Value::ofText("#ff2040"),
                                       takt4::trigger::Value::ofText("#20ff80"),
                                       takt4::trigger::Value::ofText("#2040ff")};
        palette.preset.rules.push_back(colorful);

        const std::string text = takt4::settings::toJson(palette);
        CHECK(text.find("#20ff80") != std::string::npos);
        const Settings read = takt4::settings::fromJson(text);
        REQUIRE(read.preset.rules.size() == 1);
        REQUIRE(read.preset.rules[0].dmx.color.values.size() == 3);
        CHECK(read.preset.rules[0].dmx.color.values[1].text() == "#20ff80");
    }
}

TEST_CASE("an Art-Net output survives the round trip through its own text line",
          "[settings][dmx]") {
    Settings in;
    takt4::output::OutputTarget node;
    node.name = "truss";
    node.kind = takt4::output::OutputTarget::Kind::ArtNet;
    node.host = "10.0.0.20";
    node.port = takt4::dmx::kArtNetPort;
    node.universes = {0, 1, 4};
    in.preset.outputs.push_back(node);

    const std::string text = takt4::settings::toJson(in);
    CHECK(text.find("artnet 10.0.0.20:6454 u0,1,4") != std::string::npos);

    const Settings out = roundTrip(in);
    REQUIRE(out.preset.outputs.size() == 1);
    CHECK(out.preset.outputs[0] == node);

    SECTION("a node fed everything writes no universe list at all") {
        Settings all = in;
        all.preset.outputs[0].universes.clear();
        const Settings back = roundTrip(all);
        REQUIRE(back.preset.outputs.size() == 1);
        CHECK(back.preset.outputs[0].universes.empty());
        CHECK(takt4::settings::toJson(all).find(" u") == std::string::npos);
    }
}

TEST_CASE("a preset written with the British spelling still loads", "[settings][dmx]") {
    // takt4 said "colour" everywhere until a rig asked for the other spelling on 2026-09-16.
    // Three things in a saved preset carried that word — the effect name, a channel's role,
    // and the generator's own key — and a name that does not read back does not fail loudly:
    // it silently becomes the default. A colour rule would have come back as a *fade*, and a
    // fixture's colour wheel as an unused channel. This is the file a rig actually had.
    const TempDir dir;
    const std::filesystem::path path = dir.path() / "settings.json";
    {
        std::ofstream out(path);
        out << R"({
  "version": 1,
  "preset": {
    "fixtures": [
      { "name": "Bedroom RGB", "universe": 5, "address": 70,
        "channels": ["red", "green", "blue", "colour-wheel"],
        "parked": [0, 0, 0, 0], "enabled": true }
    ],
    "rules": [
      { "id": "rule1", "name": "bedroom rgb", "send": "dmx", "trigger": "bar",
        "dmx": { "effect": "colour", "fixtures": ["Bedroom RGB"], "unit": "bars",
                 "durationBeats": 2.0,
                 "colour": { "kind": "fixed", "fixed": "#ff2040", "seed": 1 } } }
    ]
  }
})";
    }

    const takt4::settings::Settings loaded = takt4::settings::load(path);
    REQUIRE(loaded.preset.fixtures.size() == 1);
    CHECK(loaded.preset.fixtures.front().channels.size() == 4);
    CHECK(loaded.preset.fixtures.front().channels.back() == takt4::dmx::Role::ColorWheel);

    REQUIRE(loaded.preset.rules.size() == 1);
    const takt4::trigger::DmxSend& send = loaded.preset.rules.front().dmx;
    CHECK(send.effect == takt4::dmx::EffectKind::Color);
    CHECK(send.color.kind == takt4::trigger::GeneratorKind::Fixed);
    CHECK(send.color.fixed.text() == "#ff2040");

    SECTION("and saving it writes the spelling this build uses") {
        const std::filesystem::path again = dir.path() / "again.json";
        REQUIRE(takt4::settings::save(loaded, again));
        std::ifstream in(again);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find("colour") == std::string::npos);
        CHECK(text.find("\"color\"") != std::string::npos);
        // And it round-trips, which is the half a spelling check cannot say.
        const takt4::settings::Settings twice = takt4::settings::load(again);
        REQUIRE(twice.preset.rules.size() == 1);
        CHECK(twice.preset.rules.front().dmx.effect == takt4::dmx::EffectKind::Color);
        CHECK(twice.preset.fixtures.front().channels.back() == takt4::dmx::Role::ColorWheel);
    }
}
