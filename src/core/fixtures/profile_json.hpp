#pragma once

#include "core/fixtures/fixture_profile.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace takt4::fixtures {

/// The preset's library as JSON text — an array of entries, each channel a short array
/// `[role, label, parked]` with its note as a fourth element when it has one:
///
///     {"id": "p-1a2b3c4d", "format": "gdtf", "key": "773EA46D-…", "manufacturer": "…",
///      "model": "…", "revision": "…", "file": "…", "digest": "9f2c4e0a1b7d3365", "notes": [],
///      "modes": [{"name": "10-Ch", "parts": [[["dimmer", "dimmer", 0], …]], "notes": [],
///                 "refused": ""}]}
///
/// Text rather than a JSON object in the header, as `rule_json` does it: nothing outside
/// `core/fixtures` and `settings` needs to know which JSON library this is.
std::string libraryToJson(const std::vector<FixtureProfile>& library);

/// What `libraryToJson` wrote. **Never throws**, and an entry it cannot read is left out rather
/// than costing the rest; one with no id or a duplicate id is given a fresh one. Every digest is
/// recomputed from what was read.
std::vector<FixtureProfile> libraryFromJson(std::string_view text);

/// FNV-1a 64 over the canonical JSON of the profile's format, key, manufacturer, model,
/// revision and modes — the id, the file name, the notes and the digest itself left out — as 16
/// hex digits. The same definition imported twice has the same digest; a definition whose
/// channels changed in any way has another.
std::string digestOf(const FixtureProfile& profile);

/// A fresh library id ("p-" and eight hex digits) that no entry of `library` has.
std::string newProfileId(const std::vector<FixtureProfile>& library);

} // namespace takt4::fixtures
