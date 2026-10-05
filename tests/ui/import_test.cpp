// The patch editor's import: a fixture brought in from its GDTF or Open Fixture Library file, or
// from the preset's library of definitions imported before — in the editor's place, the editor
// kept behind it (the operator's decisions of 2026-10-04 and 2026-10-05).
//
// What the readers and the library do is `tests/fixtures/`'s. These are about the window: what it
// shows of a definition, what its gestures do to the patch and the library, and that nothing typed
// in the editor it covers is lost to it.

#include "core/dmx/fixture.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/fixtures/fixture_library.hpp"
#include "core/fixtures/fixture_profile.hpp"
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/rule.hpp"
#include "ui/fixtures_controller.hpp"
#include "ui/rules_controller.hpp"
#include "ui/shot.hpp"

#include "fixtures/fixture_files.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <slint-platform.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::dmx::Fixture;
using takt4::dmx::Role;
using takt4::engine::BeatEngine;
using takt4::output::OutputRunner;
using takt4::output::Transports;
using takt4::ui::FixturesController;
namespace fs = std::filesystem;

namespace {

const takt4::model::ModelWeights& weights() {
    static const takt4::model::ModelWeights loaded =
        takt4::model::ModelWeights::fromFile(fs::path(TAKT4_WEIGHTS_DIR) / "generic.bin");
    return loaded;
}

const takt4::tracking::StateSpaceModel& stateSpace() {
    static const takt4::tracking::StateSpaceModel loaded =
        takt4::tracking::StateSpaceModel::fromFile(fs::path(TAKT4_STATESPACE_DIR) / "default.bin");
    return loaded;
}

/// An engine and a **stopped** output runner — see `fixtures_test.cpp`.
struct Rig {
    std::unique_ptr<BeatEngine> engine = std::make_unique<BeatEngine>(weights(), stateSpace());
    OutputRunner runner{*engine, Transports::Config{}};
};

/// A file of this test's own, in the temp folder, removed when it goes.
struct TempFile {
    fs::path path;
    TempFile(const std::string& name, const std::string& bytes) {
        path = fs::temp_directory_path() / ("takt4-import-test-" + name);
        std::ofstream(path, std::ios::binary)
            .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    TempFile(const std::string& name, const std::vector<std::uint8_t>& bytes)
        : TempFile(name, std::string(bytes.begin(), bytes.end())) {}
    ~TempFile() {
        std::error_code ignored;
        fs::remove(path, ignored);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

/// The committed Ayrton Diablo S (Open Fixture Library): a CMY head, three modes of 36, 34 and 56
/// channels.
fs::path diabloS() {
    return takt4::test::oflFile("ayrton/diablo-s.json");
}

/// A GDTF wash of two modes: "8ch", one start address, and "Pixel", two — the head's eight at one
/// and four RGB cells at the other.
std::vector<std::uint8_t> twoAddressWash() {
    using takt4::test::gdtfChannel;
    std::string single;
    for (const auto& [offset, attribute] :
         {std::pair{"1", "Dimmer"}, std::pair{"2", "ColorAdd_R"}, std::pair{"3", "ColorAdd_G"},
          std::pair{"4", "ColorAdd_B"}, std::pair{"5", "Shutter1"}, std::pair{"6", "Pan"},
          std::pair{"7", "Tilt"}, std::pair{"8", "Zoom"}}) {
        single += gdtfChannel("Body", offset, attribute);
    }
    // Part 2: `cells` RGB cells at their own start address.
    const auto pixels = [&single](int cells) {
        std::string mode = single;
        for (int cell = 0; cell < cells; ++cell) {
            for (int c = 0; c < 3; ++c) {
                const char* attribute = c == 0   ? "ColorAdd_R"
                                        : c == 1 ? "ColorAdd_G"
                                                 : "ColorAdd_B";
                mode += gdtfChannel("Cell" + std::to_string(cell + 1),
                                    std::to_string(cell * 3 + c + 1), attribute, R"(DMXBreak="2")");
            }
        }
        return mode;
    };
    const std::string geometries =
        R"(<Geometry Name="Body"><Geometry Name="Cell1"/><Geometry Name="Cell2"/>)"
        R"(<Geometry Name="Cell3"/><Geometry Name="Cell4"/></Geometry>)";
    const std::string xml = takt4::test::gdtfDocument(
        geometries,
        takt4::test::gdtfMode("8ch", "Body", single) +
            takt4::test::gdtfMode("Pixel", "Body", pixels(4)) +
            takt4::test::gdtfMode("Pixel 2", "Body", pixels(2)),
        "1.2",
        R"(Name="Pixel Wash" Manufacturer="takt4 tests" FixtureTypeID="7a1d0f00-2222-4333-8444-555566667777")");
    return takt4::test::zipOf({{"description.xml", xml}});
}

void settle() {
    slint::platform::update_timers_and_animations();
}

void clickAt(slint::Window& window, float x, float y) {
    const slint::LogicalPosition at({x, y});
    window.dispatch_pointer_move_event(at);
    window.dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
    window.dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
    settle();
}

void typeText(slint::Window& window, const std::string& text) {
    for (const char c : text) {
        const slint::SharedString key(std::string(1, c));
        window.dispatch_key_press_event(key);
        window.dispatch_key_release_event(key);
    }
    settle();
}

constexpr int kWidth = 1000;
constexpr int kHeight = 900;

/// The window shown at the size these tests find their controls in.
void showAt(FixturesController& patch, int height = kHeight) {
    patch.show();
    (void)takt4::tests::render(patch.window(), kWidth, height);
    patch.window().window().dispatch_window_active_changed_event(true);
    settle();
}

bool isEdge(const slint::Rgb8Pixel& p) {
    return p.r == 0xef && p.g == 0xee && p.b == 0xe9;
}
bool isField(const slint::Rgb8Pixel& p) {
    return p.r == 0x11 && p.g == 0x11 && p.b == 0x11;
}

/// Where a button's 2 px off-white edge first appears along row `y`, from `from` towards `to`
/// (either way) — or -1. Every button here is drawn with one; the sheets and bands are not.
int edgeAlong(const takt4::tests::Shot& shot, int y, int from, int to) {
    const int step = from <= to ? 1 : -1;
    for (int x = from; x != to; x += step) {
        if (isEdge(shot.at(x, y))) {
            return x;
        }
    }
    return -1;
}

/// The middle of the first stretch of `wanted` pixels at least `least` tall going down column `x`
/// from `fromY` — a box's face, or a filled button's — or -1.
template <typename Is>
int faceDown(const takt4::tests::Shot& shot, int x, int fromY, int least, Is wanted) {
    int run = 0;
    for (int y = fromY; y < shot.height; ++y) {
        if (wanted(shot.at(x, y))) {
            ++run;
        } else {
            if (run >= least) {
                return y - run / 2;
            }
            run = 0;
        }
    }
    return -1;
}

/// A shot as a 24-bit BMP — what `takt4-shot` writes — for a person to look at.
void writeBmp(const takt4::tests::Shot& shot, const fs::path& path) {
    const auto put32 = [](std::ofstream& out, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out.put(static_cast<char>((value >> (8 * i)) & 0xFF));
        }
    };
    const auto put16 = [](std::ofstream& out, std::uint16_t value) {
        out.put(static_cast<char>(value & 0xFF));
        out.put(static_cast<char>(value >> 8));
    };
    const auto w = static_cast<std::uint32_t>(shot.width);
    const auto h = static_cast<std::uint32_t>(shot.height);
    const std::uint32_t row = (w * 3 + 3) & ~3u;
    std::ofstream out(path, std::ios::binary);
    out.put('B');
    out.put('M');
    put32(out, 54 + row * h);
    put32(out, 0);
    put32(out, 54);
    put32(out, 40);
    put32(out, w);
    put32(out, h);
    put16(out, 1);
    put16(out, 24);
    put32(out, 0);
    put32(out, row * h);
    put32(out, 2835);
    put32(out, 2835);
    put32(out, 0);
    put32(out, 0);
    std::vector<char> line(row, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const slint::Rgb8Pixel p = shot.at(static_cast<int>(x), static_cast<int>(h - 1 - y));
            line[x * 3] = static_cast<char>(p.b);
            line[x * 3 + 1] = static_cast<char>(p.g);
            line[x * 3 + 2] = static_cast<char>(p.r);
        }
        out.write(line.data(), static_cast<std::streamsize>(row));
    }
}

} // namespace

TEST_CASE("the import's sheets, rendered for a person to look at", "[.][import-shots]") {
    // Not a check: pictures, for the review page, of the window as the real controller draws it
    // from real files. TAKT4_SHOTS_DIR says where; the temp folder otherwise.
    fs::path folder = fs::temp_directory_path() / "takt4-import-shots";
#if defined(_MSC_VER)
    char* wanted = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&wanted, &length, "TAKT4_SHOTS_DIR") == 0 && wanted != nullptr) {
        if (*wanted != '\0') {
            folder = wanted;
        }
        std::free(wanted);
    }
#else
    if (const char* wanted = std::getenv("TAKT4_SHOTS_DIR"); wanted != nullptr && *wanted != '\0') {
        folder = wanted;
    }
#endif
    fs::create_directories(folder);
    const auto shoot = [&folder](FixturesController& patch, const std::string& name, int width,
                                 int height) {
        settle();
        const takt4::tests::Shot shot = takt4::tests::render(patch.window(), width, height);
        writeBmp(shot, folder / (name + ".bmp"));
    };
    const fs::path references = fs::path(TAKT4_REFERENCES_DIR) / "fixture-defs" / "gdtf examples";
    const fs::path rayzor = references / "Elation@Rayzor_760@2023-15-08_Release.gdtf";
    const fs::path astra =
        references /
        "Prolights@AstraWash7PIX@Rev_084_-_Updated_Physical_values_for_Shutter_Open-Close.gdtf";

    Rig rig;
    std::vector<Fixture> rig5;
    for (const auto& [name, mode, address, group] :
         {std::tuple{"wash L", 1, 1, "washes"}, std::tuple{"wash R", 1, 7, "washes"}}) {
        Fixture one = takt4::dmx::fixtureFromMode(name, static_cast<std::size_t>(mode), 0,
                                                  static_cast<std::uint16_t>(address));
        one.group = group;
        rig5.push_back(one);
    }
    takt4::dmx::ensureFixtureIds(rig5);
    FixturesController patch(rig.runner, rig5);
    patch.show();

    // The library has the Diablo S, two fixtures made from it, and the Rayzor with none.
    patch.openImport();
    patch.importFile(diabloS());
    patch.setImportCount(2);
    patch.confirmImport();
    if (fs::exists(rayzor)) {
        // Into the library with no fixture made from it, as the mockup has it: imported, and the
        // fixture taken out again.
        patch.openImport();
        patch.importFile(rayzor);
        patch.setImportUniverse("1");
        patch.confirmImport();
        patch.removeAt(static_cast<int>(patch.fixtures().size()) - 1);
        patch.openImport();
        patch.pickLibraryEntry(1); // ayrton · Diablo-S, then Elation · Rayzor 760
        // Its "Pixels 360Pan/Tilt", as the mockup has it.
        patch.pickImportMode(1);
        patch.setImportCount(2);
        patch.setImportAddress(1);
        patch.setImportUniverse("1");
        shoot(patch, "pick", 1000, 800);
        shoot(patch, "pick-tall", 1000, 1400);
        shoot(patch, "pick-narrow", 880, 800);
        patch.setImportAddress(470);
        shoot(patch, "past", 1000, 1400);
        patch.setImportAddress(1);
        patch.setImportUniverse("0");
        shoot(patch, "overlap", 1000, 1400);
        patch.closeImport();
    }
    if (fs::exists(astra)) {
        patch.openImport();
        patch.importFile(astra);
        patch.pickImportMode(2);
        patch.setImportAddress(85);
        shoot(patch, "pair", 1000, 1000);
        patch.fold(4); // B folded, to see C
        shoot(patch, "pair-where", 1000, 800);
        patch.fold(4);
        patch.closeImport();
    }
    // The Diablo S again, changed: the question.
    std::string changed = readText(diabloS());
    const std::string date = "\"lastModifyDate\": \"2019-07-24\"";
    REQUIRE(changed.find(date) != std::string::npos);
    changed.replace(changed.find(date), date.size(), "\"lastModifyDate\": \"2026-07-11\"");
    const TempFile newer("diablo-s.json", changed);
    patch.openImport();
    patch.importFile(newer.path);
    shoot(patch, "ask", 1000, 800);
    patch.closeImport();
    // A file that is not one.
    const TempFile junk("junk.gdtf", std::string("this is not a zip"));
    patch.openImport();
    patch.importFile(junk.path);
    shoot(patch, "unreadable", 1000, 800);
    patch.closeImport();

    // A fixture an import made, in the editor: its names, its notes, its definition's modes.
    patch.pick(2); // Diablo-S 1
    shoot(patch, "chan", 1000, 800);
    shoot(patch, "chan-narrow", 880, 800);
    // Edited by hand: the mode says so.
    patch.pickChannelRole(0, 0);
    shoot(patch, "chan-edited", 1000, 800);
    // And its mode list, open: the definition's modes, each with its channels.
    clickAt(patch.window().window(), 544.0f, 201.0f);
    shoot(patch, "modes", 1000, 800);
    SUCCEED("written to " << folder.string());
}

TEST_CASE("the heads of a fixture with several, rendered for a person to look at",
          "[.][heads-shots]") {
    // Not a check: pictures, for the review page, of the patch editor's head column and the rule
    // editor's heads and spread, from a real definition with three heads (a bar of three tilting
    // lamps) and from a twin-yoke made by hand. TAKT4_SHOTS_DIR says where.
    fs::path folder = fs::temp_directory_path() / "takt4-heads-shots";
#if defined(_MSC_VER)
    char* wanted = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&wanted, &length, "TAKT4_SHOTS_DIR") == 0 && wanted != nullptr) {
        if (*wanted != '\0') {
            folder = wanted;
        }
        std::free(wanted);
    }
#else
    if (const char* wanted = std::getenv("TAKT4_SHOTS_DIR"); wanted != nullptr && *wanted != '\0') {
        folder = wanted;
    }
#endif
    fs::create_directories(folder);
    const fs::path bar = fs::path(TAKT4_REFERENCES_DIR) / "fixture-defs" /
                         "openfixturelibrary examples" / "beamz" /
                         "triple-flex-centre-pro-led.json";
    REQUIRE(fs::exists(bar));

    Rig rig;
    Fixture twin;
    twin.name = "twin yoke";
    twin.address = 40;
    twin.channels = {Role::Dimmer, Role::Pan,     Role::PanFine, Role::Tilt,    Role::TiltFine,
                     Role::Pan,    Role::PanFine, Role::Tilt,    Role::TiltFine};
    twin.labels = {"Dimmer",         "Pan Upper",       "Pan Upper fine",
                   "Tilt Upper",     "Tilt Upper fine", "Pan Lower",
                   "Pan Lower fine", "Tilt Lower",      "Tilt Lower fine"};
    twin.parked.assign(twin.channels.size(), 0);
    std::vector<Fixture> start{twin};
    takt4::dmx::ensureFixtureIds(start);
    FixturesController patch(rig.runner, start);
    patch.show();
    patch.openImport();
    patch.importFile(bar);
    patch.pickImportMode(1); // 8-channel
    patch.setImportAddress(1);
    patch.confirmImport();
    REQUIRE(patch.fixtures().size() == 2);
    const auto shootPatch = [&folder, &patch](const std::string& name, int width, int height) {
        settle();
        writeBmp(takt4::tests::render(patch.window(), width, height), folder / (name + ".bmp"));
    };
    patch.pick(1);
    shootPatch("patch-bar", 1000, 800);
    patch.pick(0);
    shootPatch("patch-twin", 1000, 800);
    shootPatch("patch-twin-narrow", 880, 800);
    shootPatch("patch-twin-tall", 1000, 1300);

    takt4::ui::RulesController editor(rig.runner, {});
    editor.setPatch(patch.fixtures());
    const auto pick = [](const auto& list, auto value) {
        return static_cast<int>(std::find(list.begin(), list.end(), value) - list.begin());
    };
    const auto shootRules = [&folder, &editor](const std::string& name, int width, int height) {
        editor.tick();
        editor.tick();
        settle();
        (void)takt4::tests::render(editor.window(), width, height);
        settle();
        writeBmp(takt4::tests::render(editor.window(), width, height), folder / (name + ".bmp"));
    };
    editor.add();
    editor.rename("swing the yokes");
    editor.setAddress("/a");
    editor.pickSend(pick(takt4::trigger::kMessageKinds, takt4::trigger::Message::Kind::Dmx));
    editor.setFixtureChosen(patch.fixtures()[0].id, true);
    editor.pickEffect(pick(takt4::dmx::kEffectKinds, takt4::dmx::EffectKind::Path));
    editor.setSpread(50.0f);
    editor.window().set_only_if_folded(true);
    editor.window().set_send_folded(false);
    shootRules("rules-twin", 1000, 1100);
    shootRules("rules-twin-narrow", 960, 1100);
    editor.toggleHead(1, false);
    shootRules("rules-twin-one", 1000, 1100);
    editor.toggleHead(1, true);
    editor.setSpread(0.0f);
    editor.pickEffect(pick(takt4::dmx::kEffectKinds, takt4::dmx::EffectKind::Position));
    shootRules("rules-twin-together", 1000, 1100);
    // The bar's three heads, and both fixtures at once.
    editor.setFixtureChosen(patch.fixtures()[1].id, true);
    editor.setSpread(30.0f);
    shootRules("rules-both", 1000, 1100);
    editor.setFixtureChosen(patch.fixtures()[0].id, false);
    editor.toggleHead(0, false);
    shootRules("rules-bar", 1000, 1100);
    shootRules("rules-bar-tall", 1000, 1600);
    // A bar of sixteen, on the grid, at the window's narrowest.
    Fixture sixteen;
    sixteen.name = "tilt bar";
    sixteen.address = 100;
    for (int k = 0; k < 16; ++k) {
        sixteen.channels.push_back(Role::Tilt);
    }
    sixteen.parked.assign(sixteen.channels.size(), 0);
    std::vector<Fixture> withBar = patch.fixtures();
    withBar.push_back(sixteen);
    takt4::dmx::ensureFixtureIds(withBar);
    editor.setPatch(withBar);
    editor.setFixtureChosen(patch.fixtures()[1].id, false);
    editor.setFixtureChosen(withBar.back().id, true);
    editor.toggleHead(3, false);
    shootRules("rules-sixteen-narrow", 960, 1100);
    editor.setDuration("0");
    shootRules("rules-untimed-narrow", 960, 1100);
    editor.setDuration("1");
    editor.setSpread(0.0f);
    editor.toggleHead(3, true);
    editor.toggleHead(9, false);
    editor.toggleHead(10, false);
    editor.toggleHead(11, false);
    shootRules("rules-together-narrow", 960, 1100);
    SUCCEED("written to " << folder.string());
}

namespace {

/// A par, patched by hand at 1.
std::vector<Fixture> onePar() {
    std::vector<Fixture> patch{takt4::dmx::fixtureFromMode("par", 1, 0, 1)};
    patch[0].group = "washes";
    takt4::dmx::ensureFixtureIds(patch);
    return patch;
}

/// Where IMPORT is in the list's heading: its edge, the first along the heading's middle from the
/// list's left — the + is after it.
float importButtonX(FixturesController& patch) {
    const takt4::tests::Shot shot = takt4::tests::render(patch.window(), kWidth, kHeight);
    const int left = edgeAlong(shot, 34, 120, 260);
    REQUIRE(left > 0);
    return static_cast<float>(left + 20);
}

/// CANCEL, at the right of the import's top row: its edge, the first from the right.
float cancelButtonX(FixturesController& patch) {
    const takt4::tests::Shot shot = takt4::tests::render(patch.window(), kWidth, kHeight);
    const int right = edgeAlong(shot, 37, kWidth - 1, 600);
    REQUIRE(right > 0);
    return static_cast<float>(right - 20);
}

std::vector<std::string> labelsOf(const Fixture& fixture) {
    return fixture.labels;
}

} // namespace

TEST_CASE("IMPORT takes the editor's place, and nothing typed in the editor is lost to it",
          "[ui][dmx][import]") {
    // The operator, 2026-10-05: the import should "take editors place but be sure to not lose
    // edits that were made in the editor it replaces". A name typed and never entered, then
    // IMPORT clicked: the name is the fixture's, and CANCEL brings the editor back holding it.
    // Driven by real pointer and key events — the bugs this guards against live in the markup.
    Rig rig;
    FixturesController patch(rig.runner, onePar());
    showAt(patch);
    const float importX = importButtonX(patch);
    auto& window = patch.window().window();

    clickAt(window, 559.0f, 37.0f); // the name box
    typeText(window, " renamed");
    REQUIRE(patch.fixtures()[0].name == "par"); // typed, not entered
    // The click, and nothing after it yet: the name is the fixture's at the click, before the
    // import is open — not only a turn later, when the box lets go of the keyboard and commits
    // on its own. Every action here enters what was typed before it runs.
    const slint::LogicalPosition at({importX, 34.0f});
    window.dispatch_pointer_move_event(at);
    window.dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
    window.dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
    CHECK(patch.fixtures()[0].name == "par renamed");
    settle();
    CHECK(patch.importing());
    CHECK(patch.window().get_importing());
    CHECK(patch.fixtures()[0].name == "par renamed");
    // The editor's row is kept behind the import, hidden: still what the controller holds.
    CHECK(std::string(patch.window().get_name()) == "par renamed");

    SECTION("and CANCEL brings it back as it was") {
        clickAt(window, cancelButtonX(patch), 37.0f);
        CHECK_FALSE(patch.importing());
        CHECK(patch.selected() == 0);
        CHECK(patch.fixtures()[0].name == "par renamed");
        CHECK(patch.fixtures().size() == 1);
    }
    SECTION("and a fixture picked from the list is the editor wanted back, on it") {
        patch.pick(0);
        CHECK_FALSE(patch.importing());
    }
}

TEST_CASE("an import from a file adds its fixtures, linked, and the definition to the library",
          "[ui][dmx][import]") {
    Rig rig;
    FixturesController patch(rig.runner, onePar());
    int patched = 0;
    int libraries = 0;
    std::size_t libraryHeld = 0;
    patch.setPatchChanged([&patched](const std::vector<Fixture>&) { ++patched; });
    patch.setLibraryChanged(
        [&libraries, &libraryHeld](const std::vector<takt4::fixtures::FixtureProfile>& library) {
            ++libraries;
            libraryHeld = library.size();
        });
    auto& window = patch.window();

    patch.openImport();
    CHECK(window.get_import_address() == 4); // after the par's 1-3
    CHECK_FALSE(window.get_import_picked());
    patch.importFile(diabloS());
    CHECK(window.get_import_picked());
    CHECK(std::string(window.get_import_file_note()).find("new to this preset") == 0);
    CHECK_FALSE(window.get_import_file_error());
    CHECK(window.get_library_rows()->row_count() == 0); // not in the library until imported
    const auto modes = window.get_mode_rows();
    REQUIRE(modes->row_count() == 3);
    CHECK(std::string(modes->row_data(0)->name) == "Standard");
    CHECK(std::string(modes->row_data(0)->channels) == "36");
    CHECK(std::string(modes->row_data(2)->channels) == "56");
    CHECK(modes->row_data(0)->picked); // the first that can be imported
    CHECK_THAT(std::string(modes->row_data(0)->drives), ContainsSubstring("CMY"));
    CHECK(window.get_import_can());
    CHECK(std::string(window.get_import_takes()) == "channels 4 to 39 on universe 0");
    CHECK(std::string(window.get_import_names()) == "Diablo-S 1 · group Diablo-S");
    CHECK(libraries == 0);

    patch.setImportCount(2);
    CHECK(std::string(window.get_import_takes()) == "channels 4 to 75 on universe 0, 36 each");
    patch.confirmImport();
    CHECK_FALSE(patch.importing());
    REQUIRE(patch.fixtures().size() == 3);
    const Fixture& first = patch.fixtures()[1];
    const Fixture& second = patch.fixtures()[2];
    CHECK(first.name == "Diablo-S 1");
    CHECK(second.name == "Diablo-S 2");
    CHECK(first.group == "Diablo-S");
    CHECK(first.address == 4);
    CHECK(second.address == 40);
    CHECK(first.channels.size() == 36);
    CHECK(first.labels.size() == 36);
    CHECK(first.profile.linked());
    CHECK(first.profile.mode == "Standard");
    REQUIRE(patch.library().size() == 1);
    CHECK(first.profile.id == patch.library()[0].id);
    CHECK(libraries == 1);
    CHECK(libraryHeld == 1);
    CHECK(patched >= 1);
    CHECK(patch.selected() == 1); // the first of them, in the editor
    CHECK_THAT(std::string(window.get_status()),
               ContainsSubstring("Imported 2 Diablo-S in Standard, channels 4 to 75"));

    SECTION("the editor shows its names, its definition's modes and where they came from") {
        CHECK(window.get_has_labels());
        const auto rows = window.get_channels();
        REQUIRE(rows->row_count() == 36);
        CHECK(std::string(rows->row_data(0)->label) == first.labels[0]);
        CHECK(std::string(window.get_mode_note()) == "from ayrton · Diablo-S");
        const auto entries = window.get_modes();
        REQUIRE(entries->row_count() == 3);
        CHECK(std::string(*entries->row_data(0)) == "Standard");
        CHECK(window.get_mode_index() == 0);
        CHECK(std::string(*window.get_mode_asides()->row_data(2)) == "56");
        CHECK(std::string(window.get_mode_footnote()) ==
              "picking one puts its channels back as imported");
        // A channel the mapping had something to say about says it on its band.
        bool noted = false;
        for (std::size_t i = 0; i < rows->row_count(); ++i) {
            noted = noted || !std::string(rows->row_data(i)->note).empty();
        }
        CHECK(noted);
        // And one patched by hand shows none of it.
        patch.pick(0);
        CHECK_FALSE(window.get_has_labels());
        CHECK(std::string(window.get_mode_note()) == "fills in the channels below");
        CHECK(window.get_modes()->row_count() == takt4::dmx::builtinModes().size() + 1);
    }
    SECTION("the same file again is the library's entry, picked, and asks nothing") {
        patch.openImport();
        patch.importFile(diabloS());
        CHECK_FALSE(window.get_import_asking());
        CHECK(patch.library().size() == 1);
        REQUIRE(window.get_library_rows()->row_count() == 1);
        CHECK(window.get_library_rows()->row_data(0)->picked);
        CHECK(std::string(window.get_library_rows()->row_data(0)->users) == "2 fixtures");
        CHECK_FALSE(window.get_library_rows()->row_data(0)->removable);
        CHECK_THAT(std::string(window.get_import_file_note()), ContainsSubstring("already"));
        // Two more, numbered on from the two there are.
        patch.setImportCount(2);
        patch.confirmImport();
        REQUIRE(patch.fixtures().size() == 5);
        CHECK(patch.fixtures()[3].name == "Diablo-S 3");
        CHECK(patch.fixtures()[4].name == "Diablo-S 4");
        CHECK(patch.library().size() == 1);
        CHECK(libraries == 1); // nothing new to save
    }
    SECTION("an edit by hand says so in the mode, and picking the mode puts it back") {
        patch.pickChannelRole(0, 0); // its first channel to unused
        const auto entries = window.get_modes();
        REQUIRE(entries->row_count() == 4);
        CHECK(std::string(*entries->row_data(0)) == "Standard (edited)");
        CHECK(window.get_mode_index() == 0);
        CHECK(takt4::fixtures::isEdited(patch.fixtures()[1], patch.library()[0]));
        window.invoke_mode_picked(1); // "Standard", below it
        CHECK_FALSE(takt4::fixtures::isEdited(patch.fixtures()[1], patch.library()[0]));
        CHECK(window.get_modes()->row_count() == 3);
        CHECK(patch.fixtures()[1].profile.mode == "Standard");

        SECTION("and another mode of it is that mode, names and all") {
            window.invoke_mode_picked(2); // "Extended"
            CHECK(patch.fixtures()[1].channels.size() == 56);
            CHECK(patch.fixtures()[1].labels.size() == 56);
            CHECK(patch.fixtures()[1].profile.mode == "Extended");
            CHECK(patch.fixtures()[2].profile.mode == "Standard"); // its neighbour is not a pair
        }
    }
}

TEST_CASE("a file the library has another version of asks before anything changes",
          "[ui][dmx][import]") {
    Rig rig;
    FixturesController patch(rig.runner, onePar());
    auto& window = patch.window();
    patch.openImport();
    patch.importFile(diabloS());
    patch.setImportCount(2);
    patch.confirmImport();
    REQUIRE(patch.fixtures().size() == 3);
    const std::string id = patch.library()[0].id;

    // Revised, with a channel renamed throughout.
    std::string changed = readText(diabloS());
    const std::string date = "\"lastModifyDate\": \"2019-07-24\"";
    REQUIRE(changed.find(date) != std::string::npos);
    changed.replace(changed.find(date), date.size(), "\"lastModifyDate\": \"2026-07-11\"");
    const std::string speed = "\"Pan/Tilt Speed\"";
    REQUIRE(changed.find(speed) != std::string::npos);
    for (std::size_t at = changed.find(speed); at != std::string::npos; at = changed.find(speed)) {
        changed.replace(at, speed.size(), "\"Movement Speed\"");
    }
    const TempFile newer("diablo-s-changed.json", changed);
    const auto hasSpeed = [](const Fixture& fixture, const std::string& label) {
        return std::find(fixture.labels.begin(), fixture.labels.end(), label) !=
               fixture.labels.end();
    };
    REQUIRE(hasSpeed(patch.fixtures()[1], "Pan/Tilt Speed"));

    patch.openImport();
    patch.importFile(newer.path);
    CHECK(window.get_import_asking());
    CHECK_FALSE(window.get_import_picked()); // nothing to import until it is answered
    CHECK_THAT(std::string(window.get_ask_question()),
               ContainsSubstring("Diablo-S is in this preset's library already"));
    CHECK(std::string(window.get_ask_library()) == "revised 2019-07-24 · used by 2 fixtures");
    CHECK_THAT(std::string(window.get_ask_file()),
               ContainsSubstring("revised 2026-07-11 · 3 modes"));
    CHECK_THAT(std::string(window.get_ask_replace()),
               ContainsSubstring("Diablo-S 1 and Diablo-S 2"));
    CHECK(patch.library().size() == 1);

    SECTION("replace: the entry is the file's, and its fixtures are brought up to date") {
        window.invoke_ask_replace_clicked();
        CHECK_FALSE(window.get_import_asking());
        REQUIRE(patch.library().size() == 1);
        CHECK(patch.library()[0].id == id); // the same entry, so every link holds
        CHECK(patch.library()[0].revision == "2026-07-11");
        CHECK(hasSpeed(patch.fixtures()[1], "Movement Speed"));
        CHECK(hasSpeed(patch.fixtures()[2], "Movement Speed"));
        CHECK(patch.fixtures()[1].profile.id == id);
        CHECK(window.get_import_picked()); // and the entry picked, to import more of
        CHECK_THAT(std::string(window.get_status()),
                   ContainsSubstring("Diablo-S replaced: 2 fixtures brought up to date"));
    }
    SECTION("keep both: a second entry, and nothing patched changes") {
        window.invoke_ask_keep_clicked();
        REQUIRE(patch.library().size() == 2);
        CHECK(hasSpeed(patch.fixtures()[1], "Pan/Tilt Speed"));
        const auto rows = window.get_library_rows();
        REQUIRE(rows->row_count() == 2);
        // Told apart by their revisions, now there are two of it.
        CHECK(std::string(rows->row_data(0)->model) == "Diablo-S (2019-07-24)");
        CHECK(std::string(rows->row_data(1)->model) == "Diablo-S (2026-07-11)");
        CHECK(rows->row_data(1)->picked);
        CHECK(rows->row_data(1)->removable);
    }
    SECTION("cancel: the library as it was, and nothing picked") {
        window.invoke_ask_cancel_clicked();
        CHECK_FALSE(window.get_import_asking());
        CHECK_FALSE(window.get_import_picked());
        CHECK(patch.library().size() == 1);
        CHECK(patch.library()[0].revision == "2019-07-24");
    }
}

TEST_CASE("a definition no fixture is made from can be taken out of the library, and only that",
          "[ui][dmx][import]") {
    Rig rig;
    FixturesController patch(rig.runner, onePar());
    auto& window = patch.window();
    patch.openImport();
    patch.importFile(diabloS());
    patch.confirmImport();
    REQUIRE(patch.library().size() == 1);

    patch.openImport();
    REQUIRE(window.get_library_rows()->row_count() == 1);
    CHECK_FALSE(window.get_library_rows()->row_data(0)->removable);
    patch.removeLibraryEntry(0);
    CHECK(patch.library().size() == 1);
    CHECK(window.get_status_error());

    patch.closeImport();
    patch.removeAt(1); // the Diablo-S
    patch.openImport();
    CHECK(window.get_library_rows()->row_data(0)->removable);
    CHECK(std::string(window.get_library_rows()->row_data(0)->users) == "no fixture");
    patch.removeLibraryEntry(0);
    CHECK(patch.library().empty());
    CHECK(window.get_library_rows()->row_count() == 0);
}

TEST_CASE("a block past the end of the universe is refused, says where it fits, and adds nothing",
          "[ui][dmx][import]") {
    Rig rig;
    FixturesController patch(rig.runner, onePar());
    auto& window = patch.window();
    int libraries = 0;
    patch.setLibraryChanged(
        [&libraries](const std::vector<takt4::fixtures::FixtureProfile>&) { ++libraries; });
    patch.openImport();
    patch.importFile(diabloS());
    patch.setImportAddress(490);
    CHECK_FALSE(window.get_import_can());
    CHECK(window.get_import_takes_error());
    CHECK(std::string(window.get_import_takes()) ==
          "runs past 512: 36 channels from 490 — start at 477 or lower");
    CHECK(std::string(window.get_import_names()).empty()); // and no "named" over nothing
    patch.confirmImport();
    CHECK(patch.importing());
    CHECK(patch.fixtures().size() == 1);
    // The file's definition goes into the library only with fixtures made from it.
    CHECK(patch.library().empty());
    CHECK(libraries == 0);
    CHECK_THAT(std::string(window.get_status()), ContainsSubstring("Nothing imported"));

    SECTION("an overlap is said, and allowed") {
        patch.setImportAddress(2);
        CHECK(window.get_import_can());
        CHECK_THAT(std::string(window.get_import_overlap()), ContainsSubstring("lands on par"));
        patch.confirmImport();
        CHECK(patch.fixtures().size() == 2);
    }
}

TEST_CASE("a mode with two start addresses makes a pair, and a mode picked for one moves both",
          "[ui][dmx][import]") {
    Rig rig;
    FixturesController patch(rig.runner, onePar());
    auto& window = patch.window();
    const TempFile wash("pixel-wash.gdtf", twoAddressWash());
    patch.openImport();
    patch.importFile(wash.path);
    const auto modes = window.get_mode_rows();
    REQUIRE(modes->row_count() == 3);
    CHECK(std::string(modes->row_data(1)->channels) == "8 + 12");
    patch.pickImportMode(1);
    CHECK(window.get_import_pair());
    CHECK(std::string(window.get_import_takes()) ==
          "channels 4 to 23 on universe 0: part 1 at 4, part 2 at 12");
    patch.confirmImport();
    REQUIRE(patch.fixtures().size() == 3);
    const Fixture& one = patch.fixtures()[1];
    const Fixture& two = patch.fixtures()[2];
    CHECK(one.name == "Pixel Wash 1");
    CHECK(two.name == "Pixel Wash 1 · part 2");
    CHECK(two.address == 12);
    CHECK(!one.profile.pair.empty());
    CHECK(one.profile.pair == two.profile.pair);
    CHECK(two.profile.part == 2);

    // One of a pair is offered the definition's two-address modes only.
    patch.pick(1);
    const auto entries = window.get_modes();
    REQUIRE(entries->row_count() == 2);
    CHECK(std::string(*entries->row_data(0)) == "Pixel");
    CHECK(std::string(*entries->row_data(1)) == "Pixel 2");
    window.invoke_mode_picked(1);
    CHECK(patch.fixtures()[1].profile.mode == "Pixel 2");
    CHECK(patch.fixtures()[2].profile.mode == "Pixel 2");
    CHECK(patch.fixtures()[2].channels.size() == 6); // two cells, not four
    CHECK_THAT(std::string(window.get_status()), ContainsSubstring("Both parts"));
}

TEST_CASE("an import into an empty patch starts at 1 and leaves the editor on what it made",
          "[ui][dmx][import]") {
    Rig rig;
    FixturesController patch(rig.runner, {});
    auto& window = patch.window();
    REQUIRE(patch.selected() == -1);
    patch.openImport();
    CHECK(window.get_importing());
    CHECK(std::string(window.get_import_universe()) == "0");
    CHECK(window.get_import_address() == 1);
    patch.importFile(diabloS());
    CHECK(std::string(window.get_import_takes()) == "channels 1 to 36 on universe 0");
    patch.confirmImport();
    REQUIRE(patch.fixtures().size() == 1);
    CHECK(patch.selected() == 0);
    CHECK_FALSE(window.get_importing());
    CHECK(std::string(window.get_name()) == "Diablo-S 1");
    CHECK(patch.fixtures()[0].address == 1);

    SECTION("and cancelled, it leaves the empty patch as it was") {
        FixturesController empty(rig.runner, {});
        empty.openImport();
        empty.importFile(diabloS());
        empty.closeImport();
        CHECK(empty.fixtures().empty());
        CHECK(empty.library().empty());
        CHECK(empty.selected() == -1);
        CHECK_FALSE(empty.window().get_importing());
    }
}

TEST_CASE("a file that cannot be read says why, in plain words, where it was asked for",
          "[ui][dmx][import]") {
    Rig rig;
    FixturesController patch(rig.runner, onePar());
    auto& window = patch.window();
    const TempFile junk("junk.gdtf", std::string("this is not a zip"));
    patch.openImport();
    patch.importFile(junk.path);
    CHECK(window.get_import_file_error());
    CHECK_THAT(std::string(window.get_import_file_note()),
               ContainsSubstring("Can't read takt4-import-test-junk.gdtf: it is damaged, or not "
                                 "a GDTF file"));
    CHECK_FALSE(window.get_import_picked());
    CHECK(patch.library().empty());
}

TEST_CASE("what is typed into the import's boxes is what IMPORT makes", "[ui][dmx][import]") {
    // A count typed and never entered, then IMPORT clicked: the click commits it before IMPORT
    // reads it, as every action in this window commits what was typed first.
    Rig rig;
    FixturesController patch(rig.runner, onePar());
    showAt(patch);
    patch.openImport();
    patch.importFile(diabloS());
    patch.fold(4); // B folded: C under A
    settle();
    const takt4::tests::Shot shot = takt4::tests::render(patch.window(), kWidth, kHeight);
    // The count's box is the first field face down the column it sits in; IMPORT the first
    // filled off-white face below it, down a column near its left edge, clear of its label.
    const int countY = faceDown(shot, 460, 380, 24, isField);
    REQUIRE(countY > 0);
    const int importY = faceDown(shot, 304, countY + 20, 24, isEdge);
    REQUIRE(importY > 0);
    auto& window = patch.window().window();
    clickAt(window, 460.0f, static_cast<float>(countY));
    typeText(window, "\b3");                         // the caret is after the 1: Backspace, then 3
    REQUIRE(patch.window().get_import_count() == 1); // typed, not entered
    clickAt(window, 356.0f, static_cast<float>(importY));
    CHECK_FALSE(patch.importing());
    CHECK(patch.fixtures().size() == 4); // the par and three
}

TEST_CASE("a named fixture's channels line up under their headings, whatever their names",
          "[ui][dmx][import]") {
    // The name column sized itself from each row's own name, so a longer name ("Pan/Tilt Speed",
    // "Shutter / Strobe") pushed that row's dropdown and everything after it right of the others
    // (the render of 2026-10-05). Measured as drawn: where each band's dropdown starts, at the
    // window's opening width and at its narrowest.
    Rig rig;
    FixturesController patch(rig.runner, {});
    patch.openImport();
    patch.importFile(diabloS());
    patch.confirmImport();
    REQUIRE(patch.fixtures().size() == 1);
    REQUIRE(patch.window().get_has_labels());
    for (const int width : {1000, 880}) {
        INFO("at " << width << " wide");
        settle();
        (void)takt4::tests::render(patch.window(), width, 800);
        settle();
        const takt4::tests::Shot shot = takt4::tests::render(patch.window(), width, 800);
        // Down the channels' bands: the first field-coloured pixel along each one's middle is its
        // dropdown's face (the names and numbers are drawn on the band, not on a field).
        std::vector<int> starts;
        int run = 0;
        int runStart = -1;
        for (int y = 350; y < 790; ++y) {
            int first = -1;
            for (int x = 240; x < width - 300; ++x) {
                if (isField(shot.at(x, y))) {
                    first = x;
                    break;
                }
            }
            if (first >= 0 && run > 0 && first == runStart) {
                ++run;
                continue;
            }
            if (run >= 20) {
                starts.push_back(runStart); // a dropdown's face, 20 px or more tall
            }
            run = first >= 0 ? 1 : 0;
            runStart = first;
        }
        INFO("dropdowns found: " << starts.size());
        REQUIRE(starts.size() >= 6);
        for (const int x : starts) {
            CHECK(x == starts.front());
        }
    }
}
