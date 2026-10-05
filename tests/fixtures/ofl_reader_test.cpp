#include "core/fixtures/definition.hpp"
#include "core/fixtures/natural_order.hpp"
#include "core/fixtures/ofl_reader.hpp"

#include "fixtures/fixture_files.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::fixtures::DefChannel;
using takt4::fixtures::Definition;
using takt4::fixtures::DefMode;
using takt4::fixtures::Kind;
using takt4::fixtures::readOflText;
using takt4::test::labelsOf;
using takt4::test::modeNamed;
using takt4::test::readOfl;
using takt4::test::readOflJson;

namespace {

/// An OFL fixture around `channels` (the availableChannels object's body) and `modes`.
std::string oflFixture(const std::string& channels, const std::string& modes,
                       const std::string& more = "") {
    return R"({"$schema": "https://raw.githubusercontent.com/OpenLightingProject/open-fixture-library/schema-12.6.0/schemas/fixture.json",
              "name": "Test Fixture", "categories": ["Other"],
              "meta": {"authors": ["takt4"], "createDate": "2026-01-01", "lastModifyDate": "2026-02-02"},)" +
           more + R"("availableChannels": {)" + channels + R"(}, "modes": [)" + modes + "]}";
}

std::vector<std::string> cellsOf(const DefMode& mode) {
    std::vector<std::string> out;
    for (const DefChannel& channel : mode.parts.at(0)) {
        out.push_back(channel.cell);
    }
    return out;
}

} // namespace

TEST_CASE("OFL's natural order reads numbers as numbers and folds case", "[fixtures][ofl]") {
    using takt4::fixtures::natural::compare;
    CHECK(compare("1", "2") < 0);
    CHECK(compare("2", "10") < 0);
    CHECK(compare("Pixel 9", "Pixel 10") < 0);
    CHECK(compare("a", "A") < 0); // lower case first, but only after everything else
    CHECK(compare("a", "B") < 0);
    CHECK(compare("B", "a") > 0);
    CHECK(compare("9", "09") == 0);
    CHECK(compare("", "a") < 0);
    // Space, then punctuation, then digits, then letters: ICU's root order.
    CHECK(compare(" ", "_") < 0);
    CHECK(compare("_", "-") < 0);
    CHECK(compare("-", "(") < 0);
    CHECK(compare("$", "0") < 0);
    CHECK(compare("9", "a") < 0);
    CHECK(compare("z", "\xC3\xA9") < 0); // é after every ASCII letter
    CHECK(compare("(2, 1)", "(10, 1)") < 0);
    CHECK(compare("Top", "top") > 0);

    SECTION("the order eachPixelABC gives") {
        CHECK(takt4::fixtures::natural::sorted({"4", "3", "2", "1"}) ==
              std::vector<std::string>{"1", "2", "3", "4"});
        CHECK(takt4::fixtures::natural::sorted({"b", "B", "a10", "a9", "A9"}) ==
              std::vector<std::string>{"a9", "A9", "a10", "b", "B"});
        // Keys equal in this order keep JavaScript's: array indices first, then as given.
        CHECK(takt4::fixtures::natural::sorted({"09", "9"}) == std::vector<std::string>{"9", "09"});
        CHECK(takt4::fixtures::natural::objectKeyOrder({"Master", "2", "10", "1", "Master"}) ==
              std::vector<std::string>{"1", "2", "10", "Master"});
    }
}

TEST_CASE("OFL's value scaling repeats the last byte up and drops bytes down", "[fixtures][ofl]") {
    using takt4::fixtures::ofl::scaleRange;
    using takt4::fixtures::ofl::scaleValue;
    CHECK(scaleValue(0x12, 1, 2) == 0x1212);
    CHECK(scaleValue(0x1234, 2, 1) == 0x12);
    CHECK(scaleValue(0x1234, 2, 3) == 0x123434);
    CHECK(scaleValue(132, 1, 2) == 0x8484);
    CHECK(scaleRange(0x10, 0x20, 1, 2) == std::pair<std::uint64_t, std::uint64_t>{0x1000, 0x20FF});
    // A start cut down rounds up where a byte cut from it was not zero, and the range stays one.
    CHECK(scaleRange(0x0080, 0x0200, 2, 1) == std::pair<std::uint64_t, std::uint64_t>{1, 2});
    CHECK(scaleRange(0x0101, 0x01FF, 2, 1) == std::pair<std::uint64_t, std::uint64_t>{1, 1});
    CHECK(scaleRange(0x0100, 0x01FF, 2, 1) == std::pair<std::uint64_t, std::uint64_t>{1, 1});
}

TEST_CASE("an OFL matrix's template channels are resolved per pixel and per group",
          "[fixtures][ofl]") {
    const Definition alc4 = readOfl("adb/alc4.json");
    CHECK(alc4.format == "ofl");
    CHECK(alc4.key == "adb/alc4");
    CHECK(alc4.manufacturer == "adb");
    CHECK(alc4.model == "ALC4");
    CHECK(alc4.revision == "2018-08-24");
    CHECK(alc4.modes.size() == 6);

    // The pixel keys are listed "4", "3", "2", "1"; eachPixelABC sorts them.
    const DefMode& matrix = modeNamed(alc4, "Matrix Linear");
    REQUIRE(matrix.refusal.empty());
    const std::vector<std::string> labels = labelsOf(matrix);
    REQUIRE(labels.size() == 20);
    CHECK(std::vector<std::string>(labels.begin(), labels.begin() + 6) ==
          std::vector<std::string>{"Dimmer 1", "Color Temperature 1 (Linear)", "Red 1", "Green 1",
                                   "Blue 1", "Dimmer 2"});
    CHECK(labels.back() == "Blue 4");
    CHECK(matrix.parts[0][0].cell == "1");
    CHECK(matrix.parts[0][19].cell == "4");
    CHECK(matrix.parts[0][0].kind == Kind::Dimmer);
    CHECK(matrix.parts[0][1].kind == Kind::Other); // an effect and a colour temperature
    CHECK(matrix.parts[0][2].kind == Kind::Red);

    // A template resolved with a group key: "Master" is a list of pixels, so it is a cell.
    const DefMode& standard = modeNamed(alc4, "Standard Linear");
    CHECK(labelsOf(standard) ==
          std::vector<std::string>{"Dimmer Master", "Color Temperature Master (Linear)",
                                   "Red Master", "Green Master", "Blue Master"});
    CHECK(cellsOf(standard) == std::vector<std::string>(5, "Master"));

    // And an available channel before the pixels, with no cell.
    const DefMode& extended = modeNamed(alc4, "Extended Linear");
    REQUIRE(extended.parts.at(0).size() == 21);
    CHECK(extended.parts[0][0].label == "Master");
    CHECK(extended.parts[0][0].cell.empty());
    CHECK(extended.parts[0][0].kind == Kind::Dimmer);
}

TEST_CASE("an OFL group of every pixel is no cell, and a repeatFor list is taken as listed",
          "[fixtures][ofl]") {
    const Definition pocket = readOfl("american-dj/crazy-pocket-8.json");
    const DefMode& seventeen = modeNamed(pocket, "17-channel");
    REQUIRE(seventeen.refusal.empty());
    const std::vector<std::string> labels = labelsOf(seventeen);
    REQUIRE(labels.size() == 17);
    CHECK(labels[2] == "Tilt Top");
    CHECK(labels[3] == "Tilt Top fine");
    CHECK(labels[6] == "Red Master");
    CHECK(seventeen.parts[0][2].cell == "Top"); // a group by name pattern
    CHECK(seventeen.parts[0][3].byte == 1);     // a template's fine alias
    CHECK(seventeen.parts[0][3].kind == Kind::Tilt);
    CHECK(seventeen.parts[0][6].cell.empty()); // "Master" is "all"
    CHECK(seventeen.parts[0][6].kind == Kind::Red);

    const DefMode& twentyOne = modeNamed(pocket, "21-channel");
    REQUIRE(twentyOne.refusal.empty());
    const std::vector<std::string> wide = labelsOf(twentyOne);
    REQUIRE(wide.size() == 21);
    CHECK(std::vector<std::string>(wide.begin() + 6, wide.begin() + 14) ==
          std::vector<std::string>{"Red Top", "Green Top", "Blue Top", "Amber Top", "Red Bottom",
                                   "Green Bottom", "Blue Bottom", "Amber Bottom"});
    CHECK(twentyOne.parts[0][10].cell == "Bottom");
    CHECK(twentyOne.parts[0][9].kind == Kind::Amber);

    // "Shutter / Strobe": its first "Open" range starts at 0.
    const DefChannel& shutter = twentyOne.parts[0][14];
    CHECK(shutter.label == "Shutter / Strobe");
    CHECK(shutter.kind == Kind::Shutter);
    CHECK(shutter.openValue == 0);
}

TEST_CASE("an OFL switching channel is what its dependency's default selects", "[fixtures][ofl]") {
    const Definition c300d = readOfl("aputure/c300d.json");
    const DefMode& mode = modeNamed(c300d, "5-channel");
    REQUIRE(mode.refusal.empty());
    CHECK(labelsOf(mode) == std::vector<std::string>{"Dimmer", "Mode Selection", "FX Control",
                                                     "FX Frequency Control",
                                                     "FX Options (off by default)"});
    const std::vector<DefChannel>& part = mode.parts[0];
    CHECK(part[0].kind == Kind::Dimmer);
    CHECK(part[0].defaultValue == 0); // "0%"
    CHECK(part[4].kind == Kind::Nothing);
    REQUIRE(part[4].notes.size() == 1);
    CHECK_THAT(part[4].notes[0], ContainsSubstring("changes with 'FX Control'"));
    // A capability the library marks as needing checking says so on its channel.
    REQUIRE_FALSE(part[3].notes.empty());
    CHECK_THAT(part[3].notes[0], ContainsSubstring("What speeds do these numbers represent?"));

    SECTION("switched to a real channel, and to its fine byte") {
        const std::string channels = R"(
            "Mode": {"defaultValue": 200, "capabilities": [
                {"dmxRange": [0, 127], "type": "Effect", "effectName": "a", "switchChannels": {"Switched": "Speed", "Switched 2": null}},
                {"dmxRange": [128, 255], "type": "Effect", "effectName": "b", "switchChannels": {"Switched": "Dimmer", "Switched 2": "Dimmer fine"}}]},
            "Speed": {"capability": {"type": "EffectSpeed", "speed": "fast"}},
            "Dimmer": {"fineChannelAliases": ["Dimmer fine"], "defaultValue": 4660, "capability": {"type": "Intensity"}})";
        const Definition definition = readOflJson(oflFixture(
            channels, R"({"name": "3ch", "channels": ["Mode", "Switched", "Switched 2"]})"));
        const DefMode& switched = definition.modes.at(0);
        REQUIRE(switched.refusal.empty());
        CHECK(labelsOf(switched) == std::vector<std::string>{"Mode", "Dimmer", "Dimmer fine"});
        CHECK(switched.parts[0][1].kind == Kind::Dimmer);
        CHECK(switched.parts[0][2].byte == 1);
        // Both of the dimmer's bytes are in the mode, so it is read at 16 bits.
        CHECK(switched.parts[0][1].resolutionBytes == 2);
        CHECK(switched.parts[0][1].defaultValue == 4660);
    }
}

TEST_CASE("OFL defaults are read at the channel's value resolution and scaled to the mode's",
          "[fixtures][ofl]") {
    const Definition diablo = readOfl("ayrton/diablo-s.json");
    // "Blade System Rotation": 50% of a 16-bit range, floor(0.5 × 65535) = 0x7FFF.
    const DefMode& standard = modeNamed(diablo, "Standard");
    REQUIRE(standard.parts.at(0).size() == 36);
    const DefChannel& blades = standard.parts[0][34];
    CHECK(blades.label == "Blade System Rotation");
    CHECK(blades.resolutionBytes == 1);
    CHECK(blades.defaultValue == 0x7F);
    const DefMode& extended = modeNamed(diablo, "Extended");
    const DefChannel& bladesWide = extended.parts.at(0).at(53);
    CHECK(bladesWide.label == "Blade System Rotation");
    CHECK(bladesWide.resolutionBytes == 2);
    CHECK(bladesWide.defaultValue == 0x7FFF);
    CHECK(extended.parts[0][54].label == "Blade System Rotation fine");
    CHECK(extended.parts[0][54].byte == 1);

    SECTION("a value written at 8 bits on a 16-bit channel repeats its byte") {
        const std::string channels = R"(
            "Iris": {"fineChannelAliases": ["Iris fine"], "dmxValueResolution": "8bit", "defaultValue": 132,
                     "capabilities": [{"dmxRange": [0, 131], "type": "Iris", "openPercentStart": "100%", "openPercentEnd": "4%"},
                                      {"dmxRange": [132, 255], "type": "Iris", "openPercent": "4%"}]})";
        const Definition definition = readOflJson(oflFixture(
            channels,
            R"({"name": "a", "channels": ["Iris", "Iris fine"]}, {"name": "b", "channels": ["Iris"]})"));
        CHECK(definition.modes[0].parts[0][0].defaultValue == 0x8484);
        CHECK(definition.modes[1].parts[0][0].defaultValue == 132);
    }
}

TEST_CASE("OFL capabilities decide a channel's kind", "[fixtures][ofl]") {
    const Definition diablo = readOfl("ayrton/diablo-s.json");
    const std::vector<DefChannel>& part = modeNamed(diablo, "Standard").parts.at(0);
    CHECK(part[0].kind == Kind::Pan);
    CHECK(part[1].kind == Kind::Pan);
    CHECK(part[1].byte == 1);
    // A pan/tilt speed with maintenance ranges above it shares its range.
    CHECK(part[4].kind == Kind::PanTiltSpeed);
    CHECK(part[4].shared);
    CHECK(part[4].sharedWith == "maintenance");
    // The shutter: its default (11) and its first "Open" range, which starts at 11.
    CHECK(part[5].kind == Kind::Shutter);
    CHECK(part[5].defaultValue == 11);
    CHECK(part[5].openValue == 11);
    // Two focuses: the auto focus is a second one ("no function", then focus ranges).
    CHECK(part[11].kind == Kind::Focus);
    CHECK(part[11].ordinal == 1);
    CHECK(part[12].label == "Auto Focus");
    CHECK(part[12].kind == Kind::Focus);
    CHECK(part[12].ordinal == 2);
    CHECK_FALSE(part[12].shared);
    CHECK(part[14].kind == Kind::ColorWheel);
    // Cyan, magenta and yellow with no red, green or blue: a CMY head's flags.
    CHECK(part[15].kind == Kind::CyanSub);
    CHECK(part[16].kind == Kind::MagentaSub);
    CHECK(part[17].kind == Kind::YellowSub);
    CHECK(part[19].kind == Kind::Gobo);
    CHECK(part[21].kind == Kind::Other); // the animation wheel: rotation only

    SECTION("beside red, green and blue they are LEDs takt4 has no role for") {
        const std::string channels = R"(
            "R": {"capability": {"type": "ColorIntensity", "color": "Red"}},
            "C": {"capability": {"type": "ColorIntensity", "color": "Cyan"}},
            "M": {"capability": {"type": "ColorIntensity", "color": "Magenta"}},
            "Y": {"capability": {"type": "ColorIntensity", "color": "Yellow"}})";
        const Definition all =
            readOflJson(oflFixture(channels, R"({"name": "a", "channels": ["R", "C", "M", "Y"]},
                                                                  {"name": "b", "channels": ["C", "M", "Y"]},
                                                                  {"name": "c", "channels": ["C", "M"]})"));
        CHECK(all.modes[0].parts[0][1].kind == Kind::OtherEmitter);
        CHECK(all.modes[1].parts[0][0].kind == Kind::CyanSub);
        CHECK(all.modes[2].parts[0][0].kind == Kind::OtherEmitter);
    }
    SECTION("several drivable kinds in one channel do several things") {
        const std::string channels = R"(
            "Mix": {"capabilities": [{"dmxRange": [0, 127], "type": "Intensity"},
                                     {"dmxRange": [128, 255], "type": "ColorIntensity", "color": "Red"}]},
            "Lime Indigo": {"capabilities": [{"dmxRange": [0, 127], "type": "ColorIntensity", "color": "Lime"},
                                             {"dmxRange": [128, 255], "type": "ColorIntensity", "color": "Indigo"}]},
            "Strobe rate": {"capability": {"type": "StrobeSpeed", "speed": "fast"}},
            "Shutter / Dimmer": {"capabilities": [{"dmxRange": [0, 7], "type": "ShutterStrobe", "shutterEffect": "Closed"},
                                                  {"dmxRange": [8, 255], "type": "Intensity"}]})";
        const Definition definition = readOflJson(oflFixture(
            channels,
            R"({"name": "a", "channels": ["Mix", "Lime Indigo", "Strobe rate", "Shutter / Dimmer"]})"));
        const DefMode& mode = definition.modes[0];
        CHECK(mode.parts[0][0].kind == Kind::Other);
        REQUIRE(mode.parts[0][0].notes.size() == 1);
        CHECK(mode.parts[0][0].notes[0] == "does several things (dimmer, red)");
        CHECK(mode.parts[0][1].kind == Kind::OtherEmitter); // one kind, two colours
        CHECK(mode.parts[0][2].kind == Kind::StrobeRate);
        CHECK(mode.parts[0][3].kind == Kind::Dimmer); // ADJ's Auto Spot 150
        CHECK(mode.parts[0][3].shared);
        CHECK(mode.parts[0][3].sharedWith == "shutter strobe");
    }
}

TEST_CASE("an OFL shutter's open value is its first Open range's menu value", "[fixtures][ofl]") {
    const auto openOf = [](const std::string& capabilities) {
        const std::string channels = R"("S": {"capabilities": )" + capabilities + "}";
        const DefChannel shutter =
            readOflJson(oflFixture(channels, R"({"name": "a", "channels": ["S"]})"))
                .modes[0]
                .parts[0][0];
        REQUIRE(shutter.kind == Kind::Shutter);
        return shutter.openValue;
    };
    const std::string closed =
        R"({"dmxRange": [0, 9], "type": "ShutterStrobe", "shutterEffect": "Closed"},)";
    CHECK(openOf("[" + closed +
                 R"({"dmxRange": [10, 20], "type": "ShutterStrobe", "shutterEffect": "Open"}])") ==
          10);
    CHECK(
        openOf(
            "[" + closed +
            R"({"dmxRange": [10, 20], "type": "ShutterStrobe", "shutterEffect": "Open", "menuClick": "center"}])") ==
        15);
    CHECK(
        openOf(
            "[" + closed +
            R"({"dmxRange": [10, 20], "type": "ShutterStrobe", "shutterEffect": "Open", "menuClick": "end"}])") ==
        20);
    CHECK(
        openOf(
            "[" + closed +
            R"({"dmxRange": [10, 20], "type": "ShutterStrobe", "shutterEffect": "Open", "menuClick": "hidden"}])") ==
        10);
    // No open range: a first range of no function (an LED par's "no strobe"), else nothing.
    CHECK(
        openOf(
            R"([{"dmxRange": [0, 9], "type": "NoFunction"}, {"dmxRange": [10, 255], "type": "ShutterStrobe", "shutterEffect": "Strobe"}])") ==
        0);
    CHECK_FALSE(openOf(
        R"([{"dmxRange": [0, 9], "type": "ShutterStrobe", "shutterEffect": "Strobe"}, {"dmxRange": [10, 255], "type": "NoFunction"}])"));
}

TEST_CASE("an OFL matrix insert follows its repeatFor and channelOrder", "[fixtures][ofl]") {
    // Pixel keys hold ")\"", so these strings take a delimiter of their own.
    const std::string matrix = R"j("matrix": {"pixelCount": [2, 2, 1],
        "pixelGroups": {"2": ["(1, 1)"], "Left": {"x": ["=1"]}, "1": "all", "Odd rows": {"y": ["odd"]}}},
        "templateChannels": {
            "R $pixelKey": {"fineChannelAliases": ["R $pixelKey fine"], "capability": {"type": "ColorIntensity", "color": "Red"}},
            "G $pixelKey": {"capability": {"type": "ColorIntensity", "color": "Green"}}},)j";
    const auto mode = [&](const std::string& insert) {
        const Definition definition = readOflJson(oflFixture(
            R"("Dim": {"capability": {"type": "Intensity"}})",
            R"({"name": "m", "channels": ["Dim", {"insert": "matrixChannels", )" + insert + "}]}",
            matrix));
        REQUIRE(definition.modes[0].refusal.empty());
        return labelsOf(definition.modes[0]);
    };
    SECTION("eachPixelXYZ reads like a book, eachPixelYXZ down the columns") {
        CHECK(
            mode(
                R"("repeatFor": "eachPixelXYZ", "channelOrder": "perPixel", "templateChannels": ["R $pixelKey"])") ==
            std::vector<std::string>{"Dim", "R (1, 1)", "R (2, 1)", "R (1, 2)", "R (2, 2)"});
        CHECK(
            mode(
                R"("repeatFor": "eachPixelYXZ", "channelOrder": "perPixel", "templateChannels": ["R $pixelKey"])") ==
            std::vector<std::string>{"Dim", "R (1, 1)", "R (1, 2)", "R (2, 1)", "R (2, 2)"});
    }
    SECTION("perChannel: each template for every pixel, then the next") {
        CHECK(
            mode(
                R"j("repeatFor": ["(2, 2)", "(1, 1)"], "channelOrder": "perChannel", "templateChannels": ["R $pixelKey", null, "G $pixelKey"])j") ==
            std::vector<std::string>{"Dim", "R (2, 2)", "R (1, 1)", "(nothing)", "(nothing)",
                                     "G (2, 2)", "G (1, 1)"});
    }
    SECTION("eachPixelGroup: the file's order, with JavaScript's integer keys first") {
        const std::vector<std::string> labels = mode(
            R"("repeatFor": "eachPixelGroup", "channelOrder": "perPixel", "templateChannels": ["G $pixelKey"])");
        CHECK(labels == std::vector<std::string>{"Dim", "G 1", "G 2", "G Left", "G Odd rows"});
    }
    SECTION("a template's fine alias, listed directly") {
        const Definition definition = readOflJson(oflFixture(
            R"("Dim": {"capability": {"type": "Intensity"}})",
            R"j({"name": "m", "channels": ["R (1, 2)", "R (1, 2) fine", "G Left"]})j", matrix));
        const DefMode& listed = definition.modes[0];
        REQUIRE(listed.refusal.empty());
        CHECK(listed.parts[0][0].cell == "(1, 2)");
        CHECK(listed.parts[0][1].byte == 1);
        CHECK(listed.parts[0][1].resolutionBytes == 2);
        CHECK(listed.parts[0][2].cell == "Left");
    }
    SECTION("an insert with no matrix, or a key the file does not define, refuses the mode") {
        const Definition definition =
            readOflJson(oflFixture(R"("Dim": {"capability": {"type": "Intensity"}})",
                                   R"({"name": "a", "channels": ["Dim", "Nope"]},
               {"name": "b", "channels": [{"insert": "matrixChannels", "repeatFor": "eachPixelABC", "channelOrder": "perPixel", "templateChannels": ["X $pixelKey"]}]},
               {"name": "c", "channels": ["Dim"]})"));
        CHECK(definition.modes[0].refusal == "it uses a channel the file doesn't define ('Nope')");
        CHECK_THAT(definition.modes[1].refusal, ContainsSubstring("the file describes no pixels"));
        CHECK(definition.modes[2].refusal.empty());
    }
}

TEST_CASE("an OFL file is known by its library keys, or its folder and name", "[fixtures][ofl]") {
    const std::string channels = R"("Dim": {"capability": {"type": "Intensity"}})";
    const std::string modes =
        R"({"name": "1ch", "channels": ["Dim", null]}, {"name": "1ch", "channels": ["Dim"]})";
    const Definition fromRepository =
        readOflJson(oflFixture(channels, modes, R"("helpWanted": "Is the dimmer curve right?",)"));
    CHECK(fromRepository.key == "maker/fixture");
    CHECK(fromRepository.manufacturer == "maker");
    CHECK(fromRepository.revision == "2026-02-02");
    REQUIRE(fromRepository.notes.size() == 1);
    CHECK_THAT(fromRepository.notes[0], ContainsSubstring("Is the dimmer curve right?"));
    CHECK(fromRepository.modes[0].name == "1ch");
    CHECK(fromRepository.modes[1].name == "1ch (2)");
    CHECK(labelsOf(fromRepository.modes[0]) == std::vector<std::string>{"Dim", "(nothing)"});

    const Definition downloaded = readOflJson(
        oflFixture(channels, modes, R"("manufacturerKey": "acme", "fixtureKey": "par-1",)"));
    CHECK(downloaded.key == "acme/par-1");

    const std::string future =
        R"({"$schema": "https://raw.githubusercontent.com/OpenLightingProject/open-fixture-library/schema-13.0.0/schemas/fixture.json",
        "name": "F", "availableChannels": {"Dim": {"capability": {"type": "Intensity"}}}, "modes": [{"name": "m", "channels": ["Dim"]}]})";
    CHECK_THAT(readOflJson(future).notes.at(0),
               ContainsSubstring("version 13 of the library's format"));
}

TEST_CASE("what is not an OFL fixture is refused with a reason", "[fixtures][ofl][hostile]") {
    const auto refused = [](const std::string& text) {
        const auto result = readOflText(text, "x.json", "m", "f");
        CHECK_FALSE(result.definition);
        CHECK_FALSE(result.problem.empty());
        return result.problem;
    };
    CHECK(refused("") == "it is empty");
    CHECK_THAT(refused("{"), ContainsSubstring("it is damaged, or not a fixture file"));
    CHECK_THAT(refused("{\"name\": \"x\", \"modes\": [}"),
               ContainsSubstring("it is damaged, or not a fixture file"));
    CHECK(refused("[1, 2]") == "it isn't an Open Fixture Library fixture");
    CHECK(refused(R"({"name": "x", "modes": []})") == "it isn't an Open Fixture Library fixture");
    CHECK(refused(R"({"name": 5, "modes": [], "availableChannels": {}})") ==
          "it isn't an Open Fixture Library fixture");
    CHECK(refused(R"({"name": "x", "modes": [], "availableChannels": {}})") == "it has no modes");
    CHECK_THAT(refused("{\"name\": \"\xFF\xFE\"}"),
               ContainsSubstring("it is damaged, or not a fixture file"));

    SECTION("nested deeper than any fixture") {
        CHECK(refused(std::string(100000, '[') + std::string(100000, ']')) ==
              "it isn't a fixture file (it nests far too deep)");
    }
    SECTION("larger than any fixture") {
        CHECK(refused("{\"name\": \"" + std::string(9u * 1024u * 1024u, 'x') + "\"}") ==
              "it is too big to be a fixture file");
    }
    SECTION("a matrix of a million pixels is not built") {
        const std::string huge = oflFixture(
            R"("Dim": {"capability": {"type": "Intensity"}})",
            R"({"name": "m", "channels": [{"insert": "matrixChannels", "repeatFor": "eachPixelABC", "channelOrder": "perPixel", "templateChannels": ["R $pixelKey"]}]})",
            R"("matrix": {"pixelCount": [1000, 1000, 1]}, "templateChannels": {"R $pixelKey": {"capability": {"type": "Intensity"}}},)");
        const Definition definition = readOflJson(huge);
        CHECK_THAT(definition.modes[0].refusal, ContainsSubstring("more than 10000 pixels"));
    }
    SECTION("a mode longer than a universe is refused before it is built") {
        const std::string wide = oflFixture(
            R"("Dim": {"capability": {"type": "Intensity"}})",
            R"({"name": "m", "channels": [{"insert": "matrixChannels", "repeatFor": "eachPixelABC", "channelOrder": "perPixel", "templateChannels": ["R $pixelKey", "R $pixelKey"]}]})",
            R"("matrix": {"pixelCount": [300, 1, 1]}, "templateChannels": {"R $pixelKey": {"capability": {"type": "Intensity"}}},)");
        CHECK_THAT(readOflJson(wide).modes[0].refusal,
                   ContainsSubstring("more than a universe's 512"));
    }
}
