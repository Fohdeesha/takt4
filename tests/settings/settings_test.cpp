#include "core/settings/settings.hpp"

#include "support/scoped_env.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

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
    in.machine.midiControlPort = "MOTU Pro Audio Midi In 1";
    in.machine.midiBindings = {"note 36 ch 10 -> tap", "cc 64 ch 1 -> lock",
                               "cc 21 ch 1 -> rule/drop/enable"};
    in.machine.oscControlEnabled = true;
    in.machine.oscControlPort = 7005;
    in.machine.oscControlLocalOnly = false;
    in.machine.inputsFolded = true;
    in.machine.outputsFolded = true;
    in.preset.tempo.minBpm = 88.0;
    in.preset.tempo.maxBpm = 176.0;
    in.preset.tempo.octaveFold = false;
    in.preset.decoder = takt4::tracking::Decoder::ParticleFilter;
    in.preset.meters = {3, 4, 0, 0};
    in.preset.tempo.confidenceThreshold = 0.25;
    in.preset.tempo.latencyOffsetSeconds = -0.030;
    in.preset.tempo.keepOctaveShift = true;
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
    // And the two that were settings of their own until 2026-09-25: Link, switched on and
    // late, and a MIDI clock, early.
    takt4::output::OutputTarget drums;
    drums.name = "drums";
    drums.kind = takt4::output::OutputTarget::Kind::MidiClock;
    drums.device = "Microsoft GS Wavetable Synth 0";
    drums.delaySeconds = -0.012;
    in.preset.outputs.push_back(drums);
    REQUIRE(takt4::output::ensureLinkOutput(in.preset.outputs, true));
    in.preset.outputs.front().delaySeconds = 0.025;
    takt4::output::ensureOutputIds(in.preset.outputs);

    const Settings out = roundTrip(in);
    CHECK(out.machine.deviceName == in.machine.deviceName);
    CHECK(out.machine.hostApiName == in.machine.hostApiName);
    CHECK(out.machine.channel == 6);
    CHECK(out.machine.midiClockPort.empty()); // a clock is an output now
    CHECK(out.machine.midiControlPort == in.machine.midiControlPort);
    CHECK(out.machine.midiBindings == in.machine.midiBindings);
    CHECK(out.machine.oscControlEnabled);
    CHECK(out.machine.oscControlPort == 7005);
    CHECK_FALSE(out.machine.oscControlLocalOnly);
    CHECK(out.machine.inputsFolded);
    CHECK(out.machine.outputsFolded);
    // Each on its own, so neither is read from the other's key.
    Settings one;
    one.machine.outputsFolded = true;
    CHECK_FALSE(roundTrip(one).machine.inputsFolded);
    CHECK(roundTrip(one).machine.outputsFolded);
    one.machine.outputsFolded = false;
    one.machine.inputsFolded = true;
    CHECK(roundTrip(one).machine.inputsFolded);
    CHECK_FALSE(roundTrip(one).machine.outputsFolded);
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
    // "Keep ÷2 / ×2 for the next track", and its default — drop them — for a file that
    // predates it (the audit's H2).
    CHECK(out.preset.tempo.keepOctaveShift);
    CHECK_FALSE(Settings{}.preset.tempo.keepOctaveShift);
    CHECK_FALSE(takt4::settings::fromJson(R"({"preset": {"tempo": {}}})").preset.tempo.keepOctaveShift);
    CHECK(out.preset.oscPrefix == "/vj");
    // Every output as it went in, the Link output first, the clock among them.
    CHECK(out.preset.outputs == in.preset.outputs);
    REQUIRE(out.preset.outputs.size() == 6);
    CHECK(out.preset.outputs[0].kind == takt4::output::OutputTarget::Kind::Link);
    CHECK(out.preset.outputs[0].enabled);
    CHECK(out.preset.outputs[1].name == "wall");
    CHECK(out.preset.outputs[3].kind == takt4::output::OutputTarget::Kind::Midi);
    CHECK_FALSE(out.preset.outputs[4].enabled);
    CHECK(out.preset.outputs[5].kind == takt4::output::OutputTarget::Kind::MidiClock);
    CHECK(out.preset.outputs[5].device == "Microsoft GS Wavetable Synth 0");
    // And written where an older build reads them, so it keeps its Link and its clock.
    const std::string text = takt4::settings::toJson(in);
    CHECK(text.find(R"("link": true)") != std::string::npos);
    CHECK(text.find(R"("midiClockPort": "Microsoft GS Wavetable Synth 0")") != std::string::npos);
}

TEST_CASE("a file with Link and the MIDI clock as settings of their own loads them as outputs",
          "[settings]") {
    // What every file written before 2026-09-25 holds: a switch for Link and one port for the
    // clock. They become the Link output, first and switched on as it was, and a clock output.
    const Settings old = takt4::settings::fromJson(R"({
        "machine": {"midiClockPort": "TR-8S"},
        "preset": {"link": true, "outputs": ["deck = 127.0.0.1:7000"]}})");
    REQUIRE(old.preset.outputs.size() == 3);
    CHECK(old.preset.outputs[0].kind == takt4::output::OutputTarget::Kind::Link);
    CHECK(old.preset.outputs[0].enabled);
    CHECK(old.preset.outputs[0].name == "Link");
    CHECK(old.preset.outputs[1].name == "deck");
    CHECK(old.preset.outputs[2].kind == takt4::output::OutputTarget::Kind::MidiClock);
    CHECK(old.preset.outputs[2].device == "TR-8S");
    CHECK(old.preset.outputs[2].enabled);
    CHECK(old.machine.midiClockPort.empty());
    for (const takt4::output::OutputTarget& target : old.preset.outputs) {
        CHECK_FALSE(target.id.empty());
    }

    SECTION("Link switched off stays off, and no port is no clock") {
        const Settings off = takt4::settings::fromJson(R"({"preset": {"link": false}})");
        REQUIRE(off.preset.outputs.size() == 1);
        CHECK(off.preset.outputs[0].kind == takt4::output::OutputTarget::Kind::Link);
        CHECK_FALSE(off.preset.outputs[0].enabled);
    }

    SECTION("a file this build wrote is not given a second clock or a second Link") {
        const Settings again = roundTrip(old);
        CHECK(again.preset.outputs == old.preset.outputs);
    }
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
    // Nor how the window was folded: what a desk's screen has room for.
    CHECK(text.find("inputsFolded") < preset);
    CHECK(text.find("outputsFolded") < preset);
    CHECK(text.find("Folded", preset) == std::string::npos);
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
    // Nothing but the Link output every set has, switched off as a file with no switch meant.
    REQUIRE(out.preset.outputs.size() == 1);
    CHECK(out.preset.outputs[0].kind == takt4::output::OutputTarget::Kind::Link);
    CHECK_FALSE(out.preset.outputs[0].enabled);
    // The listening socket most of all: a build that did not have this field must not
    // load as one that opens a port.
    CHECK_FALSE(out.machine.oscControlEnabled);
    CHECK(out.machine.oscControlPort == defaults.machine.oscControlPort);
    CHECK(out.machine.oscControlLocalOnly);
    // A file from before folding opens with both sections open; so does a fold that is not a
    // yes or a no.
    CHECK_FALSE(out.machine.inputsFolded);
    CHECK_FALSE(out.machine.outputsFolded);
    const Settings odd =
        takt4::settings::fromJson(R"({"machine": {"inputsFolded": 1, "outputsFolded": "yes"}})");
    CHECK_FALSE(odd.machine.inputsFolded);
    CHECK_FALSE(odd.machine.outputsFolded);
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
    // And short of that, no further than the slider goes (the audit of 2026-09-25, L31): 0.6 s
    // ran 600 ms late behind a slider showing 250, which a touch then snapped to 250.
    const auto latencyOf = [](const char* seconds) {
        return takt4::settings::fromJson(std::string(R"({"preset": {"tempo": {"latencyOffsetSeconds": )") +
                                         seconds + "}}}")
            .preset.tempo.latencyOffsetSeconds;
    };
    CHECK_THAT(latencyOf("0.6"), WithinAbs(takt4::settings::kMaxLatencyOffsetSeconds, 1e-12));
    CHECK_THAT(latencyOf("-0.4"), WithinAbs(-takt4::settings::kMaxLatencyOffsetSeconds, 1e-12));
    CHECK_THAT(latencyOf("0.1"), WithinAbs(0.1, 1e-12));
    CHECK_THAT(latencyOf("-0.25"), WithinAbs(-0.25, 1e-12));

    const Settings channel = takt4::settings::fromJson(R"({"machine": {"channel": -4}})");
    CHECK(channel.machine.channel == 0);

    // A target with no host, or a port outside the range, is left out rather than sent to.
    const Settings targets = takt4::settings::fromJson(
        R"({"preset": {"outputs": [" = :7000", "a:0", "b:99999", "good = 10.0.0.1:7000"]}})");
    REQUIRE(targets.preset.outputs.size() == 2); // the Link output, then the one good line
    CHECK(targets.preset.outputs[1].name == "good");
    CHECK(targets.preset.outputs[1].host == "10.0.0.1");
}

TEST_CASE("an OSC prefix that is not an address falls back rather than stopping takt4",
          "[settings]") {
    // The audit's C1, third way in: `OscPublisher` throws on a prefix it cannot build an
    // address from, inside `Transports`' constructor, inside the window's — so a hand-edited
    // "vj" stopped the app opening at all. The file is not a caller worth trusting.
    const auto prefixOf = [](const std::string& value) {
        return takt4::settings::fromJson(R"({"preset": {"oscPrefix": )" + value + "}}")
            .preset.oscPrefix;
    };
    for (const char* bad : {R"("vj")", R"("/vj/")", R"("")", R"("/")", R"("/v j")", R"(7)"}) {
        INFO("prefix: " << bad);
        CHECK(prefixOf(bad) == "/takt4");
    }
    CHECK(prefixOf(R"("/vj")") == "/vj");
    CHECK(prefixOf(R"("/stage/left")") == "/stage/left");
}

TEST_CASE("a settings file written before targets had names still loads", "[settings]") {
    // The shape older builds wrote: an array of {host, port} objects under `oscTargets`.
    // Dropping it silently would cost an operator every target they had typed, which is the
    // one upgrade failure they would notice and could not diagnose.
    const Settings out = takt4::settings::fromJson(
        R"({"preset": {"oscTargets": [{"host": "192.168.1.40", "port": 7000},
                                      {"host": "", "port": 7000},
                                      {"host": "127.0.0.1", "port": 7001}]}})");
    REQUIRE(out.preset.outputs.size() == 3); // the Link output first
    CHECK(out.preset.outputs[1].host == "192.168.1.40");
    CHECK(out.preset.outputs[1].kind == takt4::output::OutputTarget::Kind::Osc);
    // Named after its own address, which is what an unnamed target has always been called.
    CHECK(out.preset.outputs[1].name == "192.168.1.40:7000");
    CHECK(out.preset.outputs[2].port == 7001);
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
    REQUIRE_FALSE(out.preset.outputs.empty());
    CHECK(out.preset.outputs.front().kind == takt4::output::OutputTarget::Kind::Link);
    CHECK(out.preset.outputs.front().enabled);
}

TEST_CASE("a missing or unreadable settings file is not an error", "[settings]") {
    const TempDir dir;
    const Settings missing = takt4::settings::load(dir.path() / "there-is-no-file.json");
    CHECK(missing.machine.deviceName.empty());
    CHECK(takt4::settings::loadChecked(dir.path() / "there-is-no-file.json").status ==
          takt4::settings::LoadStatus::Missing);

    // A directory where a file should be: openable as neither.
    CHECK(takt4::settings::load(dir.path()).machine.deviceName.empty());
    CHECK(takt4::settings::loadChecked(dir.path()).status ==
          takt4::settings::LoadStatus::Unreadable);
    CHECK(takt4::settings::load({}).machine.deviceName.empty());
    CHECK_FALSE(takt4::settings::save(Settings{}, {}));

    // Half a file, as a crash mid-write used to leave. `load` still opens on the defaults —
    // that is what keeps the app starting — but it is **not** the same answer as a missing
    // file any more, because the next thing that happened to one of these was a save of those
    // defaults over it. See "a damaged settings file is kept, never saved over".
    const std::filesystem::path truncated = dir.path() / "half.json";
    write(truncated, R"({"machine": {"deviceName": "MOTU)");
    CHECK(takt4::settings::load(truncated).machine.deviceName.empty());
    const takt4::settings::Loaded checked = takt4::settings::loadChecked(truncated);
    CHECK(checked.status == takt4::settings::LoadStatus::Corrupt);
    CHECK_FALSE(checked.problem.empty());

    // And the empty file a crash in the old truncate-then-write save produced, said plainly.
    const std::filesystem::path empty = dir.path() / "empty.json";
    write(empty, "");
    const takt4::settings::Loaded nothing = takt4::settings::loadChecked(empty);
    CHECK(nothing.status == takt4::settings::LoadStatus::Corrupt);
    CHECK(nothing.problem == "it is empty");
}

namespace {

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Every file in `dir` whose name starts with `prefix`.
std::vector<std::filesystem::path> filesStarting(const std::filesystem::path& dir,
                                                 const std::string& prefix) {
    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) {
            found.push_back(entry.path());
        }
    }
    return found;
}

} // namespace

TEST_CASE("a damaged settings file is kept, never saved over", "[settings]") {
    // The audit's C8, as it happened: a file that would not parse — a trailing comma from a
    // hand edit, or half a file from a crash — loaded as the **defaults** with no message, and
    // the save on the way out then wrote those defaults over it. A rig that was one comma away
    // from loading was gone.
    const TempDir dir;
    const std::filesystem::path file = dir.path() / "settings.json";
    const std::string damaged =
        R"({"machine": {"deviceName": "MOTU Pro Audio"}, "preset": {"link": true,}})";
    write(file, damaged);

    const takt4::settings::Startup startup = takt4::settings::openAtStartup(file, file);
    // Started, on the defaults — there is no backup to fall back to.
    CHECK(startup.settings.machine.deviceName.empty());
    CHECK(startup.writable);
    // Said, naming what happened and where the file went.
    CHECK(startup.notice.find("settings.json is damaged") != std::string::npos);
    CHECK(startup.notice.find("default settings") != std::string::npos);
    CHECK(startup.notice.find("settings.json.corrupt-") != std::string::npos);

    // And the damaged bytes are still on disk, byte for byte, under a name no save will use.
    CHECK_FALSE(std::filesystem::exists(file));
    const std::vector<std::filesystem::path> kept = filesStarting(dir.path(), "settings.json.corrupt-");
    REQUIRE(kept.size() == 1);
    CHECK(readBytes(kept.front()) == damaged);

    // A save now writes a fresh file and leaves the kept one alone.
    REQUIRE(takt4::settings::save(startup.settings, file));
    CHECK(readBytes(kept.front()) == damaged);
}

TEST_CASE("a damaged file falls back to the copy kept at the last good start", "[settings]") {
    const TempDir dir;
    const std::filesystem::path file = dir.path() / "settings.json";

    // A clean start: the settings load, and the file as it stands is kept beside it.
    Settings rig;
    rig.machine.deviceName = "MOTU Pro Audio";
    rig.preset.link = true;
    REQUIRE(takt4::settings::save(rig, file));
    const takt4::settings::Startup clean = takt4::settings::openAtStartup(file, file);
    CHECK(clean.notice.empty());
    CHECK(clean.settings.machine.deviceName == "MOTU Pro Audio");
    const std::filesystem::path backup = takt4::settings::backupFile(file);
    REQUIRE(std::filesystem::exists(backup));
    CHECK(readBytes(backup) == readBytes(file));

    // The session's own saves do not touch it: the copy worth going back to is the one from
    // before tonight's edits, not the last autosave.
    Settings edited = rig;
    edited.machine.deviceName = "something else";
    REQUIRE(takt4::settings::save(edited, file));
    CHECK(takt4::settings::load(backup).machine.deviceName == "MOTU Pro Audio");

    // Then the file is damaged, and the next start comes up on the kept copy rather than on
    // nothing — and says so.
    write(file, "{\"machine\": ");
    const takt4::settings::Startup recovered = takt4::settings::openAtStartup(file, file);
    CHECK(recovered.settings.machine.deviceName == "MOTU Pro Audio");
    REQUIRE_FALSE(recovered.settings.preset.outputs.empty());
    CHECK(recovered.settings.preset.outputs.front().enabled); // the Link output, switched on
    CHECK(recovered.notice.find("settings.json.bak") != std::string::npos);
    CHECK(filesStarting(dir.path(), "settings.json.corrupt-").size() == 1);
}

#if defined(_WIN32)
TEST_CASE("a settings file another program holds is not saved over", "[settings]") {
    // Locked, not damaged: a sync client or an editor with it open. The contents may be
    // perfectly good, so the startup must not replace them.
    const TempDir dir;
    const std::filesystem::path file = dir.path() / "settings.json";
    Settings rig;
    rig.machine.deviceName = "MOTU Pro Audio";
    REQUIRE(takt4::settings::save(rig, file));

    const HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    const takt4::settings::Startup startup = takt4::settings::openAtStartup(file, file);
    CHECK_FALSE(startup.writable);
    CHECK(startup.notice.find("could not be opened") != std::string::npos);
    CloseHandle(held);

    // Still there, still whole.
    CHECK(takt4::settings::load(file).machine.deviceName == "MOTU Pro Audio");
}
#endif

TEST_CASE("a save replaces the file whole and leaves nothing beside it", "[settings]") {
    const TempDir dir;
    const std::filesystem::path file = dir.path() / "settings.json";
    Settings first;
    first.machine.deviceName = "first";
    REQUIRE(takt4::settings::save(first, file));
    Settings second;
    second.machine.deviceName = "second";
    REQUIRE(takt4::settings::save(second, file));

    CHECK(takt4::settings::load(file).machine.deviceName == "second");
    // The temporary the bytes went to first has become the file; nothing is left over.
    CHECK_FALSE(std::filesystem::exists(dir.path() / "settings.json.tmp"));
    CHECK(filesStarting(dir.path(), "settings.json").size() == 1);
}

TEST_CASE("a save that cannot finish leaves the old file as it was", "[settings]") {
    // The failure the atomic save exists for, made to happen on purpose: the temporary cannot
    // be created (a folder is squatting on its name), so nothing is written. The old save
    // truncated the real file *first*, and any failure after that was an empty file.
    const TempDir dir;
    const std::filesystem::path file = dir.path() / "settings.json";
    Settings good;
    good.machine.deviceName = "MOTU Pro Audio";
    REQUIRE(takt4::settings::save(good, file));
    const std::string before = readBytes(file);

    std::filesystem::create_directories(dir.path() / "settings.json.tmp");
    Settings other;
    other.machine.deviceName = "never written";
    CHECK_FALSE(takt4::settings::save(other, file));
    CHECK(readBytes(file) == before);
}

#if defined(_WIN32)
namespace {

/// The environment variable that turns this test binary into the child of the test below.
constexpr const char* kSaveLoopVariable = "TAKT4_TEST_SAVE_LOOP";

/// A rig big enough that writing it takes long enough to be interrupted — a few hundred
/// kilobytes, as a show with every rule written out really is.
Settings bigRig(const std::string& name, std::size_t rules) {
    Settings rig;
    rig.machine.deviceName = name;
    for (std::size_t i = 0; i < rules; ++i) {
        takt4::trigger::Rule::Config rule;
        rule.id = name + "-" + std::to_string(i);
        rule.name = "rule " + std::to_string(i) + " of the rig called " + name;
        rule.address = "/composition/layers/" + std::to_string(i % 8 + 1) + "/clips/{}/connect";
        rig.preset.rules.push_back(rule);
    }
    return rig;
}

std::string environment(const char* name) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    const std::string out(value);
    std::free(value);
    return out;
}

} // namespace

TEST_CASE("settings save loop child of the kill test", "[.child]") {
    // Hidden: only ever run by "a save killed half way through leaves a whole file", which
    // starts this binary again with the variable set and then kills it at random.
    const std::string where = environment(kSaveLoopVariable);
    if (where.empty()) {
        SKIP("run only as the child of the kill test");
    }
    const std::filesystem::path file(where);
    const std::string a = takt4::settings::toJson(bigRig("A", 600));
    const std::string b = takt4::settings::toJson(bigRig("B", 400));
    REQUIRE(takt4::settings::saveText(a, file));
    // Tells the parent the first save has landed, so a kill from here on is a kill mid-loop.
    write(file.parent_path() / "child-started", "1");
    for (;;) {
        (void)takt4::settings::saveText(b, file);
        (void)takt4::settings::saveText(a, file);
    }
}

TEST_CASE("a save killed half way through leaves a whole file", "[settings]") {
    // **The effect, not the mechanism.** A real process saving a real rig in a loop, killed
    // with TerminateProcess at a random moment, twenty times — and after every one the file
    // must parse and be one of the two rigs, whole. The old truncate-and-rewrite save fails
    // this within the first few kills: a kill mid-write leaves a prefix of the JSON, which is
    // exactly the "power cut while saving" the audit describes.
    const TempDir dir;
    const std::filesystem::path file = dir.path() / "settings.json";
    const std::filesystem::path started = dir.path() / "child-started";

    wchar_t self[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(nullptr, self, MAX_PATH) > 0);
    std::wstring command = L"\"" + std::wstring(self) +
                           L"\" \"settings save loop child of the kill test\"";
    REQUIRE(SetEnvironmentVariableA(kSaveLoopVariable, file.string().c_str()) != 0);

    std::mt19937 random(20260923);
    std::uniform_int_distribution<int> delayMs(0, 40);
    int killedMidLoop = 0;
    for (int round = 0; round < 20; ++round) {
        std::error_code ignored;
        std::filesystem::remove(started, ignored);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        std::wstring mutableCommand = command;
        REQUIRE(CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != 0);
        // Up to ten seconds for the child to come up and make its first save.
        for (int wait = 0; wait < 1000 && !std::filesystem::exists(started); ++wait) {
            Sleep(10);
        }
        const bool running = std::filesystem::exists(started);
        if (running) {
            Sleep(static_cast<DWORD>(delayMs(random)));
            ++killedMidLoop;
        }
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, INFINITE);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        REQUIRE(running);

        INFO("round " << round);
        const takt4::settings::Loaded loaded = takt4::settings::loadChecked(file);
        REQUIRE(loaded.status == takt4::settings::LoadStatus::Loaded);
        const std::size_t rules = loaded.settings.preset.rules.size();
        CHECK((rules == 600 || rules == 400));
        CHECK(loaded.settings.machine.deviceName == (rules == 600 ? "A" : "B"));
    }
    SetEnvironmentVariableA(kSaveLoopVariable, nullptr);
    CHECK(killedMidLoop == 20);
}
#endif

namespace {

/// Clears `TAKT4_SETTINGS_DIR` for as long as it lives, and puts it back. Every test process
/// is given one (tests/support/crt_dialogs.cpp); the two tests below are about where the
/// program itself keeps its settings, so they ask with it gone.
class NoSettingsFolder {
public:
    NoSettingsFolder() {
#if defined(_WIN32)
        wchar_t* value = nullptr;
        std::size_t size = 0;
        if (_wdupenv_s(&value, &size, L"TAKT4_SETTINGS_DIR") == 0 && value != nullptr) {
            saved_ = value;
            std::free(value);
        }
        _wputenv_s(L"TAKT4_SETTINGS_DIR", L"");
#else
        // Linux too: it did nothing here, so the process's own test folder stayed named and the
        // test below asked after a folder nobody had made (the first linux-tsan run to reach it,
        // 2026-09-30).
        if (const char* value = std::getenv("TAKT4_SETTINGS_DIR"); value != nullptr) {
            saved_ = value;
            had_ = true;
        }
        ::unsetenv("TAKT4_SETTINGS_DIR");
#endif
    }
    ~NoSettingsFolder() {
#if defined(_WIN32)
        if (!saved_.empty()) {
            _wputenv_s(L"TAKT4_SETTINGS_DIR", saved_.c_str());
        }
#else
        if (had_) {
            ::setenv("TAKT4_SETTINGS_DIR", saved_.c_str(), 1);
        }
#endif
    }
    NoSettingsFolder(const NoSettingsFolder&) = delete;
    NoSettingsFolder& operator=(const NoSettingsFolder&) = delete;

private:
#if defined(_WIN32)
    std::wstring saved_;
#else
    std::string saved_;
    bool had_ = false;
#endif
};

} // namespace

TEST_CASE("the settings file lives beside the program", "[settings]") {
    // What makes takt4 something an operator can copy onto a stick: the settings travel
    // with the executable rather than staying in a profile on one machine.
    const NoSettingsFolder programs;
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

#if defined(_WIN32)
TEST_CASE("a folder named for the settings holds them, and the texts ABOUT opens", "[settings]") {
    // What every test process relies on to keep off the show's settings.json — and set the way
    // a test sets a variable, through `ScopedVariable`, which used to write only the process's
    // block: `_wdupenv_s` reads the C runtime's copy and went on seeing the old folder (the
    // audit of 2026-09-25, T16). And the texts ABOUT writes out go inside that folder rather
    // than to the `%TEMP%\takt4` the real takt4 uses (T4).
    const TempDir folder;
    const takt4::test::ScopedVariable named("TAKT4_SETTINGS_DIR", folder.path().string().c_str());
    CHECK(takt4::settings::settingsDirectory() == folder.path());
    CHECK(takt4::settings::settingsFile() == folder.path() / "settings.json");
    CHECK(takt4::settings::existingSettingsFile() == folder.path() / "settings.json");
    CHECK(takt4::settings::scratchDirectory() == folder.path() / "texts");
}

TEST_CASE("with no folder named, the texts go to takt4's own temp folder", "[settings]") {
    const NoSettingsFolder programs;
    std::error_code code;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(code);
    REQUIRE_FALSE(code);
    CHECK(takt4::settings::scratchDirectory() == temp / "takt4");
}
#endif

TEST_CASE("settings left by an older build are still read", "[settings]") {
    // The per-user location is where these used to be kept. A rig that has one there must
    // not lose its outputs, its device and its MIDI bindings just because the file moved,
    // so it is read until a save writes one beside the executable.
    const NoSettingsFolder programs;
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
    wash.id = "f-0000000a";
    wash.group = "washes";
    takt4::dmx::Fixture head = takt4::dmx::fixtureFromMode("head 1", 6, 4, 100);
    head.id = "f-0000000b";
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
    node.id = "o-000000cc";
    node.name = "truss";
    node.kind = takt4::output::OutputTarget::Kind::ArtNet;
    node.host = "10.0.0.20";
    node.port = takt4::dmx::kArtNetPort;
    node.delaySeconds = 0.04;
    in.preset.outputs.push_back(node);

    const std::string text = takt4::settings::toJson(in);
    CHECK(text.find("truss = artnet 10.0.0.20:6454 +40ms #o-000000cc") != std::string::npos);

    // After the Link output, which every loaded set has first.
    const Settings out = roundTrip(in);
    REQUIRE(out.preset.outputs.size() == 2);
    CHECK(out.preset.outputs[0].kind == takt4::output::OutputTarget::Kind::Link);
    CHECK(out.preset.outputs[1] == node);

    SECTION("a universe list an older build wrote is read and left out") {
        // Every node is fed every universe since 2026-09-25; a file with a list still loads,
        // and the next save writes none.
        const Settings old = takt4::settings::fromJson(
            R"({"version":1,"preset":{"outputs":["truss = artnet 10.0.0.20:6454 u0,1,4 #o-000000cc"]}})");
        REQUIRE(old.preset.outputs.size() == 2);
        CHECK(old.preset.outputs[1].kind == takt4::output::OutputTarget::Kind::ArtNet);
        CHECK(old.preset.outputs[1].host == "10.0.0.20");
        CHECK(takt4::settings::toJson(old).find(" u0") == std::string::npos);
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

TEST_CASE("a file routed by name loads routed by id, and a rename afterwards moves nothing",
          "[settings][trigger]") {
    // Every settings file written before outputs and fixtures had ids routes rules by their
    // *names* — the operator's report of 2026-09-23 was that renaming an output broke every rule
    // on it. Such a file has to load exactly as routed as it was, and stop depending on names.
    const std::string text = R"({"preset":{
        "outputs":["deck = 127.0.0.1:7000", "wall = 127.0.0.1:7001"],
        "fixtures":[{"name":"wash L","group":"washes","universe":0,"address":1,
                     "channels":["red","green","blue"]}],
        "rules":[
          {"id":"clips","send":"osc","address":"/clip","outputs":["wall"]},
          {"id":"fade","send":"dmx","dmx":{"fixtures":["wash L","washes"]}}]}})";
    takt4::settings::Settings loaded = takt4::settings::fromJson(text);
    REQUIRE(loaded.preset.outputs.size() == 3); // the Link output, deck, wall
    REQUIRE(loaded.preset.fixtures.size() == 1);
    REQUIRE(loaded.preset.rules.size() == 2);

    const std::string wall = loaded.preset.outputs[2].id;
    const std::string washL = loaded.preset.fixtures[0].id;
    REQUIRE_FALSE(wall.empty());
    REQUIRE_FALSE(washL.empty());
    CHECK(loaded.preset.outputs[1].id != wall);
    CHECK(loaded.preset.rules[0].outputs == std::vector<std::string>{wall});
    CHECK(loaded.preset.rules[1].dmx.fixtures == std::vector<std::string>{washL, "washes"});
    CHECK(takt4::output::resolveOutputs(loaded.preset.rules[0].outputs, loaded.preset.outputs) ==
          0b100);

    // Renamed, the same rules still reach the same things.
    loaded.preset.outputs[2].name = "back wall";
    loaded.preset.fixtures[0].name = "front wash";
    CHECK(takt4::output::resolveOutputs(loaded.preset.rules[0].outputs, loaded.preset.outputs) ==
          0b100);
    CHECK(takt4::dmx::resolveFixtures(loaded.preset.fixtures,
                                      loaded.preset.rules[1].dmx.fixtures) == 0b1);

    SECTION("and the ids are written down, so the next load is the same rig") {
        const takt4::settings::Settings again =
            takt4::settings::fromJson(takt4::settings::toJson(loaded));
        CHECK(again.preset.outputs[2].id == wall);
        CHECK(again.preset.fixtures[0].id == washL);
        CHECK(again.preset.rules[0].outputs == std::vector<std::string>{wall});
    }
}

TEST_CASE("a file from before the mono tick is heard as the stereo pair its input is in",
          "[settings]") {
    // 2026-09-28: stereo unless asked otherwise, and a file written before there was a choice
    // listened to one input only because that was all there was. It is read as the pair that
    // input is in, and marked so the window can say so; a file that says what it wants keeps it.
    const Settings old = takt4::settings::fromJson(
        R"({"machine": {"deviceName": "MOTU Pro Audio", "hostApiName": "ASIO", "channel": 10}})");
    CHECK(old.machine.channel == 10);
    CHECK_FALSE(old.machine.mono);
    CHECK(old.machine.stereoFromMono);

    const Settings mono = takt4::settings::fromJson(R"({"machine": {"channel": 10, "mono": true}})");
    CHECK(mono.machine.mono);
    CHECK_FALSE(mono.machine.stereoFromMono);

    // A fresh file is stereo, with nothing to say about it.
    const Settings fresh = takt4::settings::fromJson("{}");
    CHECK_FALSE(fresh.machine.mono);
    CHECK_FALSE(fresh.machine.stereoFromMono);

    // And the choice is written, so the next launch has one.
    Settings chosen;
    chosen.machine.channel = 3;
    chosen.machine.mono = true;
    const Settings again = roundTrip(chosen);
    CHECK(again.machine.mono);
    CHECK(again.machine.channel == 3);
    CHECK_FALSE(again.machine.stereoFromMono);
    Settings paired;
    paired.machine.channel = 3;
    CHECK_FALSE(roundTrip(paired).machine.mono);
    CHECK_FALSE(roundTrip(paired).machine.stereoFromMono);
}

TEST_CASE("the rule editor's folds and its log are remembered on this machine", "[settings]") {
    // HANDOFF §0.5: which of A to D were folded, whether the event log was open and how tall it was
    // dragged come back as they were left. The machine's, as the main window's folds are.
    Settings in;
    in.machine.ruleSectionsFolded = {true, false, true, false};
    in.machine.ruleLogOpen = true;
    in.machine.ruleLogHeight = 162.0;
    const Settings out = roundTrip(in);
    CHECK(out.machine.ruleSectionsFolded == std::array<bool, 4>{true, false, true, false});
    CHECK(out.machine.ruleLogOpen);
    CHECK_THAT(out.machine.ruleLogHeight, WithinAbs(162.0, 1e-9));
    // Each fold on its own, so none is read from its neighbour's place.
    for (std::size_t i = 0; i < 4; ++i) {
        INFO("section " << i);
        Settings one;
        one.machine.ruleSectionsFolded = {false, false, false, false};
        one.machine.ruleSectionsFolded[i] = true;
        CHECK(roundTrip(one).machine.ruleSectionsFolded == one.machine.ruleSectionsFolded);
    }
    const std::string text = takt4::settings::toJson(in);
    const std::size_t preset = text.find("\"preset\"");
    CHECK(text.find("ruleSectionsFolded") < preset);
    CHECK(text.find("ruleLogOpen") < preset);
    CHECK(text.find("ruleLogHeight") < preset);

    // A file from before: B folded as a new rule's is, the rest open, the log shut at five lines.
    const Settings old = takt4::settings::fromJson(R"({"machine": {"channel": 3}})");
    CHECK(old.machine.ruleSectionsFolded == std::array<bool, 4>{false, true, false, false});
    CHECK_FALSE(old.machine.ruleLogOpen);
    CHECK_THAT(old.machine.ruleLogHeight, WithinAbs(90.0, 1e-9));

    // And what a hand could have typed. A list of folds that is not four long is not read; a fold
    // that is not a yes or a no keeps its own default and the others are read.
    const auto folds = [](const std::string& value) {
        return takt4::settings::fromJson(R"({"machine": {"ruleSectionsFolded": )" + value + "}}")
            .machine.ruleSectionsFolded;
    };
    CHECK(folds("[true, true]") == std::array<bool, 4>{false, true, false, false});
    CHECK(folds("[true, false, true, true, true]") == std::array<bool, 4>{false, true, false, false});
    CHECK(folds("\"all\"") == std::array<bool, 4>{false, true, false, false});
    CHECK(folds("[true, 0, true, \"no\"]") == std::array<bool, 4>{true, true, true, false});
    // A height from a line to half a large screen: none at all, or one that pushes the editor off
    // the window, is held to the nearest end; one that is not a number, or too big to be one,
    // is the default.
    const auto height = [](const std::string& value) {
        return takt4::settings::fromJson(R"({"machine": {"ruleLogHeight": )" + value + "}}")
            .machine.ruleLogHeight;
    };
    CHECK_THAT(height("0"), WithinAbs(18.0, 1e-9));
    CHECK_THAT(height("-40"), WithinAbs(18.0, 1e-9));
    CHECK_THAT(height("1000000"), WithinAbs(800.0, 1e-9));
    CHECK_THAT(height("1e999"), WithinAbs(90.0, 1e-9));
    CHECK_THAT(height("\"tall\""), WithinAbs(90.0, 1e-9));
    CHECK_THAT(height("144"), WithinAbs(144.0, 1e-9));
}

TEST_CASE("the patch editor's folds are remembered on this machine", "[settings]") {
    // The same for the patch editor's A, B and C since its redesign (2026-09-30): each on its own,
    // in the machine half, all open in a file from before, and what a hand could have typed.
    for (std::size_t i = 0; i < 3; ++i) {
        INFO("section " << i);
        Settings one;
        one.machine.patchSectionsFolded[i] = true;
        CHECK(roundTrip(one).machine.patchSectionsFolded == one.machine.patchSectionsFolded);
    }
    Settings in;
    in.machine.patchSectionsFolded = {true, false, true};
    const std::string text = takt4::settings::toJson(in);
    CHECK(text.find("patchSectionsFolded") < text.find("\"preset\""));
    CHECK(roundTrip(in).machine.patchSectionsFolded == std::array<bool, 3>{true, false, true});

    const Settings old = takt4::settings::fromJson(R"({"machine": {"channel": 3}})");
    CHECK(old.machine.patchSectionsFolded == std::array<bool, 3>{false, false, false});
    const auto folds = [](const std::string& value) {
        return takt4::settings::fromJson(R"({"machine": {"patchSectionsFolded": )" + value + "}}")
            .machine.patchSectionsFolded;
    };
    CHECK(folds("[true, true]") == std::array<bool, 3>{false, false, false});
    CHECK(folds("[true, true, true, true]") == std::array<bool, 3>{false, false, false});
    CHECK(folds("{\"a\": true}") == std::array<bool, 3>{false, false, false});
    CHECK(folds("[true, \"no\", true]") == std::array<bool, 3>{true, false, true});
}
