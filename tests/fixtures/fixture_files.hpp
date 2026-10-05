#pragma once

// Building fixture definitions for the import tests: GDTF descriptions written in the test
// itself, zipped in memory, and the committed Open Fixture Library files.

#include "core/fixtures/definition.hpp"
#include "core/fixtures/gdtf_reader.hpp"
#include "core/fixtures/ofl_reader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <miniz.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace takt4::test {

/// A zip archive of `members` (name, bytes), stored or deflated.
inline std::vector<std::uint8_t>
zipOf(const std::vector<std::pair<std::string, std::string>>& members, bool deflate = true) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, bytes] : members) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(),
                                      deflate ? MZ_DEFAULT_LEVEL : MZ_NO_COMPRESSION));
    }
    void* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
    std::vector<std::uint8_t> out(static_cast<std::uint8_t*>(buffer),
                                  static_cast<std::uint8_t*>(buffer) + size);
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return out;
}

/// Where each central-directory and local header of `archive` starts, by the signatures zip
/// writes them with — for tests that break an archive on purpose.
inline std::vector<std::size_t> headersOf(const std::vector<std::uint8_t>& archive,
                                          std::uint32_t signature) {
    std::vector<std::size_t> out;
    for (std::size_t at = 0; at + 4 <= archive.size(); ++at) {
        std::uint32_t word = 0;
        std::memcpy(&word, archive.data() + at, 4);
        if (word == signature) {
            out.push_back(at);
        }
    }
    return out;
}
inline constexpr std::uint32_t kLocalHeader = 0x04034b50;
inline constexpr std::uint32_t kCentralHeader = 0x02014b50;

/// A GDTF description: `geometries` and `modes` inside a fixture type of the given identity.
inline std::string gdtfDocument(
    const std::string& geometries, const std::string& modes, const std::string& version = "1.2",
    const std::string& identity =
        R"(Name="Test Par" Manufacturer="takt4 tests" FixtureTypeID="0e6f7c1a-1111-4222-8333-444455556666")",
    const std::string& rest = "") {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<GDTF DataVersion=\"" + version +
           "\">\n<FixtureType " + identity + ">\n<AttributeDefinitions/>\n<Geometries>\n" +
           geometries + "\n</Geometries>\n<DMXModes>\n" + modes + "\n</DMXModes>\n" + rest +
           "\n</FixtureType>\n</GDTF>\n";
}

/// One DMX channel with a single logical channel and a single function of `attribute`.
inline std::string gdtfChannel(const std::string& geometry, const std::string& offset,
                               const std::string& attribute, const std::string& extra = "",
                               const std::string& functionExtra = R"(DMXFrom="0/1")") {
    return "<DMXChannel Geometry=\"" + geometry + "\" Offset=\"" + offset + "\" " + extra +
           "><LogicalChannel Attribute=\"" + attribute + "\"><ChannelFunction Attribute=\"" +
           attribute + "\" Name=\"" + attribute + " 1\" " + functionExtra +
           "/></LogicalChannel></DMXChannel>\n";
}

inline std::string gdtfMode(const std::string& name, const std::string& geometry,
                            const std::string& channels) {
    return "<DMXMode Name=\"" + name + "\" Geometry=\"" + geometry + "\"><DMXChannels>\n" +
           channels + "</DMXChannels></DMXMode>\n";
}

inline fixtures::Definition readGdtf(const std::string& xml) {
    const fixtures::ReadResult result = fixtures::readGdtfDescription(xml, "test.gdtf");
    INFO(result.problem);
    REQUIRE(result.definition);
    return *result.definition;
}

inline std::filesystem::path oflFile(const std::string& relative) {
    return std::filesystem::path(TAKT4_TEST_DATA_DIR) / "fixtures" / "ofl" / relative;
}

inline fixtures::Definition readOfl(const std::string& relative) {
    const fixtures::ReadResult result = fixtures::readOflFile(oflFile(relative));
    INFO(relative << ": " << result.problem);
    REQUIRE(result.definition);
    return *result.definition;
}

inline fixtures::Definition readOflJson(const std::string& json) {
    const fixtures::ReadResult result =
        fixtures::readOflText(json, "test.json", "maker", "fixture");
    INFO(result.problem);
    REQUIRE(result.definition);
    return *result.definition;
}

inline const fixtures::DefMode& modeNamed(const fixtures::Definition& definition,
                                          const std::string& name) {
    for (const fixtures::DefMode& mode : definition.modes) {
        if (mode.name == name) {
            return mode;
        }
    }
    FAIL("no mode named " << name);
    return definition.modes.front();
}

/// The labels of a mode's first part, in address order.
inline std::vector<std::string> labelsOf(const fixtures::DefMode& mode, std::size_t part = 0) {
    std::vector<std::string> out;
    REQUIRE(part < mode.parts.size());
    for (const fixtures::DefChannel& channel : mode.parts[part]) {
        out.push_back(channel.label);
    }
    return out;
}

} // namespace takt4::test
