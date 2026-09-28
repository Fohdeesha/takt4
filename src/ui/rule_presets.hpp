#pragma once

// What the rule editor offers ready-made: the host presets an address is built from, and
// the rig presets that add a set of rules at once. Out of `rules_controller.cpp` since
// 2026-09-28, as the 2026-09-22 audit suggested.

#include "core/trigger/rule.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace takt4::ui::rule_presets {

using trigger::Rule;

/// §5.6's host presets: *"Ship presets for Resolume 7, TouchDesigner, MadMapper, QLC+ and a
/// blank custom option. The preset is a starting point the user can edit — never a hardcoded
/// code path."*
///
/// So each one is nothing but an address template and the generators that fill it — the same
/// data an operator could have typed, arrived at by clicking. Applying one leaves every part
/// of it editable, and nothing downstream ever asks which preset a rule came from.
struct HostPreset {
    const char* label;
    /// The address, with a `{}` wherever the host expects a number that varies. Each one
    /// becomes a chip on §5.9's editor, starting on §5.8's default generator — Shuffle over
    /// 1-8 — which the operator then says what they actually meant.
    const char* address;
};

/// Only Resolume's addresses are *verified* — HANDOFF §A.4 checked them against Resolume's
/// own documentation, and §5.6 quotes them. The rest are the shapes those hosts use, and are
/// starting points in the strongest sense: a TouchDesigner address is whatever the operator
/// named their CHOP, so a preset can only offer the convention.
///
/// The first entry is §5.6's *"blank custom option"*, and it means what it says: picking it
/// empties the address and takes the host's press-then-release off with it. It used to change
/// nothing, on the reasoning that an operator who had typed an address and then brushed the
/// picker had not asked to lose it — but the picker did not show which preset the rule *was*
/// either, so going back to "custom" after a Resolume preset left the Resolume address, its
/// two chips and its release sitting under a box that said "custom". Reported from a rig.
/// The picker now follows the address (`presetOf`), so "custom" is a state as well as a
/// choice, and choosing it is choosing an empty one.
inline const std::array<HostPreset, 5> kHostPresets{{
    {"custom", ""},
    {"Resolume 7 — clip", "/composition/layers/{layer}/clips/{clip}/connect"},
    {"Resolume 7 — resync", "/composition/tempocontroller/resync"},
    {"TouchDesigner", "/takt4/{name}"},
    {"MadMapper — cue", "/medias/{cue}/play"},
}};

/// Which preset an address *is*, or 0 for none of them — which is what "custom" means.
///
/// By the address alone: it is the whole of what a preset writes that cannot also have been
/// typed, and an operator who has edited one character of it is no longer on that preset,
/// which is exactly what the picker should then say.
int presetOf(std::string_view address);

/// A release — the fired message again with a different value — after a delay in
/// milliseconds. What §5.6's press-then-release is, and what every preset here wants.
trigger::FollowUp releaseAfterMs(std::int32_t value, double milliseconds);

/// One clip-launching rule for one Resolume layer.
Rule::Config resolumeLayer(int layer, std::uint32_t everyBars, std::uint64_t seed);

/// The rig presets, in the order the picker offers them.
std::vector<Rule::Config> rigPresetRules(std::size_t index);

/// The labels the preset menu shows, in `rigPresetRules`' order — which counts from **one**,
/// zero being "no preset". The window's list holds these four and adds the one back on
/// (`rig-added(i + 1)`); it used to hold a fifth "add a preset..." entry at the front, which
/// is what a dropdown needs to have something to sit on and what a menu does not.
inline constexpr std::array<const char*, 4> kRigPresets{
    "Resolume: clips on 3 layers", "Resolume: tempo and resync", "Resolume: breathing dashboard",
    "MIDI: euclidean stabs"};

} // namespace takt4::ui::rule_presets
