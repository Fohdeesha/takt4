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
    in.preset.tempo.minBpm = 88.0;
    in.preset.tempo.maxBpm = 176.0;
    in.preset.tempo.octaveFold = false;
    in.preset.tempo.confidenceThreshold = 0.25;
    in.preset.tempo.latencyOffsetSeconds = -0.030;
    in.preset.link = true;
    in.preset.oscPrefix = "/vj";
    in.preset.oscTargets = {{"192.168.1.40", 7000}, {"127.0.0.1", 7001}};

    const Settings out = roundTrip(in);
    CHECK(out.machine.deviceName == in.machine.deviceName);
    CHECK(out.machine.hostApiName == in.machine.hostApiName);
    CHECK(out.machine.channel == 6);
    CHECK(out.machine.midiClockPort == in.machine.midiClockPort);
    CHECK_THAT(out.preset.tempo.minBpm, WithinAbs(88.0, 1e-9));
    CHECK_THAT(out.preset.tempo.maxBpm, WithinAbs(176.0, 1e-9));
    CHECK_FALSE(out.preset.tempo.octaveFold);
    CHECK_THAT(out.preset.tempo.confidenceThreshold, WithinAbs(0.25, 1e-9));
    CHECK_THAT(out.preset.tempo.latencyOffsetSeconds, WithinAbs(-0.030, 1e-9));
    CHECK(out.preset.link);
    CHECK(out.preset.oscPrefix == "/vj");
    REQUIRE(out.preset.oscTargets.size() == 2);
    CHECK(out.preset.oscTargets[0].first == "192.168.1.40");
    CHECK(out.preset.oscTargets[0].second == 7000);
    CHECK(out.preset.oscTargets[1].second == 7001);
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
    CHECK(out.preset.oscTargets.empty());
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
        R"({"preset": {"oscTargets": [{"host": "", "port": 7000},
                                      {"host": "a", "port": 0},
                                      {"host": "b", "port": 99999},
                                      {"host": "good", "port": 7000}]}})");
    REQUIRE(targets.preset.oscTargets.size() == 1);
    CHECK(targets.preset.oscTargets[0].first == "good");
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
