#pragma once

#include "core/fixtures/fixture_profile.hpp"

#include <nlohmann/json_fwd.hpp>

#include <vector>

namespace takt4::fixtures {

/// `libraryToJson` and `libraryFromJson` on a document rather than its text — for the settings
/// file, which reads and writes its library as part of one document (see `rule_json_node.hpp`).
/// Inside `core` only.
nlohmann::json libraryToNode(const std::vector<FixtureProfile>& library);
std::vector<FixtureProfile> libraryFromNode(const nlohmann::json& document);

} // namespace takt4::fixtures
