#pragma once

#include "core/trigger/rule.hpp"

#include <nlohmann/json_fwd.hpp>

#include <vector>

namespace takt4::settings {

/// `rulesToJson` and `rulesFromJson` on a document rather than its text, for the settings file,
/// whose rules are part of one document read or written whole: written out and read back in at
/// every load and every autosave's look, they cost a second parse of the largest thing a preset
/// carries — and a read that went down the document once per level for each, which a file nested
/// deep enough ran out of stack on. Inside `core/settings` only, as `rule_json.hpp` says why.
nlohmann::json rulesToNode(const std::vector<trigger::Rule::Config>& rules);
std::vector<trigger::Rule::Config> rulesFromNode(const nlohmann::json& document);

} // namespace takt4::settings
