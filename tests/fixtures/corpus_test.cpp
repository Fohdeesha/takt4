// The import run over every example definition there is on this machine — local only: the
// examples (145 GDTF files from GDTF Share, 91 from the Open Fixture Library) are not in the
// repository, and CI has none, so these skip there.

#include "core/fixtures/definition.hpp"
#include "core/fixtures/fixture_library.hpp"
#include "core/fixtures/gdtf_reader.hpp"
#include "core/fixtures/import.hpp"
#include "core/fixtures/ofl_reader.hpp"
#include "core/fixtures/profile_mapper.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using takt4::fixtures::DefChannel;
using takt4::fixtures::Definition;
using takt4::fixtures::DefMode;
using takt4::fixtures::Kind;

namespace {

fs::path examples() {
    return fs::path(TAKT4_REFERENCES_DIR) / "fixture-defs";
}

std::vector<fs::path> filesIn(const fs::path& folder, const std::string& extension) {
    std::vector<fs::path> out;
    std::error_code code;
    if (!fs::is_directory(folder, code)) {
        return out;
    }
    for (const auto& entry : fs::recursive_directory_iterator(folder, code)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// The channel counts a mode's name states, one per part: "10-Ch" is {10}, "Mode 2 (54 ch)" is
/// {54}, "Basic+Pixel_18ch+28ch" is {18, 28}. Empty when the name states none.
std::vector<std::size_t> statedCounts(const std::string& name) {
    static const std::regex count(R"((\d+)\s*-?\s*ch(?:annels?)?(?![a-z]))", std::regex::icase);
    std::vector<std::size_t> out;
    for (auto it = std::sregex_iterator(name.begin(), name.end(), count);
         it != std::sregex_iterator(); ++it) {
        out.push_back(static_cast<std::size_t>(std::stoul((*it)[1].str())));
    }
    return out;
}

std::vector<std::size_t> footprint(const DefMode& mode) {
    std::vector<std::size_t> out;
    for (const auto& part : mode.parts) {
        out.push_back(part.size());
    }
    return out;
}

/// The modes whose files contradict their own names: JB-Lighting's P18 files and Showtec's Helix
/// S5000 Q4 list fewer channels than the name says, and the Creamsource Vortex 4's mode 47 places
/// its own channels at 25–27 after two 12-channel plates, so 27 is right and "(15ch)" is not.
bool contradictsItsName(const fs::path& file, const std::string& mode) {
    struct Known {
        std::string_view file;
        std::string_view mode;
    };
    static constexpr Known kKnown[] = {
        {"JB-Lighting@P18_Profile_WW@V_1.08.gdtf", "Mode 2 (63 ch)"},
        {"JB-Lighting@P18_Wash_HP@V_1.05.gdtf", "Mode 1 (36 ch)"},
        {"JB-Lighting@P18_Wash_HP@V_1.05.gdtf", "Mode 2 (54 ch)"},
        {"JB-Lighting@P18_Wash_WW@V_1.05.gdtf", "Mode 1 (36 ch)"},
        {"JB-Lighting@P18_Wash_WW@V_1.05.gdtf", "Mode 2 (54 ch)"},
        {"Showtec@Helix_S5000_Q4@V002.gdtf", "6 channels 1"},
        {"Creamsource@Vortex_4@Revison_2-5-1.gdtf",
         "Mode 47: Split Zone CCT Hue/Sat  (15ch) 16 Bit"},
    };
    const std::string name = file.filename().string();
    for (const Known& known : kKnown) {
        if (name == known.file && mode == known.mode) {
            return true;
        }
    }
    return false;
}

Definition read(const fs::path& file) {
    const takt4::fixtures::ReadResult result = takt4::fixtures::readDefinitionFile(file);
    INFO(file.string() << ": " << result.problem);
    REQUIRE(result.definition);
    return *result.definition;
}

} // namespace

TEST_CASE("every example definition imports, and every mode is as long as its name says",
          "[fixtures][corpus]") {
    const std::vector<fs::path> gdtf = filesIn(examples() / "gdtf examples", ".gdtf");
    const std::vector<fs::path> ofl = filesIn(examples() / "openfixturelibrary examples", ".json");
    if (gdtf.empty() && ofl.empty()) {
        SKIP("references/fixture-defs is not on this machine");
    }
    std::size_t stated = 0;
    std::size_t matched = 0;
    std::size_t perPart = 0;
    std::size_t contradicted = 0;
    std::vector<std::string> wrong;
    for (const std::vector<fs::path>* files : {&gdtf, &ofl}) {
        for (const fs::path& file : *files) {
            const Definition definition = read(file);
            CHECK_FALSE(definition.key.empty());
            CHECK_FALSE(definition.modes.empty());
            const takt4::fixtures::FixtureProfile profile =
                takt4::fixtures::mapDefinition(definition);
            CHECK(profile.modes.size() == definition.modes.size());
            for (const DefMode& mode : definition.modes) {
                if (!mode.refusal.empty()) {
                    continue;
                }
                const std::vector<std::size_t> counts = statedCounts(mode.name);
                if (counts.empty()) {
                    continue;
                }
                ++stated;
                const std::vector<std::size_t> parts = footprint(mode);
                std::size_t total = 0;
                for (const std::size_t part : parts) {
                    total += part;
                }
                if (counts.size() == 1 && counts.front() == total) {
                    ++matched;
                } else if (counts == parts) {
                    ++perPart;
                } else if (contradictsItsName(file, mode.name)) {
                    ++contradicted;
                } else {
                    wrong.push_back(file.filename().string() + " \"" + mode.name +
                                    "\": " + std::to_string(total));
                }
            }
        }
    }
    for (const std::string& line : wrong) {
        UNSCOPED_INFO(line);
    }
    INFO(stated << " modes state a count: " << matched << " match it, " << perPart << " per part, "
                << contradicted << " contradict their own names");
    CHECK(wrong.empty());
    // Each of the seven is still there and still wrong: a fix to one of those files, or a change
    // here that started reading them differently, is worth knowing about.
    CHECK(contradicted == 7);
    CHECK(stated > 0);
}

// What the import makes of the examples, in numbers — for checking the design's measurements
// against the code that implements them. Hidden: run it by name.
TEST_CASE("fixture corpus report", "[.][corpus-report]") {
    struct Tally {
        std::size_t files = 0;
        std::size_t modes = 0;
        std::size_t refused = 0;
        std::size_t twoPart = 0;
        std::size_t shutters = 0;
        std::size_t open = 0;
        std::size_t drivable = 0;
        std::size_t shared = 0;
        std::size_t defaultFullDimmer = 0;
        std::size_t defaultFullColor = 0;
        std::size_t channels = 0;
        std::vector<std::string> refusals;
        std::vector<std::string> twoParts;
        std::map<std::string, std::size_t> notes;
    };
    std::map<std::string, Tally> tallies;
    for (const auto& [format, folder, extension] :
         {std::tuple<std::string, std::string, std::string>{"gdtf", "gdtf examples", ".gdtf"},
          {"ofl", "openfixturelibrary examples", ".json"}}) {
        Tally& tally = tallies[format];
        for (const fs::path& file : filesIn(examples() / folder, extension)) {
            const Definition definition = read(file);
            ++tally.files;
            for (const DefMode& mode : definition.modes) {
                ++tally.modes;
                for (const std::string& note : mode.notes) {
                    ++tally.notes[note.substr(0, std::min<std::size_t>(note.size(), 40))];
                }
                if (!mode.refusal.empty()) {
                    ++tally.refused;
                    tally.refusals.push_back(file.filename().string() + " \"" + mode.name +
                                             "\": " + mode.refusal);
                    continue;
                }
                if (mode.parts.size() == 2) {
                    ++tally.twoPart;
                    tally.twoParts.push_back(file.filename().string() + " \"" + mode.name + "\"");
                }
                for (const auto& part : mode.parts) {
                    for (const DefChannel& channel : part) {
                        ++tally.channels;
                        if (channel.byte != 0) {
                            continue;
                        }
                        if (channel.kind == Kind::Shutter) {
                            ++tally.shutters;
                            if (channel.openValue) {
                                ++tally.open;
                            }
                        }
                        const bool color =
                            channel.kind >= Kind::Red && channel.kind <= Kind::IndirectBlue;
                        const bool drivable =
                            channel.kind == Kind::Dimmer || color || channel.kind == Kind::Pan ||
                            channel.kind == Kind::Tilt || channel.kind == Kind::Zoom ||
                            channel.kind == Kind::Focus || channel.kind == Kind::PanTiltSpeed;
                        if (drivable) {
                            ++tally.drivable;
                            tally.shared += channel.shared ? 1 : 0;
                        }
                        const std::uint64_t full =
                            channel.resolutionBytes >= 8
                                ? ~std::uint64_t{0}
                                : (std::uint64_t{1} << (8 * channel.resolutionBytes)) - 1;
                        if (channel.defaultValue && *channel.defaultValue == full) {
                            tally.defaultFullDimmer += channel.kind == Kind::Dimmer ? 1 : 0;
                            tally.defaultFullColor += color ? 1 : 0;
                        }
                    }
                }
            }
        }
    }
    // Every channel read, one line each, for a diff against another reading of the same files:
    // file, mode index, part, address, label, byte, bytes, default byte, open value, kind, cell,
    // ordinal, shared.
    {
        std::ofstream dump(fs::temp_directory_path() / "takt4-corpus-dump.tsv", std::ios::binary);
        for (const auto& [folder, extension] :
             {std::pair<std::string, std::string>{"gdtf examples", ".gdtf"},
              {"openfixturelibrary examples", ".json"}}) {
            for (const fs::path& file : filesIn(examples() / folder, extension)) {
                const Definition definition = read(file);
                const std::string name = fs::relative(file, examples() / folder).generic_string();
                for (std::size_t m = 0; m < definition.modes.size(); ++m) {
                    const DefMode& mode = definition.modes[m];
                    if (!mode.refusal.empty()) {
                        dump << name << '\t' << m << "\tREFUSED\t" << mode.refusal << '\n';
                        continue;
                    }
                    for (std::size_t p = 0; p < mode.parts.size(); ++p) {
                        for (std::size_t a = 0; a < mode.parts[p].size(); ++a) {
                            const DefChannel& c = mode.parts[p][a];
                            const unsigned bytes = c.resolutionBytes;
                            const std::uint64_t value = c.defaultValue.value_or(0);
                            const unsigned shift = 8 * (bytes - 1 - c.byte);
                            dump << name << '\t' << m << '\t' << p + 1 << '\t' << a + 1 << '\t'
                                 << c.label << '\t' << int{c.byte} << '\t' << bytes << '\t'
                                 << ((value >> shift) & 0xFF) << '\t'
                                 << (c.openValue ? std::to_string(*c.openValue) : std::string("-"))
                                 << '\t' << takt4::fixtures::nameOf(c.kind) << '\t' << c.cell
                                 << '\t' << c.ordinal << '\t' << (c.shared ? 1 : 0) << '\n';
                        }
                    }
                }
            }
        }
    }
    // Every mode as the import sheet lists it — its channels, what takt4 drives, how many have no
    // role, and every note the mapping left on a channel — for reading the sheet's words against
    // every example rather than a few.
    {
        std::ofstream modes(fs::temp_directory_path() / "takt4-corpus-modes.tsv", std::ios::binary);
        for (const auto& [folder, extension] :
             {std::pair<std::string, std::string>{"gdtf examples", ".gdtf"},
              {"openfixturelibrary examples", ".json"}}) {
            for (const fs::path& file : filesIn(examples() / folder, extension)) {
                const takt4::fixtures::FixtureProfile profile =
                    takt4::fixtures::mapDefinition(read(file));
                const std::string name = fs::relative(file, examples() / folder).generic_string();
                for (const takt4::fixtures::ProfileMode& mode : profile.modes) {
                    if (!mode.importable()) {
                        modes << name << '\t' << mode.name << "\tREFUSED\t" << mode.refused << '\n';
                        continue;
                    }
                    modes << name << '\t' << mode.name << '\t'
                          << takt4::fixtures::channelCountOf(mode) << '\t'
                          << takt4::fixtures::drivesOf(mode) << '\t'
                          << takt4::fixtures::unusedIn(mode) << '\n';
                    for (const auto& part : mode.parts) {
                        for (const auto& channel : part) {
                            if (!channel.note.empty()) {
                                modes << "\tNOTE\t" << channel.label << '\t' << channel.note
                                      << '\n';
                            }
                        }
                    }
                }
            }
        }
    }
    for (const auto& [format, tally] : tallies) {
        std::cout << "== " << format << ": " << tally.files << " files, " << tally.modes
                  << " modes, " << tally.refused << " refused, " << tally.twoPart
                  << " with two parts, " << tally.channels << " channels\n"
                  << "   shutters " << tally.shutters << ", open value found " << tally.open
                  << "; drivable " << tally.drivable << ", shared " << tally.shared
                  << "; default full: dimmers " << tally.defaultFullDimmer << ", colors "
                  << tally.defaultFullColor << "\n";
        for (const std::string& line : tally.refusals) {
            std::cout << "   refused: " << line << "\n";
        }
        for (const std::string& line : tally.twoParts) {
            std::cout << "   two parts: " << line << "\n";
        }
        for (const auto& [note, count] : tally.notes) {
            std::cout << "   note x" << count << ": " << note << "\n";
        }
    }
}
