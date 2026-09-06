#pragma once

#include "core/trigger/rule.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace takt4::settings {

/// HANDOFF §5.8's rules as text, for Q7's portable half.
///
/// A file of its own rather than more of `settings.cpp`, because a rule is an order of
/// magnitude larger than anything else that travels: `Rule::Config` has eighteen fields, and
/// three of them are whole `Generator::Config`s with a list inside. Keeping it here leaves
/// `settings.cpp` readable as the description of the *file* it still is.
///
/// It lives in `core/settings` rather than in `core/trigger` for the reason
/// `tracking::TempoTracker::Options` does: the tracker and the rules describe behaviour, and
/// which characters that behaviour is written down as is this layer's business, not theirs.
/// It also keeps `<nlohmann/json.hpp>` out of `core/trigger`, which anything including
/// RTNeural must stay clear of (§6 — the two bundle different copies of it).
///
/// **These follow `settings::load`'s contract exactly: reading never fails.** Anything
/// missing keeps its default, anything of the wrong type is ignored, and a rule that comes
/// back unusable is *still a rule* — held, shown and refused at fire time by
/// `Rule::problem()`, which is §5.8's stated policy and the opposite of a generator's.
/// A preset with one broken rule in it must still open, with the other rules working and the
/// broken one visible enough to fix.

/// Every rule, as the array a preset holds. Pretty-printing is the caller's.
std::string rulesToJson(const std::vector<trigger::Rule::Config>& rules);

/// The inverse. Never throws; an entry that is not an object is skipped rather than
/// aborting the rest, so one bad line does not cost an operator their whole rule set.
std::vector<trigger::Rule::Config> rulesFromJson(std::string_view text);

} // namespace takt4::settings
