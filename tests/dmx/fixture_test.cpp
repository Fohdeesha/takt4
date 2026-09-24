#include "core/dmx/color.hpp"
#include "core/dmx/fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::dmx::Color;
using takt4::dmx::Fixture;
using takt4::dmx::Role;

TEST_CASE("a fixture's roles resolve to the DMX channels printed on its back", "[dmx][fixture]") {
    // An RGB par addressed at 11 occupies 11, 12, 13 — the numbers an operator sets on the
    // fixture itself, which is why the patch is 1-based and the buffers are not.
    Fixture par = takt4::dmx::fixtureFromMode("par", 1, 0, 11);
    REQUIRE(par.channels.size() == 3);
    CHECK(takt4::dmx::channelOf(par, Role::Red) == 11);
    CHECK(takt4::dmx::channelOf(par, Role::Green) == 12);
    CHECK(takt4::dmx::channelOf(par, Role::Blue) == 13);
    CHECK(takt4::dmx::lastChannelOf(par) == 13);

    SECTION("a role the fixture has not got answers zero, and there is no channel zero") {
        CHECK(takt4::dmx::channelOf(par, Role::Pan) == 0);
        CHECK_FALSE(takt4::dmx::has(par, Role::Dimmer));
    }

    SECTION("Unused is never a channel, however many of them the map has") {
        par.channels = {Role::Unused, Role::Red, Role::Unused};
        CHECK(takt4::dmx::channelOf(par, Role::Unused) == 0);
        CHECK(takt4::dmx::channelOf(par, Role::Red) == 12);
    }

    SECTION("a channel that would fall off the end of the universe is not a channel") {
        par.address = 511;
        CHECK(takt4::dmx::channelOf(par, Role::Red) == 511);
        CHECK(takt4::dmx::channelOf(par, Role::Green) == 512);
        CHECK(takt4::dmx::channelOf(par, Role::Blue) == 0); // would be 513
    }
}

TEST_CASE("a fixture says why it cannot be driven", "[dmx][fixture]") {
    Fixture head = takt4::dmx::fixtureFromMode("head", 6, 0, 1); // moving head 16-bit
    CHECK(takt4::dmx::problemWith(head).empty());

    SECTION("a fixture with no name is one a rule cannot aim at") {
        head.name.clear();
        CHECK_THAT(takt4::dmx::problemWith(head), ContainsSubstring("no name"));
    }

    SECTION("one patched past the end of the universe says so with the numbers") {
        head.address = 505;
        CHECK_THAT(takt4::dmx::problemWith(head), ContainsSubstring("runs off the end"));
        CHECK_THAT(takt4::dmx::problemWith(head), ContainsSubstring("505"));
    }

    SECTION("a fine byte with no coarse partner is a half-edited channel map") {
        // The symptom on a rig is a head that moves in steps for no visible reason, because
        // the fine byte is simply never written. Worth naming.
        head.channels = {Role::PanFine, Role::Tilt};
        CHECK_THAT(takt4::dmx::problemWith(head), ContainsSubstring("pan fine but no pan"));
    }

    SECTION("inverted movement limits are caught before they clamp everything to nothing") {
        head.panMin = 0.8;
        head.panMax = 0.2;
        CHECK_THAT(takt4::dmx::problemWith(head), ContainsSubstring("inverted"));
    }
}

TEST_CASE("a rule aims at fixtures by id and at groups by label", "[dmx][fixture]") {
    std::vector<Fixture> patch;
    patch.push_back(takt4::dmx::fixtureFromMode("wash L", 1, 0, 1));
    patch.push_back(takt4::dmx::fixtureFromMode("wash R", 1, 0, 4));
    patch.push_back(takt4::dmx::fixtureFromMode("head 1", 6, 0, 11));
    patch[0].group = "washes";
    patch[1].group = "washes";
    patch[2].group = "heads";
    takt4::dmx::ensureFixtureIds(patch);
    const std::string washL = patch[0].id;
    const std::string head = patch[2].id;
    REQUIRE_FALSE(washL.empty());
    CHECK(washL != patch[1].id);

    CHECK(takt4::dmx::resolveFixtures(patch, {washL}) == 0b001);
    CHECK(takt4::dmx::resolveFixtures(patch, {"washes"}) == 0b011);
    CHECK(takt4::dmx::resolveFixtures(patch, {"heads", washL}) == 0b101);

    SECTION("a fixture's name reaches nothing, so renaming it moves no rule") {
        // The operator's report of 2026-09-23, and the audit's M28: a rule aimed at a fixture
        // by name stopped reaching it the moment it was renamed.
        CHECK(takt4::dmx::resolveFixtures(patch, {"wash L"}) == 0);
        patch[0].name = "front wash";
        CHECK(takt4::dmx::resolveFixtures(patch, {washL}) == 0b001);
    }

    SECTION("naming none is none — the opposite of what an empty output list means") {
        // A rule that named no output means "every output", because sending a clip change
        // twice is harmless. A rule that named no fixture must not mean "every moving head in
        // the building", so this one is empty on purpose.
        CHECK(takt4::dmx::resolveFixtures(patch, {}) == 0);
    }

    SECTION("something this rig has not got contributes nothing and is not an error") {
        CHECK(takt4::dmx::resolveFixtures(patch, {"lasers"}) == 0);
        CHECK(takt4::dmx::resolveFixtures(patch, {"lasers", head}) == 0b100);
    }

    SECTION("a fixture reached by both its id and its group is counted once") {
        CHECK(takt4::dmx::resolveFixtures(patch, {"washes", washL}) == 0b011);
    }

    SECTION("a file that aimed by name is re-pointed at the ids, groups left as labels") {
        std::vector<std::string> aims{"wash L", "heads", "lasers"};
        takt4::dmx::aimByIds(aims, patch);
        CHECK(aims == std::vector<std::string>{washL, "heads", "lasers"});
        // Run twice, nothing moves: an id is left alone.
        takt4::dmx::aimByIds(aims, patch);
        CHECK(aims == std::vector<std::string>{washL, "heads", "lasers"});
    }

    SECTION("a duplicated fixture is given an id of its own") {
        patch.push_back(patch[0]);
        takt4::dmx::ensureFixtureIds(patch);
        CHECK(patch[3].id != washL);
        CHECK(patch[0].id == washL);
    }
}

TEST_CASE("the patch's universes are what gets buffers and frames", "[dmx][fixture]") {
    std::vector<Fixture> patch;
    patch.push_back(takt4::dmx::fixtureFromMode("a", 1, 4, 1));
    patch.push_back(takt4::dmx::fixtureFromMode("b", 1, 0, 1));
    patch.push_back(takt4::dmx::fixtureFromMode("c", 1, 4, 10));

    const std::vector<takt4::dmx::PortAddress> universes = takt4::dmx::universesOf(patch);
    REQUIRE(universes.size() == 2);
    CHECK(universes[0] == 0);
    CHECK(universes[1] == 4);

    SECTION("a fixture switched off takes its universe with it when it is the only one") {
        patch[0].enabled = false;
        patch[2].enabled = false;
        const std::vector<takt4::dmx::PortAddress> left = takt4::dmx::universesOf(patch);
        REQUIRE(left.size() == 1);
        CHECK(left[0] == 0);
    }
}

TEST_CASE("two fixtures on the same channels are found", "[dmx][fixture]") {
    // The audit's M21: an RGB par at 1 and another at 3 on one universe share channel 3 — the
    // first's blue is the second's red — and they drive each other, and nothing said so.
    std::vector<Fixture> patch;
    patch.push_back(takt4::dmx::fixtureFromMode("left", 1, 0, 1));  // 1-3
    patch.push_back(takt4::dmx::fixtureFromMode("right", 1, 0, 3)); // 3-5
    patch.push_back(takt4::dmx::fixtureFromMode("next", 1, 0, 6));  // 6-8: touches, no overlap
    patch.push_back(takt4::dmx::fixtureFromMode("far", 1, 1, 1));   // another universe
    const std::vector<takt4::dmx::Overlap> found = takt4::dmx::overlappingFixtures(patch);
    REQUIRE(found.size() == 1);
    CHECK(found[0].first == 0);
    CHECK(found[0].second == 1);
    CHECK(found[0].channel == 3);

    SECTION("a fixture switched off overlaps nothing, since it sends nothing") {
        patch[1].enabled = false;
        CHECK(takt4::dmx::overlappingFixtures(patch).empty());
    }
    SECTION("one inside another is an overlap too") {
        patch.push_back(takt4::dmx::fixtureFromMode("inside", 1, 1, 1)); // same as "far"
        const auto more = takt4::dmx::overlappingFixtures(patch);
        REQUIRE(more.size() == 2);
        CHECK(more[1].first == 3);
        CHECK(more[1].second == 4);
        CHECK(more[1].channel == 1);
    }
}

TEST_CASE("every role round-trips through the name a settings file holds", "[dmx][fixture]") {
    for (const Role role : takt4::dmx::kRoles) {
        const std::string name(takt4::dmx::nameOf(role));
        const auto back = takt4::dmx::roleOf(name);
        REQUIRE(back.has_value());
        CHECK(*back == role);
        CHECK_FALSE(takt4::dmx::labelOf(role).empty());
    }
    CHECK_FALSE(takt4::dmx::roleOf("not-a-role").has_value());
}

TEST_CASE("the built-in modes are usable patches, not just channel counts", "[dmx][fixture]") {
    const auto modes = takt4::dmx::builtinModes();
    REQUIRE(!modes.empty());
    for (std::size_t i = 0; i < modes.size(); ++i) {
        const Fixture fixture = takt4::dmx::fixtureFromMode("x", i, 0, 1);
        CHECK(takt4::dmx::problemWith(fixture).empty());
        // The parked levels have to describe every channel, or a fixture's shutter is left at
        // whatever zero happens to mean on it.
        CHECK(fixture.parked.size() == fixture.channels.size());
    }

    SECTION("a moving head is parked with its shutter open and its movement at full speed") {
        // Without this a dimmer rule "does nothing" and an operator loses an evening to it.
        const Fixture head = takt4::dmx::fixtureFromMode("head", 6, 0, 1);
        const std::uint16_t strobe = takt4::dmx::channelOf(head, Role::Strobe);
        REQUIRE(strobe != 0);
        CHECK(head.parked[strobe - head.address] == 255);
        const std::uint16_t dimmer = takt4::dmx::channelOf(head, Role::Dimmer);
        REQUIRE(dimmer != 0);
        CHECK(head.parked[dimmer - head.address] == 0);
    }

    SECTION("an out-of-range mode gives a usable fixture rather than an empty one") {
        const Fixture fixture = takt4::dmx::fixtureFromMode("x", 999, 0, 1);
        CHECK_FALSE(fixture.channels.empty());
    }
}

TEST_CASE("a color round-trips through every spelling a person uses", "[dmx][color]") {
    CHECK(takt4::dmx::formatColor(Color{255, 32, 64}) == "#ff2040");

    const auto hex = takt4::dmx::parseColor("#ff2040");
    REQUIRE(hex.has_value());
    CHECK(*hex == Color{255, 32, 64});

    CHECK(takt4::dmx::parseColor("ff2040") == hex);
    CHECK(takt4::dmx::parseColor("  #FF2040 ") == hex);
    CHECK(takt4::dmx::parseColor("255, 32, 64") == hex);

    SECTION("the three-digit short form doubles each digit, as every web tool means it") {
        CHECK(takt4::dmx::parseColor("#f00") == Color{255, 0, 0});
        CHECK(takt4::dmx::parseColor("#abc") == Color{0xaa, 0xbb, 0xcc});
    }

    SECTION("anything else is refused rather than guessed") {
        CHECK_FALSE(takt4::dmx::parseColor("").has_value());
        CHECK_FALSE(takt4::dmx::parseColor("#ff20").has_value());
        CHECK_FALSE(takt4::dmx::parseColor("#gg2040").has_value());
        CHECK_FALSE(takt4::dmx::parseColor("300, 0, 0").has_value());
        CHECK_FALSE(takt4::dmx::parseColor("1, 2").has_value());
        CHECK_FALSE(takt4::dmx::parseColor("1, 2, 3, 4").has_value());
    }
}

TEST_CASE("hue, saturation and value survive the trip back", "[dmx][color]") {
    struct Sample {
        double hue;
        Color expected;
    };
    // The six primaries, which are where an off-by-one-sector error shows up.
    const Sample samples[] = {
        {0, Color{255, 0, 0}},     {60, Color{255, 255, 0}}, {120, Color{0, 255, 0}},
        {180, Color{0, 255, 255}}, {240, Color{0, 0, 255}},  {300, Color{255, 0, 255}},
    };
    for (const Sample& sample : samples) {
        CHECK(takt4::dmx::fromHsv(sample.hue, 1.0, 1.0) == sample.expected);
    }

    SECTION("a hue past 360 wraps, because a sweep runs past it by design") {
        CHECK(takt4::dmx::fromHsv(360.0, 1.0, 1.0) == Color{255, 0, 0});
        CHECK(takt4::dmx::fromHsv(480.0, 1.0, 1.0) == Color{0, 255, 0});
        CHECK(takt4::dmx::fromHsv(-120.0, 1.0, 1.0) == Color{0, 0, 255});
    }

    SECTION("a grey has no hue, and says so rather than remembering one") {
        double hue = 99.0;
        double saturation = 9.0;
        double value = 9.0;
        takt4::dmx::toHsv(Color{128, 128, 128}, hue, saturation, value);
        CHECK(hue == 0.0);
        CHECK(saturation == 0.0);
    }
}
