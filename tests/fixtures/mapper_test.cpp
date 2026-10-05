#include "core/dmx/fixture.hpp"
#include "core/fixtures/definition.hpp"
#include "core/fixtures/profile_mapper.hpp"

#include "fixtures/fixture_files.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::dmx::Role;
using takt4::fixtures::DefChannel;
using takt4::fixtures::Definition;
using takt4::fixtures::DefMode;
using takt4::fixtures::FixtureProfile;
using takt4::fixtures::Kind;
using takt4::fixtures::mapDefinition;
using takt4::fixtures::ProfileChannel;
using takt4::fixtures::roleFor;

namespace {

DefChannel channel(Kind kind, std::uint8_t byte = 0, std::uint8_t bytes = 1) {
    DefChannel out;
    out.kind = kind;
    out.byte = byte;
    out.resolutionBytes = bytes;
    out.label = "label";
    return out;
}

/// One mode of the given channels, mapped.
std::vector<ProfileChannel> mapped(std::vector<DefChannel> channels) {
    Definition definition;
    definition.format = "gdtf";
    definition.key = "KEY";
    DefMode mode;
    mode.name = "m";
    mode.parts.push_back(std::move(channels));
    definition.modes.push_back(std::move(mode));
    const FixtureProfile profile = mapDefinition(definition);
    REQUIRE(profile.modes.size() == 1);
    REQUIRE(profile.modes[0].parts.size() == 1);
    return profile.modes[0].parts[0];
}

} // namespace

TEST_CASE("each kind is driven as its role, and the rest are unused", "[fixtures][mapper]") {
    const auto role = [](Kind kind) { return roleFor(kind, 0, false, 0, 0); };
    CHECK(role(Kind::Dimmer) == Role::Dimmer);
    CHECK(role(Kind::Red) == Role::Red);
    CHECK(role(Kind::IndirectRed) == Role::Red);
    CHECK(role(Kind::IndirectGreen) == Role::Green);
    CHECK(role(Kind::IndirectBlue) == Role::Blue);
    CHECK(role(Kind::White) == Role::White);
    CHECK(role(Kind::WarmWhite) == Role::White);
    CHECK(role(Kind::CoolWhite) == Role::White);
    CHECK(role(Kind::Amber) == Role::Amber);
    CHECK(role(Kind::Uv) == Role::Uv);
    CHECK(role(Kind::CyanSub) == Role::Cyan);
    CHECK(role(Kind::MagentaSub) == Role::Magenta);
    CHECK(role(Kind::YellowSub) == Role::Yellow);
    CHECK(role(Kind::Shutter) == Role::Strobe);
    CHECK(role(Kind::PanTiltSpeed) == Role::Speed);
    CHECK(role(Kind::Pan) == Role::Pan);
    CHECK(role(Kind::Tilt) == Role::Tilt);
    for (const Kind kind : {Kind::Nothing, Kind::OtherEmitter, Kind::HsbOrCie,
                            Kind::ColorBrightness, Kind::StrobeRate, Kind::Other}) {
        INFO(takt4::fixtures::nameOf(kind));
        CHECK(role(kind) == Role::Unused);
    }
    // The fine bytes: pan and tilt's are driven, anything else's are not.
    CHECK(roleFor(Kind::Pan, 1, false, 0, 0) == Role::PanFine);
    CHECK(roleFor(Kind::Tilt, 1, false, 0, 0) == Role::TiltFine);
    CHECK(roleFor(Kind::Pan, 2, false, 0, 0) == Role::Unused);
    CHECK(roleFor(Kind::Dimmer, 1, false, 0, 0) == Role::Unused);
    // A shared range is never driven.
    CHECK(roleFor(Kind::Dimmer, 0, true, 0, 0) == Role::Unused);
    CHECK(roleFor(Kind::Pan, 0, true, 0, 0) == Role::Unused);
    // The lowest wheel, zoom or focus present is the one driven — "gobo 2" when it is the only.
    CHECK(roleFor(Kind::Gobo, 0, false, 2, 2) == Role::Gobo);
    CHECK(roleFor(Kind::Gobo, 0, false, 3, 2) == Role::Unused);
    CHECK(roleFor(Kind::ColorWheel, 0, false, 1, 1) == Role::ColorWheel);
    CHECK(roleFor(Kind::Zoom, 0, false, 1, 1) == Role::Zoom);
    CHECK(roleFor(Kind::Focus, 0, false, 2, 1) == Role::Unused);
}

TEST_CASE("a mapped channel is parked by takt4's rule, whatever the file defaults it to",
          "[fixtures][mapper]") {
    std::vector<DefChannel> channels;
    // 0: a dimmer the file defaults to full — parked dark.
    channels.push_back(channel(Kind::Dimmer));
    channels.back().defaultValue = 255;
    // 1: a red the same.
    channels.push_back(channel(Kind::Red));
    channels.back().defaultValue = 255;
    // 2: a shutter with an open value — parked open.
    channels.push_back(channel(Kind::Shutter));
    channels.back().openValue = 32;
    channels.back().defaultValue = 0;
    // 3: a shutter with none — its default, and a note.
    channels.push_back(channel(Kind::Shutter));
    channels.back().defaultValue = 7;
    // 4: a shutter with neither — the initial function's start, and a note.
    channels.push_back(channel(Kind::Shutter));
    channels.back().fallbackValue = 3;
    // 5, 6: a 16-bit pan with a default.
    channels.push_back(channel(Kind::Pan, 0, 2));
    channels.back().defaultValue = 0x1234;
    channels.push_back(channel(Kind::Pan, 1, 2));
    channels.back().defaultValue = 0x1234;
    // 7, 8: a 16-bit tilt with none — centred.
    channels.push_back(channel(Kind::Tilt, 0, 2));
    channels.push_back(channel(Kind::Tilt, 1, 2));
    // 9: anything else, with no default — the stand-in.
    channels.push_back(channel(Kind::Other));
    channels.back().fallbackValue = 9;
    // 10: a CMY flag with a default — out of the beam.
    channels.push_back(channel(Kind::CyanSub));
    channels.back().defaultValue = 200;
    // 11: an HSB brightness — dark.
    channels.push_back(channel(Kind::ColorBrightness));
    channels.back().defaultValue = 255;
    // 12: an address no channel claims, which the file defaults to nothing.
    channels.push_back(channel(Kind::Nothing));
    // 13: an 8-bit pan with no default — 128.
    channels.push_back(channel(Kind::Pan));

    const std::vector<ProfileChannel> out = mapped(channels);
    CHECK(out[0].parked == 0);
    CHECK(out[1].parked == 0);
    CHECK(out[2].parked == 32);
    CHECK(out[2].note.empty());
    CHECK(out[3].parked == 7);
    CHECK_THAT(out[3].note, ContainsSubstring("doesn't say which value opens the shutter"));
    CHECK(out[4].parked == 3);
    CHECK_THAT(out[4].note, ContainsSubstring("check its parked level"));
    CHECK(out[5].parked == 0x12);
    CHECK(out[6].parked == 0x34);
    CHECK(out[5].role == Role::Pan);
    CHECK(out[6].role == Role::PanFine);
    CHECK(out[6].note.empty()); // a fine pan is driven
    CHECK(out[7].parked == 128);
    CHECK(out[8].parked == 0);
    CHECK(out[9].parked == 9);
    CHECK(out[10].parked == 0);
    CHECK(out[10].role == Role::Cyan);
    CHECK(out[11].parked == 0);
    CHECK(out[11].role == Role::Unused);
    CHECK(out[12].parked == 0);
    CHECK(out[13].parked == 128);
}

TEST_CASE("a mapped channel says what the mapping lost", "[fixtures][mapper]") {
    std::vector<DefChannel> channels;
    channels.push_back(channel(Kind::Dimmer, 0, 2));
    channels.push_back(channel(Kind::Dimmer, 1, 2));
    channels.push_back(channel(Kind::Dimmer));
    channels.back().shared = true;
    channels.back().sharedWith = "strobe";
    channels.push_back(channel(Kind::Gobo));
    channels.back().ordinal = 2;
    channels.push_back(channel(Kind::Gobo));
    channels.back().ordinal = 3;
    channels.push_back(channel(Kind::HsbOrCie));
    channels.push_back(channel(Kind::Red));
    channels.back().notes = {"the library marks this channel as needing checking: why"};
    channels.push_back(channel(Kind::Pan, 2, 3));

    const std::vector<ProfileChannel> out = mapped(channels);
    CHECK(out[0].role == Role::Dimmer);
    CHECK(out[1].role == Role::Unused);
    CHECK(out[1].note == "fine adjustment — takt4 sets the main channel only");
    CHECK(out[2].role == Role::Unused);
    CHECK(out[2].note ==
          "part of its range is strobe — given no role, so takt4 never sends it there");
    CHECK(out[3].role == Role::Gobo); // the only gobo wheels are 2 and 3: 2 is the first
    CHECK(out[3].note.empty());
    CHECK(out[4].role == Role::Unused);
    CHECK(out[4].note == "second gobo wheel — takt4 drives the first");
    CHECK(out[5].note ==
          "sets color by hue, saturation or color coordinates — takt4 can't drive that");
    CHECK(out[6].note == "the library marks this channel as needing checking: why");
    CHECK(out[7].role == Role::Unused);
    CHECK(out[7].note == "extra-fine adjustment — takt4 sets the main and fine channels only");
}

TEST_CASE("every channel of a role takes it — cells, zones, a master and its cells",
          "[fixtures][mapper]") {
    // The Crazy Pocket 8's 21-channel mode: red, green, blue and amber for the top and the bottom.
    const FixtureProfile profile =
        mapDefinition(takt4::test::readOfl("american-dj/crazy-pocket-8.json"));
    const takt4::fixtures::ProfileMode* mode = nullptr;
    for (const auto& one : profile.modes) {
        if (one.name == "21-channel") {
            mode = &one;
        }
    }
    REQUIRE(mode != nullptr);
    const std::vector<ProfileChannel>& part = mode->parts.at(0);
    std::vector<Role> roles;
    for (std::size_t i = 6; i < 14; ++i) {
        roles.push_back(part[i].role);
    }
    CHECK(roles == std::vector<Role>{Role::Red, Role::Green, Role::Blue, Role::Amber, Role::Red,
                                     Role::Green, Role::Blue, Role::Amber});
    CHECK(part[6].label == "Red Top");
    // Two tilts, one per bar, both driven with their fine bytes.
    CHECK(part[2].role == Role::Tilt);
    CHECK(part[3].role == Role::TiltFine);
    CHECK(part[4].role == Role::Tilt);
    CHECK(part[5].role == Role::TiltFine);
}

TEST_CASE("a CMY head imported from OFL is a dimmer, a shutter open and three flags out",
          "[fixtures][mapper]") {
    const FixtureProfile profile = mapDefinition(takt4::test::readOfl("ayrton/diablo-s.json"));
    REQUIRE(profile.modes.size() == 3);
    const std::vector<ProfileChannel>& part = profile.modes[0].parts.at(0);
    REQUIRE(part.size() == 36);
    const std::vector<Role> expected{Role::Pan,     Role::PanFine, Role::Tilt,       Role::TiltFine,
                                     Role::Unused,  Role::Strobe,  Role::Dimmer,     Role::Unused,
                                     Role::Unused,  Role::Unused,  Role::Zoom,       Role::Focus,
                                     Role::Unused,  Role::Unused,  Role::ColorWheel, Role::Cyan,
                                     Role::Magenta, Role::Yellow,  Role::Unused,     Role::Gobo};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO(i << ": " << part[i].label);
        CHECK(part[i].role == expected[i]);
    }
    CHECK(part[5].parked == 11); // the shutter open
    CHECK(part[6].parked == 0);  // the dimmer down
    CHECK(part[15].parked == 0); // the flags out
    CHECK(part[4].note ==
          "part of its range is maintenance — given no role, so takt4 never sends it there");
    CHECK(part[12].note == "second focus — takt4 drives the first");
    CHECK(part[34].parked == 0x7F); // the blades' "50%"
}
