#pragma once

// The rule editor's words: parsing what an operator types into its boxes, and spelling
// rules, values, routing and follow-ups back in the words the editor shows. Pure functions
// over `trigger` and `output` types, with no window in them, so they can be read and tested
// on their own. Out of `rules_controller.cpp` since 2026-09-28, as the 2026-09-22 audit
// suggested.

#include "core/output/output_target.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/rule.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::ui::rule_text {

using trigger::Rule;

std::string_view trim(std::string_view text) noexcept;

/// Everything between separators, trimmed, empties dropped. What turns "3, 7, 1, 12" into a
/// sequence and " 70 - 140 " into a range, which is the same job twice.
std::vector<std::string_view> split(std::string_view text, std::string_view separators);

/// A number typed into a box, or nothing. **Only a finite one**: `from_chars` reads "nan" and
/// "inf" as numbers, and a duration of inf made an effect that never ended, a hue of nan was
/// undefined behaviour in `fromHsv`, and a BPM range of nan never fired (the 2026-09-25
/// audit's L5).
std::optional<double> readNumber(std::string_view text) noexcept;

/// A finite number as an `int32_t`, clamped to its range: converting one past it is
/// undefined behaviour, and "1e12" is a finite number (L5).
std::int32_t clampedInt(double value) noexcept;

/// What a number box's keystrokes say it holds. The markup has already clamped it and spelled
/// it as a whole number (`NumberBox::typed`), so anything else is a box nobody meant.
std::optional<int> typedNumber(std::string_view text) noexcept;

/// One entry of a list an operator typed.
///
/// **Type is inferred from how it is written**, which is the same rule the preset file uses
/// and the reason a list can hold more than clip numbers: `7` is an int, `0.5` a float,
/// `intro` text. Nothing here refuses — an entry that is not a number is a name, and names
/// are exactly what an address segment often is.
trigger::Value parseValue(std::string_view text);

/// The inverse, as the field shows it back: "3, 7, 1, 12".
std::string spellValues(const std::vector<trigger::Value>& values);

std::string spellValue(const trigger::Value& value);

/// A number with no trailing zeroes, for a field an operator will edit rather than read.
std::string spellNumber(double value);

/// A weighted generator's list as its box shows it: "7:3, 12:1".
std::string spellWeights(const std::vector<trigger::WeightedChoice>& choices);

/// The live interval multiplier in the words an operator thinks in, or **empty** at 1.
///
/// The multiplier is on the *interval*, so 2 is half as often — which is exactly backwards
/// from how it reads as a number, and is why this says "half as often" rather than "×2". A
/// rule nobody has touched says nothing at all, because a permanent "×1" beside every rule in
/// the list would be noise.
std::string describeRate(double factor);

/// A comma-separated list, as the routing field shows it back.
std::string join(const std::vector<std::string>& names);

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
bool targetTakes(trigger::Message::Kind kind, const output::OutputTarget& target) noexcept;

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
                            const std::vector<output::OutputTarget>& targets);

/// What a MIDI rule's value starts at: a velocity that is plainly heard without being the
/// loudest, and the centre of a pitch bend, which is no bend at all.
inline constexpr std::int32_t kVelocity = 100;

inline constexpr std::int32_t kBendRest = 8192;

/// What a kind calls the number it carries — "note 96" and "program 96" are different
/// instructions, and the label beside the box is the only thing that says which is about to
/// go out. Used by the generator chips and by the THEN SEND rows, so the two cannot disagree.
inline const char* numberLabelOf(trigger::Message::Kind kind) {
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
inline const char* valueLabelOf(trigger::Message::Kind kind) {
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
std::string releaseLabelOf(trigger::Message::Kind sent);

/// What a lighting rule's release does to the light, which depends on the effect it lets go of.
/// `Rule::followUpsFor` is the engine's half of this and the two must say the same thing: a
/// color comes back at the row's level, a flash, pulse, strobe or plain level becomes a plain
/// level on the rule's channel, and a move has no release at all and is skipped. That last one
/// is a row that sends nothing, so it has to say so rather than look like it works.
std::string describeDmxRelease(const trigger::DmxSend& send);

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
std::string describeFollowUp(const trigger::FollowUp& entry, const Rule::Config& rule);

/// A Euclidean pattern as something a person can hear before it plays: "x..x..x.".
///
/// Two numbers are not a rhythm anybody can read, and this is the cheapest way to make them
/// one — it is the same `euclidHit` the engine fires on, so the picture cannot disagree with
/// the playing.
std::string spellEuclid(const Rule::Config& rule);

} // namespace takt4::ui::rule_text
