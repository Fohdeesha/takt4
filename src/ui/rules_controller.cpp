#include "ui/rules_controller.hpp"

#include "core/features/intensity.hpp"
#include "core/io/utf8.hpp"
#include "core/trigger/generator.hpp"
#include "ui/model_rows.hpp"
#include "ui/native_window.hpp"
#include "ui/window_state.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace takt4::ui {
namespace {

using trigger::Generator;
using trigger::GeneratorKind;
using trigger::Rule;

/// Holds `RulesController::pickingColor_` up for one call, so that everything a picker's own
/// slider publishes is marked as coming from that slider. Scoped rather than a pair of
/// assignments because the publishers it guards can raise a status, throw, or return early.
class PickingColor {
public:
    explicit PickingColor(bool& flag) noexcept : flag_(flag), was_(flag) { flag_ = true; }
    ~PickingColor() { flag_ = was_; }

    PickingColor(const PickingColor&) = delete;
    PickingColor& operator=(const PickingColor&) = delete;

private:
    bool& flag_;
    bool was_;
};

/// Every string this editor shows, made safe to show — see `io::validUtf8`. A target is named
/// after its MIDI device unless somebody named it, and that name is the driver's, in whatever
/// encoding the driver used; one byte Slint cannot decode is an abort.
slint::SharedString shared(const std::string& text) {
    return slint::SharedString(io::validUtf8(text));
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    return text;
}

/// Everything between separators, trimmed, empties dropped. What turns "3, 7, 1, 12" into a
/// sequence and " 70 - 140 " into a range, which is the same job twice.
std::vector<std::string_view> split(std::string_view text, std::string_view separators) {
    std::vector<std::string_view> parts;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t next = text.find_first_of(separators, at);
        const std::string_view part = trim(
            text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (next == std::string_view::npos) {
            break;
        }
        at = next + 1;
    }
    return parts;
}

/// A number typed into a box, or nothing. **Only a finite one**: `from_chars` reads "nan" and
/// "inf" as numbers, and a duration of inf made an effect that never ended, a hue of nan was
/// undefined behaviour in `fromHsv`, and a BPM range of nan never fired (the 2026-09-25
/// audit's L5).
std::optional<double> readNumber(std::string_view text) noexcept {
    double value = 0.0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

/// A finite number as an `int32_t`, clamped to its range: converting one past it is
/// undefined behaviour, and "1e12" is a finite number (L5).
std::int32_t clampedInt(double value) noexcept {
    return static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0));
}

/// What a number box's keystrokes say it holds. The markup has already clamped it and spelled
/// it as a whole number (`NumberBox::typed`), so anything else is a box nobody meant.
std::optional<int> typedNumber(std::string_view text) noexcept {
    const std::optional<double> number = readNumber(trim(text));
    if (!number || !std::isfinite(*number)) {
        return std::nullopt;
    }
    return static_cast<int>(std::clamp(std::round(*number), -1.0e9, 1.0e9));
}

/// One entry of a list an operator typed.
///
/// **Type is inferred from how it is written**, which is the same rule the preset file uses
/// and the reason a list can hold more than clip numbers: `7` is an int, `0.5` a float,
/// `intro` text. Nothing here refuses — an entry that is not a number is a name, and names
/// are exactly what an address segment often is.
trigger::Value parseValue(std::string_view text) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) {
        return trigger::Value{};
    }
    if (trimmed == "true") {
        return trigger::Value::ofBool(true);
    }
    if (trimmed == "false") {
        return trigger::Value::ofBool(false);
    }
    if (const std::optional<double> number = readNumber(trimmed)) {
        // A whole number typed without a point is an int, because a clip index has to come
        // back as one. `3.0` is a float, which is what writing the point asked for.
        if (trimmed.find('.') == std::string_view::npos &&
            trimmed.find('e') == std::string_view::npos && *number >= -2147483648.0 &&
            *number <= 2147483647.0) {
            return trigger::Value::ofInt(static_cast<std::int32_t>(*number));
        }
        // Clamped to what a float holds, which converting past is undefined behaviour (L5).
        const double most = std::numeric_limits<float>::max();
        return trigger::Value::ofFloat(static_cast<float>(std::clamp(*number, -most, most)));
    }
    return trigger::Value::ofText(trimmed);
}

/// The inverse, as the field shows it back: "3, 7, 1, 12".
std::string spellValues(const std::vector<trigger::Value>& values) {
    std::string text;
    for (const trigger::Value& value : values) {
        if (!text.empty()) {
            text += ", ";
        }
        value.appendTo(text);
    }
    return text;
}

std::string spellValue(const trigger::Value& value) {
    std::string text;
    value.appendTo(text);
    return text;
}

/// A number with no trailing zeroes, for a field an operator will edit rather than read.
std::string spellNumber(double value) {
    if (value == std::floor(value) && std::abs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    std::string text = std::to_string(value);
    while (text.size() > 1 && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

/// A weighted generator's list as its box shows it: "7:3, 12:1".
std::string spellWeights(const std::vector<trigger::WeightedChoice>& choices) {
    std::string text;
    for (const trigger::WeightedChoice& choice : choices) {
        if (!text.empty()) {
            text += ", ";
        }
        choice.value.appendTo(text);
        text += ":" + spellNumber(choice.weight);
    }
    return text;
}

/// `staleRows`, added to `into`: whether any row of `model` has to be built again. The
/// indices are kept for `rebuildRows` to renew one at a time.
template <typename Row, typename Changed>
bool markStale(const slint::VectorModel<Row>& model, const std::vector<Row>& rows,
               std::vector<std::size_t>& into, Changed changed) {
    const std::vector<std::size_t> stale = staleRows(model, rows, changed);
    into.insert(into.end(), stale.begin(), stale.end());
    return !stale.empty();
}

/// The live interval multiplier in the words an operator thinks in, or **empty** at 1.
///
/// The multiplier is on the *interval*, so 2 is half as often — which is exactly backwards
/// from how it reads as a number, and is why this says "half as often" rather than "×2". A
/// rule nobody has touched says nothing at all, because a permanent "×1" beside every rule in
/// the list would be noise.
std::string describeRate(double factor) {
    if (factor == 1.0) {
        return {};
    }
    if (factor > 1.0) {
        return spellNumber(factor) + "× slower";
    }
    return spellNumber(1.0 / factor) + "× faster";
}

/// A comma-separated list, as the routing field shows it back.
std::string join(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        if (!text.empty()) {
            text += ", ";
        }
        text += name;
    }
    return text;
}

/// Whether a rule that sends `kind` can reach a target of `target`'s kind **at all**.
///
/// **A UI question with a wire answer.** `RuleSink::sendMidi` walks the targets and asks each
/// for a MIDI port; `sendOsc` asks `OscPublisher` which of them are OSC. So a MIDI rule routed
/// to an OSC target already sends nothing, and an Art-Net target is named by no rule at
/// all — `OutputTarget::Kind::ArtNet` says so: a lighting rule picks its *fixtures*, and the
/// fixture says which universe it is in, so the node never has to be chosen.
///
/// The editor did not know any of that and offered all three lists to all three kinds.
/// Reported from a rig on 2026-09-16: *"why is my rdm10 artnet destination showing up as an
/// option for midi and OSC output!?"* A choice that cannot do anything is worse than no
/// choice: ticking it looks like routing and is silence.
bool targetTakes(trigger::Message::Kind kind, const output::OutputTarget& target) noexcept {
    if (trigger::isDmx(kind)) {
        return false; // the patch routes these; the editor hides the list entirely
    }
    return trigger::isMidi(kind) ? target.kind == output::OutputTarget::Kind::Midi
                                 : target.kind == output::OutputTarget::Kind::Osc;
}

/// What a rule's routing currently reaches, beside the field the names were typed into.
///
/// Three different things worth saying, and they are not interchangeable. A rule that names
/// nothing goes everywhere and should say which "everywhere" is, or an operator who has just
/// added a second output has no way to know the rule now hits it too. A name that matches
/// nothing is the one real mistake here — and is *kept* rather than corrected, because a
/// preset from another rig should still say what it meant (`Rule::Config::outputs`), so
/// saying so is the only way it gets noticed.
///
/// **A switched-off output is not reached**, and is said: it sends nothing, and counting it
/// said "reaches 2 outputs" over a rule that reached one (the 2026-09-25 audit's L12).
std::string describeRouting(trigger::Message::Kind kind, const std::vector<std::string>& ids,
                            const std::vector<output::OutputTarget>& targets) {
    // Only what this kind can reach — the same filter the tick list uses, because a line
    // reading "every output: RDM10" over a MIDI rule names a node the rule can never send to.
    std::vector<const output::OutputTarget*> reachable;
    for (const output::OutputTarget& target : targets) {
        if (targetTakes(kind, target)) {
            reachable.push_back(&target);
        }
    }
    if (reachable.empty()) {
        return targets.empty()
                   ? "no outputs yet"
                   : std::string("no ") + (trigger::isMidi(kind) ? "MIDI" : "OSC") + " output yet";
    }
    std::string off;
    const auto noteOff = [&off](const output::OutputTarget& target) {
        off += off.empty() ? "" : ", ";
        off += target.name;
    };
    const auto switchedOff = [&off] {
        return off + (off.find(',') == std::string::npos ? " is" : " are") + " switched off";
    };
    if (ids.empty()) {
        std::string all;
        for (const output::OutputTarget* target : reachable) {
            if (!target->enabled) {
                noteOff(*target);
                continue;
            }
            all += all.empty() ? "" : ", ";
            all += target->name;
        }
        if (all.empty()) {
            return "every output: " + switchedOff();
        }
        return "every output: " + all + (off.empty() ? "" : "; " + switchedOff());
    }
    // Two ways a routed output can reach nothing, and they are different mistakes. One that
    // has been deleted is gone and can only be un-ticked. One this rig *has* on the wrong kind
    // of target — a MIDI rule still routed to an OSC feed after the send kind was changed — is
    // a routing the operator has to undo, and saying "gone" over an output plainly there would
    // read as a bug in takt4.
    std::size_t gone = 0;
    std::string wrongKind;
    std::size_t reached = 0;
    for (const std::string& id : ids) {
        const output::OutputTarget* const target = output::findTarget(targets, id);
        if (target != nullptr && targetTakes(kind, *target) && !target->enabled) {
            noteOff(*target);
        } else if (target != nullptr && targetTakes(kind, *target)) {
            ++reached;
        } else if (target != nullptr) {
            wrongKind += wrongKind.empty() ? "" : ", ";
            wrongKind += target->name;
        } else {
            ++gone;
        }
    }
    std::string trouble;
    if (gone > 0) {
        trouble = gone == 1 ? "1 output it was routed to is gone"
                            : std::to_string(gone) + " outputs it was routed to are gone";
    }
    if (!wrongKind.empty()) {
        trouble += trouble.empty() ? "" : "; ";
        trouble += wrongKind +
                   (wrongKind.find(',') == std::string::npos ? " is not " : " are not ") +
                   (trigger::isMidi(kind) ? "MIDI" : "OSC");
    }
    if (!off.empty()) {
        trouble += trouble.empty() ? "" : "; ";
        trouble += switchedOff();
    }
    if (trouble.empty()) {
        return "reaches " + std::to_string(reached) + (reached == 1 ? " output" : " outputs");
    }
    return reached == 0 ? trouble
                        : "reaches " + std::to_string(reached) +
                              (reached == 1 ? " output; " : " outputs; ") + trouble;
}

/// What a MIDI rule's value starts at: a velocity that is plainly heard without being the
/// loudest, and the centre of a pitch bend, which is no bend at all.
constexpr std::int32_t kVelocity = 100;
constexpr std::int32_t kBendRest = 8192;

/// What a kind calls the number it carries — "note 96" and "program 96" are different
/// instructions, and the label beside the box is the only thing that says which is about to
/// go out. Used by the generator chips and by the THEN SEND rows, so the two cannot disagree.
const char* numberLabelOf(trigger::Message::Kind kind) {
    switch (kind) {
    case trigger::Message::Kind::MidiCc:
        return "cc";
    case trigger::Message::Kind::MidiProgramChange:
        return "program";
    default:
        return "note"; // note on and note off are both a note
    }
}

/// And what it calls the value — a velocity, a bend, or just a value.
const char* valueLabelOf(trigger::Message::Kind kind) {
    switch (kind) {
    case trigger::Message::Kind::MidiNote:
    case trigger::Message::Kind::MidiNoteOff:
        return "velocity";
    case trigger::Message::Kind::MidiPitchBend:
        return "bend";
    case trigger::Message::Kind::Dmx:
        return "level"; // a lighting release's value is the brightness it lets go to
    default:
        return "value"; // OSC's argument and a CC's value are both just a value
    }
}

/// What kind a *release* of `sent` actually goes out as — the one place the inheritance is
/// spelled, so the label, the value label and `Rule::followUpsFor` cannot drift apart. A note
/// on releases as a real Note Off (see `trigger::Message::Kind`); everything else releases as
/// itself with a different value.
constexpr trigger::Message::Kind releasedAs(trigger::Message::Kind sent) noexcept {
    return sent == trigger::Message::Kind::MidiNote ? trigger::Message::Kind::MidiNoteOff : sent;
}

/// The first entry of a follow-up row's kind dropdown, naming what it inherits: "release (same
/// note)" rather than "release". The parenthetical is the whole point of the entry — it is the
/// one choice that follows a number the rule *drew*, and a rig shuffling its notes could not
/// tell that from the word alone.
std::string releaseLabelOf(trigger::Message::Kind sent) {
    switch (sent) {
    case trigger::Message::Kind::MidiNote:
    case trigger::Message::Kind::MidiNoteOff:
        return "release (same note)";
    case trigger::Message::Kind::MidiCc:
        return "release (same cc)";
    case trigger::Message::Kind::MidiProgramChange:
        return "release (same program)";
    case trigger::Message::Kind::MidiPitchBend:
        return "release (centre)";
    case trigger::Message::Kind::Dmx:
        return "release (same fixtures)";
    case trigger::Message::Kind::Osc:
        break;
    }
    return "release (same address)";
}

/// What a lighting rule's release does to the light, which depends on the effect it lets go of.
/// `Rule::followUpsFor` is the engine's half of this and the two must say the same thing: a
/// color comes back at the row's level, a flash, pulse, strobe or plain level becomes a plain
/// level on the rule's channel, and a move has no release at all and is skipped. That last one
/// is a row that sends nothing, so it has to say so rather than look like it works.
std::string describeDmxRelease(const trigger::DmxSend& send) {
    if (dmx::takesMovement(send.effect)) {
        return "nothing — a move has no release, so this row sends nothing";
    }
    if (dmx::takesColor(send.effect)) {
        return "the same color at this level, same fixtures";
    }
    return std::string(dmx::labelOf(send.role)) + " to this level, same fixtures";
}

/// What a follow-up row will *really* send, beside the row that configures it.
///
/// A release is the whole reason this exists. The row says "release", and what that means
/// depends on the rule it hangs off — a note off on the note that was drawn, the same address
/// with a different argument, the same CC with a different value. Without this the editor
/// looked like it made note on and note off exclusive, which is how it was read on a rig:
/// *"you made note on and note off exclusive!? that will not work."* They are not, and this
/// is the line that says so.
///
/// Blank for an explicit kind, whose own row already spells out everything it sends — except
/// for the one explicit kind that is a release with the inheritance taken out. A rule that
/// *draws* its note and is followed by a hand-typed note off releases a note it never played,
/// and the row cannot show that by listing its fields: both boxes are filled in and look
/// right. Reported from a rig, which read the fixed box as the feature being absent — *"its
/// making me set a static note number ... I need it to send a note off to whatever note it
/// just sent a note on to"*. It is not absent; it is the entry above, and this says so.
std::string describeFollowUp(const trigger::FollowUp& entry, const Rule::Config& rule) {
    if (entry.kind) {
        if (!trigger::followUpFits(*entry.kind, rule.sendKind)) {
            return "not sent — " + std::string(trigger::labelOf(*entry.kind)) + " cannot follow " +
                   std::string(trigger::labelOf(rule.sendKind));
        }
        const bool releasesTheSameThing = (rule.sendKind == trigger::Message::Kind::MidiNote &&
                                           *entry.kind == trigger::Message::Kind::MidiNoteOff) ||
                                          *entry.kind == rule.sendKind;
        // Only worth saying where the number moves. A fixed note followed by a fixed note off
        // is two spellings of the same thing and neither is wrong.
        if (releasesTheSameThing && trigger::sendsNumber(*entry.kind) &&
            rule.number.kind != trigger::GeneratorKind::Fixed) {
            return "fixed " + std::string(numberLabelOf(*entry.kind)) + " " +
                   std::to_string(entry.number) + " — the trigger sends a different one each " +
                   "time. Pick \"release\" to follow it.";
        }
        return {};
    }
    switch (rule.sendKind) {
    case trigger::Message::Kind::MidiNote:
        return "note off, same note, ch " + std::to_string(rule.channel);
    case trigger::Message::Kind::MidiNoteOff:
        return "note off again, same note";
    case trigger::Message::Kind::MidiCc:
        return "the same CC, ch " + std::to_string(rule.channel);
    case trigger::Message::Kind::MidiProgramChange:
        return "the same program — nothing to release";
    case trigger::Message::Kind::MidiPitchBend:
        return "pitch bend, ch " + std::to_string(rule.channel);
    case trigger::Message::Kind::Dmx:
        return describeDmxRelease(rule.dmx);
    case trigger::Message::Kind::Osc:
        break;
    }
    return "the same address";
}

/// A Euclidean pattern as something a person can hear before it plays: "x..x..x.".
///
/// Two numbers are not a rhythm anybody can read, and this is the cheapest way to make them
/// one — it is the same `euclidHit` the engine fires on, so the picture cannot disagree with
/// the playing.
std::string spellEuclid(const Rule::Config& rule) {
    if (!trigger::takesPulses(rule.trigger)) {
        return {};
    }
    const std::uint32_t steps = std::max<std::uint32_t>(1, rule.every);
    if (steps > 64) {
        return std::to_string(rule.pulses) + " in " + std::to_string(steps);
    }
    std::string pattern;
    pattern.reserve(steps);
    for (std::uint32_t step = 0; step < steps; ++step) {
        pattern += trigger::euclidHit(step, rule.pulses, steps) ? 'x' : '.';
    }
    return pattern;
}

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
const std::array<HostPreset, 5> kHostPresets{{
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

/// A release — the fired message again with a different value — after a delay in
/// milliseconds. What §5.6's press-then-release is, and what every preset here wants.
trigger::FollowUp releaseAfterMs(std::int32_t value, double milliseconds) {
    trigger::FollowUp entry;
    entry.value = trigger::Value::ofInt(value);
    entry.unit = trigger::DelayUnit::Milliseconds;
    entry.delaySeconds = milliseconds / 1000.0;
    return entry;
}

/// One clip-launching rule for one Resolume layer.
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

/// The rig presets, in the order the picker offers them.
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

/// The labels the preset menu shows, in `rigPresetRules`' order — which counts from **one**,
/// zero being "no preset". The window's list holds these four and adds the one back on
/// (`rig-added(i + 1)`); it used to hold a fifth "add a preset..." entry at the front, which
/// is what a dropdown needs to have something to sit on and what a menu does not.
constexpr std::array<const char*, 4> kRigPresets{
    "Resolume: clips on 3 layers", "Resolume: tempo and resync", "Resolume: breathing dashboard",
    "MIDI: euclidean stabs"};

} // namespace

RulesController::RulesController(output::OutputRunner& runner,
                                 std::vector<trigger::Rule::Config> rules)
    : runner_(runner), rules_(std::move(rules)), window_(RulesWindow::create()),
      listModel_(std::make_shared<slint::VectorModel<RuleRow>>()),
      choiceModel_(std::make_shared<slint::VectorModel<OutputChoice>>()),
      fixtureModel_(std::make_shared<slint::VectorModel<OutputChoice>>()),
      slotModel_(std::make_shared<slint::VectorModel<SlotRow>>()),
      paletteModel_(std::make_shared<slint::VectorModel<PaletteEntry>>()),
      followModel_(std::make_shared<slint::VectorModel<FollowRow>>()),
      logModel_(std::make_shared<slint::VectorModel<slint::SharedString>>()) {
    window_->set_rules(listModel_);
    window_->set_output_choices(choiceModel_);
    window_->set_fixture_choices(fixtureModel_);
    window_->set_slots(slotModel_);
    window_->set_palette(paletteModel_);
    window_->set_follow_ups(followModel_);
    window_->set_log(logModel_);

    // The dropdowns, filled from the enums themselves so a kind added to `core/trigger`
    // appears here without anyone remembering to add it.
    auto triggers = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::Trigger which : trigger::kTriggers) {
        triggers->push_back(shared(std::string(trigger::labelOf(which))));
    }
    window_->set_trigger_names(triggers);

    auto sends = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::Message::Kind kind : trigger::kMessageKinds) {
        sends->push_back(shared(std::string(trigger::labelOf(kind))));
    }
    window_->set_send_kinds(sends);

    auto kinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const GeneratorKind kind : trigger::kGeneratorKinds) {
        kinds->push_back(shared(std::string(trigger::labelOf(kind))));
    }
    window_->set_generator_kinds(kinds);
    // A color's chip offers only what can make a color (`trigger::handsBackValues`).
    auto colorKinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const GeneratorKind kind : trigger::kColorGeneratorKinds) {
        colorKinds->push_back(shared(std::string(trigger::labelOf(kind))));
    }
    window_->set_color_generator_kinds(colorKinds);

    auto sources = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::LiveSource source : trigger::kLiveSources) {
        sources->push_back(shared(std::string(trigger::labelOf(source))));
    }
    window_->set_live_sources(sources);

    auto shapes = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::RampShape shape : trigger::kRampShapes) {
        shapes->push_back(shared(std::string(trigger::labelOf(shape))));
    }
    window_->set_ramp_shapes(shapes);

    auto hosts = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const HostPreset& preset : kHostPresets) {
        hosts->push_back(slint::SharedString(preset.label));
    }
    window_->set_host_presets(hosts);

    auto rigs = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const char* label : kRigPresets) {
        rigs->push_back(slint::SharedString(label));
    }
    window_->set_rig_presets(rigs);

    auto units = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::DelayUnit unit : trigger::kDelayUnits) {
        units->push_back(shared(std::string(trigger::labelOf(unit))));
    }
    window_->set_follow_up_units(units);

    // The lighting dropdowns, filled from `core/dmx`'s own tables for the same reason the
    // others are: an effect added there appears here without anyone remembering to add it.
    auto effects = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::EffectKind kind : dmx::kEffectKinds) {
        effects->push_back(shared(std::string(dmx::labelOf(kind))));
    }
    window_->set_effect_kinds(effects);

    auto roles = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::Role role : dmx::kAimableRoles) {
        roles->push_back(shared(std::string(dmx::labelOf(role))));
    }
    window_->set_effect_roles(roles);

    auto curves = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::Curve curve : dmx::kCurves) {
        curves->push_back(shared(std::string(dmx::labelOf(curve))));
    }
    window_->set_effect_curves(curves);

    auto paths = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::PathShape shape : dmx::kPathShapes) {
        paths->push_back(shared(std::string(dmx::labelOf(shape))));
    }
    window_->set_effect_shapes(paths);

    auto colorModes = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::ColorMode mode : trigger::kColorModes) {
        colorModes->push_back(shared(std::string(trigger::labelOf(mode))));
    }
    window_->set_color_modes(colorModes);

    // **Every action commits what was being typed before it runs** (the audit of 2026-09-25,
    // M18) — see `typing_`. A box's own commit is not wrapped: its setter clears its own record
    // (`typed`), and while it has the keyboard no other box can have one. What the box keystrokes
    // are is said once, here, rather than at the top of forty setters, where the forty-first
    // would have been the one that forgot.
    const auto finishing = [this](auto action) {
        return [this, action](auto... args) {
            commitTyping();
            action(args...);
        };
    };
    // And a box's own commit is dropped when it only repeats what an action already committed
    // for it — see `echo_`. By then the action may have put another rule, or another row, under
    // the box: + ADD shows the new rule's "every" in the box that was typed 7 into, and the late
    // commit of that 7 went to the new rule.
    const auto number = [](int n) { return slint::SharedString(std::to_string(n)); };

    window_->on_rule_picked(finishing([this](int index) { pick(index); }));
    window_->on_rule_picked_with(finishing(
        [this](int index, bool control, bool shift) { pickWith(index, control, shift); }));
    window_->on_rule_added(finishing([this] { add(); }));
    window_->on_rule_removed(finishing([this] { remove(); }));
    window_->on_rule_duplicated(finishing([this] { duplicate(); }));
    // Through `DeleteGuard`, which drops the second click of a double-click on ×: the row
    // below moves up under the pointer and would take it (the audit of 2026-09-25, L10).
    window_->on_rule_removed_at(finishing([this](int index) {
        if (ruleMarks_.press(index)) {
            removeAt(index);
        }
    }));
    window_->on_rule_duplicated_at(finishing([this](int index) { duplicateAt(index); }));
    window_->on_rig_added(finishing([this](int index) { addRig(index); }));
    window_->on_rule_enabled_changed(finishing([this](bool on) { setEnabled(on); }));
    window_->on_rule_muted_changed(finishing([this](bool on) { setMuted(on); }));
    window_->on_rule_rate_changed(finishing([this](float factor) { nudgeRate(factor); }));
    // The name commits on every keystroke, so there is never anything of its own to carry.
    window_->on_rule_renamed([this](const slint::SharedString& n) { rename(std::string(n)); });
    window_->on_rule_tested(finishing([this] { test(); }));
    window_->on_panic_clicked(finishing([this] { panic(); }));
    window_->on_panic_released(finishing([this] { releasePanic(); }));

    window_->on_trigger_picked(finishing([this](int index) { pickTrigger(index); }));
    window_->on_every_changed([this, number](int every) {
        if (!echoes(TypedIn::Rule, kEveryBox, number(every))) {
            setEvery(every);
        }
    });
    window_->on_pulses_changed([this, number](int pulses) {
        if (!echoes(TypedIn::Rule, kPulsesBox, number(pulses))) {
            setPulses(pulses);
        }
    });
    window_->on_output_chosen(
        finishing([this](const slint::SharedString& name, bool chosen) {
            setOutputChosen(std::string(name), chosen);
        }));
    window_->on_all_outputs_chosen(finishing([this] { chooseAllOutputs(); }));
    window_->on_fixture_chosen(
        finishing([this](const slint::SharedString& name, bool chosen) {
            setFixtureChosen(std::string(name), chosen);
        }));
    window_->on_no_fixtures_chosen(finishing([this] { chooseNoFixtures(); }));
    window_->on_effect_picked(finishing([this](int index) { pickEffect(index); }));
    window_->on_effect_role_picked(finishing([this](int index) { pickRole(index); }));
    window_->on_effect_curve_picked(finishing([this](int index) { pickCurve(index); }));
    window_->on_effect_shape_picked(finishing([this](int index) { pickPathShape(index); }));
    window_->on_effect_duration_edited([this](const slint::SharedString& text) {
        if (!echoes(TypedIn::Rule, kDurationBox, text)) {
            setDuration(std::string(text));
        }
    });
    window_->on_effect_unit_picked(finishing([this](int unit) { pickDurationUnit(unit); }));
    window_->on_effect_base_changed([this, number](int level) {
        if (!echoes(TypedIn::Rule, kBaseBox, number(level))) {
            setBase(level);
        }
    });
    window_->on_effect_cycles_edited([this](const slint::SharedString& text) {
        if (!echoes(TypedIn::Rule, kCyclesBox, text)) {
            setCycles(std::string(text));
        }
    });
    window_->on_effect_duty_changed(finishing([this](float percent) { setDuty(percent); }));
    window_->on_effect_hue_edited([this](const slint::SharedString& text) {
        if (!echoes(TypedIn::Rule, kHueBox, text)) {
            setHueRange(std::string(text));
        }
    });
    window_->on_effect_size_changed(finishing([this](float percent) { setSize(percent); }));
    window_->on_color_mode_picked(finishing([this](int index) { pickColorMode(index); }));
    window_->on_palette_added(finishing([this] { addPaletteColor(); }));
    window_->on_palette_removed(finishing([this](int index) { removePaletteColor(index); }));
    window_->on_palette_color_changed(
        finishing([this](int index, float hue, float sat, float val) {
            setPaletteColor(index, hue, sat, val);
        }));
    window_->on_palette_hex_edited([this](int index, const slint::SharedString& text) {
        if (!echoes(TypedIn::Palette, 0, text)) {
            setPaletteHex(index, std::string(text));
        }
    });
    window_->on_palette_typed([this](int index, const slint::SharedString& t) {
        noteTyping(TypedIn::Palette, index, 0, std::string(t));
    });
    window_->on_picker_open_changed([this](bool open) { setPickerOpen(open); });

    // Widened explicitly, here and on `probability` below: Slint hands a slider's value over
    // as a float and both settings are held as double. GCC and Clang refuse the implicit
    // promotion under -Wdouble-promotion -Werror, and MSVC accepts it silently — which is
    // why these two were still here after the same fix went in for the engine.
    window_->on_min_confidence_changed(
        finishing([this](float v) { setMinConfidence(static_cast<double>(v)); }));
    window_->on_intensity_changed(
        finishing([this](int which, bool on) { setIntensity(which, on); }));
    window_->on_bpm_range_edited([this](const slint::SharedString& t) {
        if (!echoes(TypedIn::Rule, kBpmRangeBox, t)) {
            setBpmRange(std::string(t));
        }
    });
    window_->on_probability_changed(
        finishing([this](float v) { setProbability(static_cast<double>(v)); }));
    window_->on_cooldown_changed([this](const slint::SharedString& t) {
        if (!echoes(TypedIn::Rule, kCooldownBox, t)) {
            setCooldown(std::string(t));
        }
    });

    window_->on_send_picked(finishing([this](int index) { pickSend(index); }));
    window_->on_address_edited([this](const slint::SharedString& a) {
        if (!echoes(TypedIn::Rule, kAddressBox, a)) {
            setAddress(std::string(a));
        }
    });
    window_->on_channel_changed([this, number](int channel) {
        if (!echoes(TypedIn::Rule, kChannelBox, number(channel))) {
            setChannel(channel);
        }
    });
    window_->on_host_preset_picked(finishing([this](int index) { pickHostPreset(index); }));
    window_->on_send_value_changed(finishing([this](bool on) { setSendValue(on); }));
    window_->on_rule_typed([this](int field, const slint::SharedString& t) {
        noteTyping(TypedIn::Rule, 0, field, std::string(t));
    });

    window_->on_follow_up_added(finishing([this] { addFollowUp(); }));
    window_->on_follow_up_removed(finishing([this](int index) {
        if (followMarks_.press(index)) { // a double-click on × is one deletion (L10)
            removeFollowUp(index);
        }
    }));
    window_->on_follow_kind_picked(
        finishing([this](int index, int choice) { pickFollowKind(index, choice); }));
    // A × on a row above moves the box that had the keyboard onto the next follow-up before its
    // late commit arrives, which is the other thing the echo is for.
    window_->on_follow_number_changed([this, number](int index, int n) {
        if (!echoes(TypedIn::FollowUp, 2, number(n))) {
            setFollowNumber(index, n);
        }
    });
    window_->on_follow_value_edited([this](int index, const slint::SharedString& t) {
        if (!echoes(TypedIn::FollowUp, 0, t)) {
            setFollowValue(index, std::string(t));
        }
    });
    window_->on_follow_delay_edited([this](int index, const slint::SharedString& t) {
        if (!echoes(TypedIn::FollowUp, 1, t)) {
            setFollowDelay(index, std::string(t));
        }
    });
    window_->on_follow_unit_picked(
        finishing([this](int index, int unit) { pickFollowUnit(index, unit); }));

    window_->on_slot_kind_picked(finishing([this](int slot, int kind) { pickSlotKind(slot, kind); }));
    window_->on_slot_pool_changed(finishing([this](int slot, bool list) { setSlotPool(slot, list); }));
    window_->on_slot_range_edited([this](int slot, const slint::SharedString& t) {
        if (!echoes(TypedIn::Slot, 1, t)) {
            setSlotRange(slot, std::string(t));
        }
    });
    window_->on_slot_values_edited([this](int slot, const slint::SharedString& t) {
        if (!echoes(TypedIn::Slot, 2, t)) {
            setSlotValues(slot, std::string(t));
        }
    });
    window_->on_slot_typed([this](int slot, int field, const slint::SharedString& t) {
        noteTyping(TypedIn::Slot, slot, field, std::string(t));
    });
    window_->on_follow_typed([this](int index, int field, const slint::SharedString& t) {
        noteTyping(TypedIn::FollowUp, index, field, std::string(t));
    });
    window_->on_slot_normalise_edited([this](int slot, const slint::SharedString& t) {
        if (!echoes(TypedIn::Slot, 4, t)) {
            setSlotNormalise(slot, std::string(t));
        }
    });
    window_->on_slot_weights_edited([this](int slot, const slint::SharedString& t) {
        if (!echoes(TypedIn::Slot, 3, t)) {
            setSlotWeights(slot, std::string(t));
        }
    });
    window_->on_slot_no_repeat_changed([this, number](int slot, int n) {
        if (!echoes(TypedIn::Slot, 5, number(n))) {
            setSlotNoRepeat(slot, n);
        }
    });
    window_->on_slot_color_changed(
        finishing([this](int slot, float hue, float saturation, float bright) {
            setSlotColor(slot, hue, saturation, bright);
        }));
    window_->on_slot_fixed_edited([this](int slot, const slint::SharedString& t) {
        if (!echoes(TypedIn::Slot, 0, t)) {
            setSlotFixed(slot, std::string(t));
        }
    });
    window_->on_slot_live_picked(
        finishing([this](int slot, int source) { pickSlotLive(slot, source); }));
    window_->on_slot_shape_picked(
        finishing([this](int slot, int shape) { pickSlotShape(slot, shape); }));
    window_->on_slot_ramp_bars_changed([this, number](int slot, int bars) {
        if (!echoes(TypedIn::Slot, 6, number(bars))) {
            setSlotRampBars(slot, bars);
        }
    });
    window_->on_slot_ramp_float_changed(
        finishing([this](int slot, bool asFloat) { setSlotRampFloat(slot, asFloat); }));

    window_->on_log_cleared(finishing([this] { clearLog(); }));

    // The size this opens at, set here rather than in the markup: Slint takes a window's
    // initial size from what its content asks for, not from the Window's own
    // `preferred-width` — so this window opened at its minimum, 900 x 560, whatever the
    // markup preferred. See `kRulesWindowWidth`. Before the first `show()`, which is what
    // makes the backend leave it alone; a later drag is the operator's and is kept.
    // No larger than the screen has room for, though: see `fitToScreen` (the audit's M26).
    const LogicalExtent opening = fitToScreen({kRulesWindowWidth, kRulesWindowHeight});
    window_->window().set_size(slint::LogicalSize({opening.width, opening.height}));

    // The window's own close box goes through `hide` like CLOSE does, so what was being typed is
    // committed and `visible_` is told (the audit of 2026-09-25, M16).
    window_->window().on_close_requested([this] {
        hide();
        return slint::CloseRequestResponse::HideWindow;
    });

    resettle(0);
    publishAll();
}

void RulesController::show() {
    window_->show();
    visible_ = true;
    // **And brought forward**, which `show()` does not do for a window that is already up: it
    // leaves it exactly where it was in the Z order, so pressing TRIGGERS with the editor
    // behind the main window looked like a button that did nothing. Reported from a rig.
    //
    // By a substring of the title, which is how `bringWindowToFront` finds a window at all —
    // "triggers" appears in this window's title and in no other of ours. A no-op away from
    // Windows and under the testing backend, where there is no platform window to raise.
    (void)bringWindowToFront("triggers");
}

void RulesController::hide() {
    commitTyping();
    window_->hide();
    visible_ = false;
    // A picker goes with the window, and nothing in the markup will say so — a popup reports
    // that it closed, not that the window under it went away. Left set, it would hold every
    // repeater rebuild for the rest of the session (see `pickerOpen_`), which is the "editing
    // one changes them all" bug coming back by the side door. Nothing is on screen and nothing
    // has a pointer grab, so clearing it here is safe in a way that clearing it anywhere else
    // would not be.
    pickerOpen_ = false;
}

const trigger::Rule::Config* RulesController::findRule(std::string_view id) const noexcept {
    for (const Rule::Config& rule : rules_) {
        if (rule.id == id) {
            return &rule;
        }
    }
    return nullptr;
}

std::uint64_t RulesController::freshSeed(const std::vector<Rule::Config>& pending) const {
    // 977 apart, as seeds always were: a rule's generators take the seeds just above its own
    // (`matchSegmentsToAddress`), so neighbours need room between them.
    constexpr std::uint64_t kStride = 977;
    std::uint64_t seed = 1;
    for (const auto* set : {&rules_, &pending}) {
        for (const Rule::Config& rule : *set) {
            seed = std::max(seed, rule.seed + kStride);
        }
    }
    return seed;
}

trigger::Rule::Config* RulesController::current() noexcept {
    if (selected_ < 0 || static_cast<std::size_t>(selected_) >= rules_.size()) {
        return nullptr;
    }
    return &rules_[static_cast<std::size_t>(selected_)];
}

const trigger::Rule::Config* RulesController::current() const noexcept {
    return const_cast<RulesController*>(this)->current();
}

void RulesController::noteTyping(TypedIn where, int index, int field, std::string text) {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    typing_ = Typing{rule->id, where, index, field, std::move(text)};
    echo_.reset();
}

void RulesController::rowRemoved(TypedIn where, int index) noexcept {
    // What was being typed into a row follows the row: gone with the one removed, or up one
    // with a row below it. Left at its old index, the next `commitTyping` wrote it into the row
    // that took that place, or into nothing (the 2026-09-25 audit's L13). The window commits
    // before a × runs; any other caller does not.
    const Rule::Config* rule = current();
    if (!typing_ || typing_->where != where || rule == nullptr || typing_->ruleId != rule->id) {
        return;
    }
    if (typing_->index == index) {
        typing_.reset();
    } else if (typing_->index > index) {
        --typing_->index;
    }
}

void RulesController::typed(TypedIn where, int index, int field) noexcept {
    if (typing_ && typing_->where == where && typing_->index == index && typing_->field == field) {
        typing_.reset();
    }
}

bool RulesController::typingInto(RuleBox box) const noexcept {
    const Rule::Config* rule = current();
    return typing_ && rule != nullptr && typing_->ruleId == rule->id &&
           typing_->where == TypedIn::Rule && typing_->field == box;
}

bool RulesController::echoes(TypedIn where, int field, const slint::SharedString& text) noexcept {
    // Not by row: the row is exactly what may have moved. By the kind of box and what it holds,
    // which only a keystroke can change — and a keystroke clears the echo.
    if (!echo_ || echo_->where != where || echo_->field != field ||
        std::string_view(text) != echo_->text) {
        return false;
    }
    echo_.reset();
    return true;
}

void RulesController::commitTyping() {
    if (!typing_) {
        return;
    }
    const Typing pending = *typing_;
    typing_.reset();
    const Rule::Config* rule = current();
    if (rule == nullptr || rule->id != pending.ruleId) {
        return; // the rule it was typed for is not the one showing; nothing to put it in
    }
    echo_ = pending;
    const std::optional<int> number = typedNumber(pending.text);
    switch (pending.where) {
    case TypedIn::FollowUp:
        if (pending.field == 0) {
            setFollowValue(pending.index, pending.text);
        } else if (pending.field == 1) {
            setFollowDelay(pending.index, pending.text);
        } else if (number) {
            setFollowNumber(pending.index, *number);
        }
        return;
    case TypedIn::Palette:
        setPaletteHex(pending.index, pending.text);
        return;
    case TypedIn::Rule:
        switch (pending.field) {
        case kAddressBox:
            setAddress(pending.text);
            break;
        case kBpmRangeBox:
            setBpmRange(pending.text);
            break;
        case kCooldownBox:
            setCooldown(pending.text);
            break;
        case kDurationBox:
            setDuration(pending.text);
            break;
        case kCyclesBox:
            setCycles(pending.text);
            break;
        case kHueBox:
            setHueRange(pending.text);
            break;
        case kEveryBox:
            if (number) {
                setEvery(*number);
            }
            break;
        case kPulsesBox:
            if (number) {
                setPulses(*number);
            }
            break;
        case kChannelBox:
            if (number) {
                setChannel(*number);
            }
            break;
        case kBaseBox:
            if (number) {
                setBase(*number);
            }
            break;
        default:
            break;
        }
        return;
    case TypedIn::Slot:
        break;
    }
    switch (pending.field) {
    case 0:
        setSlotFixed(pending.index, pending.text);
        break;
    case 1:
        setSlotRange(pending.index, pending.text);
        break;
    case 2:
        setSlotValues(pending.index, pending.text);
        break;
    case 3:
        setSlotWeights(pending.index, pending.text);
        break;
    case 4:
        setSlotNormalise(pending.index, pending.text);
        break;
    case 5:
        if (number) {
            setSlotNoRepeat(pending.index, *number);
        }
        break;
    case 6:
        if (number) {
            setSlotRampBars(pending.index, *number);
        }
        break;
    default:
        break;
    }
}

void RulesController::choseSlot(int slot) noexcept {
    Rule::Config* rule = current();
    if (rule == nullptr || slot < 0) {
        return;
    }
    const std::vector<trigger::Slot> layout = trigger::slotLayout(*rule);
    const auto index = static_cast<std::size_t>(slot);
    if (index < layout.size() && layout[index].role == trigger::SlotRole::Number) {
        rule->numberChosen = true;
    }
}

Generator::Config* RulesController::slotConfig(int slot) noexcept {
    // Through `trigger::slotLayout`, which `publishSlots` builds the rows from and which is
    // tested against the order a fire records: the index of a row is the index of the slot,
    // for every kind and every effect, with no second walk of the conditions to keep in step.
    Rule::Config* rule = current();
    if (rule == nullptr || slot < 0) {
        return nullptr;
    }
    const std::vector<trigger::Slot> layout = trigger::slotLayout(*rule);
    const auto index = static_cast<std::size_t>(slot);
    return index < layout.size() ? &trigger::slotGenerator(*rule, layout[index]) : nullptr;
}

void RulesController::commit() {
    // Whatever the status line was saying was about the *last* edit — "a range is two
    // numbers", most often — and this one worked. Cleared here rather than by a timer so
    // the message stays up for exactly as long as it is the last thing that happened; the
    // few callers that raise a status of their own do it after committing.
    setStatus({}, false);
    runner_.post(output::OutputCommand::rules(rules_));
    if (changed_) {
        changed_(rules_);
    }
    publishList();
    publishSlots();
    // With the slots, because these rows describe the same rule and read from the same fields
    // — a release's summary names the channel, so changing the channel has to move it.
    publishFollowUps();
}

void RulesController::matchSegmentsToAddress(Rule::Config& rule) {
    // §5.8's validity is mostly this count, so an edit that adds a `{}` should hand the
    // operator a chip rather than a rule that refuses to fire until they work out why.
    // Extra generators are kept until the address stops needing them, and a shortened
    // address drops from the end — which is where a deleted placeholder came from.
    const std::size_t wanted = trigger::countPlaceholders(rule.address);
    while (rule.segments.size() < wanted) {
        Generator::Config segment;
        // Shuffle over 1-8 is §5.8's own default and the useful thing for a clip grid.
        segment.seed = rule.seed + rule.segments.size() + 1;
        rule.segments.push_back(segment);
    }
    if (rule.segments.size() > wanted) {
        rule.segments.resize(wanted);
    }
}

void RulesController::setRules(std::vector<trigger::Rule::Config> rules) {
    // Nothing half-typed is carried into a set it was not typed for.
    typing_.reset();
    // A whole new set, so the counts and last values start again. A preset may well reuse
    // the ids of the set it replaces — `add` numbers them "rule1", "rule2" — and a card
    // inheriting a number from a different rule that happened to share its id is a readout
    // that is quietly wrong, which is worse than one that reads zero.
    firesSeen_.clear();
    slotsSeen_.clear();
    // And the mutes and rates last seen on the running rules, which the loaded set starts without
    // (the audit of 2026-09-25, M11): kept, they showed a rule muted that the new set had not.
    mutedSeen_.clear();
    rateSeen_.clear();
    rules_ = std::move(rules);
    resettle(selected_ < 0 ? 0 : selected_);
    publishAll();
}

std::vector<int> RulesController::chosen() const {
    std::vector<int> rows;
    for (std::size_t i = 0; i < chosen_.size(); ++i) {
        if (chosen_[i]) {
            rows.push_back(static_cast<int>(i));
        }
    }
    return rows;
}

void RulesController::resettle(int wanted) {
    chosen_.assign(rules_.size(), false);
    if (rules_.empty()) {
        selected_ = -1;
        anchor_ = -1;
        return;
    }
    selected_ = std::clamp(wanted, 0, static_cast<int>(rules_.size()) - 1);
    chosen_[static_cast<std::size_t>(selected_)] = true;
    anchor_ = selected_;
}

void RulesController::pick(int index) {
    pickWith(index, false, false);
}

void RulesController::pickWith(int index, bool control, bool shift) {
    if (index < 0 || static_cast<std::size_t>(index) >= rules_.size()) {
        return;
    }
    // What is half-typed in a box goes to the rule it was typed into, before that stops being
    // the rule on screen — see `typing_`.
    commitTyping();
    chosen_.resize(rules_.size(), false);
    if (control) {
        // In or out, one row at a time. Never out of the last one: a selection of nothing has
        // no editor to show and no rule for the marks to act on, so a control-click that
        // would empty it simply leaves that row selected.
        chosen_[static_cast<std::size_t>(index)] = !chosen_[static_cast<std::size_t>(index)];
        if (chosen().empty()) {
            chosen_[static_cast<std::size_t>(index)] = true;
        }
        anchor_ = index;
    } else if (shift && anchor_ >= 0 && static_cast<std::size_t>(anchor_) < rules_.size()) {
        // The run from the anchor to here, replacing whatever was chosen. The anchor stays
        // put, so shift-clicking again grows or shrinks the same run.
        const int low = std::min(anchor_, index);
        const int high = std::max(anchor_, index);
        chosen_.assign(rules_.size(), false);
        for (int i = low; i <= high; ++i) {
            chosen_[static_cast<std::size_t>(i)] = true;
        }
    } else {
        chosen_.assign(rules_.size(), false);
        chosen_[static_cast<std::size_t>(index)] = true;
        anchor_ = index;
    }
    // **The editor always shows a row that is in the selection**, and this used to be a plain
    // `selected_ = index`. Control-clicking a chosen row takes it *out* of the selection, so
    // that assignment left the card on screen showing a rule the marks would not act on: press
    // × on any lit row and six other rules go while the one being read stays. Found by the
    // soak test below rather than by anybody using it, which is the only reason it is written
    // down here and not in a bug report.
    //
    // The nearest row that is still chosen, because a selection is a run more often than not
    // and the eye is where the click was.
    if (chosen_[static_cast<std::size_t>(index)]) {
        selected_ = index;
    } else {
        const int last = static_cast<int>(rules_.size()) - 1;
        selected_ = index;
        for (int away = 1; away <= last; ++away) {
            if (index - away >= 0 && chosen_[static_cast<std::size_t>(index - away)]) {
                selected_ = index - away;
                break;
            }
            if (index + away <= last && chosen_[static_cast<std::size_t>(index + away)]) {
                selected_ = index + away;
                break;
            }
        }
    }
    // A new selection is a new last-fired line: the old one belonged to another rule and
    // leaving it up would credit this one with what that one sent.
    lastFired_.clear();
    lastFiredAt_ = -1.0;
    publishSelected();
    publishSlots();
    publishList(); // the chosen flags, which are what the rows draw themselves from
    publishFiring();
    window_->set_selected(selected_);
    // Said out loud, because a selection of six looks like a selection of six only if you
    // know to count the highlighted rows — and because what × does next depends on it.
    const std::size_t count = chosen().size();
    setStatus(count > 1 ? std::to_string(count) +
                              " triggers selected — the marks on any of "
                              "them act on all " +
                              std::to_string(count)
                        : std::string{},
              false);
}

void RulesController::add() {
    commitTyping();
    Rule::Config rule;
    // A fresh id that is legal as an OSC address segment (§5.7 addresses a rule by it) and
    // that nothing else has. Numbered rather than named, because a name is the operator's
    // to write and an id is only ever machine-facing.
    for (int n = static_cast<int>(rules_.size()) + 1;; ++n) {
        const std::string candidate = "rule" + std::to_string(n);
        const auto clash = [&candidate](const Rule::Config& other) {
            return other.id == candidate;
        };
        if (std::none_of(rules_.begin(), rules_.end(), clash)) {
            rule.id = candidate;
            break;
        }
    }
    // A name, rather than the blank the list showed as "(unnamed)". It is the operator's to
    // change and most of them will, but a list of "(unnamed)" rows is a list that cannot be
    // read, and "Trigger #5" at least says which one this is. Numbered past anything already
    // called that, so deleting from the middle and adding again does not make two.
    for (int n = static_cast<int>(rules_.size()) + 1;; ++n) {
        const std::string candidate = "Trigger #" + std::to_string(n);
        const auto clash = [&candidate](const Rule::Config& other) {
            return other.name == candidate;
        };
        if (std::none_of(rules_.begin(), rules_.end(), clash)) {
            rule.name = candidate;
            break;
        }
    }
    // Distinct seeds, so two rules in a preset do not fire the same clip as each other —
    // `Generator::Config::seed`'s whole reason. See `freshSeed`.
    rule.seed = freshSeed();
    rule.value.kind = GeneratorKind::Fixed;
    rule.value.fixed = trigger::Value::ofInt(1);
    // **Switched on.** This was off, on the reasoning that a half-built rule must not fire
    // into somebody's rig while they are still typing — and the reasoning was sound and the
    // result was wrong. A rule with no address is invalid, so it cannot fire whatever this
    // says (`Rule::problem`); what "off" actually bought was every new rule and every preset
    // arriving inert, behind an unlabelled tick box, with nothing on screen saying why
    // nothing happened. Reported from a rig: "it's not obvious that they're entirely
    // disabled". The box is labelled now, and a rule an operator asked for is armed.
    rule.enabled = true;
    rules_.push_back(rule);
    resettle(static_cast<int>(rules_.size()) - 1);
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
}

void RulesController::remove() {
    commitTyping();
    const std::vector<int> going = chosen();
    if (going.empty()) {
        return;
    }
    // From the back, so the indices ahead of each erase are still the ones just measured.
    for (auto row = going.rbegin(); row != going.rend(); ++row) {
        const auto at = static_cast<std::size_t>(*row);
        if (at >= rules_.size()) {
            continue;
        }
        // Forgotten with the rule. `add` recycles ids — delete "rule1" and the next rule
        // added is called "rule1" again — so a count left behind here would be handed to a
        // rule that has never fired.
        firesSeen_.erase(rules_[at].id);
        slotsSeen_.erase(rules_[at].id);
        rules_.erase(rules_.begin() + static_cast<std::ptrdiff_t>(at));
    }
    // The row that moved up into the first deleted one's place, which is where the eye is.
    resettle(going.front());
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
}

void RulesController::duplicate() {
    commitTyping();
    const std::vector<int> sources = chosen();
    if (sources.empty()) {
        return;
    }
    // Every copy goes after the last of the originals, in the originals' own order — six
    // rules duplicated read as six more in the same order, not as six pairs.
    std::vector<Rule::Config> copies;
    copies.reserve(sources.size());
    for (const int row : sources) {
        const auto at = static_cast<std::size_t>(row);
        if (at >= rules_.size()) {
            continue;
        }
        const Rule::Config& source = rules_[at];
        Rule::Config copy = source;
        copy.id += "-copy";
        const auto taken = [this, &copies](const std::string& id) {
            return std::any_of(rules_.begin(), rules_.end(),
                               [&id](const Rule::Config& o) { return o.id == id; }) ||
                   std::any_of(copies.begin(), copies.end(),
                               [&id](const Rule::Config& o) { return o.id == id; });
        };
        for (int n = 2; taken(copy.id); ++n) {
            copy.id = source.id + "-copy" + std::to_string(n);
        }
        if (!copy.name.empty()) {
            copy.name += " copy";
        }
        // A different stream, or the copy would fire exactly what the original fires — which
        // is never what duplicating a rule is for. From `freshSeed`, past every copy made so
        // far too; `source.seed + 977` was the next rule's seed as often as not (L9).
        copy.seed = freshSeed(copies);
        copies.push_back(std::move(copy));
    }
    const int after = sources.back() + 1;
    rules_.insert(rules_.begin() + after, copies.begin(), copies.end());
    // The copies are what the operator is now looking at, so they are what is selected.
    chosen_.assign(rules_.size(), false);
    for (std::size_t i = 0; i < copies.size(); ++i) {
        chosen_[static_cast<std::size_t>(after) + i] = true;
    }
    selected_ = after;
    anchor_ = after;
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
}

void RulesController::removeAt(int index) {
    commitTyping();
    if (index < 0 || static_cast<std::size_t>(index) >= chosen_.size() ||
        !chosen_[static_cast<std::size_t>(index)]) {
        pick(index);
    }
    remove();
}

void RulesController::duplicateAt(int index) {
    commitTyping();
    if (index < 0 || static_cast<std::size_t>(index) >= chosen_.size() ||
        !chosen_[static_cast<std::size_t>(index)]) {
        pick(index);
    }
    duplicate();
}

void RulesController::addRig(int index) {
    if (index <= 0) {
        return; // the picker's own label
    }
    commitTyping();
    const std::vector<Rule::Config> added = rigPresetRules(static_cast<std::size_t>(index));
    if (added.empty()) {
        return;
    }
    // Appended rather than replacing: an operator adding a second rig to a set they have
    // already built has not asked to lose the first.
    for (Rule::Config rule : added) {
        // An id already in use would make §5.7's `/ctl/rule/<id>/enable` ambiguous, and
        // `TriggerEngine::find` takes the first. Numbered up rather than refused: adding the
        // same rig twice is a reasonable thing to do with three more layers in mind.
        std::uint64_t copies = 0;
        std::string id = rule.id;
        while (findRule(id) != nullptr) {
            ++copies;
            id = rule.id + "-" + std::to_string(copies + 1);
        }
        if (copies > 0) {
            rule.id = std::move(id);
        }
        // A different stream too, or a second copy of the rig fires exactly what the first
        // does — or a rule of the operator's own that happened to have this seed does.
        if (std::any_of(rules_.begin(), rules_.end(),
                        [&rule](const Rule::Config& held) { return held.seed == rule.seed; })) {
            rule.seed = freshSeed();
        }
        rules_.push_back(std::move(rule));
    }
    // The whole rig that was just added, selected — so it can be re-routed or deleted again
    // in one gesture, which is the other half of a preset being a starting point.
    chosen_.assign(rules_.size(), false);
    const int first = static_cast<int>(rules_.size()) - static_cast<int>(added.size());
    for (std::size_t i = static_cast<std::size_t>(first); i < rules_.size(); ++i) {
        chosen_[i] = true;
    }
    selected_ = first;
    anchor_ = first;
    commit();
    publishSelected();
    publishFiring();
    window_->set_selected(selected_);
}

void RulesController::rename(const std::string& name) {
    if (Rule::Config* rule = current()) {
        rule->name = name;
        commit();
    }
}

void RulesController::setEnabled(bool on) {
    if (Rule::Config* rule = current()) {
        rule->enabled = on;
        commit();
        // **And to the running rule itself**, as MUTE is (the audit of 2026-09-25, M10). A set
        // posted with the switch unchanged keeps the live switch (`Rule::carryFrom`), and after a
        // control surface had switched this rule the editor's copy said what the surface did — so
        // unticking it posted the value the running rule was already configured with, the rule
        // went on firing, and the box said it was off.
        runner_.post(output::OutputCommand::ruleEnabled(rule->id, on));
    }
}

void RulesController::setMuted(bool on) {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    // Straight to the output thread, and **not** through `commit()`: this is not part of the
    // rule's configuration and must not be saved. `commit` would also republish the whole rule
    // set, which resets every live gesture on every other rule — the thing this is trying to
    // set on one of them.
    mutedSeen_[rule->id] = on;
    runner_.post(output::OutputCommand::ruleMuted(rule->id, on));
    publishSelected();
    publishList();
}

void RulesController::nudgeRate(double factor) {
    const Rule::Config* rule = current();
    if (rule == nullptr || !(factor > 0.0)) {
        return;
    }
    // The same clamp the rule itself applies, mirrored so the readout cannot claim a rate the
    // output thread refused. `Rule::setRate` is the authority; this has to agree with it.
    const double current = rateSeen_.count(rule->id) != 0 ? rateSeen_[rule->id] : 1.0;
    rateSeen_[rule->id] = std::clamp(current * factor, 0.0625, 64.0);
    runner_.post(output::OutputCommand::ruleRate(rule->id, factor, /*relative=*/true));
    publishSelected();
}

void RulesController::test() {
    if (const Rule::Config* rule = current()) {
        runner_.post(output::OutputCommand::testRule(rule->id));
    }
}

void RulesController::panic() {
    // Engage, whatever the state: a press on PANIC is never a request to let go. It was a
    // toggle, and the second click of a double-click undid the first.
    runner_.panic(true);
    // **Shown from what was asked for, not read back.** `panic` *posts*: the output thread
    // applies it about a millisecond later, so reading the flag here returns the state the
    // press was leaving, and the button lit the wrong way for a frame. On a PANIC button that
    // is the worst possible place for a flicker. `tick` puts it back in step if the change
    // somehow did not take.
    window_->set_panicked(true);
}

void RulesController::releasePanic() {
    runner_.panic(false);
    window_->set_panicked(false);
}

void RulesController::pickTrigger(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= trigger::kTriggers.size()) {
        return;
    }
    rule->trigger = trigger::kTriggers[static_cast<std::size_t>(index)];
    commit();
    publishSelected();
}

void RulesController::setEvery(int every) {
    typed(TypedIn::Rule, 0, kEveryBox);
    if (Rule::Config* rule = current()) {
        rule->every = static_cast<std::uint32_t>(std::max(1, every));
        commit();
        // Back into the box: a `NumberBox` never writes its own value, so it shows what this
        // says it is — the std `SpinBox` it replaced set itself and so needed nothing here.
        window_->set_every(static_cast<int>(rule->every));
    }
}

void RulesController::setTargets(std::vector<output::OutputTarget> targets) {
    // **Only when they have actually changed.** The main window tells us this from
    // `publishOutputs`, which runs on its redraw timer — thirty times a second while the
    // tracker does. `publishSelected` writes every field of the selected rule, including the
    // name, the address and the routing, so republishing unconditionally meant those fields
    // were reset thirty times a second and a rule could not be typed into while anything was
    // playing, which is the only time anybody edits one.
    if (targets == targets_) {
        return;
    }
    targets_ = std::move(targets);
    publishSelected(); // the "reaches ..." line beside the routing field
}

void RulesController::setPulses(int pulses) {
    typed(TypedIn::Rule, 0, kPulsesBox);
    if (Rule::Config* rule = current()) {
        rule->pulses = static_cast<std::uint32_t>(std::max(0, pulses));
        commit();
        publishSelected(); // the pattern the two numbers name
    }
}

void RulesController::setOutputs(const std::string& text) {
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    rule->outputs.clear();
    for (const std::string_view part : split(text, ",;")) {
        rule->outputs.emplace_back(part);
    }
    // Names are what a person types; the rule keeps the ids of the outputs they name.
    output::routeByIds(rule->outputs, targets_);
    commit();
    publishSelected();
}

void RulesController::setOutputChosen(const std::string& id, bool chosen) {
    Rule::Config* rule = current();
    if (rule == nullptr || id.empty()) {
        return;
    }
    const auto at = std::find(rule->outputs.begin(), rule->outputs.end(), id);
    if (chosen) {
        if (at == rule->outputs.end()) {
            rule->outputs.push_back(id);
        }
    } else if (at != rule->outputs.end()) {
        rule->outputs.erase(at);
    }
    // Nothing to say about an empty list: it already means every output, which is what the
    // "every output" line then shows as ticked. `resolveOutputs` and this cannot disagree
    // because there is only one representation of "everywhere".
    commit();
    publishSelected();
}

void RulesController::chooseAllOutputs() {
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    rule->outputs.clear();
    commit();
    publishSelected();
}

void RulesController::setPatch(std::vector<dmx::Fixture> patch) {
    // Only when they have actually changed, for the reason `setTargets` gives at length: this
    // is told from the main window's redraw timer, and republishing the selected rule thirty
    // times a second would make it impossible to type into.
    if (patch == patch_) {
        return;
    }
    patch_ = std::move(patch);
    publishSelected();
}

void RulesController::setFixtureChosen(const std::string& key, bool chosen) {
    Rule::Config* rule = current();
    if (rule == nullptr || key.empty()) {
        return;
    }
    std::vector<std::string>& aims = rule->dmx.fixtures;
    const auto at = std::find(aims.begin(), aims.end(), key);
    if (chosen) {
        if (at == aims.end()) {
            aims.push_back(key);
        }
    } else if (at != aims.end()) {
        aims.erase(at);
    }
    // Un-ticking the last one leaves the rule reaching **nothing**, which is the opposite of
    // what un-ticking the last output does. `Rule::validate` reports it as a problem, so the
    // card says so rather than the rig doing something nobody asked for.
    commit();
    publishSelected();
}

void RulesController::chooseNoFixtures() {
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    rule->dmx.fixtures.clear();
    commit();
    publishSelected();
}

void RulesController::pickSlotShape(int slot, int shape) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr || shape < 0 ||
        static_cast<std::size_t>(shape) >= trigger::kRampShapes.size()) {
        return;
    }
    config->shape = trigger::kRampShapes[static_cast<std::size_t>(shape)];
    commit();
}

void RulesController::setSlotRampBars(int slot, int bars) {
    typed(TypedIn::Slot, slot, 6);
    if (Generator::Config* config = slotConfig(slot)) {
        config->rampBars = static_cast<std::uint32_t>(std::max(1, bars));
        commit();
    }
}

void RulesController::setSlotRampFloat(int slot, bool asFloat) {
    if (Generator::Config* config = slotConfig(slot)) {
        config->rampFloat = asFloat;
        commit();
    }
}

void RulesController::setMinConfidence(double value) {
    if (Rule::Config* rule = current()) {
        rule->conditions.minConfidence = std::clamp(value, 0.0, 1.0);
        commit();
    }
}

void RulesController::setIntensity(int which, bool allowed) {
    Rule::Config* rule = current();
    if (rule == nullptr || which < 0 || which > 2) {
        return;
    }
    rule->conditions.intensities[static_cast<std::size_t>(which)] = allowed;
    commit();
}

void RulesController::setBpmRange(const std::string& text) {
    typed(TypedIn::Rule, 0, kBpmRangeBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    const std::string_view view(text);
    // "any" and an empty field both mean the same thing, and both are what an operator
    // types to undo a range they no longer want.
    if (trim(view).empty() || trim(view) == "any") {
        rule->conditions.minBpm = 0.0;
        rule->conditions.maxBpm = 1000.0;
        commit();
        publishSelected();
        return;
    }
    // A dash after a digit, so "-30 - 40" is not read as three numbers. Anything separating
    // two numbers is accepted, which is what "70-140", "70 to 140" and "70 140" all are.
    const std::vector<std::string_view> parts = split(view, "-–to ");
    if (parts.size() != 2) {
        setStatus("A BPM range is two numbers, like 120 - 140.", true);
        return;
    }
    const std::optional<double> low = readNumber(parts[0]);
    const std::optional<double> high = readNumber(parts[1]);
    if (!low || !high) {
        setStatus("A BPM range is two numbers, like 120 - 140.", true);
        return;
    }
    rule->conditions.minBpm = std::min(*low, *high);
    rule->conditions.maxBpm = std::max(*low, *high);
    commit();
    publishSelected();
}

void RulesController::setProbability(double value) {
    if (Rule::Config* rule = current()) {
        rule->conditions.probability = std::clamp(value, 0.0, 1.0);
        commit();
    }
}

void RulesController::setCooldown(const std::string& text) {
    typed(TypedIn::Rule, 0, kCooldownBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    const std::string_view trimmed = trim(text);
    // An empty box is no cooldown, which is what an operator clears it to mean. Anything else
    // that is not a number is said rather than silently read as zero — `text.to-float()` used
    // to do that in the markup, so a typo turned a two-second cooldown off without a word.
    if (trimmed.empty()) {
        rule->conditions.cooldownSeconds = 0.0;
    } else {
        const std::optional<double> number = readNumber(trimmed);
        if (!number) {
            setStatus("A cooldown is a number of milliseconds, like 250.", true);
            return;
        }
        rule->conditions.cooldownSeconds = std::max(0.0, *number) / 1000.0;
    }
    commit();
    publishSelected();
}

void RulesController::pickSend(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= trigger::kMessageKinds.size()) {
        return;
    }
    const trigger::Message::Kind was = rule->sendKind;
    rule->sendKind = trigger::kMessageKinds[static_cast<std::size_t>(index)];
    // **A number nobody chose does not go out** (the audit's C7, and the operator's call: the
    // rule stays armed and waits). A rule switched to a note or a controller used to fire at
    // once with whatever the number generator held — a shuffle over 1 to 8 by default, so CC 7
    // at one on channel 1, to every MIDI output, on some bar: a synth's volume gone. A number
    // carries over only where it means the same thing (`trigger::sameNumber`).
    if (trigger::sendsNumber(rule->sendKind) && !trigger::sameNumber(was, rule->sendKind)) {
        const std::uint64_t seed = rule->number.seed;
        rule->number = Generator::Config{};
        rule->number.kind = GeneratorKind::Fixed;
        rule->number.seed = seed;
        rule->numberChosen = false;
    }
    // And a velocity that is still another kind's default becomes this kind's. A new rule's
    // value is OSC's `1`, which as a velocity is a note too quiet to hear; pitch bend is 14-bit
    // with its rest at 8192, where 100 is a bend nearly all the way down.
    if (trigger::sendsValue(rule->sendKind) && rule->value.kind == GeneratorKind::Fixed) {
        const trigger::Value rest = trigger::Value::ofInt(
            rule->sendKind == trigger::Message::Kind::MidiPitchBend ? kBendRest : kVelocity);
        for (const std::int32_t untouched : {1, kVelocity, kBendRest}) {
            if (rule->value.fixed == trigger::Value::ofInt(untouched)) {
                rule->value.fixed = rest;
                break;
            }
        }
    }
    // A follow-up left on the far side of the OSC/MIDI divide would be **silently dropped** at
    // fire time (`trigger::followUpFits`) — a clip pressed and never released, which is the
    // worst failure this editor has. Turned into releases instead, which is what an operator
    // switching a rule's send kind means by the rows they already had: let go of what was
    // pressed. An explicit number goes with the kind that carried it; there is nowhere to put
    // one on the other side.
    int stranded = 0;
    for (trigger::FollowUp& owed : rule->followUps) {
        if (owed.kind && !trigger::followUpFits(*owed.kind, rule->sendKind)) {
            owed.kind.reset();
            ++stranded;
        }
    }
    commit();
    publishSelected();
    if (stranded > 0) {
        setStatus(
            std::to_string(stranded) +
                (stranded == 1 ? " follow-up became a release" : " follow-ups became releases") +
                ", because what they sent cannot follow a " +
                std::string(trigger::labelOf(rule->sendKind)) + ".",
            false);
    }
}

void RulesController::setAddress(const std::string& address) {
    typed(TypedIn::Rule, 0, kAddressBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    rule->address = address;
    matchSegmentsToAddress(*rule);
    commit();
    publishSelected();
}

void RulesController::setChannel(int channel) {
    typed(TypedIn::Rule, 0, kChannelBox);
    if (Rule::Config* rule = current()) {
        rule->channel = std::clamp(channel, 1, 16);
        commit();
        window_->set_channel(rule->channel); // see `setEvery`
    }
}

// --- the lighting half ------------------------------------------------------------------

void RulesController::pickEffect(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= dmx::kEffectKinds.size()) {
        return;
    }
    const dmx::EffectKind effect = dmx::kEffectKinds[static_cast<std::size_t>(index)];
    if (rule->dmx.effect == effect) {
        return;
    }
    rule->dmx.effect = effect;
    // A curve that suits the effect, but only where the operator has not chosen one — which
    // cannot be told apart from the default here, so this only moves the two that are
    // *wrong* rather than merely unfashionable. A movement wants to ease at both ends so the
    // head does not jerk; a fade wants to ease out because the eye's response to light is
    // not linear either. Neither is a preference: linear movement visibly snaps.
    if (dmx::takesMovement(effect) && rule->dmx.curve == dmx::Curve::EaseOut) {
        rule->dmx.curve = dmx::Curve::EaseInOut;
    }
    commit();
    publishSelected();
}

void RulesController::pickRole(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= dmx::kAimableRoles.size()) {
        return;
    }
    rule->dmx.role = dmx::kAimableRoles[static_cast<std::size_t>(index)];
    commit();
    publishSelected();
}

void RulesController::pickCurve(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 || static_cast<std::size_t>(index) >= dmx::kCurves.size()) {
        return;
    }
    rule->dmx.curve = dmx::kCurves[static_cast<std::size_t>(index)];
    commit();
}

void RulesController::pickPathShape(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= dmx::kPathShapes.size()) {
        return;
    }
    rule->dmx.shape = dmx::kPathShapes[static_cast<std::size_t>(index)];
    commit();
}

void RulesController::setDuration(const std::string& text) {
    typed(TypedIn::Rule, 0, kDurationBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    const std::optional<double> number = readNumber(trim(text));
    if (!number || *number < 0.0) {
        setStatus("A duration is a number, and not a negative one.", true);
        publishSelected();
        return;
    }
    // Into whichever field the row's unit names, so switching units does not rewrite the
    // number the operator is not currently looking at — the same reasoning
    // `trigger::FollowUp::delayBeats` gives for holding two.
    if (rule->dmx.unit == trigger::DelayUnit::Milliseconds) {
        rule->dmx.durationSeconds = *number / 1000.0;
    } else {
        rule->dmx.durationBeats = *number;
    }
    commit();
    publishSelected();
}

void RulesController::pickDurationUnit(int unit) {
    Rule::Config* rule = current();
    if (rule == nullptr || unit < 0 ||
        static_cast<std::size_t>(unit) >= trigger::kDelayUnits.size()) {
        return;
    }
    rule->dmx.unit = trigger::kDelayUnits[static_cast<std::size_t>(unit)];
    commit();
    publishSelected();
}

void RulesController::setBase(int level) {
    typed(TypedIn::Rule, 0, kBaseBox);
    if (Rule::Config* rule = current()) {
        rule->dmx.base = std::clamp(level, 0, 255);
        commit();
        window_->set_effect_base(rule->dmx.base); // see `setEvery`
    }
}

void RulesController::setCycles(const std::string& text) {
    typed(TypedIn::Rule, 0, kCyclesBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    const std::optional<double> number = readNumber(trim(text));
    if (!number || !(*number > 0.0)) {
        setStatus("A number of cycles is a number above zero.", true);
        publishSelected();
        return;
    }
    rule->dmx.cycles = std::min(*number, 1024.0);
    commit();
    publishSelected();
}

void RulesController::setDuty(float percent) {
    if (Rule::Config* rule = current()) {
        rule->dmx.duty = std::clamp(static_cast<double>(percent) / 100.0, 0.0, 1.0);
        commit();
    }
}

void RulesController::setHueRange(const std::string& text) {
    typed(TypedIn::Rule, 0, kHueBox);
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    const std::vector<std::string_view> parts = split(trim(text), "-–to ");
    if (parts.size() != 2) {
        setStatus("A hue sweep is two angles in degrees, like 0 - 360.", true);
        publishSelected();
        return;
    }
    const std::optional<double> low = readNumber(parts[0]);
    const std::optional<double> high = readNumber(parts[1]);
    if (!low || !high) {
        setStatus("A hue sweep is two angles in degrees, like 0 - 360.", true);
        publishSelected();
        return;
    }
    // **Not sorted**, unlike a BPM range. A sweep from 360 to 0 goes round the wheel the other
    // way, and a sweep from 0 to 720 goes round twice — both are instructions somebody means,
    // and putting them in order would silently turn them into something else.
    rule->dmx.hueFrom = std::clamp(*low, -3600.0, 3600.0);
    rule->dmx.hueTo = std::clamp(*high, -3600.0, 3600.0);
    commit();
    publishSelected();
}

void RulesController::setSize(float percent) {
    if (Rule::Config* rule = current()) {
        rule->dmx.size = std::clamp(static_cast<double>(percent) / 100.0, 0.0, 1.0);
        commit();
    }
}

void RulesController::pickHostPreset(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 || static_cast<std::size_t>(index) >= kHostPresets.size()) {
        return;
    }
    if (index == 0) {
        // **Nothing, when it already is custom.** A dropdown reports a pick of the entry it is
        // already showing — the mouse wheel over a focused one does it a notch at a time — and
        // this box reads "custom" for every address of the operator's own, so a scroll past it
        // wiped the address and every follow-up (the audit of 2026-09-25, M19).
        if (presetOf(rule->address) == 0) {
            return;
        }
        // §5.6's *"blank custom option"*, blanked. The address goes, its chips go with it —
        // `matchSegmentsToAddress` drops a generator when the placeholder it filled does —
        // and so does the press-then-release, which is a thing about Resolume's `connect`
        // and not about OSC. What is left is a rule that sends a value to an address the
        // operator is about to type, which is what the entry has always claimed to be.
        rule->address.clear();
        matchSegmentsToAddress(*rule);
        rule->followUps.clear();
        commit();
        publishSelected();
        setStatus("Cleared the address. Type one, or pick a host to start from.", false);
        return;
    }
    const HostPreset& preset = kHostPresets[static_cast<std::size_t>(index)];
    rule->sendKind = trigger::Message::Kind::Osc;
    rule->address = preset.address;
    matchSegmentsToAddress(*rule);
    // §5.6's Resolume line, whole: "int 1 (press) then 0 (release)". §7.4 is emphatic that
    // a rule sending only the 1 leaves the clip held, so a preset that filled the address
    // and left the release off would have shipped the trap rather than the answer.
    if (trigger::countPlaceholders(rule->address) > 0 ||
        rule->address.find("connect") != std::string::npos) {
        rule->value.kind = GeneratorKind::Fixed;
        rule->value.fixed = trigger::Value::ofInt(1);
        // Replaced rather than added to: picking a host preset is choosing that host's whole
        // gesture, and appending a second release to one an operator already had would send
        // the 0 twice.
        rule->followUps.assign(1, releaseAfterMs(0, 50));
    }
    commit();
    publishSelected();
}

void RulesController::setSendValue(bool on) {
    if (Rule::Config* rule = current()) {
        rule->sendValue = on;
        commit();
    }
}

trigger::FollowUp* RulesController::followConfig(int index) noexcept {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 || static_cast<std::size_t>(index) >= rule->followUps.size()) {
        return nullptr;
    }
    return &rule->followUps[static_cast<std::size_t>(index)];
}

void RulesController::addFollowUp() {
    Rule::Config* rule = current();
    if (rule == nullptr) {
        return;
    }
    if (rule->followUps.size() >= trigger::kMaxFollowUps) {
        setStatus("A trigger can send " + std::to_string(trigger::kMaxFollowUps) +
                      " follow-ups at most.",
                  true);
        return;
    }
    trigger::FollowUp entry;
    // **A release, one beat later.** What an operator adding a row nearly always means is
    // "let go of what was just pressed" — a note off for a note, a 0 for Resolume's connect —
    // and the length they mean it for is musical rather than a number of milliseconds. A
    // first row of "release after 1 beat" is the laser rig's whole ask, ready to test.
    entry.unit = trigger::DelayUnit::Beats;
    entry.delayBeats = 1.0;
    // Except on a pitch bend, where letting go is the **centre** and not the bottom. Zero is
    // hard left on a 14-bit bend; 8192 is the wheel released, which is what a release means.
    if (rule->sendKind == trigger::Message::Kind::MidiPitchBend) {
        entry.value = trigger::Value::ofInt(8192);
    }
    rule->followUps.push_back(entry);
    commit();
    publishSelected();
}

void RulesController::removeFollowUp(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 || static_cast<std::size_t>(index) >= rule->followUps.size()) {
        return;
    }
    rule->followUps.erase(rule->followUps.begin() + index);
    rowRemoved(TypedIn::FollowUp, index);
    commit();
    publishSelected();
}

void RulesController::pickFollowKind(int index, int choice) {
    trigger::FollowUp* entry = followConfig(index);
    if (entry == nullptr || choice < 0 || static_cast<std::size_t>(choice) > followKinds_.size()) {
        return;
    }
    // Zero is "release" — no kind at all, which is what makes the follow-up *be* the message
    // that fired. Everything past it indexes the kinds this rule is allowed to reach.
    entry->kind = choice == 0 ? std::optional<trigger::Message::Kind>{}
                              : followKinds_[static_cast<std::size_t>(choice) - 1];
    commit();
    publishSelected();
}

void RulesController::setFollowNumber(int index, int number) {
    typed(TypedIn::FollowUp, index, 2);
    if (trigger::FollowUp* entry = followConfig(index)) {
        entry->number = std::clamp(number, 0, 127);
        commit();
        publishSelected();
    }
}

void RulesController::setFollowValue(int index, const std::string& text) {
    typed(TypedIn::FollowUp, index, 0);
    if (trigger::FollowUp* entry = followConfig(index)) {
        entry->value = parseValue(text);
        commit();
        publishSelected();
    }
}

void RulesController::setFollowDelay(int index, const std::string& text) {
    typed(TypedIn::FollowUp, index, 1);
    trigger::FollowUp* entry = followConfig(index);
    if (entry == nullptr) {
        return;
    }
    const std::optional<double> number = readNumber(trim(text));
    if (!number) {
        setStatus("A delay is a number, like 1 or 50.", true);
        return;
    }
    // Into whichever of the two numbers the row is showing, leaving the other alone: they are
    // different magnitudes of the same idea, and a single field would turn "50" into "0.077"
    // the moment the unit changed. See `trigger::FollowUp::delayBeats`.
    if (entry->unit == trigger::DelayUnit::Milliseconds) {
        entry->delaySeconds = std::max(0.0, *number) / 1000.0;
    } else {
        entry->delayBeats = std::max(0.0, *number);
    }
    commit();
    publishSelected();
}

void RulesController::pickFollowUnit(int index, int unit) {
    trigger::FollowUp* entry = followConfig(index);
    if (entry == nullptr || unit < 0 ||
        static_cast<std::size_t>(unit) >= trigger::kDelayUnits.size()) {
        return;
    }
    entry->unit = trigger::kDelayUnits[static_cast<std::size_t>(unit)];
    commit();
    publishSelected();
}

void RulesController::pickSlotKind(int slot, int kind) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr || kind < 0 ||
        static_cast<std::size_t>(kind) >= trigger::kGeneratorKinds.size()) {
        return;
    }
    const GeneratorKind picked = trigger::kGeneratorKinds[static_cast<std::size_t>(kind)];
    if (config == paletteConfig() && !trigger::handsBackValues(picked)) {
        return; // not offered there (`color-generator-kinds`); nothing else may set it either
    }
    config->kind = picked;

    // **A color switched to shuffle gets a palette, not a range.**
    //
    // `Generator::Config` draws from the integers 1 to 8 by default, which is §5.8's own
    // reading and right for a clip index. On a color it is nonsense in both directions: the
    // draws are numbers `dmx::parseColor` cannot read, so every fire fell back to white, and
    // the editor drew a box saying "1 - 8" over a generator whose values are `#ff2040`.
    // Reported from a rig on 2026-09-16: *"I define a range 1-9 which maps to what!? it just
    // stays the same #ffff color code"*.
    //
    // So picking shuffle, random or cycle on the color chip seeds six colors the operator
    // can then edit, which is what picking them meant.
    // **And a DMX number switched to shuffle gets that number's range.** The same default,
    // the same nonsense: a level shuffled over 1 to 8 is a lamp between 0.4 % and 3 % of full,
    // and a pan over 1 to 8 is the extreme edge of the head's own window. Only when the range
    // is still the untouched 1-8 — a range the operator has narrowed on purpose is theirs.
    if (const auto natural = slotRange(slot);
        natural && trigger::takesPool(config->kind) && config->low == 1 && config->high == 8) {
        config->low = natural->first;
        config->high = natural->second;
    }

    if (config == paletteConfig() && trigger::takesPool(config->kind)) {
        config->pool = trigger::Pool::List;
        if (config->values.empty()) {
            std::string text;
            config->fixed.appendTo(text);
            const std::optional<dmx::Color> was = dmx::parseColor(text);
            config->values = trigger::defaultPalette();
            if (was && *was != dmx::kWhite) {
                // The color they had stays, at the front — switching to a shuffle should add
                // colors rather than replace the one already chosen.
                config->values.insert(config->values.begin(),
                                      trigger::Value::ofText(dmx::formatColor(*was)));
            }
        }
        pickedPalette_.clear();
    }
    // **A kind is not a number.** Picking "shuffle" on a MIDI number nobody had chosen marked it
    // chosen and armed the rule over the generator's default range, so it fired CC 1 to 8 at
    // 100 before a range was typed — C7 through another door (the 2026-09-25 audit's L11). The
    // number is chosen when its numbers are entered; a live value's are its source's.
    if (picked == GeneratorKind::Live) {
        choseSlot(slot);
    }
    commit();
    publishSelected();
}

void RulesController::setSlotPool(int slot, bool list) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    config->pool = list ? trigger::Pool::List : trigger::Pool::Range;
    // A list switched on with nothing in it is what an operator sees the moment they tick
    // the box, and an empty list sends zero. Seeding it from the range they already had is
    // what makes the switch show them their own values rather than a blank.
    if (list && config->values.empty()) {
        const std::int64_t span = static_cast<std::int64_t>(config->high) - config->low + 1;
        if (span > 0 && span <= 64) {
            for (std::int32_t value = config->low; value <= config->high; ++value) {
                config->values.push_back(trigger::Value::ofInt(value));
            }
        }
    }
    // Nor is the tick a number: a list seeded from the default range is not one anybody chose
    // (L11). Entering it is.
    commit();
}

void RulesController::setSlotRange(int slot, const std::string& text) {
    typed(TypedIn::Slot, slot, 1);
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    const std::vector<std::string_view> parts = split(text, "-– ");
    if (parts.size() != 2) {
        setStatus("A range is two numbers, like 1 - 12.", true);
        return;
    }
    const std::optional<double> low = readNumber(parts[0]);
    const std::optional<double> high = readNumber(parts[1]);
    if (!low || !high) {
        setStatus("A range is two numbers, like 1 - 12.", true);
        return;
    }
    config->low = clampedInt(*low);
    config->high = clampedInt(*high);
    choseSlot(slot);
    commit();
}

void RulesController::setSlotValues(int slot, const std::string& text) {
    typed(TypedIn::Slot, slot, 2);
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    config->values.clear();
    for (const std::string_view part : split(text, ",;")) {
        config->values.push_back(parseValue(part));
    }
    // Typing a list is saying the values come from a list, so the switch follows rather than
    // making the operator tick a box to make what they typed take effect.
    config->pool = trigger::Pool::List;
    choseSlot(slot);
    commit();
}

void RulesController::setSlotNormalise(int slot, const std::string& text) {
    typed(TypedIn::Slot, slot, 4);
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    const std::vector<std::string_view> parts = split(text, "-– ");
    const std::optional<double> low = parts.size() == 2 ? readNumber(parts[0]) : std::nullopt;
    const std::optional<double> high = parts.size() == 2 ? readNumber(parts[1]) : std::nullopt;
    if (!low || !high || !(*high > *low)) {
        setStatus("A tempo range is two numbers, the lower first, like 20 - 500.", true);
        return;
    }
    config->normaliseLow = *low;
    config->normaliseHigh = *high;
    choseSlot(slot);
    commit();
}

void RulesController::setSlotWeights(int slot, const std::string& text) {
    typed(TypedIn::Slot, slot, 3);
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    std::vector<trigger::WeightedChoice> choices;
    for (const std::string_view part : split(text, ",;")) {
        const std::string_view entry = trim(part);
        if (entry.empty()) {
            continue;
        }
        trigger::WeightedChoice choice;
        const std::size_t colon = entry.rfind(':');
        if (colon != std::string_view::npos) {
            const std::optional<double> weight = readNumber(trim(entry.substr(colon + 1)));
            if (!weight || *weight < 0.0) {
                setStatus("A weight is a number of 0 or more, as in 7:3 — 7 three times as often.",
                          true);
                return;
            }
            choice.value = parseValue(trim(entry.substr(0, colon)));
            // Bounded, so a sum of them stays a number the draw can divide by (L5).
            choice.weight = std::min(*weight, 1.0e9);
        } else {
            choice.value = parseValue(entry);
        }
        choices.push_back(choice);
    }
    config->choices = std::move(choices);
    // **No repeat guard on a weighted draw.** The guard refuses the last value and draws again,
    // so with two values it alternates them and the weights mean nothing — measured: 7:3, 12:1
    // came out 201 to 199. A weighted generator is asked how *often* each value comes up, and
    // this row has no "no repeat" box to turn the guard off with, so it is off.
    config->noRepeatWithin = 0;
    choseSlot(slot);
    commit();
}

void RulesController::setSlotNoRepeat(int slot, int within) {
    typed(TypedIn::Slot, slot, 5);
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    const GeneratorKind kind = config->kind;
    config->noRepeatWithin = static_cast<std::size_t>(std::max(0, within));
    commit();
    // **Said out loud, because it has been asked twice.** A number beside the word "no
    // repeat" does not say what it guards, and what it guards is different on the two kinds
    // an operator is choosing between. On shuffle it is *not* redundant — a bag of eight
    // cannot repeat inside itself, but the last draw of one bag and the first of the next are
    // independent, so it repeats at the seam one time in eight, and that is the repeat an
    // audience sees. See `Generator::Config::noRepeatWithin`.
    if (within <= 0) {
        setStatus("Repeats allowed — including the same value twice in a row.", false);
        return;
    }
    const std::string draws = std::to_string(within) + (within == 1 ? " draw" : " draws");
    setStatus(kind == GeneratorKind::Shuffle
                  ? "Never the same as the last " + draws +
                        ". A shuffle cannot repeat inside one pass of its values; this is what "
                        "closes the join between one pass and the next."
                  : "Never the same as the last " + draws + ".",
              false);
}

void RulesController::setSlotFixed(int slot, const std::string& text) {
    typed(TypedIn::Slot, slot, 0);
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    const trigger::Value value = parseValue(text);
    // A MIDI note or controller is a number. Text here would go out as note 0 — and, typed
    // into a number nobody has chosen yet, would arm the rule on it.
    const Rule::Config* rule = current();
    if (slot == 0 && trigger::sendsNumber(rule->sendKind) &&
        value.kind() == trigger::Value::Kind::Text) {
        setStatus(std::string("A ") + numberLabelOf(rule->sendKind) + " number is a number, 0 to 127.",
                  true);
        return;
    }
    config->fixed = value;
    // Typed, so a color's sliders follow what was typed, as the palette's do (`setPaletteHex`):
    // held, they kept the saturation and brightness the picker was last left at, and a hex
    // typed under them came back on the next drag as something else (the audit of 2026-09-25,
    // L14).
    pickedColors_.erase(slot);
    choseSlot(slot);
    commit();
}

void RulesController::setSlotColor(int slot, float hue, float saturation, float brightness) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr) {
        return;
    }
    const dmx::Color picked =
        dmx::fromHsv(static_cast<double>(hue), static_cast<double>(saturation) / 100.0,
                     static_cast<double>(brightness) / 100.0);
    config->fixed = trigger::Value::ofText(dmx::formatColor(picked));
    // The three numbers the picker moved, kept as they were moved rather than re-derived from
    // the color they produced. Dragging brightness to zero makes a black, and black has no
    // hue to read back — so a picker that re-derived would snap to red under the operator's
    // hand the moment they dimmed it, and again when they desaturated it.
    pickedColors_[slot] = Hsv{hue, saturation, brightness};
    // And out to the fixtures as it is dragged — see `previewColor`, which is the whole
    // difference between choosing a color and choosing a swatch.
    previewColor(picked);
    // The publishing `commit` does is the controller echoing a drag back at the element that
    // caused it, which must not be read as a row the operator has to have rebuilt under them.
    // See `pickingColor_`: this is the crash.
    const PickingColor picking(pickingColor_);
    commit();
}

void RulesController::setPickerOpen(bool open) {
    pickerOpen_ = open;
    // A swatch's hex box lives in its picker, and a click outside drops the picker with the box
    // in it — before the box can say it lost the focus. What was typed there is committed now.
    if (!open) {
        commitTyping();
    }
}

void RulesController::previewColor(dmx::Color color) {
    // **The color goes to the lamp while it is being chosen.**
    //
    // Asked for on 2026-09-16, and the reason is one nobody who has programmed lights will
    // argue with: `#20ff80` on a screen and `#20ff80` out of a fixture are not the same
    // color. Three LEDs, a diffuser and a wall between them, and what an operator is
    // choosing is what comes out of the *fixture* — so the picker sends it as they drag and
    // they pick against the real thing rather than against a swatch.
    //
    // The same fixtures the rule names, so the preview lands where the rule will. A rule that
    // names none is a rule that reaches nothing, which the editor already says beside the
    // fixture picker; there is nothing to preview on and nothing is sent.
    //
    // A snap, not a fade: a preview that took two bars to arrive would be a preview of
    // whatever the slider was doing two bars ago. And it is **left showing** afterwards, which
    // is what programming a light means — the next rule to fire on those fixtures takes them
    // back, and Stop's blackout clears it. PANIC does not: it freezes the lights where they
    // are, the preview with them (`DmxEngine::cancelAll`).
    const Rule::Config* rule = current();
    if (rule == nullptr || rule->sendKind != trigger::Message::Kind::Dmx) {
        return;
    }
    // And none while PANIC is engaged — the runner drops it then (the audit's M17), so the
    // editor says why the fixtures are not following the picker.
    if (runner_.panicked()) {
        setStatus("PANIC is engaged, so the preview is not sent to the lights. Press RELEASE "
                  "on the main window first.",
                  true);
        return;
    }
    const dmx::FixtureSet mask = dmx::resolveFixtures(patch_, rule->dmx.fixtures);
    if (mask.none()) {
        return;
    }
    dmx::Payload payload;
    payload.kind = dmx::EffectKind::Color;
    payload.color = color;
    payload.durationSeconds = 0.0f;
    runner_.post(output::OutputCommand::effect(mask, payload));
}

void RulesController::pickColorMode(int index) {
    Rule::Config* rule = current();
    if (rule == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= trigger::kColorModes.size()) {
        return;
    }
    rule->dmx.colorMode = trigger::kColorModes[static_cast<std::size_t>(index)];
    commit();
    publishSelected();
}

std::optional<std::pair<int, int>> RulesController::slotRange(int slot) noexcept {
    // What the whole of this slot's range *is*, for the slots where that is a fact rather than
    // a preference: a DMX level is a byte and a pan is a percentage of the fixture's own
    // window. Nothing for an OSC segment or a MIDI value, where the operator's range is the
    // only one that means anything.
    Rule::Config* rule = current();
    if (rule == nullptr || rule->sendKind != trigger::Message::Kind::Dmx || slot < 0) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(slot);
    std::size_t at = 0;
    if (dmx::takesRole(rule->dmx.effect)) {
        if (index == at) {
            return std::pair{0, 255};
        }
        ++at;
    }
    if (dmx::takesColor(rule->dmx.effect)) {
        if (rule->dmx.colorMode == trigger::ColorMode::Mix) {
            if (index >= at && index < at + 3) {
                return std::pair{0, 255};
            }
            at += 3;
        } else {
            ++at; // the palette, which is a list of colors and has no range
        }
    }
    if (rule->dmx.effect == dmx::EffectKind::Position && index >= at && index < at + 2) {
        return std::pair{0, 100};
    }
    return std::nullopt;
}

trigger::Generator::Config* RulesController::paletteConfig() noexcept {
    Rule::Config* rule = current();
    if (rule == nullptr || rule->sendKind != trigger::Message::Kind::Dmx ||
        !dmx::takesColor(rule->dmx.effect) || rule->dmx.colorMode != trigger::ColorMode::Palette) {
        return nullptr;
    }
    return &rule->dmx.color;
}

void RulesController::addPaletteColor() {
    Generator::Config* config = paletteConfig();
    if (config == nullptr) {
        return;
    }
    // Adding a color is saying the color comes from a list, so the pool follows — the same
    // reasoning `setSlotValues` gives, and without it the first + on a fixed color would add
    // an entry to a list nothing draws from.
    config->pool = trigger::Pool::List;
    if (config->kind == GeneratorKind::Fixed) {
        // The color that was showing becomes the palette's first entry rather than being
        // thrown away, and the kind becomes the one an operator adding a second color means.
        config->values.clear();
        std::string text;
        config->fixed.appendTo(text);
        if (const auto parsed = dmx::parseColor(text)) {
            config->values.push_back(trigger::Value::ofText(dmx::formatColor(*parsed)));
        }
        config->kind = GeneratorKind::Shuffle;
    }
    if (config->values.empty()) {
        config->values = trigger::defaultPalette();
    } else {
        // A new swatch is white, which is unmistakably "I have not picked this yet" — a
        // duplicate of the last one would look like the + had done nothing.
        config->values.push_back(trigger::Value::ofText(dmx::formatColor(dmx::kWhite)));
    }
    commit();
    rebuildAll_ = true;
    rowsDirty_ = true;
    publishSelected();
}

void RulesController::removePaletteColor(int index) {
    Generator::Config* config = paletteConfig();
    if (config == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= config->values.size()) {
        return;
    }
    config->values.erase(config->values.begin() + index);
    rowRemoved(TypedIn::Palette, index);
    pickedPalette_.clear(); // every entry after this one has moved
    // **No picker is open now**, whichever swatch this was. REMOVE is inside the picker and
    // closes it first, and any other click would have closed an open one on its way in. The
    // swatch's own watcher says so for every swatch but the last: the last one's element goes
    // at once, the watcher with it, and the editor held every rebuild for good (the audit of
    // 2026-09-25, M20).
    pickerOpen_ = false;
    commit();
    rebuildAll_ = true;
    rowsDirty_ = true;
    publishSelected();
}

void RulesController::setPaletteColor(int index, float hue, float saturation, float brightness) {
    Generator::Config* config = paletteConfig();
    if (config == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= config->values.size()) {
        return;
    }
    const dmx::Color picked =
        dmx::fromHsv(static_cast<double>(hue), static_cast<double>(saturation) / 100.0,
                     static_cast<double>(brightness) / 100.0);
    config->values[static_cast<std::size_t>(index)] =
        trigger::Value::ofText(dmx::formatColor(picked));
    // The sliders as they were dragged, for the reason `setSlotColor` gives at length: a
    // black has no hue to read back, so re-deriving them would snap the picker to red.
    pickedPalette_[index] = Hsv{hue, saturation, brightness};
    previewColor(picked);
    // As in `setSlotColor`, and for the same reason: a row a slider is driving is a row whose
    // element must survive. See `pickingColor_`.
    const PickingColor picking(pickingColor_);
    commit();
    publishSelected();
}

void RulesController::setPaletteHex(int index, const std::string& text) {
    typed(TypedIn::Palette, index, 0);
    Generator::Config* config = paletteConfig();
    if (config == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= config->values.size()) {
        return;
    }
    const std::optional<dmx::Color> parsed = dmx::parseColor(text);
    if (!parsed) {
        setStatus("A color is #ff2040, ff2040, #f24, or 255, 32, 64.", true);
        publishSelected();
        return;
    }
    config->values[static_cast<std::size_t>(index)] =
        trigger::Value::ofText(dmx::formatColor(*parsed));
    pickedPalette_.erase(index); // typed, so the sliders should follow what was typed
    commit();
    publishSelected();
}

void RulesController::publishPalette() {
    const Generator::Config* config = paletteConfig();
    if (config == nullptr || !trigger::takesPool(config->kind)) {
        // A fixed color has its own swatch on the slot row; there is no palette to show.
        paletteModel_->clear();
        window_->set_palette_shown(false);
        return;
    }
    window_->set_palette_shown(true);

    std::vector<PaletteEntry> rows;
    rows.reserve(config->values.size());
    for (std::size_t i = 0; i < config->values.size(); ++i) {
        std::string text;
        config->values[i].appendTo(text);
        // What could not be read is shown as white and says so in its own box, which is the
        // same answer `Rule::buildPayload` gives the wire — the editor must not show a color
        // the fire would not send.
        const dmx::Color color = dmx::parseColor(text).value_or(dmx::kWhite);
        PaletteEntry row{};
        row.swatch = slint::Color::from_rgb_uint8(color.r, color.g, color.b);
        row.hex = shared(dmx::formatColor(color));
        const auto held = pickedPalette_.find(static_cast<int>(i));
        if (held != pickedPalette_.end()) {
            row.hue = held->second.hue;
            row.sat = held->second.saturation;
            row.val = held->second.brightness;
        } else {
            double hue = 0.0;
            double saturation = 1.0;
            double value = 1.0;
            dmx::toHsv(color, hue, saturation, value);
            row.hue = static_cast<float>(hue);
            row.sat = static_cast<float>(saturation * 100.0);
            row.val = static_cast<float>(value * 100.0);
        }
        rows.push_back(std::move(row));
    }
    // **Not while a slider is driving it.** A swatch the operator is dragging is a swatch
    // whose row moved on every pixel, and rebuilding the repeater would destroy the popup
    // holding the slider they are still holding. See `pickingColor_` — this is the crash of
    // 2026-09-16, and taking this condition out makes the test below it fail with ninety
    // resets for a ninety-pixel drag.
    if (!pickingColor_ &&
        markStale(*paletteModel_, rows, stalePalette_, [](const PaletteEntry& was, const PaletteEntry& now) {
            return was != now;
        })) {
        rowsDirty_ = true;
    }
    writeRows(*paletteModel_, rows);
}

void RulesController::pickSlotLive(int slot, int source) {
    Generator::Config* config = slotConfig(slot);
    if (config == nullptr || source < 0 ||
        static_cast<std::size_t>(source) >= trigger::kLiveSources.size()) {
        return;
    }
    config->source = trigger::kLiveSources[static_cast<std::size_t>(source)];
    choseSlot(slot);
    commit();
}

void RulesController::adoptLive(const std::vector<output::OutputRunner::LiveRule>& live) {
    bool enabledMoved = false;
    for (const output::OutputRunner::LiveRule& one : live) {
        const auto rule = std::find_if(rules_.begin(), rules_.end(),
                                       [&one](const Rule::Config& config) { return config.id == one.id; });
        if (rule == rules_.end()) {
            continue;
        }
        if (rule->enabled != one.enabled) {
            rule->enabled = one.enabled;
            enabledMoved = true;
        }
        mutedSeen_[one.id] = one.muted;
        rateSeen_[one.id] = one.rate;
    }
    if (enabledMoved && changed_) {
        changed_(rules_);
    }
    publishList();
    if (const Rule::Config* rule = current()) {
        window_->set_rule_enabled(rule->enabled);
        const auto muted = mutedSeen_.find(rule->id);
        window_->set_rule_muted(muted != mutedSeen_.end() && muted->second);
        const auto rate = rateSeen_.find(rule->id);
        window_->set_rule_rate(shared(describeRate(rate == rateSeen_.end() ? 1.0 : rate->second)));
    }
}

void RulesController::clearLog() {
    log_.clear();
    logModel_->clear();
}

void RulesController::tick() {
    // First, and outside every widget callback: a publisher found a row it could not honestly
    // update in place, and this is where the repeater is built again. See `rowsDirty_`.
    if (rowsDirty_) {
        rebuildRows();
    }
    // What a control surface changed behind this editor's back. See `adoptLive`. Only once the
    // output thread has got to the last set posted — this editor's own last edit, most often:
    // before then the switches it reports are the set before, and adopting them undid the edit.
    // The version is left unseen meanwhile, so they are read the moment they are current.
    if (const std::uint64_t version = runner_.liveRulesVersion();
        version != liveSeen_ && runner_.liveRulesCurrent()) {
        liveSeen_ = version;
        adoptLive(runner_.liveRules());
    }
    // Drained whether or not the window is up: the buffer is bounded and dropping the
    // oldest, so a log that is never drained would quietly lose the start of a set. It is
    // also the cheapest possible call when nothing has fired.
    const std::vector<output::OutputRunner::Fired> fired = runner_.takeFired();
    if (!fired.empty()) {
        const Rule::Config* rule = current();
        for (const output::OutputRunner::Fired& entry : fired) {
            // A muted rule still runs and still "fires" — that is what keeps it in phase — but
            // nothing left, and the log said it had (the audit's M11).
            const std::string message =
                entry.muted ? entry.message + "  (muted, not sent)" : entry.message;
            // **By the name the list shows**, not the id — "rule1-copy2" is a word this window
            // shows nowhere else, so a log line could not be matched to a rule (the audit of
            // 2026-09-25, L38). Named as the rule is called when it fired; the id stays the
            // key, and only one no longer in the set is said to be gone.
            const auto named =
                std::find_if(rules_.begin(), rules_.end(),
                             [&entry](const Rule::Config& one) { return one.id == entry.ruleId; });
            const std::string who = named == rules_.end() ? "(a rule since deleted)"
                                    : named->name.empty() ? "(unnamed)"
                                                          : named->name;
            log_.push_back(spellNumber(entry.when) + "s  " + who + "  " + message);
            lastFiredAnywhere_ = message;
            if (!entry.followUp) {
                // The press, not the release: see `OutputRunner::Fired::followUp`.
                ++firesSeen_[entry.ruleId];
                slotsSeen_[entry.ruleId] = entry.slots;
            }
            if (rule != nullptr && entry.ruleId == rule->id) {
                lastFired_ = message;
                lastFiredAt_ = entry.when;
            }
        }
        if (log_.size() > kLogLines) {
            log_.erase(log_.begin(),
                       log_.begin() + static_cast<std::ptrdiff_t>(log_.size() - kLogLines));
        }
        std::vector<slint::SharedString> lines;
        lines.reserve(log_.size());
        // Newest first: the thing that just happened is what an operator is looking for, and
        // a list that scrolls itself is a list they have to chase.
        for (auto line = log_.rbegin(); line != log_.rend(); ++line) {
            lines.push_back(shared(*line));
        }
        writeRows(*logModel_, lines);
        publishList();
        publishSlots();
    }

    publishFiring();
    window_->set_panicked(runner_.panicked());
}

void RulesController::publishAll() {
    publishList();
    publishSelected();
    publishSlots();
    publishFiring();
    window_->set_selected(selected_);
}

std::uint64_t RulesController::firesOf(std::string_view ruleId) const {
    const auto at = firesSeen_.find(std::string(ruleId));
    return at == firesSeen_.end() ? 0 : at->second;
}

void RulesController::publishList() {
    std::vector<RuleRow> rows;
    rows.reserve(rules_.size());
    for (const Rule::Config& config : rules_) {
        // Built rather than read from the runner: `OutputRunner::triggers()` is the output
        // thread's and is safe to read only while it is stopped. A `Rule` costs a few
        // generators to construct and this runs on a list of a handful, once per edit.
        const Rule rule(config);
        RuleRow row{};
        row.name = shared(config.name);
        row.enabled = config.enabled;
        row.problem = shared(rule.problem());
        // By id, so a rule deleted from the middle does not hand its count to the one that
        // moves up into its place. See `firesSeen_`.
        row.fires = static_cast<int>(firesOf(config.id));
        row.chosen = rows.size() < chosen_.size() && chosen_[rows.size()];
        rows.push_back(std::move(row));
    }
    // And the card's own line for the one being edited, from the same `Rule` — here rather than
    // only in `publishSelected`, because `commit` does not call that and nearly every edit that
    // changes whether a rule can fire goes through `commit`.
    window_->set_rule_problem(selected_ >= 0 && static_cast<std::size_t>(selected_) < rows.size()
                                  ? rows[static_cast<std::size_t>(selected_)].problem
                                  : slint::SharedString(""));
    // In place: this runs on the redraw timer whenever a rule fires, and the fire counts
    // move on every beat. See `writeRows`.
    writeRows(*listModel_, rows);
}

void RulesController::publishSelected() {
    const Rule::Config* rule = current();
    window_->set_selected(selected_);
    if (rule == nullptr) {
        window_->set_rule_name(slint::SharedString(""));
        window_->set_rule_enabled(false);
        window_->set_rule_problem(slint::SharedString(""));
        window_->set_address(slint::SharedString(""));
        return;
    }
    window_->set_rule_name(shared(rule->name));
    window_->set_rule_enabled(rule->enabled);
    window_->set_rule_problem(shared(Rule(*rule).problem()));

    const auto triggerIndex =
        std::find(trigger::kTriggers.begin(), trigger::kTriggers.end(), rule->trigger) -
        trigger::kTriggers.begin();
    window_->set_trigger_index(static_cast<int>(triggerIndex));
    window_->set_trigger_takes_every(trigger::takesEvery(rule->trigger));
    window_->set_every(static_cast<int>(rule->every));
    window_->set_trigger_takes_pulses(trigger::takesPulses(rule->trigger));
    window_->set_pulses(static_cast<int>(rule->pulses));
    window_->set_euclid_pattern(shared(spellEuclid(*rule)));

    // §5.6's rule subset, and what it currently reaches. Both are needed: the first is what
    // the rule names and the second is whether this rig has it, which is the one question a
    // preset from another rig raises.
    publishOutputChoices();
    window_->set_outputs_available(
        shared(describeRouting(rule->sendKind, rule->outputs, targets_)));

    window_->set_min_confidence(static_cast<float>(rule->conditions.minConfidence));
    window_->set_allow_calm(rule->conditions.allows(features::Intensity::Calm));
    window_->set_allow_normal(rule->conditions.allows(features::Intensity::Normal));
    window_->set_allow_intense(rule->conditions.allows(features::Intensity::Intense));
    window_->set_probability(static_cast<float>(rule->conditions.probability));
    // Spelled here rather than by the markup, because both boxes are two-way bound and so the
    // property holds *text* — see `bpm-field`, where the one-way version's failure is written
    // down. `spellNumber` so a range reads "70 - 140" and not "70.000000 - 140.000000".
    //
    // And so writing one writes over what is being typed in it. Every action commits that first
    // (`commitTyping`), so this skips only a republish from outside — the main window's outputs
    // or patch changing — which would otherwise take the keystrokes out from under the operator
    // (the audit of 2026-09-25, M18).
    if (!typingInto(kBpmRangeBox)) {
        window_->set_bpm_range(shared(spellNumber(rule->conditions.minBpm) + " - " +
                                      spellNumber(rule->conditions.maxBpm)));
    }
    if (!typingInto(kCooldownBox)) {
        window_->set_cooldown_ms(
            shared(spellNumber(rule->conditions.cooldownSeconds * 1000.0)));
    }

    const auto sendIndex =
        std::find(trigger::kMessageKinds.begin(), trigger::kMessageKinds.end(), rule->sendKind) -
        trigger::kMessageKinds.begin();
    window_->set_send_index(static_cast<int>(sendIndex));
    window_->set_sends_osc(rule->sendKind == trigger::Message::Kind::Osc);
    // Three families now rather than two, so "not OSC" is no longer "MIDI": the channel
    // spinner has to stay off a lighting rule, which has no channel at all.
    window_->set_sends_midi(trigger::isMidi(rule->sendKind));
    publishDmx();
    if (!typingInto(kAddressBox)) {
        window_->set_address(shared(rule->address));
    }
    // Which preset this address is, so the picker describes the rule in front of it rather
    // than the last thing anybody clicked in it.
    window_->set_host_preset_index(presetOf(rule->address));
    window_->set_channel(rule->channel);
    window_->set_send_value(rule->sendValue);

    // The live gestures, from this class's own mirror — the output thread's rules are not safe
    // to read while it runs. See `mutedSeen_`.
    const auto muted = mutedSeen_.find(rule->id);
    window_->set_rule_muted(muted != mutedSeen_.end() && muted->second);
    const auto rate = rateSeen_.find(rule->id);
    const double factor = rate == rateSeen_.end() ? 1.0 : rate->second;
    window_->set_rule_rate(shared(describeRate(factor)));

    publishFollowUps();
}

void RulesController::publishFollowUps() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        followModel_->clear();
        followsBuiltFor_.clear();
        followKinds_.clear();
        followRelease_.clear();
        window_->set_can_add_follow_up(false);
        return;
    }
    // Another rule's rows are built from nothing, as the chips are (`slotsBuiltFor_`). The boxes
    // on them no longer go deaf, so nothing else rebuilds them — and a box that had the keyboard
    // would go on holding what was typed for the rule before, over this one's value, and commit
    // it into this one when it let go.
    if (rule->id != followsBuiltFor_) {
        followModel_->clear();
        staleFollows_.clear();
        followsBuiltFor_ = rule->id;
    }

    // What a follow-up on *this* rule may be, and the two things left out of it.
    //
    // Kinds on the far side of the OSC/MIDI divide go, because `Rule::followUpsFor` would
    // drop them — a MIDI follow-up to an OSC rule has no channel or note to inherit — and
    // offering a choice that does nothing is worse than not offering it. `pickSend` turns any
    // that a change of send kind stranded into releases, so no row can name one.
    //
    // And OSC goes, because on an OSC rule it *is* the first entry: the same address with a
    // different argument is what a release means, so the two would behave identically. DMX
    // goes for the same reason on a lighting rule (the audit's M10): a DMX follow-up with no
    // effect of its own — and nothing here can give it one — is exactly the release, so the
    // list offered "release" and "DMX" and both did the same thing. A MIDI kind matching the
    // rule's own is a different matter and stays — it carries a number of its own where a
    // release inherits the one that fired, which is a real second gesture.
    std::vector<trigger::Message::Kind> allowed;
    for (const trigger::Message::Kind kind : trigger::kMessageKinds) {
        if (kind != trigger::Message::Kind::Osc && kind != trigger::Message::Kind::Dmx &&
            trigger::followUpFits(kind, rule->sendKind)) {
            allowed.push_back(kind);
        }
    }
    // **"release", and what it releases.** It is the default, and the only entry that can
    // follow a *drawn* number, since the note to let go of is the one the shuffle picked —
    // which is exactly the thing a rig went looking for and did not find: it read the bare
    // word "release" as some other gesture, picked the explicit note off, and found a box
    // demanding one fixed note. The entry now says what it inherits, so the dynamic one is the
    // one that looks dynamic.
    const std::string release = releaseLabelOf(rule->sendKind);
    if (allowed != followKinds_ || release != followRelease_) {
        followKinds_ = allowed;
        followRelease_ = release;
        auto labels = std::make_shared<slint::VectorModel<slint::SharedString>>();
        labels->push_back(shared(release));
        for (const trigger::Message::Kind kind : followKinds_) {
            labels->push_back(shared(std::string(trigger::labelOf(kind))));
        }
        window_->set_follow_kinds(labels);
    }
    window_->set_can_add_follow_up(rule->followUps.size() < trigger::kMaxFollowUps);

    std::vector<FollowRow> rows;
    rows.reserve(rule->followUps.size());
    for (const trigger::FollowUp& entry : rule->followUps) {
        FollowRow row{};
        int choice = 0;
        if (entry.kind) {
            const auto at = std::find(followKinds_.begin(), followKinds_.end(), *entry.kind);
            // A kind the rule can no longer reach — the send kind was changed under it — reads
            // as a release, which is what it will actually behave as (`followUpFits` drops it,
            // and a release is what the row then means). The configuration is left alone until
            // the operator touches the row, so switching kinds back restores what they had.
            choice = at == followKinds_.end() ? 0 : static_cast<int>(at - followKinds_.begin()) + 1;
        }
        row.kind_index = choice;
        // A release inherits the fired message's number, so it has none of its own to show.
        row.takes_number = entry.kind && trigger::sendsNumber(*entry.kind);
        row.number_label = shared(row.takes_number ? numberLabelOf(*entry.kind) : "");
        row.number = entry.number;
        row.takes_value = !entry.kind || trigger::sendsValue(*entry.kind) ||
                          *entry.kind == trigger::Message::Kind::Osc;
        // Except the release of a move, which is skipped whole: a level box beside it would
        // be a setting that does nothing. The summary says why.
        if (!entry.kind && rule->sendKind == trigger::Message::Kind::Dmx &&
            dmx::takesMovement(rule->dmx.effect)) {
            row.takes_value = false;
        }
        // Named for whatever the row resolves to, which for a release is the *released* kind
        // and not the rule's: a note on's release carries a release velocity, and a box
        // holding a bare 0 had an operator asking what it was.
        row.value_label =
            shared(valueLabelOf(entry.kind ? *entry.kind : releasedAs(rule->sendKind)));
        row.value = shared(spellValue(entry.value));
        const auto unitIndex =
            std::find(trigger::kDelayUnits.begin(), trigger::kDelayUnits.end(), entry.unit) -
            trigger::kDelayUnits.begin();
        row.unit = static_cast<int>(unitIndex);
        row.delay = shared(entry.unit == trigger::DelayUnit::Milliseconds
                               ? spellNumber(entry.delaySeconds * 1000.0)
                               : spellNumber(entry.delayBeats));
        row.summary = shared(describeFollowUp(entry, *rule));
        rows.push_back(std::move(row));
    }

    // Nothing on these rows moves on its own — no live readout — so a change is one the
    // controller made and has to get back into a widget that may have gone deaf. See
    // `rowsDirty_`. **Except the three boxes**: a `NumberBox` and a `LiveField` never go deaf,
    // and a row rebuilt for one of them was the row, and the box the operator had just tabbed
    // into, destroyed under the next keystroke (the audit of 2026-09-25, M17).
    if (markStale(*followModel_, rows, staleFollows_,
                  [](const FollowRow& was, const FollowRow& now) {
                      FollowRow ignoring = was;
                      ignoring.number = now.number;
                      ignoring.value = now.value;
                      ignoring.delay = now.delay;
                      return ignoring != now;
                  })) {
        rowsDirty_ = true;
    }
    writeRows(*followModel_, rows);
}

void RulesController::publishOutputChoices() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        choiceModel_->clear();
        window_->set_outputs_all(false);
        window_->set_outputs_summary(slint::SharedString(""));
        return;
    }

    const auto ids = rule->outputs;
    const auto routed = [&ids](const std::string& id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };

    std::vector<OutputChoice> rows;
    rows.reserve(targets_.size() + ids.size());
    for (const output::OutputTarget& target : targets_) {
        // **Only the targets this rule's kind can actually reach.** See `targetTakes`: a MIDI
        // rule ticked against an OSC target sends nothing, and nothing routes to an Art-Net
        // node by name at all. A target this rule *names* but cannot reach is still listed,
        // below, as a name that reaches nothing — which is the truth and is how it gets
        // un-ticked.
        if (!targetTakes(rule->sendKind, target)) {
            continue;
        }
        OutputChoice row{};
        row.key = shared(target.id);
        row.name = shared(target.name);
        row.chosen = routed(target.id);
        row.missing = false;
        rows.push_back(std::move(row));
    }
    // Outputs this rule is routed to that the rig no longer has, and ones it has that this
    // kind cannot reach — a rule switched from OSC to MIDI while still routed to an OSC target.
    // Listed rather than dropped, because the outcome is the same either way: the rule holds
    // it and it reaches nothing, and the list is where that is seen and un-ticked.
    for (const std::string& id : ids) {
        const output::OutputTarget* const target = output::findTarget(targets_, id);
        if (target == nullptr || !targetTakes(rule->sendKind, *target)) {
            OutputChoice row{};
            row.key = shared(id);
            row.name = shared(target != nullptr ? target->name : std::string("an output that is gone"));
            row.chosen = true;
            row.missing = true;
            rows.push_back(std::move(row));
        }
    }

    // **These are tick boxes, and a tick box drops its binding the moment it is clicked.**
    //
    // The same mechanism the generator chips and the THEN SEND rows are rebuilt for, on the
    // one publisher that did not take part in it. A `CheckBox` bound `checked: choice.chosen`
    // stops following the model as soon as somebody ticks it — so after routing one rule to
    // "lights", every rule selected afterwards showed "lights" already ticked, and clicking
    // it to route *that* rule un-ticked it and did nothing. Nothing on this row moves on its
    // own, so any change at all is one the controller made and has to get back into a box
    // that may have gone deaf.
    if (markStale(*choiceModel_, rows, staleChoices_, [](const OutputChoice& was, const OutputChoice& now) {
            return was != now;
        })) {
        rowsDirty_ = true;
    }
    writeRows(*choiceModel_, rows);

    std::vector<std::string> shown;
    for (const std::string& id : ids) {
        const output::OutputTarget* const target = output::findTarget(targets_, id);
        shown.push_back(target != nullptr ? target->name : std::string("an output that is gone"));
    }
    window_->set_outputs_all(ids.empty());
    window_->set_outputs_summary(shared(ids.empty() ? "every output" : join(shown)));
}

void RulesController::publishFixtureChoices() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        fixtureModel_->clear();
        window_->set_fixtures_summary(slint::SharedString(""));
        window_->set_fixtures_available(slint::SharedString(""));
        window_->set_fixtures_reaches_nothing(false);
        return;
    }

    const std::vector<std::string>& aims = rule->dmx.fixtures;
    const auto aimed = [&aims](const std::string& key) {
        return std::find(aims.begin(), aims.end(), key) != aims.end();
    };

    std::vector<OutputChoice> rows;
    rows.reserve(patch_.size() + aims.size());
    // Groups first, because aiming at "heads" is what an operator reaches for and a list that
    // buried it under six fixture names would hide the useful half. Each group once, in the
    // order the patch introduces it.
    std::vector<std::string> groups;
    for (const dmx::Fixture& fixture : patch_) {
        if (fixture.group.empty() ||
            std::find(groups.begin(), groups.end(), fixture.group) != groups.end()) {
            continue;
        }
        groups.push_back(fixture.group);
        OutputChoice row{};
        row.key = shared(fixture.group); // a group is its label; see `dmx::Fixture::group`
        row.name = shared(fixture.group);
        row.chosen = aimed(fixture.group);
        row.missing = false;
        rows.push_back(std::move(row));
    }
    // Only the first `dmx::kMaxRoutableFixtures` (512): a rule carries its fixtures as a bit
    // each, and `dmx::resolveFixtures` reaches no further, so a tick past them would look like
    // aiming and reach nothing (the audit's M21). The patch editor says as much when one past the
    // last is added.
    const std::size_t reachable = std::min(patch_.size(), dmx::kMaxRoutableFixtures);
    for (std::size_t i = 0; i < reachable; ++i) {
        const dmx::Fixture& fixture = patch_[i];
        OutputChoice row{};
        row.key = shared(fixture.id);
        row.name = shared(fixture.name);
        row.chosen = aimed(fixture.id);
        row.missing = false;
        rows.push_back(std::move(row));
    }
    // What this rule aims at that the patch has no fixture or group for — a fixture since
    // deleted, or a group label no fixture carries any more. Listed rather than dropped, so
    // the routing that reaches nothing is seen and can be un-ticked.
    for (const std::string& key : aims) {
        const bool here = std::find(groups.begin(), groups.end(), key) != groups.end() ||
                          dmx::findFixture(patch_, key) != nullptr;
        if (!here) {
            OutputChoice row{};
            row.key = shared(key);
            // A group's label reads as itself; a fixture's id means nothing to anybody.
            row.name = shared(key.rfind("f-", 0) == 0 ? std::string("a fixture that is gone") : key);
            row.chosen = true;
            row.missing = true;
            rows.push_back(std::move(row));
        }
    }

    // Tick boxes, which drop their binding the moment they are clicked — see
    // `publishOutputChoices`, which found that the hard way.
    if (markStale(*fixtureModel_, rows, staleFixtures_, [](const OutputChoice& was, const OutputChoice& now) {
            return was != now;
        })) {
        rowsDirty_ = true;
    }
    writeRows(*fixtureModel_, rows);

    std::vector<std::string> shown;
    for (const std::string& key : aims) {
        const dmx::Fixture* const fixture = dmx::findFixture(patch_, key);
        shown.push_back(fixture != nullptr ? fixture->name
                        : key.rfind("f-", 0) == 0 ? std::string("a fixture that is gone")
                                                  : key);
    }
    window_->set_fixtures_summary(
        shared(aims.empty() ? "nothing — this rule sends nowhere" : join(shown)));
    // What the rule actually reaches on *this* rig, in fixtures. The count matters: "heads"
    // reaching three fixtures and "heads" reaching none look identical in a list of ticks.
    const std::size_t reached = dmx::resolveFixtures(patch_, aims).count();
    std::string available;
    if (aims.empty()) {
        available = "pick at least one — a DMX rule with no fixtures does nothing";
    } else if (reached == 0) {
        available = "reaches nothing on this rig";
    } else {
        available =
            "reaches " + std::to_string(reached) + (reached == 1 ? " fixture" : " fixtures");
    }
    window_->set_fixtures_available(shared(available));
    window_->set_fixtures_reaches_nothing(reached == 0);
}

std::string RulesController::describeRoleReach(const trigger::DmxSend& send) const {
    // **What the rule editor could not say, and should have.** A rule aimed at `dimmer` over
    // an RGB par used to be a rule that fired, logged, counted and lit nothing — the fixture
    // has no dimmer channel, so the effect reached no channel and the only trace was a number
    // in `DmxEngine::missed()` that nothing shows. Reported from a rig on 2026-09-16:
    // *"Choosing the closest thing available right now, dimmer, does absolutely nothing."*
    //
    // The engine now fakes a dimmer out of the color (see `dmx::aims`), so the honest line
    // says which of the two is happening — and still says "reaches nothing" for the aim that
    // really does, a pan on a wash.
    if (!dmx::takesRole(send.effect)) {
        return {};
    }
    const dmx::FixtureSet mask = dmx::resolveFixtures(patch_, send.fixtures);
    if (mask.none()) {
        return {}; // "reaches no fixtures" is already said beside the fixture picker
    }
    std::size_t reached = 0;
    std::size_t faked = 0;
    std::size_t total = 0;
    const std::size_t count = std::min(patch_.size(), dmx::kMaxRoutableFixtures);
    for (std::size_t i = 0; i < count; ++i) {
        if (!mask.test(i)) {
            continue;
        }
        ++total;
        if (dmx::has(patch_[i], send.role)) {
            ++reached;
        } else if (dmx::aims(patch_[i], send.role)) {
            ++faked;
        }
    }
    if (total == 0) {
        return {};
    }
    const std::string channel(dmx::labelOf(send.role));
    if (reached + faked == 0) {
        return "none of them has a " + channel + " channel — this reaches nothing";
    }
    if (faked > 0 && reached == 0) {
        return faked == total ? "no " + channel + " channel: drives the color instead"
                              : "some have no " + channel + " channel: those drive the color";
    }
    if (reached < total) {
        return std::to_string(total - reached) + " of " + std::to_string(total) + " have no " +
               channel + " channel";
    }
    return {};
}

void RulesController::publishDmx() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        window_->set_sends_dmx(false);
        return;
    }
    const trigger::DmxSend& send = rule->dmx;
    window_->set_sends_dmx(rule->sendKind == trigger::Message::Kind::Dmx);

    const auto effectIndex =
        std::find(dmx::kEffectKinds.begin(), dmx::kEffectKinds.end(), send.effect) -
        dmx::kEffectKinds.begin();
    window_->set_effect_index(static_cast<int>(effectIndex));
    const auto roleIndex =
        std::find(dmx::kAimableRoles.begin(), dmx::kAimableRoles.end(), send.role) -
        dmx::kAimableRoles.begin();
    window_->set_effect_role_index(static_cast<int>(
        roleIndex < static_cast<long long>(dmx::kAimableRoles.size()) ? roleIndex : 0));
    const auto curveIndex =
        std::find(dmx::kCurves.begin(), dmx::kCurves.end(), send.curve) - dmx::kCurves.begin();
    window_->set_effect_curve_index(static_cast<int>(curveIndex));
    const auto shapeIndex =
        std::find(dmx::kPathShapes.begin(), dmx::kPathShapes.end(), send.shape) -
        dmx::kPathShapes.begin();
    window_->set_effect_shape_index(static_cast<int>(shapeIndex));

    // Which controls this effect actually uses. Everything else is hidden rather than
    // disabled: a strobe's duty cycle greyed out on a fade is a control an operator has to
    // work out is irrelevant, and there are eight of them.
    window_->set_effect_takes_role(dmx::takesRole(send.effect));
    window_->set_effect_takes_base(dmx::takesBase(send.effect));
    window_->set_effect_takes_cycles(dmx::takesCycles(send.effect));
    window_->set_effect_takes_duty(send.effect == dmx::EffectKind::Strobe);
    window_->set_effect_takes_hue(send.effect == dmx::EffectKind::HueSweep);
    window_->set_effect_takes_shape(send.effect == dmx::EffectKind::Path);
    window_->set_effect_takes_curve(send.effect != dmx::EffectKind::Strobe);
    window_->set_effect_takes_color(dmx::takesColor(send.effect));

    const auto colorModeIndex =
        std::find(trigger::kColorModes.begin(), trigger::kColorModes.end(), send.colorMode) -
        trigger::kColorModes.begin();
    window_->set_color_mode_index(static_cast<int>(colorModeIndex));
    window_->set_effect_role_note(shared(describeRoleReach(send)));

    window_->set_effect_base(send.base);
    window_->set_effect_duty(static_cast<float>(send.duty * 100.0));
    window_->set_effect_size(static_cast<float>(send.size * 100.0));
    // The three text boxes are bound both ways, so writing one writes over whatever is in it —
    // left alone while it holds keystrokes nobody has committed (see `typingInto`).
    if (!typingInto(kHueBox)) {
        window_->set_effect_hue(
            shared(spellNumber(send.hueFrom) + " - " + spellNumber(send.hueTo)));
    }
    if (!typingInto(kCyclesBox)) {
        window_->set_effect_cycles(shared(spellNumber(send.cycles)));
    }
    const auto unitIndex =
        std::find(trigger::kDelayUnits.begin(), trigger::kDelayUnits.end(), send.unit) -
        trigger::kDelayUnits.begin();
    window_->set_effect_unit(static_cast<int>(unitIndex));
    if (!typingInto(kDurationBox)) {
        window_->set_effect_duration(shared(send.unit == trigger::DelayUnit::Milliseconds
                                                ? spellNumber(send.durationSeconds * 1000.0)
                                                : spellNumber(send.durationBeats)));
    }
    publishFixtureChoices();
}

void RulesController::publishSlots() {
    const Rule::Config* rule = current();
    if (rule == nullptr) {
        slotModel_->clear();
        slotsBuiltFor_.clear();
        return;
    }

    // **Rebuild from scratch when the rule or its send kind changes, and only then.**
    //
    // `writeRows` updates rows in place so that a box being typed into is not destroyed under
    // the cursor — see its header, which is right about that and did not go far enough. The
    // consequence it records is that a widget that assigns its own value — a `ComboBox` picked
    // from, and the `LineEdit`s these boxes were until the audit of 2026-09-25 — loses its
    // binding the moment it is used, because Slint drops a binding when the property is
    // assigned. Within one rule that is harmless: what was picked is what is meant.
    //
    // Across rules it is not. Selecting a different rule updates the model rows, the dead
    // widget ignores them, and the operator sees the *previous* rule's range, list and mode
    // on every rule they click — which reads exactly like one edit having changed them all.
    // It was reported that way on 2026-09-12, and the giveaway was that toggling the `list`
    // checkbox "fixed" it: that flips an `if`, so the element is destroyed and the new one
    // comes up bound.
    //
    // Clearing first makes the repeater build fresh items, so every box is bound again. It
    // costs the focus, which is right here — the operator just clicked another rule — and it
    // never happens on the `tick` path, where the id and kind are unchanged.
    // The DMX effect counts as a change of kind here, because it decides *which* generators a
    // rule has: switching from a fade to a position takes the level chip away and puts pan and
    // tilt in its place, and a row reused across that would be a pan box still bound to the
    // level it used to be.
    const bool rebuild = rule->id != slotsBuiltFor_ || rule->sendKind != slotsKind_ ||
                         rule->dmx.effect != slotsEffect_ || rule->dmx.colorMode != slotsColorMode_;
    if (rebuild) {
        slotModel_->clear();
        slotsBuiltFor_ = rule->id;
        slotsKind_ = rule->sendKind;
        slotsEffect_ = rule->dmx.effect;
        // The color mode counts for the same reason the effect does: it decides *which*
        // generators the rule has — one color chip or three component chips — and a row
        // reused across that would be a red box still bound to the palette it used to be.
        slotsColorMode_ = rule->dmx.colorMode;
        // The picker's sliders belong to the rows that were on screen, and the rows are about
        // to be different ones. Kept across an ordinary republish — that is the whole point of
        // holding them — and dropped when the slots themselves change, so another rule's
        // picker opens on that rule's own color.
        pickedColors_.clear();
    }

    // What this rule's generators produced last time it fired, in the order the chips are
    // built below — which is the order `Rule::lastSlots` promises. Empty until it has fired.
    const auto seen = slotsSeen_.find(rule->id);
    const std::vector<trigger::Value>* const produced =
        seen == slotsSeen_.end() ? nullptr : &seen->second;

    std::vector<SlotRow> rows;
    const auto push = [&rows, produced, this](const std::string& label,
                                              const Generator::Config& raw, bool color = false) {
        // What the generator **accepted**, not what was typed. `Generator::config()` gives
        // back the clamped configuration, so §5.8's clamp-never-refuse policy shows on
        // screen instead of being silent: a range typed backwards comes back the right way
        // round, and a no-repeat wider than the pool comes back cut to what can be
        // satisfied. An operator who cannot see the clamp has no way to know it happened.
        const Generator::Config config = Generator(raw).config();
        SlotRow row{};
        row.label = shared(label);
        const auto kindIndex = std::find(trigger::kGeneratorKinds.begin(),
                                         trigger::kGeneratorKinds.end(), config.kind) -
                               trigger::kGeneratorKinds.begin();
        row.kind_index = static_cast<int>(kindIndex);
        row.takes_pool = trigger::takesPool(config.kind);
        row.is_list = config.pool == trigger::Pool::List;
        row.low = config.low;
        row.high = config.high;
        row.values = shared(spellValues(config.values));
        row.no_repeat = static_cast<int>(config.noRepeatWithin);
        row.fixed = shared(spellValue(config.fixed));
        row.is_fixed = config.kind == GeneratorKind::Fixed;
        row.is_live = config.kind == GeneratorKind::Live;
        row.is_normalised = row.is_live && config.source == trigger::LiveSource::BpmNormalised;
        row.normalise = shared(spellNumber(config.normaliseLow) + " - " +
                               spellNumber(config.normaliseHigh));
        row.is_weighted = config.kind == GeneratorKind::Weighted;
        row.weights = shared(spellWeights(config.choices));
        const auto sourceIndex =
            std::find(trigger::kLiveSources.begin(), trigger::kLiveSources.end(), config.source) -
            trigger::kLiveSources.begin();
        row.live_index = static_cast<int>(sourceIndex);
        row.is_ramp = config.kind == GeneratorKind::Ramp;
        const auto shapeIndex =
            std::find(trigger::kRampShapes.begin(), trigger::kRampShapes.end(), config.shape) -
            trigger::kRampShapes.begin();
        row.shape_index = static_cast<int>(shapeIndex);
        row.ramp_bars = static_cast<int>(config.rampBars);
        row.ramp_float = config.rampFloat;
        // §5.9's "what this slot last produced", paired by position: this chip is being
        // pushed at index `rows.size()`, which is the slot `Rule::lastSlots` put there.
        // Blank rather than stale when the rule has not fired since it was last edited —
        // adding a placeholder shifts every chip after it, and a number under the wrong box
        // is worse than no number.
        row.last = produced != nullptr && rows.size() < produced->size()
                       ? shared(spellValue((*produced)[rows.size()]))
                       : slint::SharedString("");

        // A color slot gets a swatch and a picker, because `#20ff80` is the one value in
        // this window nobody can read. The swatch shows what the generator is *set* to — a
        // fixed color, or the first of a palette — so an operator glancing at the row sees
        // the color rather than its arithmetic.
        row.is_color = color;
        if (color) {
            std::string text;
            if (config.kind == GeneratorKind::Fixed) {
                config.fixed.appendTo(text);
            } else if (!config.values.empty()) {
                config.values.front().appendTo(text);
            }
            const dmx::Color swatch = dmx::parseColor(text).value_or(dmx::kWhite);
            row.swatch = slint::Color::from_rgb_uint8(swatch.r, swatch.g, swatch.b);
            // The sliders as the operator last left them, or read off the color the first
            // time this row is drawn. See `pickedColors_` for why they are not re-derived
            // every time.
            const auto held = pickedColors_.find(static_cast<int>(rows.size()));
            if (held != pickedColors_.end()) {
                row.hue = held->second.hue;
                row.sat = held->second.saturation;
                row.val = held->second.brightness;
            } else {
                double hue = 0.0;
                double saturation = 1.0;
                double value = 1.0;
                dmx::toHsv(swatch, hue, saturation, value);
                row.hue = static_cast<float>(hue);
                row.sat = static_cast<float>(saturation * 100.0);
                row.val = static_cast<float>(value * 100.0);
            }
        }
        rows.push_back(std::move(row));
    };

    // Named by the placeholder they fill, so a chip and the address read together — §5.9's
    // "template segments render as editable chips".
    const std::vector<std::string> names = [&rule] {
        std::vector<std::string> found;
        std::size_t at = 0;
        while ((at = rule->address.find('{', at)) != std::string::npos) {
            const std::size_t close = rule->address.find('}', at);
            if (close == std::string::npos) {
                break;
            }
            found.push_back(rule->address.substr(at, close - at + 1));
            at = close + 1;
        }
        return found;
    }();
    // One row per generator the rule draws, in `trigger::slotLayout`'s order — the order a fire
    // records in `Rule::lastSlots`, and the order `slotConfig` finds a row's generator by. It
    // used to be walked here and in `slotConfig` beside the core's own, with comments saying the
    // three must not drift apart (the audit's Low items); there is one walk now, and a test
    // holds it to what a fire records. Program change has nowhere to put a value and pitch
    // bend is nothing but one; a lighting effect has only the generators it uses.
    for (const trigger::Slot& slot : trigger::slotLayout(*rule)) {
        const Generator::Config& generator = trigger::slotGenerator(*rule, slot);
        switch (slot.role) {
        case trigger::SlotRole::Segment:
            push(slot.segment < names.size() ? names[slot.segment] : "{?}", generator);
            break;
        case trigger::SlotRole::Value:
            push(valueLabelOf(rule->sendKind), generator);
            break;
        case trigger::SlotRole::Number:
            push(numberLabelOf(rule->sendKind), generator);
            if (!rule->numberChosen) {
                // An empty box asking for one, not the zero a fixed generator holds — a zero
                // would read as a number already chosen, and the rule is not firing because it
                // is not.
                rows.back().fixed = slint::SharedString("");
                rows.back().wanting = true;
            }
            break;
        case trigger::SlotRole::Level:
            push(std::string(dmx::labelOf(rule->dmx.role)), generator);
            break;
        case trigger::SlotRole::Color:
            push("color", generator, /*color=*/true);
            break;
        // A mix is three chips holding numbers, and the numbers are DMX bytes, so they are
        // labelled and ranged like every other byte in this window rather than pretending to be
        // colors.
        case trigger::SlotRole::Red:
            push("red", generator);
            break;
        case trigger::SlotRole::Green:
            push("green", generator);
            break;
        case trigger::SlotRole::Blue:
            push("blue", generator);
            break;
        // Percentages of each fixture's own movement window — see `DmxSend::pan`. Labelled with
        // the unit, because 50 meaning "half way across what I allowed" rather than "DMX 50" is
        // the one thing about this pair that is not obvious.
        case trigger::SlotRole::Pan:
            push("pan %", generator);
            break;
        case trigger::SlotRole::Tilt:
            push("tilt %", generator);
            break;
        }
    }
    // In place, and this is the one that mattered: these rows are the generator chips, they
    // are full of text boxes, and `tick` republishes them every time a rule fires. See
    // `writeRows`. Nothing here moves between fires *except* the last-produced readout, so
    // the common case writes no rows at all and the boxes are left entirely alone.
    //
    // And when something else moves — a range this clamped back the right way round, a list
    // seeded from a range, another rule's values — the row has to come back as a new element
    // or a box that has been typed into will go on showing what was typed. That readout is
    // the field to exclude: it ticks over on every beat, and rebuilding for it would tear the
    // boxes down under the operator's hands.
    //
    // The other exclusion is not a field but a cause: a color slider being dragged changes
    // this row's swatch and its hex on every pixel, and the picker holding that slider is a
    // popup inside this very repeater. See `pickingColor_`.
    if (!pickingColor_ &&
        markStale(*slotModel_, rows, staleSlots_, [](const SlotRow& was, const SlotRow& now) {
            SlotRow ignoring = was;
            ignoring.last = now.last;
            // And whatever a `LiveField` or a `NumberBox` shows, since neither goes deaf: a row
            // rebuilt for one of them took the box beside it — the one the operator had just
            // tabbed into — down with it (the audit of 2026-09-25, M17). What is left is what the
            // std widgets hold, which do assign their own values: the dropdowns, the tick boxes
            // and the picker's sliders.
            ignoring.low = now.low;
            ignoring.high = now.high;
            ignoring.values = now.values;
            ignoring.fixed = now.fixed;
            ignoring.normalise = now.normalise;
            ignoring.weights = now.weights;
            ignoring.ramp_bars = now.ramp_bars;
            ignoring.no_repeat = now.no_repeat;
            return ignoring != now;
        })) {
        rowsDirty_ = true;
    }
    writeRows(*slotModel_, rows);
    publishPalette();
}

void RulesController::rebuildRows() {
    // **Never while a picker is open.** A color picker is a `PopupWindow` belonging to one of
    // the items about to be thrown away, so a rebuild here destroys the popup — and with it
    // the slider the operator has hold of. The rebuild is not cancelled, only held: the flag
    // stays set and `tick` takes it on the first redraw after the picker closes, which is
    // before the operator can have typed into anything. See `setPickerOpen`.
    if (pickerOpen_) {
        return;
    }
    rowsDirty_ = false;
    if (!rebuildAll_) {
        // **Only the rows that moved** — see `renewRows`. A new element is the only way a
        // `ComboBox` that has been picked from, or a `CheckBox` that has been clicked, starts
        // following its model again, and the rows around it — the box just clicked into among
        // them — have no reason to be thrown away with it.
        renewRows(*slotModel_, staleSlots_);
        renewRows(*paletteModel_, stalePalette_);
        renewRows(*followModel_, staleFollows_);
        renewRows(*choiceModel_, staleChoices_);
        renewRows(*fixtureModel_, staleFixtures_);
        return;
    }
    rebuildAll_ = false;
    staleSlots_.clear();
    stalePalette_.clear();
    staleFollows_.clear();
    staleChoices_.clear();
    staleFixtures_.clear();
    // Emptied, so the repeaters throw their items away and build new ones — a palette swatch
    // added or taken away moves every one after it. Publishing straight afterwards leaves
    // nothing on screen for a frame.
    slotModel_->clear();
    paletteModel_->clear();
    followModel_->clear();
    choiceModel_->clear();
    fixtureModel_->clear();
    publishSlots();
    publishFollowUps();
    publishOutputChoices();
    publishFixtureChoices();
    // Every publisher compares against an empty model, so none can find a surviving row that
    // moved. Asserting that here rather than trusting it: a rebuild that set the flag again
    // would spin at thirty frames a second, tearing every box down as fast as it drew.
    rowsDirty_ = false;
}

void RulesController::publishFiring() {
    window_->set_last_fired(shared(lastFired_));
    if (lastFiredAt_ < 0.0) {
        window_->set_last_fired_ago(slint::SharedString(""));
        return;
    }
    // **The runner's clock, because that is the clock the timestamp was taken on.** This used
    // to subtract `Fired::when` from seconds since *this controller* was built, which are two
    // clocks with no shared origin: the runner's began at Start and this one at launch, so
    // "just now" read as however long the operator had spent setting up. Both halves of a
    // subtraction have to come off one clock, and `OutputRunner::elapsed` is it.
    const double ago = runner_.elapsed() - lastFiredAt_;
    // Seconds, coarsely. A message that landed a moment ago and one that landed a minute ago
    // are different facts; a tenth of a second between them is not.
    window_->set_last_fired_ago(
        shared(ago < 1.0 ? "just now" : spellNumber(std::floor(ago)) + "s ago"));
}

void RulesController::setStatus(const std::string& text, bool error) {
    window_->set_status(shared(text));
    window_->set_status_is_error(error);
}

} // namespace takt4::ui
