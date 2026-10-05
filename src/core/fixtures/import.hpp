#pragma once

#include "core/fixtures/definition.hpp"
#include "core/fixtures/fixture_profile.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace takt4::fixtures {

/// A fixture definition file of either format read into a `Definition`: a ".gdtf" as GDTF, a
/// ".json" as the Open Fixture Library's, and any other name by what it starts with — a zip
/// archive's signature, or a JSON object. Never throws.
ReadResult readDefinitionFile(const std::filesystem::path& path);

/// The same, converted to what the library keeps (`mapDefinition`) — or nothing, with `problem`
/// saying why the file could not be read at all.
std::optional<FixtureProfile> importProfile(const std::filesystem::path& path,
                                            std::string& problem);

} // namespace takt4::fixtures
