#include "core/settings/settings.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <fstream>
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

TEST_CASE("the settings file has a home on this machine", "[settings]") {
    // Not asserting the exact path — it is the platform's to decide — but it has to be
    // absolute, under a directory, and named the same every time.
    const std::filesystem::path file = takt4::settings::settingsFile();
    if (file.empty()) {
        SKIP("this environment names no config directory");
    }
    CHECK(file.is_absolute());
    CHECK(file.filename() == "settings.json");
    CHECK(file.parent_path() == takt4::settings::settingsDirectory());
    CHECK(takt4::settings::settingsDirectory().filename() == "takt4");
}
