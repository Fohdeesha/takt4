#pragma once

// What the rule editor offers ready-made: the host presets an address is built from, and
// the rig presets that add a set of rules at once. Out of `rules_controller.cpp` since
// 2026-09-28, as the 2026-09-22 audit suggested.

#include "core/dmx/fixture.hpp"
#include "core/trigger/rule.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
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
/// zero being "no preset". The window's list holds these and adds the one back on
/// (`rig-added(i + 1)`); it used to hold a fifth "add a preset..." entry at the front, which
/// is what a dropdown needs to have something to sit on and what a menu does not.
///
/// The last asks before it adds anything — see `kLiberationPreset`. Last so that the others keep
/// the places a hand used to them reaches for.
inline constexpr std::array<const char*, 5> kRigPresets{
    "Resolume: clips on 3 layers", "Resolume: tempo and resync", "Resolume: breathing dashboard",
    "MIDI: euclidean stabs", "Liberation: lasers…"};

// --- Liberation ---------------------------------------------------------------------------
//
// Liberation's lasers over Art-Net, driven the way the operator's Chataigne module
// drives them (2026-10-05): a zone renders only while it is armed, lit and has a clip, so a clip
// effect sets all three at once (`dmx::EffectKind::Clip`). **Not a rig that can be added blind**:
// how many lasers, which clips, where Liberation is and where its zones are patched are the
// operator's to say, so this entry opens a prompt (`RulesController::openLiberation`) and the
// rules come from `planLiberation`.

/// The Liberation entry of the preset menu, counting from one as `rigPresetRules` does. It adds
/// nothing by itself: it opens the prompt.
inline constexpr std::size_t kLiberationPreset = 5;

/// Up to four lasers (the operator's call: 1 to 4).
inline constexpr int kMaxLasers = 4;

/// When a laser's clip changes, as the prompt offers it. **Staggered by default** — laser 1 every
/// beat, laser 2 every two, laser 3 every bar, laser 4 every two bars (the operator's own
/// stagger) — so four lasers do not all change on one beat.
struct LaserTiming {
    const char* label;
    trigger::Trigger trigger;
    std::uint32_t every;
};
inline constexpr std::array<LaserTiming, 6> kLaserTimings{{
    {"every beat", trigger::Trigger::Beat, 1},
    {"every 2 beats", trigger::Trigger::Beat, 2},
    {"every bar", trigger::Trigger::Bar, 1},
    {"every 2 bars", trigger::Trigger::Bar, 2},
    {"every 4 bars", trigger::Trigger::Bar, 4},
    {"every 8 bars", trigger::Trigger::Bar, 8},
}};

/// How a laser's content moves, when the prompt is asked to move it: a figure round the centre of
/// the zone, or a new random spot each time.
inline constexpr std::array<const char*, 4> kLaserMoves{"circle", "figure 8", "sweep",
                                                        "random spots"};

/// What the prompt asks.
struct LiberationAsk {
    int lasers = 1;
    /// Each laser's clips, as typed: "1-1" to "21-1" — every clip between them in Liberation's
    /// deck order (the operator's example and their choice of reading, 2026-10-05).
    std::array<std::string, kMaxLasers> from{"1-1", "1-1", "1-1", "1-1"};
    std::array<std::string, kMaxLasers> to{"21-1", "21-1", "21-1", "21-1"};
    /// An index into `kLaserTimings` for each.
    std::array<int, kMaxLasers> timing{0, 1, 2, 3};
    /// Where Liberation runs: this computer, unless the operator says otherwise.
    std::string host = "127.0.0.1";
    /// And the port its DMX input listens on: Art-Net's own unless Liberation was told otherwise
    /// (asked beside the IP since 2026-10-06 — *"it has box for IP, but not port"*). A test aims
    /// it at a listener of its own, so nothing it adds reaches a Liberation running here.
    std::uint16_t port = dmx::kArtNetPort;
    /// **Liberation's numbering**, from 1 — what is typed into Liberation's own window. The zones
    /// stack from here 32 channels apart, onto the next universe where a block would cross 512,
    /// as Liberation's own window stacks them.
    int universe = 1;
    int address = 1;
    /// A movement rule per laser too, and how far it may take the content: `amount` percent of
    /// the way from the centre to the zone's edge, kept as each zone's movement window.
    bool move = false;
    int moveShape = 0;
    int amount = 25;
    /// One turn of the figure — or one new spot — every this many bars.
    int bars = 4;
};

/// What the prompt would add, worked out afresh on every edit so the prompt can say where each
/// zone lands and what is wrong before anything is added.
struct LiberationPlan {
    /// Why nothing can be added yet, or empty.
    std::string problem;
    /// The whole patch afterwards: what was there, the zones it reuses, and the new ones.
    std::vector<dmx::Fixture> patch;
    /// The fixture id each laser's rules aim at.
    std::vector<std::string> zones;
    /// One clip rule per laser, and a movement rule after each when asked.
    std::vector<Rule::Config> rules;
    /// Per laser: where its zone is, in Liberation's numbering — "universe 1 · 1-32" — and how
    /// many clips its range holds — "101 clips". Empty where the row cannot be read.
    std::array<std::string, kMaxLasers> where;
    std::array<std::string, kMaxLasers> clips;
    /// Zones the patch had already, at exactly these addresses, used rather than patched again.
    int reused = 0;
    /// What to set up in Liberation for this to light anything, one step to a line.
    std::string instructions;
};

LiberationPlan planLiberation(const LiberationAsk& ask, const std::vector<dmx::Fixture>& patch);

} // namespace takt4::ui::rule_presets
