#include "core/dmx/fixture.hpp"
#include "core/fixtures/fixture_library.hpp"
#include "core/fixtures/profile_json.hpp"
#include "core/fixtures/profile_mapper.hpp"
#include "core/settings/settings.hpp"

#include "fixtures/fixture_files.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::dmx::Fixture;
using takt4::dmx::Role;
using takt4::fixtures::Arrival;
using takt4::fixtures::FixtureProfile;
using takt4::fixtures::ProfileChannel;
using takt4::fixtures::ProfileMode;

namespace {

ProfileChannel ch(Role role, std::string label, std::uint8_t parked = 0) {
    return ProfileChannel{role, std::move(label), parked, {}};
}

ProfileMode mode(std::string name, std::vector<std::vector<ProfileChannel>> parts) {
    ProfileMode out;
    out.name = std::move(name);
    out.parts = std::move(parts);
    return out;
}

/// A definition of three modes: "A" (dimmer, red, green), "B" (two channels) and "Pair" (two
/// start addresses, three channels and two).
FixtureProfile definition(const std::string& revision = "r1") {
    FixtureProfile profile;
    profile.format = "gdtf";
    profile.key = "AAAA-BBBB";
    profile.manufacturer = "Maker";
    profile.model = "Wash";
    profile.revision = revision;
    profile.file = "Maker@Wash.gdtf";
    profile.modes.push_back(
        mode("A", {{ch(Role::Dimmer, "dimmer"), ch(Role::Red, "red"), ch(Role::Green, "green")}}));
    profile.modes.push_back(
        mode("B", {{ch(Role::Pan, "pan", 128), ch(Role::Strobe, "shutter 1", 255)}}));
    profile.modes.push_back(
        mode("Pair", {{ch(Role::Dimmer, "dimmer"), ch(Role::Red, "red · Cell 1"),
                       ch(Role::Blue, "blue · Cell 1")},
                      {ch(Role::Red, "red · Cell 2"), ch(Role::Blue, "blue · Cell 2")}}));
    ProfileMode refused;
    refused.name = "Broken";
    refused.refused = "it needs 3 start addresses; takt4 handles 2 at most";
    profile.modes.push_back(refused);
    return profile;
}

std::vector<std::string> namesOf(const std::vector<Fixture>& fixtures) {
    std::vector<std::string> out;
    for (const Fixture& fixture : fixtures) {
        out.push_back(fixture.name);
    }
    return out;
}

} // namespace

TEST_CASE("a definition's digest is the same for the same content and changes with any of it",
          "[fixtures][library]") {
    const FixtureProfile one =
        takt4::fixtures::mapDefinition(takt4::test::readOfl("ayrton/diablo-s.json"));
    const FixtureProfile two =
        takt4::fixtures::mapDefinition(takt4::test::readOfl("ayrton/diablo-s.json"));
    const std::string digest = takt4::fixtures::digestOf(one);
    CHECK(digest.size() == 16);
    CHECK(digest == takt4::fixtures::digestOf(two));

    // Not the id, the file name, the notes or the digest itself.
    FixtureProfile renamed = one;
    renamed.id = "p-12345678";
    renamed.file = "elsewhere.json";
    renamed.notes.push_back("a note");
    renamed.digest = "nonsense";
    CHECK(takt4::fixtures::digestOf(renamed) == digest);

    // Any channel's role, label, parked level or note; a mode's name; the revision.
    for (int change = 0; change < 6; ++change) {
        FixtureProfile changed = one;
        ProfileChannel& first = changed.modes[0].parts[0][0];
        switch (change) {
        case 0:
            first.role = Role::Unused;
            break;
        case 1:
            first.label += "!";
            break;
        case 2:
            first.parked = 1;
            break;
        case 3:
            first.note = "a note";
            break;
        case 4:
            changed.modes[0].name += " 2";
            break;
        case 5:
            changed.revision = "later";
            break;
        }
        INFO("change " << change);
        CHECK(takt4::fixtures::digestOf(changed) != digest);
    }
}

TEST_CASE("an import is added, reused, or asks replace or keep both", "[fixtures][library]") {
    std::vector<FixtureProfile> library;
    CHECK(takt4::fixtures::compareWithLibrary(library, definition()).arrival == Arrival::New);
    const std::size_t index = takt4::fixtures::addProfile(library, definition());
    const std::string id = library[index].id;
    CHECK(id.starts_with("p-"));
    CHECK(id.size() == 10);
    CHECK(library[index].digest == takt4::fixtures::digestOf(definition()));

    const auto same = takt4::fixtures::compareWithLibrary(library, definition());
    CHECK(same.arrival == Arrival::Same);
    CHECK(same.index == 0);

    const auto changed = takt4::fixtures::compareWithLibrary(library, definition("r2"));
    CHECK(changed.arrival == Arrival::Changed);
    CHECK(changed.index == 0);

    SECTION("keep both: a second entry, shown with its revision") {
        const std::size_t second = takt4::fixtures::addProfile(library, definition("r2"));
        CHECK(library.size() == 2);
        CHECK(library[second].id != id);
        CHECK(takt4::fixtures::displayName(library, library[0]) == "Wash (r1)");
        CHECK(takt4::fixtures::displayName(library, library[1]) == "Wash (r2)");
        // Either one, imported again, is the same as itself.
        CHECK(takt4::fixtures::compareWithLibrary(library, definition("r2")).arrival ==
              Arrival::Same);
        CHECK(takt4::fixtures::compareWithLibrary(library, definition("r2")).index == 1);
    }
    SECTION("a single entry is shown by its model alone") {
        CHECK(takt4::fixtures::displayName(library, library[0]) == "Wash");
    }
    SECTION("another fixture type of the same key in another format is new") {
        FixtureProfile ofl = definition();
        ofl.format = "ofl";
        CHECK(takt4::fixtures::compareWithLibrary(library, ofl).arrival == Arrival::New);
    }
}

TEST_CASE("import makes named, grouped fixtures laid end to end", "[fixtures][library]") {
    std::vector<FixtureProfile> library;
    takt4::fixtures::addProfile(library, definition());
    const FixtureProfile& profile = library[0];
    std::vector<Fixture> patch;
    patch.push_back(takt4::dmx::fixtureFromMode("Wash 1", 1, 0, 100));
    patch.push_back(takt4::dmx::fixtureFromMode("Wash 3", 1, 0, 110));
    takt4::dmx::ensureFixtureIds(patch);

    const auto made = takt4::fixtures::makeFixtures(profile, "A", 3, 2, 10, patch);
    REQUIRE(made.problem.empty());
    // The smallest numbers nobody has: 2, 4, 5.
    CHECK(namesOf(made.fixtures) == std::vector<std::string>{"Wash 2", "Wash 4", "Wash 5"});
    CHECK(made.firstChannel == 10);
    CHECK(made.lastChannel == 18);
    std::set<std::string> ids;
    for (std::size_t i = 0; i < made.fixtures.size(); ++i) {
        const Fixture& fixture = made.fixtures[i];
        CHECK(fixture.group == "Wash");
        CHECK(fixture.universe == 2);
        CHECK(fixture.address == static_cast<std::uint16_t>(10 + 3 * i));
        CHECK(fixture.channels == std::vector<Role>{Role::Dimmer, Role::Red, Role::Green});
        CHECK(fixture.labels == std::vector<std::string>{"dimmer", "red", "green"});
        CHECK(fixture.parked == std::vector<std::uint8_t>{0, 0, 0});
        CHECK(fixture.profile.id == profile.id);
        CHECK(fixture.profile.mode == "A");
        CHECK(fixture.profile.part == 1);
        CHECK(fixture.profile.pair.empty());
        CHECK(takt4::fixtures::findProfile(library, fixture.profile.id) == &profile);
        CHECK_FALSE(takt4::fixtures::isEdited(fixture, profile));
        CHECK(takt4::dmx::problemWith(fixture).empty());
        CHECK(takt4::dmx::findFixture(patch, fixture.id) == nullptr);
        ids.insert(fixture.id);
    }
    CHECK(ids.size() == 3);

    SECTION("a block that would run past 512 makes nothing, and says why") {
        const auto refused = takt4::fixtures::makeFixtures(profile, "A", 3, 0, 505, patch);
        CHECK(refused.fixtures.empty());
        CHECK(refused.problem == "runs past 512: 9 channels from 505 — start at 504 or lower");
        // And a block no start address can hold says to import fewer, not where to start.
        CHECK(takt4::fixtures::makeFixtures(profile, "A", 171, 0, 1, patch).problem ==
              "runs past 512: 513 channels are more than one universe holds — import fewer");
        CHECK(takt4::fixtures::makeFixtures(profile, "A", 2, 0, 507, patch).problem.empty());
    }
    SECTION("a refused mode, a mode that is not there, none of them, or nowhere to put them") {
        CHECK(takt4::fixtures::makeFixtures(profile, "Broken", 1, 0, 1, patch).problem ==
              "it needs 3 start addresses; takt4 handles 2 at most");
        CHECK(takt4::fixtures::makeFixtures(profile, "Nope", 1, 0, 1, patch).problem ==
              "pick a mode");
        CHECK(takt4::fixtures::makeFixtures(profile, "A", 0, 0, 1, patch).problem ==
              "how many must be at least 1");
        CHECK(takt4::fixtures::makeFixtures(profile, "A", 1, 0, 0, patch).problem ==
              "the start address must be 1 to 512");
    }
}

TEST_CASE("a two-address mode makes two linked fixtures per copy, part 2 after part 1",
          "[fixtures][library]") {
    std::vector<FixtureProfile> library;
    takt4::fixtures::addProfile(library, definition());
    const FixtureProfile& profile = library[0];
    const auto made = takt4::fixtures::makeFixtures(profile, "Pair", 2, 0, 1, {});
    REQUIRE(made.problem.empty());
    CHECK(namesOf(made.fixtures) ==
          std::vector<std::string>{"Wash 1", "Wash 1 · part 2", "Wash 2", "Wash 2 · part 2"});
    CHECK(made.fixtures[0].address == 1);
    CHECK(made.fixtures[1].address == 4);
    CHECK(made.fixtures[2].address == 6);
    CHECK(made.fixtures[3].address == 9);
    CHECK(made.lastChannel == 10);
    CHECK(made.fixtures[0].profile.part == 1);
    CHECK(made.fixtures[1].profile.part == 2);
    CHECK(made.fixtures[1].channels == std::vector<Role>{Role::Red, Role::Blue});
    CHECK_FALSE(made.fixtures[0].profile.pair.empty());
    CHECK(made.fixtures[0].profile.pair == made.fixtures[1].profile.pair);
    CHECK(made.fixtures[2].profile.pair == made.fixtures[3].profile.pair);
    CHECK(made.fixtures[0].profile.pair != made.fixtures[2].profile.pair);
    CHECK(made.fixtures[1].group == "Wash");

    // One of a pair is offered the two-address modes only; a single fixture the others.
    CHECK(takt4::fixtures::modesFor(made.fixtures[1], profile) == std::vector<std::string>{"Pair"});
    const auto single = takt4::fixtures::makeFixtures(profile, "A", 1, 0, 20, made.fixtures);
    CHECK(takt4::fixtures::modesFor(single.fixtures.at(0), profile) ==
          std::vector<std::string>{"A", "B"});
}

TEST_CASE("a linked fixture's mode changes from its definition, and an edit is shown, not stored",
          "[fixtures][library]") {
    std::vector<FixtureProfile> library;
    takt4::fixtures::addProfile(library, definition());
    const FixtureProfile& profile = library[0];
    std::vector<Fixture> patch = takt4::fixtures::makeFixtures(profile, "A", 1, 0, 1, {}).fixtures;
    REQUIRE(patch.size() == 1);

    SECTION("a parked level or a label edited is not an edit of the map") {
        patch[0].parked[0] = 99;
        patch[0].labels[1] = "mine";
        CHECK_FALSE(takt4::fixtures::isEdited(patch[0], profile));
    }
    SECTION("a role or a channel count edited is, and re-picking the mode puts it back") {
        patch[0].channels[1] = Role::Amber;
        CHECK(takt4::fixtures::isEdited(patch[0], profile));
        patch[0].channels.push_back(Role::Unused);
        CHECK(takt4::fixtures::isEdited(patch[0], profile));
        REQUIRE(takt4::fixtures::remode(patch, 0, profile, "A"));
        CHECK_FALSE(takt4::fixtures::isEdited(patch[0], profile));
        CHECK(patch[0].channels.size() == 3);
    }
    SECTION("to another mode of its shape, and not to one of the other shape") {
        REQUIRE(takt4::fixtures::remode(patch, 0, profile, "B"));
        CHECK(patch[0].channels == std::vector<Role>{Role::Pan, Role::Strobe});
        CHECK(patch[0].parked == std::vector<std::uint8_t>{128, 255});
        CHECK(patch[0].labels == std::vector<std::string>{"pan", "shutter 1"});
        CHECK(patch[0].profile.mode == "B");
        CHECK_FALSE(takt4::fixtures::remode(patch, 0, profile, "Pair"));
        CHECK_FALSE(takt4::fixtures::remode(patch, 0, profile, "Broken"));
        CHECK(patch[0].profile.mode == "B");
    }
    SECTION("one of a pair takes its partner with it, and goes alone once the partner is gone") {
        std::vector<Fixture> pairs =
            takt4::fixtures::makeFixtures(profile, "Pair", 1, 0, 1, {}).fixtures;
        FixtureProfile wider = definition();
        wider.modes.push_back(
            mode("Pair 2", {{ch(Role::Dimmer, "d")},
                            {ch(Role::Green, "g"), ch(Role::Green, "g2"), ch(Role::Green, "g3")}}));
        std::vector<FixtureProfile> both{wider};
        both[0].id = profile.id;
        REQUIRE(takt4::fixtures::remode(pairs, 1, both[0], "Pair 2"));
        CHECK(pairs[0].channels == std::vector<Role>{Role::Dimmer});
        CHECK(pairs[1].channels == std::vector<Role>{Role::Green, Role::Green, Role::Green});
        CHECK(pairs[0].profile.mode == "Pair 2");
        pairs.erase(pairs.begin());
        REQUIRE(takt4::fixtures::remode(pairs, 0, both[0], "Pair"));
        CHECK(pairs[0].channels == std::vector<Role>{Role::Red, Role::Blue});
        CHECK(pairs[0].profile.part == 2);
    }
}

TEST_CASE("replacing a definition re-maps its fixtures and keeps everything the operator set",
          "[fixtures][library]") {
    std::vector<FixtureProfile> library;
    takt4::fixtures::addProfile(library, definition());
    const std::string id = library[0].id;
    std::vector<Fixture> patch =
        takt4::fixtures::makeFixtures(library[0], "A", 2, 0, 1, {}).fixtures;
    std::vector<Fixture> more =
        takt4::fixtures::makeFixtures(library[0], "B", 1, 0, 50, patch).fixtures;
    patch.insert(patch.end(), more.begin(), more.end());
    patch.push_back(takt4::dmx::fixtureFromMode("by hand", 1, 0, 200));
    takt4::dmx::ensureFixtureIds(patch);
    patch[0].name = "Stage left";
    patch[0].group = "front";
    patch[0].universe = 3;
    patch[0].address = 33;
    patch[0].panMin = 0.2;
    patch[0].enabled = false;
    const Fixture handMade = patch[3];

    // The new revision: mode A gains a blue; mode B is gone.
    FixtureProfile next = definition("r2");
    next.modes[0].parts[0].push_back(ch(Role::Blue, "blue", 0));
    next.modes[0].parts[0][0].label = "intensity";
    next.modes.erase(next.modes.begin() + 1);
    const std::vector<std::string> unlinked =
        takt4::fixtures::replaceProfile(library, 0, next, patch);

    CHECK(library.size() == 1);
    CHECK(library[0].id == id);
    CHECK(library[0].revision == "r2");
    CHECK(library[0].digest == takt4::fixtures::digestOf(library[0]));
    // Re-mapped, everything else kept.
    CHECK(patch[0].channels == std::vector<Role>{Role::Dimmer, Role::Red, Role::Green, Role::Blue});
    CHECK(patch[0].labels.front() == "intensity");
    CHECK(patch[0].parked.size() == 4);
    CHECK(patch[0].name == "Stage left");
    CHECK(patch[0].group == "front");
    CHECK(patch[0].universe == 3);
    CHECK(patch[0].address == 33);
    CHECK(patch[0].panMin == 0.2);
    CHECK_FALSE(patch[0].enabled);
    CHECK(patch[0].profile.id == id);
    CHECK(patch[1].channels.size() == 4);
    // The fixture on the mode that went keeps its map and loses its link.
    CHECK(unlinked == std::vector<std::string>{"Wash 3"});
    CHECK(patch[2].channels == std::vector<Role>{Role::Pan, Role::Strobe});
    CHECK_FALSE(patch[2].profile.linked());
    // A fixture patched by hand is not touched.
    CHECK(patch[3] == handMade);

    SECTION("an entry stays while a fixture uses it, and goes when asked once none does") {
        CHECK(takt4::fixtures::usersOf(patch, id) == 2);
        CHECK_FALSE(takt4::fixtures::removeProfile(library, id, patch));
        CHECK(library.size() == 1);
        patch.erase(patch.begin(), patch.begin() + 2);
        CHECK(takt4::fixtures::usersOf(patch, id) == 0);
        CHECK(takt4::fixtures::removeProfile(library, id, patch));
        CHECK(library.empty());
        CHECK_FALSE(takt4::fixtures::removeProfile(library, id, patch));
    }
}

TEST_CASE("the library and the fixtures' labels and links survive a save and a load",
          "[fixtures][library][settings]") {
    takt4::settings::Settings settings;
    takt4::fixtures::addProfile(
        settings.preset.library,
        takt4::fixtures::mapDefinition(takt4::test::readOfl("ayrton/diablo-s.json")));
    takt4::fixtures::addProfile(settings.preset.library, definition());
    const FixtureProfile& diablo = settings.preset.library[0];
    settings.preset.fixtures =
        takt4::fixtures::makeFixtures(diablo, "Standard", 2, 1, 1, {}).fixtures;
    const auto pair = takt4::fixtures::makeFixtures(settings.preset.library[1], "Pair", 1, 1, 100,
                                                    settings.preset.fixtures);
    settings.preset.fixtures.insert(settings.preset.fixtures.end(), pair.fixtures.begin(),
                                    pair.fixtures.end());
    settings.preset.fixtures.push_back(takt4::dmx::fixtureFromMode("by hand", 1, 0, 300));
    takt4::settings::assignIds(settings.preset);

    const std::string text = takt4::settings::toJson(settings);
    CHECK_THAT(text, ContainsSubstring("\"fixtureLibrary\""));
    const takt4::settings::Settings back = takt4::settings::fromJson(text);
    CHECK(back.preset.library == settings.preset.library);
    CHECK(back.preset.fixtures == settings.preset.fixtures);
    CHECK(back.preset.fixtures.back().labels.empty());
    CHECK_FALSE(back.preset.fixtures.back().profile.linked());
    CHECK(back.preset.fixtures[2].profile.pair == back.preset.fixtures[3].profile.pair);
    // And once more, unchanged: the format is stable once read (a load adds the Link output row).
    const std::string again = takt4::settings::toJson(back);
    CHECK(takt4::settings::toJson(takt4::settings::fromJson(again)) == again);

    SECTION("a patched-by-hand file writes nothing new") {
        takt4::settings::Settings plain;
        plain.preset.fixtures.push_back(takt4::dmx::fixtureFromMode("par", 1, 0, 1));
        takt4::settings::assignIds(plain.preset);
        const std::string written = takt4::settings::toJson(plain);
        CHECK_THAT(written, !ContainsSubstring("fixtureLibrary"));
        CHECK_THAT(written, !ContainsSubstring("\"labels\""));
        CHECK_THAT(written, !ContainsSubstring("\"profile\""));
    }
    SECTION("a link to an entry the file does not have is no link") {
        takt4::settings::Settings broken = settings;
        broken.preset.library.erase(broken.preset.library.begin());
        const takt4::settings::Settings loaded =
            takt4::settings::fromJson(takt4::settings::toJson(broken));
        CHECK_FALSE(loaded.preset.fixtures[0].profile.linked());
        CHECK(loaded.preset.fixtures[0].channels == settings.preset.fixtures[0].channels);
        CHECK(loaded.preset.fixtures[2].profile.linked());
    }
    SECTION("a damaged library entry is left out, and its fixtures with it unlinked") {
        std::string damaged = text;
        const std::size_t at = damaged.find("\"format\": \"ofl\"");
        REQUIRE(at != std::string::npos);
        damaged.replace(at, std::string("\"format\": \"ofl\"").size(), "\"format\": 7");
        const takt4::settings::Settings loaded = takt4::settings::fromJson(damaged);
        CHECK(loaded.preset.library.size() == 1);
        CHECK_FALSE(loaded.preset.fixtures[0].profile.linked());
        CHECK(loaded.preset.fixtures[2].profile.linked());
    }
}

TEST_CASE("a library read back is never trusted for more than it says",
          "[fixtures][library][hostile]") {
    CHECK(takt4::fixtures::libraryFromJson("").empty());
    CHECK(takt4::fixtures::libraryFromJson("not json").empty());
    CHECK(takt4::fixtures::libraryFromJson("{}").empty());
    const auto library = takt4::fixtures::libraryFromJson(R"([
        {"id": "p-1", "format": "gdtf", "key": "K", "model": "M", "modes": [
            {"name": "ok", "parts": [[["dimmer", "d", 300], ["nonsense", 5], 7, ["red", "r", -4, "note"]]]},
            {"name": "three parts", "parts": [[["red", "r", 0]], [["red", "r", 0]], [["red", "r", 0]]]},
            {"name": "empty", "parts": []}]},
        {"id": "p-1", "format": "ofl", "key": "a/b", "modes": []},
        {"format": "", "key": "x"},
        5])");
    REQUIRE(library.size() == 2);
    const ProfileMode& ok = library[0].modes[0];
    REQUIRE(ok.importable());
    REQUIRE(ok.parts[0].size() == 4);
    CHECK(ok.parts[0][0].parked == 255);
    CHECK(ok.parts[0][1].role == Role::Unused); // a role it cannot read is a channel all the same
    CHECK(ok.parts[0][2].role == Role::Unused);
    CHECK(ok.parts[0][3].parked == 0);
    CHECK(ok.parts[0][3].note == "note");
    CHECK_FALSE(library[0].modes[1].importable());
    CHECK_FALSE(library[0].modes[2].importable());
    // Two entries cannot share an id.
    CHECK(library[1].id != library[0].id);
    CHECK(library[1].id.starts_with("p-"));
}

TEST_CASE("a mode says what takt4 drives in it, in the import sheet's words",
          "[fixtures][library]") {
    using takt4::fixtures::drivesOf;
    // The Rayzor 760's "Pixels" mode as the mockup the operator approved shows it (2026-10-05):
    // two dimmers, seven RGBW cells, a 16-bit head, two shutters, a wheel, a zoom, a speed.
    std::vector<ProfileChannel> pixels;
    pixels.push_back(ch(Role::Pan, "pan"));
    pixels.push_back(ch(Role::PanFine, "pan fine"));
    pixels.push_back(ch(Role::Tilt, "tilt"));
    pixels.push_back(ch(Role::TiltFine, "tilt fine"));
    pixels.push_back(ch(Role::Speed, "pan/tilt speed"));
    pixels.push_back(ch(Role::Dimmer, "dimmer"));
    pixels.push_back(ch(Role::Dimmer, "dimmer · Ring"));
    pixels.push_back(ch(Role::Strobe, "shutter 1"));
    pixels.push_back(ch(Role::Strobe, "shutter 1 · Ring"));
    for (int cell = 1; cell <= 7; ++cell) {
        for (const Role role : {Role::Red, Role::Green, Role::Blue, Role::White}) {
            pixels.push_back(ch(role, "cell " + std::to_string(cell)));
        }
    }
    pixels.push_back(ch(Role::ColorWheel, "color wheel 1"));
    pixels.push_back(ch(Role::Zoom, "zoom"));
    pixels.push_back(ch(Role::Unused, "prism 1"));
    CHECK(drivesOf(mode("Pixels", {pixels})) ==
          "dimmer ×2 · RGBW ×7 · pan/tilt 16-bit · strobe ×2 · color wheel · zoom · speed");
    CHECK(takt4::fixtures::unusedIn(mode("Pixels", {pixels})) == 1);
    CHECK(takt4::fixtures::channelCountOf(mode("Pixels", {pixels})) == "40");

    SECTION("one word for a family every cell has; the rest by name") {
        CHECK(drivesOf(mode(
                  "par", {{ch(Role::Red, ""), ch(Role::Green, ""), ch(Role::Blue, "")}})) == "RGB");
        CHECK(drivesOf(mode("a", {{ch(Role::Red, ""), ch(Role::Green, ""), ch(Role::Blue, ""),
                                   ch(Role::White, ""), ch(Role::Amber, ""), ch(Role::Uv, "")}})) ==
              "RGBWA+UV");
        // Two cells of RGB and one white: the white is a word of its own.
        CHECK(drivesOf(mode("b", {{ch(Role::Red, ""), ch(Role::Green, ""), ch(Role::Blue, ""),
                                   ch(Role::Red, ""), ch(Role::Green, ""), ch(Role::Blue, ""),
                                   ch(Role::White, "")}})) == "RGB ×2 · white");
        CHECK(drivesOf(mode("c", {{ch(Role::Red, ""), ch(Role::Blue, "")}})) == "red · blue");
        CHECK(drivesOf(mode("d", {{ch(Role::Cyan, ""), ch(Role::Magenta, ""), ch(Role::Yellow, ""),
                                   ch(Role::Dimmer, "")}})) == "dimmer · CMY");
    }
    SECTION("movement counts heads, and says a head that only tilts") {
        CHECK(drivesOf(mode("e", {{ch(Role::Tilt, ""), ch(Role::Tilt, "")}})) == "tilt ×2");
        CHECK(drivesOf(mode("f", {{ch(Role::Pan, ""), ch(Role::Tilt, ""), ch(Role::Pan, ""),
                                   ch(Role::Tilt, "")}})) == "pan/tilt ×2");
    }
    SECTION("every part of a two-address mode, counted together") {
        const ProfileMode pair =
            mode("Pair", {{ch(Role::Dimmer, ""), ch(Role::Unused, "")}, {ch(Role::Dimmer, "")}});
        CHECK(drivesOf(pair) == "dimmer ×2");
        CHECK(takt4::fixtures::channelCountOf(pair) == "2 + 1");
        CHECK(takt4::fixtures::unusedIn(pair) == 1);
    }
    SECTION("a mode where nothing has a role says so") {
        CHECK(drivesOf(mode("g", {{ch(Role::Unused, ""), ch(Role::Unused, "")}})) ==
              "nothing takt4 drives");
    }
}

TEST_CASE("the library lists its entries by maker and model, ignoring case",
          "[fixtures][library]") {
    std::vector<FixtureProfile> library;
    for (const auto& [maker, model, revision] :
         {std::tuple{"elation", "Rayzor 760", "b"}, std::tuple{"Ayrton", "Diablo S", ""},
          std::tuple{"Elation", "Fuze", ""}, std::tuple{"elation", "Rayzor 760", "a"}}) {
        FixtureProfile entry;
        entry.manufacturer = maker;
        entry.model = model;
        entry.revision = revision;
        library.push_back(entry);
    }
    CHECK(takt4::fixtures::libraryOrder(library) == std::vector<std::size_t>{1, 2, 3, 0});
    CHECK(takt4::fixtures::libraryOrder({}).empty());
}
