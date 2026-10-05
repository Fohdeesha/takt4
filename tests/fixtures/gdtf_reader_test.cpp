#include "core/fixtures/definition.hpp"
#include "core/fixtures/gdtf_attributes.hpp"
#include "core/fixtures/gdtf_reader.hpp"

#include "fixtures/fixture_files.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::fixtures::DefChannel;
using takt4::fixtures::Definition;
using takt4::fixtures::DefMode;
using takt4::fixtures::Kind;
using takt4::fixtures::readGdtfArchive;
using takt4::fixtures::readGdtfDescription;
using takt4::fixtures::gdtf::parseDmxValue;
using takt4::test::gdtfChannel;
using takt4::test::gdtfDocument;
using takt4::test::gdtfMode;
using takt4::test::labelsOf;
using takt4::test::readGdtf;

namespace {

std::uint64_t valueOf(const char* text, unsigned bytes) {
    bool clamped = false;
    const auto value = parseDmxValue(text, bytes, clamped);
    REQUIRE(value);
    CHECK_FALSE(clamped);
    return *value;
}

/// The definition's one mode — a copy, since the definition is usually a temporary.
DefMode onlyMode(const Definition& definition) {
    REQUIRE(definition.modes.size() == 1);
    return definition.modes.front();
}

} // namespace

TEST_CASE("a GDTF DMX value is converted to the channel's bytes by mirroring or shifting",
          "[fixtures][gdtf]") {
    CHECK(valueOf("255/1", 1) == 255);
    // Mirroring, GDTF's default: the byte repeated, so full stays full.
    CHECK(valueOf("255/1", 2) == 65535);
    CHECK(valueOf("128/1", 2) == 0x8080);
    CHECK(valueOf("1/1", 3) == 0x010101);
    // Copies of the low byte: 0x1234 on three bytes.
    CHECK(valueOf("4660/2", 3) == 0x123434);
    // Shifting, asked for with "s".
    CHECK(valueOf("255/1s", 2) == 65280);
    CHECK(valueOf("1/1s", 3) == 0x010000);
    // More bytes than the channel has: the top ones.
    CHECK(valueOf("32768/2", 1) == 128);
    CHECK(valueOf("0/4", 1) == 0);
    // A bare number is one byte's worth.
    CHECK(valueOf("12", 1) == 12);
    CHECK(valueOf(" 7 / 1 ", 1) == 7);

    SECTION("a value too large for its own byte count is clamped, and says so") {
        bool clamped = false;
        CHECK(parseDmxValue("300/1", 1, clamped) == 255);
        CHECK(clamped);
        CHECK(parseDmxValue("99999999999999999999999/2", 2, clamped) == 65535);
        CHECK(clamped);
    }
    SECTION("anything else is not a value") {
        bool clamped = false;
        for (const char* text : {"", "abc", "5/0", "5/9", "-1/1", "1/x", "1/1x", "0x10/1", "/1"}) {
            INFO(text);
            CHECK_FALSE(parseDmxValue(text, 1, clamped));
        }
        CHECK_FALSE(parseDmxValue("1/1", 0, clamped));
        CHECK_FALSE(parseDmxValue("1/1", 9, clamped));
    }
}

TEST_CASE("a GDTF mode's channels land at their offsets, and a virtual one is left out",
          "[fixtures][gdtf]") {
    const std::string geometries = R"(<Geometry Name="Body"><Beam Name="Beam"/></Geometry>)";
    const std::string channels =
        // GDTF's virtual dimmer, which has no address at all.
        R"(<DMXChannel Geometry="Body" Offset=""><LogicalChannel Attribute="Dimmer"><ChannelFunction Attribute="Dimmer" DMXFrom="0/1" Default="255/1"/></LogicalChannel></DMXChannel>)" +
        gdtfChannel("Body", "1", "Dimmer", "", R"(DMXFrom="0/1" Default="255/1")") +
        gdtfChannel("Beam", "2", "ColorAdd_R") + gdtfChannel("Beam", "3", "ColorAdd_G") +
        gdtfChannel("Beam", "4", "ColorAdd_B") + gdtfChannel("Beam", "6", "ColorAdd_W");
    const Definition definition =
        readGdtf(gdtfDocument(geometries, gdtfMode("6ch", "Body", channels)));
    const DefMode& mode = onlyMode(definition);
    REQUIRE(mode.refusal.empty());
    REQUIRE(mode.parts.size() == 1);
    CHECK(labelsOf(mode) ==
          std::vector<std::string>{"dimmer", "red", "green", "blue", "(not in the file)", "white"});
    const std::vector<DefChannel>& part = mode.parts[0];
    CHECK(part[0].kind == Kind::Dimmer);
    CHECK(part[0].defaultValue == 255);
    CHECK(part[1].kind == Kind::Red);
    CHECK(part[4].kind == Kind::Nothing);
    CHECK(part[5].kind == Kind::White);
    CHECK(part[1].cell.empty());
    CHECK(definition.format == "gdtf");
    CHECK(definition.key == "0E6F7C1A-1111-4222-8333-444455556666");
    CHECK(definition.manufacturer == "takt4 tests");
    CHECK(definition.model == "Test Par");
}

TEST_CASE("a GDTF channel of several bytes takes one address per byte, coarse first",
          "[fixtures][gdtf]") {
    const std::string channels =
        gdtfChannel("Body", "1,2", "Pan", "", R"(DMXFrom="0/1" Default="32768/2")") +
        gdtfChannel("Body", "5,3,4", "Tilt") + gdtfChannel("Body", "6", "Zoom");
    const DefMode& mode = onlyMode(
        readGdtf(gdtfDocument(R"(<Geometry Name="Body"/>)", gdtfMode("m", "Body", channels))));
    REQUIRE(mode.parts.size() == 1);
    const std::vector<DefChannel>& part = mode.parts[0];
    REQUIRE(part.size() == 6);
    CHECK(labelsOf(mode) ==
          std::vector<std::string>{"pan", "pan fine", "tilt fine", "tilt fine 2", "tilt", "zoom"});
    CHECK(part[0].byte == 0);
    CHECK(part[1].byte == 1);
    CHECK(part[0].resolutionBytes == 2);
    CHECK(part[0].defaultValue == 0x8000);
    // Offsets listed coarse to finest, whatever their order on the wire.
    CHECK(part[4].kind == Kind::Tilt);
    CHECK(part[4].byte == 0);
    CHECK(part[2].byte == 1);
    CHECK(part[3].byte == 2);
    CHECK(part[4].resolutionBytes == 3);
    CHECK_FALSE(part[4].defaultValue);
    CHECK(part[5].kind == Kind::Zoom);
    CHECK(part[5].ordinal == 1);
}

TEST_CASE("a GDTF channel's default comes from its initial function", "[fixtures][gdtf]") {
    const auto channel = [](const std::string& initial) {
        return R"(<DMXChannel Geometry="Body" Offset="1" )" + initial +
               R"(><LogicalChannel Attribute="Gobo1">
                    <ChannelFunction Attribute="Gobo1" Name="Select" DMXFrom="0/1" Default="0/1"/>
                    <ChannelFunction Attribute="Gobo1SelectSpin" Name="Spin" DMXFrom="128/1" Default="200/1"/>
                  </LogicalChannel>
                  <LogicalChannel Attribute="Gobo1WheelSpin">
                    <ChannelFunction Attribute="Gobo1WheelSpin" Name="Wheel" DMXFrom="0/1" Default="77/1"/>
                  </LogicalChannel></DMXChannel>)";
    };
    const auto read = [&](const std::string& initial) {
        return onlyMode(readGdtf(gdtfDocument(R"(<Geometry Name="Body"/>)",
                                              gdtfMode("m", "Body", channel(initial)))))
            .parts[0][0];
    };
    SECTION("named in full: geometry_attribute.logical.function") {
        const DefChannel spin = read(R"(InitialFunction="Body_Gobo1.Gobo1.Spin")");
        CHECK(spin.defaultValue == 200);
        CHECK(spin.fallbackValue == 128);
        // In another logical channel too: the alternatives are searched as well.
        CHECK(read(R"(InitialFunction="Body_Gobo1.Gobo1WheelSpin.Wheel")").defaultValue == 77);
    }
    SECTION("not named, or named wrongly: the first function of the first logical channel") {
        CHECK(read("").defaultValue == 0);
        CHECK(read(R"(InitialFunction="Body_Gobo1.Gobo1.Nothing")").defaultValue == 0);
        CHECK(read(R"(InitialFunction="body_gobo1.gobo1.spin")").defaultValue == 0);
    }
    SECTION("it is the gobo wheel, whichever function starts it") {
        const DefChannel spin = read(R"(InitialFunction="Body_Gobo1.Gobo1.Spin")");
        CHECK(spin.kind == Kind::Gobo);
        CHECK(spin.ordinal == 1);
        CHECK(spin.label == "gobo wheel 1");
        CHECK_FALSE(spin.shared);
    }
    SECTION("GDTF 1.0 kept the default on the channel itself") {
        const std::string old =
            R"(<DMXChannel Geometry="Body" Offset="1" Default="12/1"><LogicalChannel Attribute="Dimmer"><ChannelFunction Attribute="Dimmer" DMXFrom="0/1"/></LogicalChannel></DMXChannel>)";
        const DefChannel dimmer =
            onlyMode(readGdtf(gdtfDocument(R"(<Geometry Name="Body"/>)", gdtfMode("m", "Body", old),
                                           "1.0")))
                .parts[0][0];
        CHECK(dimmer.defaultValue == 12);
    }
}

TEST_CASE("a GDTF geometry reference places its channels once per reference", "[fixtures][gdtf]") {
    const std::string geometries = R"(
        <Geometry Name="Bar">
          <GeometryReference Name="Pixel 1" Geometry="Pixel"><Break DMXBreak="1" DMXOffset="1"/></GeometryReference>
          <GeometryReference Name=" Pixel 2 " Geometry="Pixel"><Break DMXBreak="1" DMXOffset="4"/></GeometryReference>
        </Geometry>
        <Geometry Name="Pixel"><Beam Name="PixelBeam"/></Geometry>)";
    const std::string channels = gdtfChannel("Bar", "7", "Dimmer") +
                                 gdtfChannel("Pixel", "1", "ColorAdd_R", R"(DMXBreak="1")") +
                                 gdtfChannel("PixelBeam", "2", "ColorAdd_G") +
                                 gdtfChannel("Pixel", "3", "ColorAdd_B");
    const DefMode& mode =
        onlyMode(readGdtf(gdtfDocument(geometries, gdtfMode("Pixels", "Bar", channels))));
    REQUIRE(mode.refusal.empty());
    CHECK(labelsOf(mode) ==
          std::vector<std::string>{"red · Pixel 1", "green · Pixel 1", "blue · Pixel 1",
                                   "red · Pixel 2", "green · Pixel 2", "blue · Pixel 2", "dimmer"});
    CHECK(mode.parts[0][3].cell == "Pixel 2"); // trimmed
    CHECK(mode.parts[0][6].cell.empty());
    CHECK(mode.notes.empty());

    SECTION("a reference with no break for the channel's places it with no offset, and says so") {
        const std::string missing = R"(
            <Geometry Name="Bar"><GeometryReference Name="Pixel 1" Geometry="Pixel"><Break DMXBreak="2" DMXOffset="9"/></GeometryReference></Geometry>
            <Geometry Name="Pixel"/>)";
        const DefMode& placed = onlyMode(readGdtf(
            gdtfDocument(missing, gdtfMode("m", "Bar", gdtfChannel("Pixel", "1", "ColorAdd_R")))));
        REQUIRE(placed.refusal.empty());
        CHECK(labelsOf(placed) == std::vector<std::string>{"red · Pixel 1"});
        REQUIRE(placed.notes.size() == 1);
        CHECK_THAT(placed.notes[0], ContainsSubstring("doesn't say where \"Pixel 1\" starts"));
    }
}

TEST_CASE("a GDTF channel whose break is Overwrite takes the reference's last break",
          "[fixtures][gdtf]") {
    const std::string geometries = R"(
        <Geometry Name="Rig">
          <GeometryReference Name="Cell A" Geometry="Cell"><Break DMXBreak="1" DMXOffset="1"/><Break DMXBreak="2" DMXOffset="1"/></GeometryReference>
          <GeometryReference Name="Cell B" Geometry="Cell"><Break DMXBreak="1" DMXOffset="3"/><Break DMXBreak="2" DMXOffset="2"/></GeometryReference>
        </Geometry>
        <Geometry Name="Cell"/>)";
    const std::string channels = gdtfChannel("Rig", "1", "Dimmer") +
                                 gdtfChannel("Cell", "1", "ColorAdd_R", R"(DMXBreak="Overwrite")") +
                                 gdtfChannel("Cell", "2", "ColorAdd_G", R"(DMXBreak="1")");
    const DefMode& mode =
        onlyMode(readGdtf(gdtfDocument(geometries, gdtfMode("Two", "Rig", channels))));
    REQUIRE(mode.refusal.empty());
    // Break 1 is part 1, break 2 part 2: the overwritten red is on each cell's last break (2).
    REQUIRE(mode.parts.size() == 2);
    CHECK(labelsOf(mode, 0) == std::vector<std::string>{"dimmer", "green · Cell A",
                                                        "(not in the file)", "green · Cell B"});
    CHECK(labelsOf(mode, 1) == std::vector<std::string>{"red · Cell A", "red · Cell B"});
}

TEST_CASE(
    "a GDTF mode that needs more than two start addresses, or a universe and more, is refused",
    "[fixtures][gdtf]") {
    const std::string body = R"(<Geometry Name="Body"/>)";
    SECTION("three breaks") {
        const std::string channels = gdtfChannel("Body", "1", "Dimmer", R"(DMXBreak="1")") +
                                     gdtfChannel("Body", "1", "ColorAdd_R", R"(DMXBreak="2")") +
                                     gdtfChannel("Body", "1", "ColorAdd_G", R"(DMXBreak="3")");
        const DefMode& mode =
            onlyMode(readGdtf(gdtfDocument(body, gdtfMode("m", "Body", channels))));
        CHECK(mode.refusal == "it needs 3 start addresses; takt4 handles 2 at most");
        CHECK(mode.parts.empty());
    }
    SECTION("past 512") {
        const DefMode& mode = onlyMode(readGdtf(
            gdtfDocument(body, gdtfMode("m", "Body", gdtfChannel("Body", "513", "Dimmer")))));
        CHECK(mode.refusal == "it needs 513 channels, more than a universe's 512");
        const DefMode& huge = onlyMode(readGdtf(
            gdtfDocument(body, gdtfMode("m", "Body", gdtfChannel("Body", "900000000", "Dimmer")))));
        CHECK_THAT(huge.refusal, ContainsSubstring("more than a universe's 512"));
    }
    SECTION("before the first address") {
        const DefMode& mode = onlyMode(readGdtf(
            gdtfDocument(body, gdtfMode("m", "Body", gdtfChannel("Body", "0", "Dimmer")))));
        CHECK_THAT(mode.refusal, ContainsSubstring("before the fixture's first address"));
    }
    SECTION("an offset that is not a number") {
        const DefMode& mode = onlyMode(readGdtf(
            gdtfDocument(body, gdtfMode("m", "Body", gdtfChannel("Body", "1,x", "Dimmer")))));
        CHECK_THAT(mode.refusal, ContainsSubstring("an address that isn't a number"));
    }
    SECTION("nothing in it at all") {
        const DefMode& mode = onlyMode(readGdtf(gdtfDocument(body, gdtfMode("m", "Body", ""))));
        CHECK(mode.refusal == "the file lists no channels for it");
    }
    SECTION("a geometry the file does not have") {
        const DefMode& mode = onlyMode(readGdtf(
            gdtfDocument(body, gdtfMode("m", "Nowhere", gdtfChannel("Body", "1", "Dimmer")))));
        CHECK(mode.refusal ==
              "the file doesn't describe the part of the fixture this mode is for (\"Nowhere\")");
    }
}

TEST_CASE("a GDTF channel the mode's geometry does not reach refuses the mode, not the file",
          "[fixtures][gdtf]") {
    SECTION("a top-level geometry nothing references") {
        const std::string geometries = R"(<Geometry Name="Base"/><Geometry Name="Ring"/>)";
        const std::string modes =
            gdtfMode("Broken", "Base",
                     gdtfChannel("Base", "1", "Dimmer") + gdtfChannel("Ring", "2", "Shutter1")) +
            gdtfMode("Fine", "Base", gdtfChannel("Base", "1", "Dimmer"));
        const Definition definition = readGdtf(gdtfDocument(geometries, modes));
        REQUIRE(definition.modes.size() == 2);
        CHECK(definition.modes[0].refusal ==
              "a channel is on a part of the fixture this mode doesn't reach (\"Ring\")");
        CHECK(definition.modes[1].refusal.empty());
    }
    SECTION("one reached only through a reference inside a referenced geometry") {
        const std::string geometries = R"(
            <Geometry Name="Top"><GeometryReference Name="Outer" Geometry="Middle"><Break DMXBreak="1" DMXOffset="1"/></GeometryReference></Geometry>
            <Geometry Name="Middle"><GeometryReference Name="Inner" Geometry="Leaf"><Break DMXBreak="1" DMXOffset="1"/></GeometryReference></Geometry>
            <Geometry Name="Leaf"/>)";
        const DefMode& mode = onlyMode(readGdtf(
            gdtfDocument(geometries, gdtfMode("m", "Top", gdtfChannel("Leaf", "1", "Dimmer")))));
        CHECK_THAT(mode.refusal, ContainsSubstring("nested inside another"));
    }
    SECTION("geometries inside a reference are ignored, and said to be") {
        const std::string geometries = R"(
            <Geometry Name="Base">
              <GeometryReference Name="Ring" Geometry="Spark"><Break DMXBreak="1" DMXOffset="2"/><GeometryReference Name="Stray" Geometry="Spark"/></GeometryReference>
            </Geometry>
            <Geometry Name="Spark"/>)";
        const DefMode& mode = onlyMode(readGdtf(
            gdtfDocument(geometries, gdtfMode("m", "Base",
                                              gdtfChannel("Base", "1", "Dimmer") +
                                                  gdtfChannel("Spark", "1", "ColorAdd_W")))));
        REQUIRE(mode.refusal.empty());
        CHECK(labelsOf(mode) == std::vector<std::string>{"dimmer", "white · Ring"});
        REQUIRE(mode.notes.size() == 1);
        CHECK_THAT(mode.notes[0], ContainsSubstring("inside \"Ring\", where none belong"));
    }
    SECTION("a reference to its own geometry does not send the reader round in circles") {
        const std::string geometries = R"(
            <Geometry Name="Loop"><GeometryReference Name="Self" Geometry="Loop"><Break DMXBreak="1" DMXOffset="3"/></GeometryReference></Geometry>)";
        const DefMode& mode = onlyMode(readGdtf(gdtfDocument(
            geometries, gdtfMode("m", "Loop", gdtfChannel("Elsewhere", "1", "Dimmer")))));
        CHECK_THAT(mode.refusal, ContainsSubstring("doesn't reach"));
    }
}

TEST_CASE("one GDTF address claimed twice is one channel or the first of two", "[fixtures][gdtf]") {
    const std::string geometries =
        R"(<Geometry Name="Base"><Beam Name="Beam 1"/><Beam Name="Beam 2"/></Geometry>)";
    SECTION("the same attribute on two beams is one channel for both (CKC's Blinder WW2)") {
        const DefMode& mode = onlyMode(
            readGdtf(gdtfDocument(geometries, gdtfMode("1 Channel", "Base",
                                                       gdtfChannel("Beam 1", "1", "Dimmer") +
                                                           gdtfChannel("Beam 2", "1", "dimmer")))));
        REQUIRE(mode.refusal.empty());
        CHECK(labelsOf(mode) == std::vector<std::string>{"dimmer · Beam 1 + Beam 2"});
        CHECK(mode.notes.empty());
    }
    SECTION("on two addresses the beams tell them apart") {
        const DefMode& mode = onlyMode(
            readGdtf(gdtfDocument(geometries, gdtfMode("2 Channel", "Base",
                                                       gdtfChannel("Beam 1", "1", "Dimmer") +
                                                           gdtfChannel("Beam 2", "2", "Dimmer") +
                                                           gdtfChannel("Base", "3", "Shutter1")))));
        CHECK(labelsOf(mode) ==
              std::vector<std::string>{"dimmer · Beam 1", "dimmer · Beam 2", "shutter 1"});
    }
    SECTION("two attributes on one address: the first is kept, and the mode says so") {
        const DefMode& mode = onlyMode(readGdtf(
            gdtfDocument(geometries, gdtfMode("m", "Base",
                                              gdtfChannel("Beam 1", "1", "Dimmer") +
                                                  gdtfChannel("Beam 1", "1", "ColorAdd_R")))));
        REQUIRE(mode.refusal.empty());
        CHECK(labelsOf(mode) == std::vector<std::string>{"dimmer"});
        REQUIRE(mode.notes.size() == 1);
        CHECK(mode.notes[0] == "the file gives channel 1 two meanings — the first was kept");
    }
}

TEST_CASE("a GDTF shutter is parked at the value the file says opens it", "[fixtures][gdtf]") {
    const auto openOf = [](const std::string& functions, const std::string& attribute = "Shutter1",
                           const std::string& extra = "") {
        const std::string channel = R"(<DMXChannel Geometry="Body" Offset="1" )" + extra +
                                    R"(><LogicalChannel Attribute=")" + attribute + R"(">)" +
                                    functions + "</LogicalChannel></DMXChannel>";
        const DefChannel shutter = onlyMode(readGdtf(gdtfDocument(R"(<Geometry Name="Body"/>)",
                                                                  gdtfMode("m", "Body", channel))))
                                       .parts[0][0];
        REQUIRE(shutter.kind == Kind::Shutter);
        return shutter.openValue;
    };
    SECTION("1: a channel set named open, in a shutter function") {
        CHECK(openOf(R"(<ChannelFunction Attribute="Shutter1" Name="Shutter" DMXFrom="0/1">
                          <ChannelSet Name="Closed" DMXFrom="0/1"/><ChannelSet Name="Shutter Open" DMXFrom="32/1"/>
                        </ChannelFunction>)") == 32);
        // As a word: "reopen" and "opening" are not "open".
        CHECK(openOf(R"(<ChannelFunction Attribute="Shutter1" Name="Shutter" DMXFrom="0/1">
                          <ChannelSet Name="Reopen" DMXFrom="5/1"/><ChannelSet Name="Opening" DMXFrom="6/1"/>
                          <ChannelSet Name="Main_Shutter_Open" DMXFrom="40/1"/>
                        </ChannelFunction>)") == 40);
    }
    SECTION("2: a shutter function named open") {
        CHECK(openOf(R"(<ChannelFunction Attribute="Shutter1" Name="Closed" DMXFrom="0/1"/>
                        <ChannelFunction Attribute="Shutter1" Name="Open" DMXFrom="20/1"/>)") ==
              20);
    }
    SECTION("3: a shutter function whose physical range is 1 to 1") {
        CHECK(
            openOf(
                R"(<ChannelFunction Attribute="Shutter1" Name="Shut" DMXFrom="0/1" PhysicalFrom="0" PhysicalTo="0"/>
                        <ChannelFunction Attribute="Shutter1" Name="Full" DMXFrom="10/1" PhysicalFrom="1.0" PhysicalTo="1"/>)") ==
            10);
    }
    SECTION("4: a strobe-only channel whose default is in its 'no strobe' range") {
        CHECK(
            openOf(
                R"(<ChannelFunction Attribute="NoFeature" Name="No strobe" DMXFrom="0/1" Default="5/1"/>
                        <ChannelFunction Attribute="Shutter1Strobe" Name="Strobe" DMXFrom="10/1"/>)",
                "Shutter1Strobe") == 5);
    }
    SECTION("5: none of those, and the file names no open state") {
        CHECK_FALSE(openOf(
            R"(<ChannelFunction Attribute="Shutter1Strobe" Name="Strobe" DMXFrom="0/1" Default="0/1">
                                <ChannelSet Name="Min" DMXFrom="0/1"/><ChannelSet Name="Max" DMXFrom="255/1"/>
                              </ChannelFunction>)",
            "Shutter1Strobe"));
    }
}

TEST_CASE("a GDTF strobe channel is the shutter only where there is no shutter",
          "[fixtures][gdtf]") {
    const std::string body = R"(<Geometry Name="Body"/>)";
    const DefMode& alone = onlyMode(readGdtf(gdtfDocument(
        body, gdtfMode("m", "Body", gdtfChannel("Body", "1", "Shutter1StrobeRandom")))));
    CHECK(alone.parts[0][0].kind == Kind::Shutter);
    CHECK(alone.parts[0][0].label == "random strobe 1");
    const DefMode& beside = onlyMode(readGdtf(
        gdtfDocument(body, gdtfMode("m", "Body",
                                    gdtfChannel("Body", "1", "Shutter1") +
                                        gdtfChannel("Body", "2", "Shutter1StrobeEffect")))));
    CHECK(beside.parts[0][0].kind == Kind::Shutter);
    CHECK(beside.parts[0][1].kind == Kind::StrobeRate);
}

TEST_CASE("a GDTF attribute is named the same whatever its case, and kept as written when it is "
          "the file's own",
          "[fixtures][gdtf]") {
    const DefMode& mode = onlyMode(readGdtf(gdtfDocument(
        R"(<Geometry Name="Body"/>)",
        gdtfMode("m", "Body",
                 gdtfChannel("Body", "1", "dimmer") + gdtfChannel("Body", "2", "COLORADD_R") +
                     gdtfChannel("Body", "3", "ColorTemperatureCTO") +
                     gdtfChannel("Body", "4", "Green / Magenta") +
                     gdtfChannel("Body", "5", "Gobo2Pos") + gdtfChannel("Body", "6", "Color2") +
                     gdtfChannel("Body", "7", "ColorRGB_Blue") +
                     gdtfChannel("Body", "8", "HSB_Hue") + gdtfChannel("Body", "9", "ColorSub_M") +
                     gdtfChannel("Body", "10", "Effects2Adjust3")))));
    CHECK(labelsOf(mode) == std::vector<std::string>{"dimmer", "red", "Color Temperature CTO",
                                                     "Green / Magenta", "gobo 2 rotation",
                                                     "color wheel 2", "blue (indirect)", "hue",
                                                     "magenta", "effects 2 adjust 3"});
    const std::vector<DefChannel>& part = mode.parts[0];
    CHECK(part[0].kind == Kind::Dimmer);
    CHECK(part[1].kind == Kind::Red);
    CHECK(part[2].kind == Kind::Other);
    CHECK(part[4].kind == Kind::Other);
    CHECK(part[5].kind == Kind::ColorWheel);
    CHECK(part[5].ordinal == 2);
    CHECK(part[6].kind == Kind::IndirectBlue);
    CHECK(part[7].kind == Kind::HsbOrCie);
    CHECK(part[8].kind == Kind::MagentaSub);
    CHECK(takt4::fixtures::gdtf::isAnnexAttribute("prism1MSPEED"));
    CHECK_FALSE(takt4::fixtures::gdtf::isAnnexAttribute("Prism0"));
}

TEST_CASE("a GDTF channel that is a dimmer for part of its range and something else for the rest "
          "is shared",
          "[fixtures][gdtf]") {
    const auto channel = [](const std::string& second) {
        return R"(<DMXChannel Geometry="Body" Offset="1"><LogicalChannel Attribute="Dimmer">
                    <ChannelFunction Attribute="Dimmer" Name="Dim" DMXFrom="0/1"/>)" +
               second + "</LogicalChannel></DMXChannel>";
    };
    const auto read = [&](const std::string& second) {
        return onlyMode(readGdtf(gdtfDocument(R"(<Geometry Name="Body"/>)",
                                              gdtfMode("m", "Body", channel(second)))))
            .parts[0][0];
    };
    const DefChannel strobing =
        read(R"(<ChannelFunction Attribute="Shutter1Strobe" Name="Strobe" DMXFrom="200/1"/>)");
    CHECK(strobing.kind == Kind::Dimmer);
    CHECK(strobing.shared);
    CHECK(strobing.sharedWith == "Strobe");
    // A range that does nothing is not a second function.
    CHECK_FALSE(
        read(R"(<ChannelFunction Attribute="NoFeature" Name="Off" DMXFrom="250/1"/>)").shared);
    CHECK_FALSE(
        read(R"(<ChannelFunction Attribute="dimmer" Name="Dim 2" DMXFrom="250/1"/>)").shared);
}

TEST_CASE("a GDTF file is known by its FixtureTypeID, and refused only when it is not GDTF at all",
          "[fixtures][gdtf]") {
    const std::string body = R"(<Geometry Name="Body"/>)";
    const std::string mode = gdtfMode("m", "Body", gdtfChannel("Body", "1", "Dimmer"));

    SECTION("the identity and the last revision") {
        const std::string revisions =
            R"(<Revisions><Revision Date="2020-01-01T00:00:00" Text="first"/><Revision Date="2021-02-03T04:05:06" Text=" second "/></Revisions>)";
        const Definition definition = readGdtf(gdtfDocument(
            body, mode, "1.1",
            R"(Name="" LongName="Long Name" ShortName="LN" Manufacturer="Maker" FixtureTypeID="abcdef01-2345-6789-abcd-ef0123456789")",
            revisions));
        CHECK(definition.key == "ABCDEF01-2345-6789-ABCD-EF0123456789");
        CHECK(definition.model == "Long Name");
        CHECK(definition.revision == "2021-02-03T04:05:06  second");
        CHECK(definition.notes.empty());
    }
    SECTION("no FixtureTypeID: maker and model, and a note") {
        const Definition definition =
            readGdtf(gdtfDocument(body, mode, "1.2", R"(Name="Par" Manufacturer="Maker")"));
        CHECK(definition.key == "gdtf:Maker/Par");
        REQUIRE(definition.notes.size() == 1);
        CHECK_THAT(definition.notes[0], ContainsSubstring("no fixture ID"));
    }
    SECTION("versions") {
        CHECK(readGdtf(gdtfDocument(body, mode, "1.0")).notes.empty());
        const Definition later = readGdtf(gdtfDocument(body, mode, "1.3"));
        REQUIRE(later.notes.size() == 1);
        CHECK_THAT(later.notes[0], ContainsSubstring("read as 1.2"));
        const Definition none = readGdtf(gdtfDocument(body, mode, ""));
        CHECK_THAT(none.notes.at(0), ContainsSubstring("no GDTF version"));
        const auto refused = readGdtfDescription(gdtfDocument(body, mode, "2.0"), "x.gdtf");
        CHECK_FALSE(refused.definition);
        CHECK(refused.problem == "it is GDTF version 2.0; takt4 reads version 1");
    }
    SECTION("a mode name used twice is told apart") {
        const Definition definition = readGdtf(gdtfDocument(body, mode + mode + mode));
        REQUIRE(definition.modes.size() == 3);
        CHECK(definition.modes[0].name == "m");
        CHECK(definition.modes[1].name == "m (2)");
        CHECK(definition.modes[2].name == "m (3)");
    }
    SECTION("what is not GDTF") {
        for (const char* text :
             {"", "   ", "not xml at all", "<GDTF DataVersion=\"1.2\">", "<Other/>",
              "<GDTF DataVersion=\"1.2\"/>", "<GDTF DataVersion=\"1.2\"><FixtureType/></GDTF>",
              "\xEF\xBB\xBF<GDTF><FixtureType><DMXModes/></FixtureType></GDTF>"}) {
            INFO(text);
            const auto result = readGdtfDescription(text, "x.gdtf");
            CHECK_FALSE(result.definition);
            CHECK_FALSE(result.problem.empty());
        }
    }
    SECTION("a byte order mark is not a problem") {
        CHECK(readGdtfDescription("\xEF\xBB\xBF" + gdtfDocument(body, mode), "x.gdtf").definition);
    }
}

TEST_CASE("a GDTF archive is read through its description.xml alone", "[fixtures][gdtf]") {
    const std::string xml = gdtfDocument(R"(<Geometry Name="Body"/>)",
                                         gdtfMode("m", "Body", gdtfChannel("Body", "1", "Dimmer")));
    const std::string model(200000, 'x'); // what a model file would weigh, never inflated
    for (const bool deflate : {false, true}) {
        INFO("deflated: " << deflate);
        const auto archive = takt4::test::zipOf(
            {{"thumbnail.png", "png"}, {"models/gltf/Base.glb", model}, {"description.xml", xml}},
            deflate);
        const auto result = readGdtfArchive(archive, "x.gdtf");
        INFO(result.problem);
        REQUIRE(result.definition);
        CHECK(result.definition->modes.at(0).parts.at(0).at(0).kind == Kind::Dimmer);
        CHECK(result.definition->fileName == "x.gdtf");
    }

    SECTION("an archive without one, or with one only in a folder, is refused") {
        CHECK(readGdtfArchive(takt4::test::zipOf({{"models/description.xml", xml}}), "x.gdtf")
                  .problem == "there is no \"description.xml\" inside it, so it isn't a GDTF file");
        CHECK(readGdtfArchive(takt4::test::zipOf({{"Description.XML", xml}}), "x.gdtf").problem ==
              "there is no \"description.xml\" inside it, so it isn't a GDTF file");
    }
}

TEST_CASE("a damaged or hostile GDTF archive is refused with a reason, never read past its limits",
          "[fixtures][gdtf][hostile]") {
    const std::string xml = gdtfDocument(R"(<Geometry Name="Body"/>)",
                                         gdtfMode("m", "Body", gdtfChannel("Body", "1", "Dimmer")));
    const auto refuse = [](const std::vector<std::uint8_t>& archive) {
        const auto result = readGdtfArchive(archive, "x.gdtf");
        CHECK_FALSE(result.definition);
        CHECK_FALSE(result.problem.empty());
        return result.problem;
    };

    SECTION("not an archive, empty, or cut short") {
        refuse({});
        refuse(std::vector<std::uint8_t>(xml.begin(), xml.end()));
        const auto archive = takt4::test::zipOf({{"description.xml", xml}});
        for (const std::size_t keep : {std::size_t{10}, archive.size() / 2, archive.size() - 10}) {
            INFO("kept " << keep);
            refuse(std::vector<std::uint8_t>(archive.begin(),
                                             archive.begin() + static_cast<std::ptrdiff_t>(keep)));
        }
    }
    SECTION("damaged in the middle: the check sum catches it") {
        auto archive = takt4::test::zipOf({{"description.xml", xml}}, false);
        const auto local = takt4::test::headersOf(archive, takt4::test::kLocalHeader);
        REQUIRE(local.size() == 1);
        archive[local[0] + 30 + 15 + 40] ^= 0x20; // a byte of the stored XML
        CHECK_THAT(refuse(archive), ContainsSubstring("description.xml"));
    }
    SECTION("encrypted") {
        auto archive = takt4::test::zipOf({{"description.xml", xml}});
        for (const std::size_t at : takt4::test::headersOf(archive, takt4::test::kCentralHeader)) {
            archive[at + 8] |= 0x01; // general purpose flag: encrypted
        }
        for (const std::size_t at : takt4::test::headersOf(archive, takt4::test::kLocalHeader)) {
            archive[at + 6] |= 0x01;
        }
        CHECK(refuse(archive) == "it is password-protected");
    }
    SECTION("compressed with a method takt4 cannot read") {
        auto archive = takt4::test::zipOf({{"description.xml", xml}});
        for (const std::size_t at : takt4::test::headersOf(archive, takt4::test::kCentralHeader)) {
            archive[at + 10] = 14; // LZMA
        }
        for (const std::size_t at : takt4::test::headersOf(archive, takt4::test::kLocalHeader)) {
            archive[at + 8] = 14;
        }
        CHECK(refuse(archive) == "it is packed in a way takt4 can't unpack");
    }
    SECTION("larger than takt4 reads, said by the archive") {
        // 40 MB of spaces deflates to a few dozen kilobytes.
        const std::string huge = xml + std::string(40u * 1024u * 1024u, ' ');
        const auto archive = takt4::test::zipOf({{"description.xml", huge}});
        CHECK(archive.size() < 1024u * 1024u);
        CHECK_THAT(refuse(archive), ContainsSubstring("takt4 reads up to 32 MB"));
    }
    SECTION("larger than its archive says: a zip bomb stops at the size it claimed") {
        const std::string huge = xml + std::string(40u * 1024u * 1024u, ' ');
        auto archive = takt4::test::zipOf({{"description.xml", huge}});
        // Claim 1 KB uncompressed, in both headers.
        const std::uint32_t claim = 1024;
        for (const std::size_t at : takt4::test::headersOf(archive, takt4::test::kCentralHeader)) {
            std::memcpy(archive.data() + at + 24, &claim, 4);
        }
        for (const std::size_t at : takt4::test::headersOf(archive, takt4::test::kLocalHeader)) {
            std::memcpy(archive.data() + at + 22, &claim, 4);
        }
        const auto start = std::chrono::steady_clock::now();
        CHECK_THAT(refuse(archive), ContainsSubstring("description.xml"));
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
    }
}

TEST_CASE("hostile GDTF XML is read or refused without crashing", "[fixtures][gdtf][hostile]") {
    SECTION("geometries nested a hundred thousand deep") {
        std::string deep;
        for (int i = 0; i < 100000; ++i) {
            deep += "<Geometry Name=\"g" + std::to_string(i) + "\">";
        }
        for (int i = 0; i < 100000; ++i) {
            deep += "</Geometry>";
        }
        const std::string xml =
            gdtfDocument(deep, gdtfMode("m", "g0", gdtfChannel("g99999", "1", "Dimmer")));
        const auto result = readGdtfDescription(xml, "deep.gdtf");
        REQUIRE(result.definition);
        CHECK(result.definition->modes.at(0).parts.at(0).at(0).kind == Kind::Dimmer);
    }
    SECTION("an entity that would expand to a billion is not expanded") {
        std::string laughs = "<?xml version=\"1.0\"?>\n<!DOCTYPE GDTF [\n<!ENTITY lol \"lol\">\n";
        for (int i = 1; i <= 9; ++i) {
            laughs += "<!ENTITY lol" + std::to_string(i) + " \"";
            for (int j = 0; j < 10; ++j) {
                laughs += "&lol" + (i == 1 ? std::string() : std::to_string(i - 1)) + ";";
            }
            laughs += "\">\n";
        }
        laughs +=
            "]>\n<GDTF DataVersion=\"1.2\"><FixtureType Name=\"&lol9;\" Manufacturer=\"m\" "
            "FixtureTypeID=\"1\"><Geometries><Geometry Name=\"Body\"/></Geometries><DMXModes>" +
            gdtfMode("m", "Body", gdtfChannel("Body", "1", "Dimmer")) +
            "</DMXModes></FixtureType></GDTF>";
        const auto start = std::chrono::steady_clock::now();
        const auto result = readGdtfDescription(laughs, "laughs.gdtf");
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
        REQUIRE(result.definition);
        CHECK(result.definition->model.size() <= 80);
    }
    SECTION("ten thousand modes and channels with names nobody would write") {
        std::string modes;
        for (int i = 0; i < 2000; ++i) {
            modes +=
                gdtfMode(std::string(300, 'M'), "Body",
                         gdtfChannel("Body", std::to_string(1 + i % 500), std::string(500, 'A')));
        }
        const auto result =
            readGdtfDescription(gdtfDocument(R"(<Geometry Name="Body"/>)", modes), "x");
        REQUIRE(result.definition);
        CHECK(result.definition->modes.size() == 2000);
        CHECK(result.definition->modes.back().name.size() <= 80 + 8);
        CHECK(result.definition->modes[0].parts.at(0).at(0).label.size() <= 80);
    }
}
