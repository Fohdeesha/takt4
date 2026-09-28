#include "ui/rule_presets.hpp"

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
        resync.conditions.minConfidence = 0.5; // not while it is still hunting
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
    default:
        return {};
    }
}

} // namespace takt4::ui::rule_presets
