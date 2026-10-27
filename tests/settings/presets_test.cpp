#include "core/io/utf8.hpp"
#include "core/settings/presets.hpp"
#include "core/settings/settings.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using takt4::settings::findPreset;
using takt4::settings::listPresets;
using takt4::settings::presetNameProblem;

namespace {

/// A file of `text` at `path`, as something other than takt4 would leave one.
void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

} // namespace

TEST_CASE("a preset's name is one a file can have on every system takt4 runs on", "[settings]") {
    for (const char* good : {"Friday late set", "v1.2", "Caf\xC3\xA9 \xE2\x80\x94 lasers", "COM10",
                             "CONSOLE", "a.CON", "1"}) {
        INFO(good);
        CHECK(presetNameProblem(good).empty());
    }
    CHECK(presetNameProblem(std::string(64, 'x')).empty());
    for (const std::string& bad : {std::string(),
                                   std::string(65, 'x'),
                                   std::string("a/b"),
                                   std::string("a\\b"),
                                   std::string("a:b"),
                                   std::string("why?"),
                                   std::string("\"quoted\""),
                                   std::string("a*"),
                                   std::string("<b>"),
                                   std::string("a|b"),
                                   std::string(".hidden"),
                                   std::string("end."),
                                   std::string("end "),
                                   std::string("CON"),
                                   std::string("con"),
                                   std::string("Nul"),
                                   std::string("COM1"),
                                   std::string("lpt9.show"),
                                   std::string("tab\there"),
                                   std::string("\xFF\xFE")}) {
        INFO(takt4::io::validUtf8(bad));
        CHECK_FALSE(presetNameProblem(bad).empty());
    }
    CHECK(takt4::settings::trimmedPresetName("  Friday late set \t") == "Friday late set");
}

TEST_CASE("a preset holds the show and not the machine, is listed by its name, and loads as it "
          "was saved",
          "[settings]") {
    const takt4::test::TempDir temp;
    const std::filesystem::path folder = temp.path() / "presets"; // made by the first save
    takt4::settings::Settings show;
    show.preset.phraseBars = 12;
    show.preset.oscPrefix = "/show";
    show.preset.externalBeatsPerBar = 3;
    show.machine.oscControlPort = 9123; // this desk's, and no preset's
    show.machine.deviceName = "MOTU Pro Audio";
    // As a show is when it has been loaded once: with the Link output every loaded one is given.
    show = takt4::settings::fromJson(takt4::settings::toJson(show));
    const std::string name = "Caf\xC3\xA9 \xE2\x80\x94 lasers";

    std::string problem;
    REQUIRE(takt4::settings::savePreset(show.preset, folder, name, problem));
    CHECK(problem.empty());
    const std::filesystem::path file = takt4::settings::presetFile(folder, name);
    REQUIRE(std::filesystem::exists(file));
    // Named as typed, on disk and in the list, accents and dash and all.
    CHECK(takt4::io::pathText(file.filename()) == name + ".json");
    CHECK(listPresets(folder) == std::vector<std::string>{name});

    std::string text;
    {
        std::ifstream in(file, std::ios::binary); // closed again before the file is replaced
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(text.find("\"machine\"") == std::string::npos);
    CHECK(text.find("9123") == std::string::npos);
    CHECK(text.find("MOTU") == std::string::npos);

    const takt4::settings::Loaded loaded = takt4::settings::loadChecked(file);
    REQUIRE(loaded.status == takt4::settings::LoadStatus::Loaded);
    CHECK_FALSE(loaded.settings.machine.inFile);
    CHECK(loaded.settings.preset.phraseBars == 12);
    CHECK(loaded.settings.preset.oscPrefix == "/show");
    CHECK(loaded.settings.preset.externalBeatsPerBar == 3);
    CHECK(takt4::settings::presetJson(loaded.settings.preset) ==
          takt4::settings::presetJson(show.preset));

    // Saved again under the same name, it is the same one, replaced.
    show.preset.phraseBars = 16;
    REQUIRE(takt4::settings::savePreset(show.preset, folder, name, problem));
    CHECK(listPresets(folder).size() == 1);
    CHECK(takt4::settings::loadChecked(file).settings.preset.phraseBars == 16);
}

TEST_CASE("the presets are the folder's settings files by name, sorted, and nothing else",
          "[settings]") {
    const takt4::test::TempDir temp;
    const std::filesystem::path folder = temp.path();
    const takt4::settings::Preset preset;
    std::string problem;
    for (const char* name : {"beta", "Alpha", "gamma"}) {
        REQUIRE(takt4::settings::savePreset(preset, folder, name, problem));
    }
    writeFile(folder / "delta.JSON", takt4::settings::presetJson(preset)); // a shouted extension
    writeFile(folder / "notes.txt", "not a preset");
    writeFile(folder / ".hidden.json", "{}");
    std::filesystem::create_directories(folder / "folder.json");
    CHECK(listPresets(folder) == std::vector<std::string>{"Alpha", "beta", "delta", "gamma"});
    CHECK(listPresets(temp.path() / "not there").empty());
    CHECK(listPresets({}).empty());

    // A bad name is refused before anything is written.
    CHECK_FALSE(takt4::settings::savePreset(preset, folder, "a/b", problem));
    CHECK_FALSE(problem.empty());
    CHECK_FALSE(takt4::settings::savePreset(preset, {}, "fine", problem));
    CHECK(listPresets(folder).size() == 4);
}

TEST_CASE("a preset is found by its name, or by its name in another case when only one has it",
          "[settings]") {
    const std::vector<std::string> names{"Alpha", "beta", "Show", "show"};
    CHECK(findPreset(names, "Alpha") == "Alpha");
    CHECK(findPreset(names, "alpha") == "Alpha");
    CHECK(findPreset(names, "BETA") == "beta");
    CHECK(findPreset(names, "  beta ") == "beta"); // as typed, spaces and all
    CHECK(findPreset(names, "Show") == "Show");    // spelled exactly: that one
    CHECK(findPreset(names, "show") == "show");
    CHECK_FALSE(findPreset(names, "SHOW")); // two in another case: neither is meant more
    CHECK_FALSE(findPreset(names, "gamma"));
    CHECK_FALSE(findPreset(names, ""));
    CHECK_FALSE(findPreset({}, "Alpha"));
}

TEST_CASE("deleting a preset deletes its file and no other", "[settings]") {
    const takt4::test::TempDir temp;
    const std::filesystem::path folder = temp.path() / "presets";
    const takt4::settings::Preset preset;
    std::string problem;
    REQUIRE(takt4::settings::savePreset(preset, folder, "keep", problem));
    REQUIRE(takt4::settings::savePreset(preset, folder, "gone", problem));
    writeFile(temp.path() / "settings.json", "{}"); // beside the folder, as the real one is

    REQUIRE(takt4::settings::deletePreset(folder, "gone", problem));
    CHECK(listPresets(folder) == std::vector<std::string>{"keep"});
    CHECK_FALSE(takt4::settings::deletePreset(folder, "gone", problem));
    CHECK(problem == "no preset \"gone\"");
    // A name that is not one never reaches outside the folder.
    CHECK_FALSE(takt4::settings::deletePreset(folder, "../settings", problem));
    CHECK_FALSE(problem.empty());
    CHECK(std::filesystem::exists(temp.path() / "settings.json"));
    CHECK(listPresets(folder) == std::vector<std::string>{"keep"});
}
