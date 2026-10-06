#include "ui/rule_presets.hpp"

#include "core/dmx/liberation.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace takt4::ui::rule_presets {

using trigger::Generator;
using trigger::GeneratorKind;

int presetOf(std::string_view address) {
    for (std::size_t i = 1; i < kHostPresets.size(); ++i) {
        if (address == kHostPresets[i].address) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// --- rig presets -------------------------------------------------------------------------
//
// A host preset fills in one rule's address. A *rig* preset builds several rules at once,
// which is a different thing and the one an operator actually starts from: "random clips on
// three layers" is three rules, and nobody wants to build the same rule three times and
// remember to give each a different seed.
//
// Still §5.6's rule about presets — "a starting point the user can edit, never a hardcoded
// code path". Every one of these produces ordinary `Rule::Config`s that the editor then edits
// like any other, and nothing downstream ever asks which preset a rule came from.

trigger::FollowUp releaseAfterMs(std::int32_t value, double milliseconds) {
    trigger::FollowUp entry;
    entry.value = trigger::Value::ofInt(value);
    entry.unit = trigger::DelayUnit::Milliseconds;
    entry.delaySeconds = milliseconds / 1000.0;
    return entry;
}

Rule::Config resolumeLayer(int layer, std::uint32_t everyBars, std::uint64_t seed) {
    Rule::Config rule;
    rule.id = "layer" + std::to_string(layer);
    rule.name = "Layer " + std::to_string(layer) + " — random clip";
    rule.enabled = true; // armed, like every rule the editor makes — see `add`
    rule.trigger = trigger::Trigger::Bar;
    rule.every = everyBars;
    rule.address = "/composition/layers/{layer}/clips/{clip}/connect";

    Generator::Config which;
    which.kind = GeneratorKind::Fixed;
    which.fixed = trigger::Value::ofInt(layer);

    Generator::Config clip;
    clip.kind = GeneratorKind::Shuffle; // §5.8's default, and why: repeats read as bugs
    clip.low = 1;
    clip.high = 8;
    clip.noRepeatWithin = 2;
    rule.segments = {which, clip};

    rule.value.kind = GeneratorKind::Fixed;
    rule.value.fixed = trigger::Value::ofInt(1);
    // §7.4: connect is a mouse click. Without the release the clip stays held.
    rule.followUps.push_back(releaseAfterMs(0, 50));
    rule.seed = seed;
    return rule;
}

std::vector<Rule::Config> rigPresetRules(std::size_t index) {
    switch (index) {
    case 1: {
        // The ask this was built for: random clips on three Resolume layers at once. Each
        // layer gets its own rule so each can be switched off, re-timed or re-routed on its
        // own — and its own seed, or all three would fire the same clip as each other, which
        // is `Generator::Config::seed`'s whole reason.
        //
        // Staggered periods rather than three identical ones: 4, 8 and 16 bars means the
        // three layers change at different times and the combination keeps moving. Three
        // layers all changing on the same downbeat is one event, not three.
        //
        // **Highest layer first, because that is the way Resolume draws them.** Resolume
        // stacks layer 3 above 2 above 1, so a list running 1, 2, 3 down the screen is the
        // operator's rig upside down — reported on 2026-09-12 as "the order it adds them is
        // backwards, which can be confusing". The layer *numbers*, their periods and their
        // seeds are unchanged; only the order they are added in is.
        return {resolumeLayer(3, 16, 303), resolumeLayer(2, 8, 202), resolumeLayer(1, 4, 101)};
    }
    case 2: {
        // Resolume's own tempo, kept in step with the tracker. §5.6 and §A.4 verified both
        // addresses: the tempo is "float, normalised 0-1 across 20-500 BPM", which is exactly
        // what the `Live` generator's `BpmNormalised` source exists for.
        Rule::Config tempo;
        tempo.id = "tempo";
        tempo.name = "Resolume tempo follows takt4";
        tempo.enabled = true;
        tempo.trigger = trigger::Trigger::TempoChange;
        tempo.address = "/composition/tempocontroller/tempo";
        tempo.value.kind = GeneratorKind::Live;
        tempo.value.source = trigger::LiveSource::BpmNormalised;
        tempo.value.normaliseLow = 20.0;
        tempo.value.normaliseHigh = 500.0;
        tempo.seed = 404;

        Rule::Config resync;
        resync.id = "resync";
        resync.name = "Resync Resolume when the lock returns";
        resync.enabled = true;
        resync.trigger = trigger::Trigger::LockChange;
        resync.address = "/composition/tempocontroller/resync";
        resync.value.kind = GeneratorKind::Fixed;
        resync.value.fixed = trigger::Value::ofInt(1);
        // Not while it is still hunting — and B switched on for it, since a condition set with B
        // off is one kept for later and not one that applies.
        resync.conditionsOn = true;
        resync.conditions.minConfidence = 0.5;
        resync.seed = 505;
        return {tempo, resync};
    }
    case 3: {
        // Something that *moves* rather than jumping: a dashboard parameter breathing over
        // four bars, locked to the downbeat. This is what `Ramp` is for, and the one rule
        // here that fires on every beat — a ramp is only as smooth as it is sampled.
        Rule::Config breathe;
        breathe.id = "breathe";
        breathe.name = "Dashboard breathes over 4 bars";
        breathe.enabled = true;
        breathe.trigger = trigger::Trigger::Beat;
        breathe.address = "/composition/dashboard/link1";
        breathe.value.kind = GeneratorKind::Ramp;
        breathe.value.shape = trigger::RampShape::Sine;
        breathe.value.rampBars = 4;
        breathe.value.rampFloat = true;
        breathe.value.low = 0;
        breathe.value.high = 1;
        breathe.seed = 606;
        return {breathe};
    }
    case 4: {
        // A Euclidean pattern out to MIDI: three hits over eight beats, the tresillo, on a
        // note a lighting desk or a sampler can learn. The rhythm nothing else here can make.
        Rule::Config stabs;
        stabs.id = "stabs";
        stabs.name = "Euclidean stabs — 3 in 8";
        stabs.enabled = true;
        stabs.trigger = trigger::Trigger::Euclid;
        stabs.every = 8;
        stabs.pulses = 3;
        stabs.sendKind = trigger::Message::Kind::MidiNote;
        stabs.channel = 10; // where a drum map lives on most hardware
        stabs.number.kind = GeneratorKind::Shuffle;
        stabs.number.low = 36;
        stabs.number.high = 43;
        stabs.value.kind = GeneratorKind::Fixed;
        stabs.value.fixed = trigger::Value::ofInt(110);
        // A real Note Off eighty milliseconds later, on whichever note the shuffle drew —
        // which is what a release inherits and nobody could type. See `trigger::FollowUp`.
        stabs.followUps.push_back(releaseAfterMs(0, 80));
        stabs.seed = 707;
        return {stabs};
    }
    case kLiberationPreset:
        // Asks first: see `planLiberation`.
        return {};
    default:
        return {};
    }
}

namespace {

namespace liberation = dmx::liberation;

/// "laser 2", for a message about one row of the prompt.
std::string laserName(int laser) {
    return "laser " + std::to_string(laser + 1);
}

/// "1, 33 and 65".
std::string spoken(const std::vector<std::string>& items) {
    std::string text;
    for (std::size_t i = 0; i < items.size(); ++i) {
        text += i == 0 ? "" : i + 1 == items.size() ? " and " : ", ";
        text += items[i];
    }
    return text;
}

Rule::Config clipRuleFor(int laser, const std::string& zone, int low, int high,
                         const LaserTiming& timing) {
    Rule::Config rule;
    rule.id = "laser" + std::to_string(laser + 1);
    rule.name = "Laser " + std::to_string(laser + 1) + " — Liberation clips";
    rule.enabled = true;
    rule.trigger = timing.trigger;
    rule.every = timing.every;
    rule.sendKind = trigger::Message::Kind::Dmx;
    rule.dmx.effect = dmx::EffectKind::Clip;
    rule.dmx.fixtures = {zone};
    rule.dmx.clip = trigger::clipShuffle(liberation::clipAt(low), liberation::clipAt(high));
    rule.dmx.level = trigger::fixedNumber(255);
    rule.seed = 9101 + static_cast<std::uint64_t>(laser);
    return rule;
}

Rule::Config moveRuleFor(int laser, const std::string& zone, int shape, int bars) {
    Rule::Config rule;
    rule.id = "laser" + std::to_string(laser + 1) + "-move";
    rule.name =
        "Laser " + std::to_string(laser + 1) + " — " + kLaserMoves[static_cast<std::size_t>(shape)];
    rule.enabled = true;
    // Re-fired every turn, so it never stops: a figure that ends where it began, started again.
    rule.trigger = trigger::Trigger::Bar;
    rule.every = static_cast<std::uint32_t>(bars);
    rule.sendKind = trigger::Message::Kind::Dmx;
    rule.dmx.fixtures = {zone};
    rule.dmx.unit = trigger::DelayUnit::Bars;
    rule.dmx.durationBeats = static_cast<double>(bars);
    if (shape == 3) {
        // A new random spot in the zone's window each time, glided to over the whole period, so
        // it is always on its way somewhere.
        rule.dmx.effect = dmx::EffectKind::Position;
        rule.dmx.curve = dmx::Curve::EaseInOut;
        trigger::Generator::Config anywhere;
        anywhere.kind = trigger::GeneratorKind::Random;
        anywhere.pool = trigger::Pool::Range;
        anywhere.low = 0;
        anywhere.high = 100;
        anywhere.noRepeatWithin = 0;
        rule.dmx.pan = anywhere;
        rule.dmx.tilt = anywhere;
    } else {
        rule.dmx.effect = dmx::EffectKind::Path;
        rule.dmx.curve = dmx::Curve::Linear;
        rule.dmx.shape = shape == 0   ? dmx::PathShape::Circle
                         : shape == 1 ? dmx::PathShape::Figure8
                                      : dmx::PathShape::Sweep;
        // The whole window, which is the amount the operator asked for.
        rule.dmx.size = 1.0;
        rule.dmx.cycles = 1.0;
    }
    rule.seed = 9201 + static_cast<std::uint64_t>(laser);
    return rule;
}

} // namespace

LiberationPlan planLiberation(const LiberationAsk& ask, const std::vector<dmx::Fixture>& patch) {
    LiberationPlan plan;
    plan.patch = patch;
    const int lasers = std::clamp(ask.lasers, 1, kMaxLasers);
    const auto fail = [&plan](std::string why) {
        if (plan.problem.empty()) {
            plan.problem = std::move(why);
        }
    };

    // Where each zone goes: 32 channels apart from the first, onto the next universe where a
    // block would cross channel 512 — which is how Liberation's own window stacks profiles.
    int universe = std::clamp(ask.universe, 1, static_cast<int>(dmx::kMaxPortAddress) + 1);
    int address = std::clamp(ask.address, 1, static_cast<int>(dmx::kChannelsPerUniverse));
    const std::size_t before = plan.patch.size();
    std::vector<std::size_t> touched;
    struct Placed {
        int universe;
        int address;
    };
    std::vector<Placed> placed;
    for (int laser = 0; laser < lasers; ++laser) {
        if (address + static_cast<int>(liberation::kZoneChannels) - 1 >
            static_cast<int>(dmx::kChannelsPerUniverse)) {
            ++universe;
            address = 1;
        }
        if (universe > static_cast<int>(dmx::kMaxPortAddress) + 1) {
            fail("the zones run past the last universe there is");
            break;
        }
        placed.push_back(Placed{universe, address});
        const auto at = static_cast<std::size_t>(laser);
        plan.where[at] = "universe " + std::to_string(universe) + " · " + std::to_string(address) +
                         "-" + std::to_string(address + 31);

        const dmx::PortAddress port = liberation::portAddressOf(universe);
        const auto existing = std::find_if(plan.patch.begin(),
                                           plan.patch.begin() + static_cast<std::ptrdiff_t>(before),
                                           [&](const dmx::Fixture& fixture) {
                                               return liberation::isZone(fixture) &&
                                                      fixture.universe == port &&
                                                      fixture.address == address;
                                           });
        if (existing != plan.patch.begin() + static_cast<std::ptrdiff_t>(before)) {
            // A zone already patched here is this laser's: used as it is, and switched on, since
            // driving it is what was asked.
            existing->enabled = true;
            plan.zones.push_back(existing->id);
            touched.push_back(static_cast<std::size_t>(existing - plan.patch.begin()));
            ++plan.reused;
        } else {
            dmx::Fixture zone =
                liberation::zone(laserName(laser), port, static_cast<std::uint16_t>(address));
            zone.id = dmx::newFixtureId(plan.patch);
            zone.group = "lasers";
            plan.zones.push_back(zone.id);
            touched.push_back(plan.patch.size());
            plan.patch.push_back(std::move(zone));
        }
        address += static_cast<int>(liberation::kZoneChannels);
    }

    // Each laser's clips, read as Liberation names them.
    std::array<std::pair<int, int>, kMaxLasers> ranges{};
    for (int laser = 0; laser < lasers; ++laser) {
        const auto at = static_cast<std::size_t>(laser);
        const std::optional<liberation::Clip> from = liberation::parseClip(ask.from[at]);
        const std::optional<liberation::Clip> to = liberation::parseClip(ask.to[at]);
        if (!from || !to) {
            const std::string& bad = !from ? ask.from[at] : ask.to[at];
            fail(
                laserName(laser) + ": " +
                (bad.empty() ? std::string("a clip is missing") : "\"" + bad + "\" is not a clip") +
                " — type it as Liberation names it, the column then the row (0 to 4), like 21-1");
            continue;
        }
        const int low = std::min(liberation::indexOf(*from), liberation::indexOf(*to));
        const int high = std::max(liberation::indexOf(*from), liberation::indexOf(*to));
        ranges[at] = {low, high};
        const int count = high - low + 1;
        plan.clips[at] = std::to_string(count) + (count == 1 ? " clip" : " clips");
    }

    std::string host = ask.host;
    host.erase(0, host.find_first_not_of(" \t"));
    host.erase(host.find_last_not_of(" \t") + 1);
    if (host.empty() || host.find(' ') != std::string::npos) {
        fail("type the address of the computer Liberation runs on — 127.0.0.1 if it is this one");
    }

    // The amount each zone may move, as its window: centre ± half of it either side.
    const int amount = std::clamp(ask.amount, 1, 100);
    if (ask.move) {
        for (const std::size_t index : touched) {
            dmx::Fixture& zone = plan.patch[index];
            zone.panMin = zone.tiltMin = 0.5 - amount / 200.0;
            zone.panMax = zone.tiltMax = 0.5 + amount / 200.0;
        }
    }

    // Nothing else of the patch may sit on a zone's channels.
    for (const dmx::Overlap& overlap : dmx::overlappingFixtures(plan.patch)) {
        const auto zoneAt = [&touched](std::size_t index) {
            return std::find(touched.begin(), touched.end(), index) != touched.end();
        };
        if (!zoneAt(overlap.first) && !zoneAt(overlap.second)) {
            continue; // a clash the rig had before this, and not this prompt's to judge
        }
        const std::size_t zone = zoneAt(overlap.first) ? overlap.first : overlap.second;
        const std::size_t other = zone == overlap.first ? overlap.second : overlap.first;
        const auto laser =
            static_cast<int>(std::find(touched.begin(), touched.end(), zone) - touched.begin());
        // Both numberings: the prompt's is Liberation's, and the fixture in the way is found in
        // takt4's patch editor by takt4's.
        const dmx::PortAddress shared = plan.patch[zone].universe;
        fail(laserName(laser) + " would share channel " + std::to_string(overlap.channel) +
             " with " + plan.patch[other].name + " (universe " +
             std::to_string(liberation::liberationUniverseOf(shared)) + " here, " +
             dmx::describePortAddress(shared) +
             " in takt4's patch) — start the zones at another universe or address");
    }

    if (!plan.problem.empty()) {
        plan.rules.clear();
    } else {
        for (int laser = 0; laser < lasers; ++laser) {
            const auto at = static_cast<std::size_t>(laser);
            const int timing =
                std::clamp(ask.timing[at], 0, static_cast<int>(kLaserTimings.size()) - 1);
            plan.rules.push_back(clipRuleFor(laser, plan.zones[at], ranges[at].first,
                                             ranges[at].second,
                                             kLaserTimings[static_cast<std::size_t>(timing)]));
            if (ask.move) {
                const int shape =
                    std::clamp(ask.moveShape, 0, static_cast<int>(kLaserMoves.size()) - 1);
                plan.rules.push_back(
                    moveRuleFor(laser, plan.zones[at], shape, std::clamp(ask.bars, 1, 64)));
            }
        }
    }

    // What to do in Liberation, in its own words and numbering.
    std::vector<std::string> spots;
    bool oneUniverse = true;
    for (const Placed& one : placed) {
        oneUniverse = oneUniverse && one.universe == placed.front().universe;
    }
    for (const Placed& one : placed) {
        spots.push_back(oneUniverse ? std::to_string(one.address)
                                    : "universe " + std::to_string(one.universe) + " address " +
                                          std::to_string(one.address));
    }
    const std::string profiles =
        std::to_string(placed.size()) + (placed.size() == 1 ? " profile" : " profiles");
    plan.instructions =
        "1.  Liberation > Settings > DMX Input Settings: tick Enable DMX Input, protocol Art-Net" +
        (ask.port != dmx::kArtNetPort ? ", port " + std::to_string(ask.port) : std::string()) +
        ".\n"
        "2.  View > DMX Input Settings: add " +
        profiles + ", Extended 32ch, " +
        (placed.empty() ? std::string()
         : oneUniverse  ? "universe " + std::to_string(placed.front().universe) +
                             (placed.size() == 1 ? ", address " : ", addresses ") + spoken(spots)
                       : spoken(spots)) +
        ".\n"
        "3.  Set Liberation's tempo source to Ableton Link — takt4 switches its own Link on.";
    return plan;
}

} // namespace takt4::ui::rule_presets
