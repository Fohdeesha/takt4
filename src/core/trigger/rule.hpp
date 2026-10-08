#pragma once

#include "core/dmx/color.hpp"
#include "core/dmx/effect.hpp"
#include "core/dmx/fixture.hpp"
#include "core/dmx/liberation.hpp"
#include "core/trigger/context.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/value.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takt4::trigger {

/// HANDOFF §5.8's WHEN column: *"every beat · every N beats · every bar · every N bars · on
/// downbeat · on tempo change · on lock or unlock · on intensity change · on onset · on
/// manual hotkey"*.
///
/// Ten entries, eight kinds and a count, because "every beat" is "every N beats" with N of
/// one and a UI can offer them as two lines of the same thing. Nothing else collapses:
/// `Bar` and `Downbeat` look alike and are not, since a rule on every fourth bar and a rule
/// on every downbeat want different arithmetic.
enum class Trigger : std::uint8_t {
    Beat,
    Bar,
    Downbeat,
    TempoChange,
    LockChange,
    IntensityChange,
    /// A hit the intensity classifier heard (`features::IntensityClassifier`, counted in
    /// `engine::EngineIntensity::onsets`); the output thread calls `TriggerEngine::onOnset`
    /// when that count moves.
    Onset,
    /// §5.8's *"manual hotkey"* — `TriggerEngine::manual`, which §5.7 will reach from OSC
    /// and MIDI alongside the rest.
    Manual,
    /// A Euclidean rhythm: `pulses` hits spread as evenly as they go over `every` beats,
    /// counted from the first beat of the pattern.
    ///
    /// **Not in §5.8's list**, and added on the user's ask for *"cool randomization or
    /// algorithmic settings ... based on the bpm of the song and downbeat"*. It earns its
    /// place because it is the one pattern that is neither "every N" nor random: 3-in-8 is
    /// the tresillo, 5-in-8 the cinquillo, 5-in-16 the bossa. An operator gets those by
    /// typing two numbers, and they are locked to the tracker's own beat rather than to a
    /// clock of their own.
    Euclid,
};

inline constexpr std::array<Trigger, 9> kTriggers{
    Trigger::Beat,        Trigger::Bar,        Trigger::Downbeat,
    Trigger::TempoChange, Trigger::LockChange, Trigger::IntensityChange,
    Trigger::Onset,       Trigger::Manual,     Trigger::Euclid};

std::string_view labelOf(Trigger trigger) noexcept;
std::string_view nameOf(Trigger trigger) noexcept;
std::optional<Trigger> triggerOf(std::string_view name) noexcept;

/// True where `Trigger::every` means anything — the two §5.8 spells with an N, and
/// `Euclid`, where it is the number of steps the pattern spans.
bool takesEvery(Trigger trigger) noexcept;

/// True where `Trigger::pulses` means anything — `Euclid` alone.
bool takesPulses(Trigger trigger) noexcept;

/// True where `Rule::Config::onBeat` means anything: `Bar`, which fires on that beat of its bars,
/// and `Beat`, whose every-N count is laid from that beat of the bar. See `TriggerEngine::
/// beatSatisfies`.
bool takesBeatOfBar(Trigger trigger) noexcept;

/// The most beats of the bar `Rule::Config::onBeat` can name — as many as a bar the tracker can
/// report (`--meters` takes up to 16).
inline constexpr std::uint32_t kMaxBeatOfBar = 16;

/// The longest a rule may wait between its trigger and its send (`Rule::Config::delayOn`): five
/// minutes, past any musical delay anybody types. A settings file edited by hand can hold
/// anything, and a fire held for a day is a rule that has quietly stopped.
inline constexpr double kMaxFireDelaySeconds = 300.0;

/// True where `Rule::Config::cooldownSeconds` means anything: the triggers that can come in
/// bursts — an onset, a tempo, lock or intensity change, the M key. **Not** beats, bars, the
/// downbeat or a Euclidean pattern, which come round at the music's own pace: a cooldown there
/// only ever thinned a steady pattern into an irregular one, and on a bar it never did anything
/// at all. The editor switches the control off for them and the engine ignores it (HANDOFF §0.5,
/// the rule editor locked 2026-09-30) — a control shown switched off must do nothing.
bool takesCooldown(Trigger trigger) noexcept;

/// How §5.8's *"optional follow-up value after a delay"* counts that delay.
///
/// Milliseconds was the only answer, and it is the wrong one for what the follow-up is
/// usually doing. A device's own lag is a fixed number of milliseconds — a media server takes
/// as long to key a clip at 92 BPM as at 140 — but a *release* is a length of music: "hold it
/// for two beats" is one gesture at any tempo, and in milliseconds it has to be retyped
/// whenever the record changes. Both are wanted, so both are offered.
///
/// `Bars` is `Beats` times the meter the tracker is reporting, taken at the moment the rule
/// fires. Nothing here assumes four (§5.5).
enum class DelayUnit : std::uint8_t { Milliseconds, Beats, Bars };

inline constexpr std::array<DelayUnit, 3> kDelayUnits{DelayUnit::Milliseconds, DelayUnit::Beats,
                                                      DelayUnit::Bars};

std::string_view labelOf(DelayUnit unit) noexcept;
std::string_view nameOf(DelayUnit unit) noexcept;
std::optional<DelayUnit> delayUnitOf(std::string_view name) noexcept;

/// Whether step `step` of a `pulses`-in-`steps` Euclidean pattern is a hit.
///
/// Bjorklund's construction, computed directly rather than by the recursive bit-string
/// algorithm: hit *k* of the pattern falls on step `floor(k * steps / pulses)`, which is the
/// same distribution and needs no allocation, no table and no state. `step` counts from zero.
///
/// Degenerate cases are answered rather than refused, as §5.8's clamping policy asks: no
/// pulses is silence, and pulses at or above steps is every step.
bool euclidHit(std::uint32_t step, std::uint32_t pulses, std::uint32_t steps) noexcept;

/// §5.8's ONLY IF column: *"confidence above threshold · intensity in set · BPM in range ·
/// probability percentage"*. Its fifth, *"minimum cooldown in ms"*, is `Rule::Config::
/// cooldownSeconds` since 2026-09-30: it says how often the trigger may come round, which is
/// the WHEN's business, and the editor shows it there.
///
/// Every field defaults to "does not exclude anything", so a rule with untouched conditions
/// fires whenever its trigger says to. That is the only sane default for a stage whose
/// entire job is to *stop* a rule firing: an operator adding a rule and seeing nothing
/// happen has no way to tell a broken rule from a condition they did not know was set.
///
/// **And none of it applies unless `Rule::Config::conditionsOn` says so** — the tick on the
/// editor's B heading. Off, the rule fires every time its trigger comes round, whatever is
/// set here; the values are kept, so switching B back on brings them back.
struct Conditions {
    /// Fires only at or above this. `TempoState::confidence`, 0 to 1.
    double minConfidence = 0.0;
    /// Whether each intensity is in the set, indexed by `features::Intensity`'s own value —
    /// which is §5.6's wire number, so the array and the OSC address cannot drift apart.
    std::array<bool, 3> intensities{true, true, true};
    /// Inclusive, on the *published* tempo — the number on screen, after the fold and any
    /// ÷2, because that is the one an operator reads before typing a range.
    double minBpm = 0.0;
    double maxBpm = 1000.0;
    /// §5.8's *"probability percentage"*, as a fraction. Rolled after everything else, so a
    /// rule that was going to be excluded anyway does not consume a draw — otherwise two
    /// rules sharing a stream would fire in a pattern neither of them describes.
    double probability = 1.0;

    bool allows(features::Intensity intensity) const noexcept {
        return intensities[static_cast<std::size_t>(intensity)];
    }

    /// Whether any of the four can exclude a fire — what a file written before B had a switch
    /// is read by: a rule that used one loads with B on, so nothing fires differently, and one
    /// that used none loads with it off.
    bool excludesAnything() const noexcept {
        return minConfidence > 0.0 || !intensities[0] || !intensities[1] || !intensities[2] ||
               minBpm > 0.0 || maxBpm < 1000.0 || probability < 1.0;
    }
};

/// What a fired rule hands the transports.
///
/// One struct for all three kinds rather than three, because the follow-up scheduler holds
/// them in a queue and a queue of one type is the difference between a `std::vector` and a
/// hierarchy. `kind` says which fields mean anything.
struct Message {
    /// **Note On and Note Off are separate kinds, and that is not tidiness.** Until
    /// 2026-09-12 the only note status on the wire was `0x90`, and a release was that with
    /// velocity zero — which `rule_sink.cpp` justified as "a note-off on every device made
    /// since 1983". It is not: the operator's laser controller (Liberation) holds
    /// its clip until a real `0x80` arrives, so a rig built on the velocity-zero convention
    /// never released. The convention is common, not universal, and a sender cannot tell
    /// which end it has. So takt4 sends what the standard says, and a release of a `MidiNote`
    /// is a `MidiNoteOff` (`Rule::followUpsFor`).
    ///
    /// `number` and `value` mean different things per kind, and `sendsNumber` /
    /// `sendsValue` say which are live so the editor can label and hide accordingly:
    ///   - `MidiNote` / `MidiNoteOff`: note, velocity
    ///   - `MidiCc`: controller, value
    ///   - `MidiProgramChange`: program, and nothing else — it is a two-byte message
    ///   - `MidiPitchBend`: no number, and a **14-bit** value (0-16383, centre 8192)
    enum class Kind : std::uint8_t {
        Osc,
        MidiNote,
        MidiNoteOff,
        MidiCc,
        MidiProgramChange,
        MidiPitchBend,
        /// One lighting effect, aimed at fixtures rather than at an address or a channel —
        /// `fixtures` and `payload` below, and nothing else on this struct.
        ///
        /// **It carries a whole effect where the others carry a number, and that is what DMX
        /// is.** An OSC message is an event a receiver acts on; a DMX level is a *state* the
        /// controller has to keep sending. So the thing a rule hands over is not "channel 5 =
        /// 200" but "fade these fixtures to 200 over two bars", which `dmx::DmxEngine` then
        /// spends the next two bars executing. See `dmx::Payload`, which is deliberately
        /// allocation-free so that this struct still is.
        Dmx,
    };

    Kind kind = Kind::Osc;
    /// Osc: the address, with every `{...}` segment already filled in. Owned, because the
    /// follow-up queue outlives the rule's own buffer. One allocation per fire past the
    /// short-string optimisation, which on the output thread is nothing (§4.2 forbids
    /// allocation on the audio thread and nowhere else).
    std::string address;
    /// Osc: the argument, when there is one. §5.6's Resolume resync takes `int 1`; some
    /// addresses take none at all.
    Value argument;
    bool hasArgument = true;
    /// MIDI: the channel, 1 to 16 as a person counts them.
    int channel = 1;
    /// MIDI: the note number or the controller number.
    int number = 0;
    /// MIDI: the velocity or the controller value, 0 to 127.
    int value = 127;

    /// Dmx: which fixtures of the patch this reaches, one bit each, in the order
    /// `dmx::DmxEngine::patch()` holds them. **Zero is no fixtures** — a DMX rule that has not
    /// been routed sends nothing, which is the opposite of what an empty `outputs` means and
    /// is deliberate; `dmx::resolveFixtures` says why.
    ///
    /// A mask rather than the names, for the reasons `outputs` gives: a fire allocates
    /// nothing, and a follow-up outlives the rule that owed it.
    dmx::FixtureSet fixtures{};
    /// Dmx: the effect, already resolved — every generator run, every duration converted to
    /// seconds against the tempo that was playing.
    dmx::Payload payload;

    /// §5.6's *"rule subset"*: which of the app's outputs this goes to, one bit each, in
    /// the order `output::Transports::outputs()` holds them. All bits — `output::
    /// kAllOutputs` — means everywhere, which is what an unrouted rule means and what
    /// every rule meant before routing existed.
    ///
    /// **A mask rather than the names themselves**, for two reasons that both matter. A
    /// fire allocates nothing extra, where a `vector<string>` per message would be three
    /// allocations at 3.6 messages a second per rule. And a follow-up outlives its rule:
    /// §5.8 says a pending release is still sent after the rule set has been replaced, so
    /// anything the message pointed *at* would dangle exactly when it was needed.
    std::uint64_t outputs = ~std::uint64_t{0};

    /// When the thing this message is about **happens**, on `Context::now`'s clock — which is
    /// not always when it is sent. The output thread fires a beat ahead of time on a
    /// prediction, so a message about it carries the beat's own time here, and the release that
    /// follows it carries that plus its delay. `output::RuleSink` offsets every target's latency
    /// from this, never from the round that happens to send it: that is what lets a negative
    /// offset put a message *before* the beat it belongs to (the audit's H4). Stamped by
    /// `TriggerEngine` as it sends; zero before that.
    double moment = 0.0;

    /// **A follow-up that is a gesture of its own** — one with a kind of its own (`FollowUp::kind`:
    /// a CC a bar after a note, a second note two beats on) rather than the release of what
    /// fired. PANIC, STOP, a mute or a delete send every release still owed at once, so nothing
    /// is left latched; a gesture is not a release, and is dropped rather than sent early — a CC
    /// owed a bar on went out the instant STOP was pressed.
    bool gesture = false;
};

/// A routing mask — `Message::outputs` or `Message::fixtures` — after the list it indexes has
/// changed: each bit moved to where its entry went, `moved[old]` being the new index or
/// negative for an entry that is gone. Every bit set stays every bit set, which is what an
/// unrouted rule's "everywhere" is.
///
/// What lets a message already on its way survive an edit to the outputs or the patch: it
/// carries bits rather than names, which is what keeps a fire allocation-free, and the bits
/// have to follow their outputs rather than land on whatever now sits where they were.
std::uint64_t remapBits(std::uint64_t mask, const std::vector<int>& moved) noexcept;
/// The same for a set of fixtures. No "every bit" case: a set of fixtures that names none names
/// none, and one that names them all names them all.
dmx::FixtureSet remapBits(const dmx::FixtureSet& mask, const std::vector<int>& moved) noexcept;

inline constexpr std::array<Message::Kind, 7> kMessageKinds{
    Message::Kind::Osc,    Message::Kind::MidiNote,          Message::Kind::MidiNoteOff,
    Message::Kind::MidiCc, Message::Kind::MidiProgramChange, Message::Kind::MidiPitchBend,
    Message::Kind::Dmx};

/// True for the five MIDI kinds — the test every MIDI-shaped branch wants, written once so
/// that adding a kind does not mean finding every `!= Osc` in the tree.
///
/// **It used to be `kind != Osc`, and that was right until there were three families.** The
/// day `Dmx` arrived, every `!= Osc` in the tree started claiming a lighting effect was a MIDI
/// message — which would have sent one down a MIDI cable as a status byte built from an
/// effect kind. Spelled out rather than negated for that reason.
constexpr bool isMidi(Message::Kind kind) noexcept {
    return kind == Message::Kind::MidiNote || kind == Message::Kind::MidiNoteOff ||
           kind == Message::Kind::MidiCc || kind == Message::Kind::MidiProgramChange ||
           kind == Message::Kind::MidiPitchBend;
}

/// True for the one lighting kind.
constexpr bool isDmx(Message::Kind kind) noexcept {
    return kind == Message::Kind::Dmx;
}

/// Whether the kind puts `Message::number` on the wire. False for pitch bend, which is all
/// value, and for OSC, which has neither.
constexpr bool sendsNumber(Message::Kind kind) noexcept {
    return kind == Message::Kind::MidiNote || kind == Message::Kind::MidiNoteOff ||
           kind == Message::Kind::MidiCc || kind == Message::Kind::MidiProgramChange;
}

/// Whether a number chosen for one kind means the same thing sent as another. A note is a note
/// whether it goes on or off; a controller and a program are each only themselves, and a note
/// 60 carried over to a CC would be a fader nobody asked to move.
constexpr bool sameNumber(Message::Kind a, Message::Kind b) noexcept {
    const auto family = [](Message::Kind kind) {
        return kind == Message::Kind::MidiNoteOff ? Message::Kind::MidiNote : kind;
    };
    return sendsNumber(a) && sendsNumber(b) && family(a) == family(b);
}

/// Whether the kind puts `Message::value` on the wire. False for program change, which is a
/// two-byte message and has nowhere to put one.
constexpr bool sendsValue(Message::Kind kind) noexcept {
    return isMidi(kind) && kind != Message::Kind::MidiProgramChange;
}

/// The inclusive ceiling on `Message::value` for a kind: pitch bend is 14-bit, everything
/// else is a data byte. `Rule` clamps to this and §5.9's editor reads it for its own range.
constexpr int valueCeiling(Message::Kind kind) noexcept {
    return kind == Message::Kind::MidiPitchBend ? 16383 : 127;
}

/// What §5.9's SEND dropdown offers, and what a preset spells the choice with.
std::string_view labelOf(Message::Kind kind) noexcept;
std::string_view nameOf(Message::Kind kind) noexcept;
std::optional<Message::Kind> messageKindOf(std::string_view name) noexcept;

/// `clockSeconds`, or `beats` worth of the tempo in `context` — whichever `unit` names. The first
/// is the wall-clock figure `DelayUnit::Milliseconds` asks for, held in seconds.
///
/// The one place a musical length becomes a wall-clock one, shared by a follow-up's *delay*
/// and a DMX effect's *duration* so the two cannot disagree about what a bar is. Falls back to
/// the millisecond figure when the unit is musical and there is no tempo to measure against,
/// and uses the meter the tracker reports rather than four (§5.5).
double musicalSeconds(const Context& context, DelayUnit unit, double clockSeconds,
                      double beats) noexcept;

/// A rule's lighting instruction, before its generators have been run — the configuration
/// half of `dmx::Payload`.
///
/// **The two are separate types on purpose, and the split is the same one `Rule::Config` and
/// `Message` already make.** This holds generators, names and units: what the operator typed.
/// `dmx::Payload` holds numbers, a mask and seconds: what a fire decided. Keeping the units
/// here is also what keeps `core/dmx` free of any dependency on `core/trigger`, so the engine
/// that runs the lights knows nothing about rules.
///
/// **The color and the level are generators like everything else**, which is not a
/// flourish: it is what makes *"a random color from my palette, never the same one twice"*
/// fall out of the machinery the clip triggers already use, rather than being a second
/// random-color feature with its own bag and its own bugs. A palette is a `Shuffle` over a
/// `List` of text values — `#ff2040`, `#20ff80` — and no code here knows that is what it is
/// looking at.
/// How a lighting rule decides what color to send.
///
/// **Two modes, because an operator asks for the color two different ways and neither can
/// say what the other says.** "These six colors, never the same one twice" is a palette; "a
/// random color, but keep the blue high and the green out of it" is three ranges. A palette
/// cannot express the second without listing every color in it, and three ranges cannot
/// express the first at all.
enum class ColorMode : std::uint8_t {
    /// `DmxSend::color` decides it: a `Fixed` text value is one color from the picker, and
    /// a `Shuffle`/`Random`/`Cycle`/`Weighted` over a `List` of them is a palette — a `Cycle`
    /// walks it in order, a `Shuffle` never repeats within a bag. **Not `Live` or `Ramp`**,
    /// which compute numbers, and a number is not a color (`trigger::handsBackValues`; the
    /// editor does not offer them here). A ramped or live color is a `Mix`.
    Palette,
    /// `DmxSend::red`, `green` and `blue` decide it, one generator each, 0 to 255.
    ///
    /// Three generators rather than a "random color" tick box, because that is what makes
    /// *"within limits I set for r g and b specifically"* fall out of machinery that already
    /// exists — a limit is a `Random` over a range — and because it costs nothing to let the
    /// other kinds in: a `Ramp` on red over four bars, or a `Live` green that follows the
    /// intensity, are gestures nobody had to write code for.
    Mix,
};

inline constexpr std::array<ColorMode, 2> kColorModes{ColorMode::Palette, ColorMode::Mix};

std::string_view labelOf(ColorMode mode) noexcept;
std::string_view nameOf(ColorMode mode) noexcept;
std::optional<ColorMode> colorModeOf(std::string_view name) noexcept;

/// The color generator a rule starts with: **one fixed color**, not a shuffle.
///
/// `Generator::Config`'s own default is §5.8's — shuffle over the integers 1 to 8 — which is
/// right for a clip index and nonsense for a color: every draw is a number `dmx::parseColor`
/// cannot read, so every fire fell back to white and the editor showed a range box asking
/// which eight colors 1 through 8 were. Reported from a rig on 2026-09-16: *"I define a range
/// 1-8 which maps to what!? it just stays the same #ffff color code"*.
Generator::Config paletteOf(dmx::Color color) noexcept;

/// And the palette a color generator seeds when it is switched to a kind that draws from a
/// pool. Six colors that look like six colors on a wall — an operator who picked "shuffle"
/// wants to see a shuffle, and can then edit the swatches.
std::vector<Value> defaultPalette();

/// One component's generator: `Random` over the whole byte, which is what "a random color"
/// means before anybody has narrowed it.
Generator::Config componentMix() noexcept;

/// A generator that always produces `value` — what every *number* a lighting rule carries
/// starts as.
///
/// **The same mistake the color had, in the field next to it.** `Generator::Config` defaults
/// to shuffling 1 to 8, and a DMX level is 0 to 255: an untouched rule faded its fixtures to
/// somewhere between 0.4 % and 3 % of full, which on a lamp is indistinguishable from the rule
/// not having fired. A pan of 1 to 8 is likewise the extreme left-hand edge of the movement
/// window rather than "somewhere". The default has to be the number an operator means before
/// they have said anything — full, and centre.
Generator::Config fixedNumber(int value) noexcept;

/// The clip generator a lighting rule starts with: **a shuffle over Liberation clips 1-1 to 21-1**
/// in deck order (`dmx::liberation::indexOf`), the operator's own example of 2026-10-05. A clip is
/// held as its place in deck order, so a range is two of them and a shuffle over it is every clip
/// between, as Liberation counts them.
Generator::Config clipShuffle(dmx::liberation::Clip from, dmx::liberation::Clip to) noexcept;

struct DmxSend {
    dmx::EffectKind effect = dmx::EffectKind::Level;
    /// Which channel, for the kinds `dmx::takesRole` names.
    dmx::Role role = dmx::Role::Dimmer;
    dmx::Curve curve = dmx::Curve::EaseOut;
    dmx::PathShape shape = dmx::PathShape::Circle;

    /// The fixtures this rule aims at, by `dmx::Fixture::id`, and the groups, by their label.
    /// **Empty reaches nothing**, and the rule reports it as a problem rather than firing into
    /// the dark — see `dmx::resolveFixtures` for why this is the opposite of what an empty output
    /// list means.
    ///
    /// Ids rather than indices, which move the moment a fixture above is deleted, and rather
    /// than names, which are the operator's to change; a group is its label, which is what the
    /// gesture of relabelling a fixture means (`dmx::Fixture::group`).
    std::vector<std::string> fixtures;

    /// The target level, peak or high end, 0 to 255. Clamped, not wrapped. Full by default —
    /// see `fixedNumber`, and what a shuffle over 1 to 8 does to a lamp. A laser clip's intensity.
    Generator::Config level = fixedNumber(255);
    /// Which Liberation clip, for `dmx::EffectKind::Clip`: a place in deck order, which the editor
    /// shows and reads as Liberation's own "x-y". See `clipShuffle`.
    Generator::Config clip = clipShuffle({1, 1}, {21, 1});
    /// The low end, for the kinds `dmx::takesBase` names.
    int base = 0;
    /// Which of the two ways below decides the color.
    ColorMode colorMode = ColorMode::Palette;
    /// The color, as text a `dmx::parseColor` understands. A `Fixed` text value is the
    /// color picker; a `Shuffle` over a list is a palette. Used when `colorMode` is
    /// `Palette`.
    Generator::Config color = paletteOf(dmx::kWhite);
    /// The three components, 0 to 255, used when `colorMode` is `Mix`. Clamped, not wrapped.
    Generator::Config red = componentMix();
    Generator::Config green = componentMix();
    Generator::Config blue = componentMix();

    /// How long the effect runs — **not** how long until it starts, which is a follow-up's
    /// delay. Spelled in beats by default, because a fade that follows the tempo is the thing
    /// this app exists to make possible.
    DelayUnit unit = DelayUnit::Beats;
    /// The duration in seconds, used when `unit` is `Milliseconds`. Two numbers rather than
    /// one converted between them, for the reason `FollowUp::delayBeats` gives.
    double durationSeconds = 0.5;
    double durationBeats = 1.0;

    /// How many times a repeating effect repeats within the duration.
    double cycles = 1.0;
    /// A strobe's on-fraction.
    double duty = 0.5;
    /// A hue sweep's ends, in degrees. It may run past 360, and backwards.
    double hueFrom = 0.0;
    double hueTo = 360.0;

    /// Where a `Position` goes, as a **percentage of each fixture's own movement window**, so
    /// that one rule aimed at six differently-rigged heads means the same gesture on all six.
    /// Generators, so that "a random position every four bars" is `Random` over 0 to 100 and
    /// "sweep across the room over a phrase" is a `Ramp` — neither of which needed any code of
    /// its own.
    /// The middle of the window by default, for the reason `fixedNumber` gives: an untouched
    /// position rule pointed every head at the extreme corner of its own travel.
    Generator::Config pan = fixedNumber(50);
    Generator::Config tilt = fixedNumber(50);
    /// A `Path`'s radius as a fraction of the window's half-width, 0 to 1.
    double size = 0.5;
    /// Which heads a movement moves on a fixture with several (`dmx::headsOf`): bit n for head
    /// n + 1, none for every head — so one rule can move the left yoke and another the right. See
    /// `dmx::Payload::heads`. Read by the kinds `dmx::takesMovement` names.
    std::uint32_t heads = 0;
    /// How far apart those heads move, 0 to 1 — "stagger movement between selected heads". See
    /// `dmx::Payload::spread`. 0, together, unless the operator asks.
    double spread = 0.0;
};

/// What a follow-up replaces of a DMX rule's own effect.
///
/// Present for the case the operator asked for first: *"fade in and fade out"* is one rule
/// with a fade up and a follow-up two bars later that fades down. The follow-up needs a
/// duration of its own — a two-bar fade out is not the same number as the two-bar wait before
/// it — and a kind of its own, because the way out of a flash is not another flash.
struct DmxFollow {
    dmx::EffectKind effect = dmx::EffectKind::Level;
    /// Used by the color kinds. A release with no `DmxFollow` dims the fired color instead,
    /// which is what "let go" means for a light that is already lit.
    dmx::Color color = dmx::kBlack;
    dmx::Curve curve = dmx::Curve::EaseOut;

    DelayUnit unit = DelayUnit::Beats;
    double durationSeconds = 0.5;
    double durationBeats = 1.0;
};

/// One of the messages a rule sends *after* the one it fired — §5.8's *"optional follow-up
/// value after a delay"*, grown into a list.
///
/// **It was one value and a delay, and that was not enough.** §5.6's press-then-release is
/// the case it was written for, and it reads as a tick box with a number beside it: send a 1,
/// send a 0 fifty milliseconds later. A rig then asked for the thing a tick box cannot say —
/// *"I need a note on trigger to trigger Liberation laser clips. when the beat is done, I
/// need the option to send a note off. in the same trigger"* — and the honest answer is that
/// a rule sends a **sequence**, of which press-then-release is the shortest interesting one.
/// So each entry says what it sends rather than only what value it carries.
///
/// Every delay is measured **from the fire**, never from the entry above it. Two entries at
/// one beat and two beats are a note held for a beat and something else on the next, which is
/// how an operator counts them; chaining them would make each one's timing depend on edits
/// made to the one before.
struct FollowUp {
    /// What to send, or nothing for a **release** — the message that fired, with a new value,
    /// and `MidiNote` turned into `MidiNoteOff`.
    ///
    /// Release is the default and the only thing that can follow a *drawn* number: a rule
    /// that shuffles its note has to let go of the note it drew, which nobody can type in
    /// advance. An explicit kind is for a second gesture rather than the release of the first
    /// — a note on, then a CC a bar later.
    ///
    /// **An explicit kind must be on the same side of the OSC/MIDI divide as the rule.** One
    /// that is not is skipped rather than sent: a MIDI follow-up to an OSC rule has no channel
    /// or note to inherit, and an OSC follow-up to a MIDI rule has no address.
    std::optional<Message::Kind> kind;
    /// The release velocity, controller value, bend, or OSC argument.
    Value value = Value::ofInt(0);
    /// The note or controller number, when `kind` names one that takes one. A release inherits
    /// the fired message's instead, which is the whole point of a release.
    int number = 0;

    /// For a DMX rule: what this follow-up does to the lights instead of re-sending the
    /// rule's own effect.
    ///
    /// Null is the release, and a release of a light is a **dim**: the fired effect again with
    /// its level replaced by `value`, or — for a color — the fired color scaled by it. At
    /// zero that is a fade to black over the same time the rule faded up in, which is
    /// press-then-release spelled the way a lamp understands it.
    ///
    /// A release of a *movement* effect is skipped rather than invented. There is no sensible
    /// "let go" of a pan: sending the head home would be a gesture nobody asked for, and
    /// re-firing the move would make one rule fight itself.
    std::optional<DmxFollow> dmx;

    DelayUnit unit = DelayUnit::Milliseconds;
    /// The delay **in seconds**, used when `unit` is `Milliseconds` — the unit the editor shows
    /// it in, which is where the name of the unit comes from.
    double delaySeconds = 0.05;
    /// The same delay in beats — or in bars — used when `unit` says so. See `DelayUnit` for
    /// why both exist, and `Rule::followUpDelay` for the arithmetic.
    ///
    /// **Two numbers rather than one converted between units.** They are different magnitudes
    /// of the same idea — fifty milliseconds against two beats — and a single field would turn
    /// a 50 into a 0.077 the moment the unit changed, which is a box nobody can type in and a
    /// preset nobody can read.
    double delayBeats = 1.0;
};

/// How many follow-ups one rule may carry. Not a limit anybody should meet: it is here so a
/// settings file hand-edited into nonsense cannot make one fire queue thousands of messages.
inline constexpr std::size_t kMaxFollowUps = 8;

/// Whether `kind` can follow a rule that sends `sent` — the family divide `FollowUp::kind`
/// describes. A release (no kind) always can, since it *is* the fired message.
///
/// Three families now rather than two: an OSC follow-up to a DMX rule has no address to
/// inherit, and a DMX follow-up to a MIDI rule has no fixtures.
constexpr bool followUpFits(Message::Kind kind, Message::Kind sent) noexcept {
    return isMidi(kind) == isMidi(sent) && isDmx(kind) == isDmx(sent);
}

/// Where a fired rule's messages go.
///
/// An interface rather than a reference to `output::Transports`, for two reasons that both
/// matter. A rule sends to *any* OSC address, which is a different thing from §5.6's
/// generic namespace that `output::OscPublisher` owns — and §5.6 wants each target to have
/// "its own host, port, enabled state and rule subset", which is routing this layer should
/// not know about. And a test has to be able to say exactly what a rule sent, which is what
/// a recording implementation gives it.
///
/// Called on the output thread, between a beat and the next MIDI tick. Keep it short.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void send(const Message& message) = 0;
};

/// One rule: §5.8's *when → only if → send*.
///
/// Configuration is plain data so it can travel in a preset (Q7) and be edited by clicking
/// (§5.9's *"an internal representation the user never types"*). The state — the generators'
/// bags, the cooldown clock, what the last tempo was — lives in the `Rule` around it.
///
/// **An invalid rule never fires, and says why.** That is the opposite of `Generator`'s
/// clamp-everything policy, and deliberately: a generator with a silly range sends a silly
/// number, while a rule whose address has more `{}` than it has generators would send a
/// *different address* — and an OSC address arriving at a lighting desk with a segment
/// missing is worse than nothing arriving at all.
class Rule {
public:
    /// §5.6's own example, and the shape the address templating exists for:
    ///
    ///     /composition/layers/{L}/clips/{C}/connect   int 1 (press) then 0 (release)
    ///
    /// Each `{...}` is filled by the matching entry of `segments`, in order. What is inside
    /// the braces is ignored — §5.6 writes `{L}` and `{C}`, §5.9 says the address is
    /// assembled by clicking rather than typed, so the names are documentation and the
    /// position is what binds.
    struct Config {
        /// §5.7's `/<app>/ctl/rule/<id>/enable <0|1>` addresses a rule by this, so it has to
        /// be usable as an OSC address segment: printable ASCII, none of OSC 1.0's reserved
        /// characters, and not empty.
        std::string id = "rule";
        /// What §5.9's rule card puts in its title bar.
        std::string name;
        bool enabled = true;

        Trigger trigger = Trigger::Bar;
        /// Every Nth beat or bar, counted from the first — so "every 4 bars" fires on bars
        /// 1, 5 and 9 rather than 4, 8 and 12. An operator counting a phrase in starts at
        /// one. Beats are counted on the bar's grid from `onBeat` once the tracker has a bar,
        /// and from the first beat until then. Ignored where `takesEvery` is false; zero is
        /// read as one.
        std::uint32_t every = 1;
        /// How many of `every` steps a `Euclid` pattern hits. Ignored by every other
        /// trigger. Zero is silence and is left as the operator typed it — a pattern being
        /// built up from nothing passes through it.
        std::uint32_t pulses = 3;
        /// **Which beat of the bar**, 1 to `kMaxBeatOfBar` — the operator's ask of 2026-10-06:
        /// *"not just fire every two beats, but WHICH two beats ... one trigger firing on the
        /// first beat of a bar, another on the second, another on the third"*.
        ///
        /// For `Bar`, the beat of every Nth bar it fires on: every bar on beat 3. For `Beat`, the
        /// beat its every-N count is laid from, **on the bar's grid** rather than from whichever
        /// beat came first — every 2 beats from beat 2 is the backbeat, 2 and 4 of every bar;
        /// every 8 from beat 5 is the downbeat of every second bar. One is the downbeat, which is
        /// what every rule meant before this existed. Ignored where `takesBeatOfBar` is false.
        std::uint32_t onBeat = 1;
        /// **A wait between the trigger and the send** — the operator's ask of the same day, in
        /// the same breath: *"an optional delay setting default to unticked in the when section,
        /// where u can set it to static time in ms or beats / bars"*. Off by default, as nothing
        /// optional is on until asked for. On, every fire is held this long before it goes, and
        /// its follow-ups after it; with `onBeat` it reaches the places between the beats — every
        /// bar on beat 2, half a beat late, is the "and" of 2.
        ///
        /// The two numbers are kept apart for the reason `FollowUp::delayBeats` gives, and the
        /// wait is settled against the tempo at the fire (`Rule::fireDelay`). A held fire is
        /// **dropped**, never sent early, by everything that stops the rig — PANIC, Stop, quit,
        /// no signal — and by its rule being muted, switched off or deleted before it goes.
        bool delayOn = false;
        DelayUnit delayUnit = DelayUnit::Beats;
        double delaySeconds = 0.1;
        double delayBeats = 0.5;
        /// How far the published tempo has to move to count as a `TempoChange`, as a
        /// fraction. The published tempo is *refined* continuously once locked — the beat
        /// spacing resolves it to a fraction of a BPM and it moves most beats — so an exact
        /// comparison would make this trigger mean "every beat". Measured against the tempo
        /// that last fired rather than the last one seen, so a slow drift is one change and
        /// not a stream of them.
        double tempoChangeTolerance = 0.02;
        /// §5.8's *"minimum cooldown in ms"*, in seconds, against `Context::now`: after the rule
        /// fires it waits this long before it can fire again. Measured from the last time this
        /// rule *fired*, not from the last time it was evaluated. **Read only where
        /// `takesCooldown(trigger)`** — the triggers that come in bursts; on a beat, a bar, the
        /// downbeat or a Euclidean pattern it is kept and ignored, and the editor shows it
        /// switched off. It was one of the conditions until 2026-09-30, and a file from before
        /// then still has it there (`settings::rulesFromJson` reads both).
        double cooldownSeconds = 0.0;

        /// Whether the ONLY IF stage applies at all — the tick on the editor's B heading (HANDOFF
        /// §0.5, locked 2026-09-30). **Off by default**: nothing optional is on until somebody
        /// switches it on, and off is a rule that fires every time its trigger comes round. The
        /// conditions are kept while it is off, so it can be set for later and switched on.
        bool conditionsOn = false;
        Conditions conditions;

        Message::Kind sendKind = Message::Kind::Osc;
        /// The OSC address template. Ignored for the MIDI kinds.
        std::string address;
        /// One per `{...}` in `address`, in order.
        std::vector<Generator::Config> segments;
        /// The argument, or the MIDI velocity / controller value.
        Generator::Config value;
        /// False for an address that takes no argument at all.
        bool sendValue = true;
        /// MIDI: the channel as a person counts it, 1 to 16.
        int channel = 1;
        /// MIDI: which note or controller. A generator, so a rule can shuffle notes the way
        /// it shuffles clips.
        Generator::Config number;
        /// MIDI: whether `number` is one the operator chose. **False is a rule that does not
        /// fire**, and `problem` says why. A rule switched to a note or a controller has no
        /// number anybody meant, and the one it would otherwise draw from — a generator's own
        /// default, a shuffle over 1 to 8 — sent CC 7 at a value of one to every MIDI output on
        /// some bar, which silences a synth (the audit's C7). True by default, so every rule
        /// already in a file goes on working.
        bool numberChosen = true;

        /// The lighting instruction, for `sendKind == Dmx`. Ignored by every other kind, and
        /// kept rather than cleared when the kind changes — an operator switching a rule to
        /// MIDI to try something should get their fixtures and their fade back when they
        /// switch it back.
        DmxSend dmx;

        /// §5.8's *"optional follow-up value after a delay"*, as many as the rule needs.
        /// Empty is a rule that sends one message and is done. See `FollowUp`, and
        /// `Rule::followUpsFor` for what each entry turns into.
        std::vector<FollowUp> followUps;

        /// Seeds every generator this rule owns, each offset from it, so two rules in a
        /// preset do not fire the same clip as each other. See `Generator::Config::seed`.
        std::uint64_t seed = 1;

        /// §5.6's *"rule subset"*: the outputs this rule sends to, by `output::OutputTarget::id`.
        /// **Empty means every output**, which is what a rule an operator has not routed means
        /// and what every rule meant before there was more than one target.
        ///
        /// Ids rather than indices, which move the moment an output above is deleted, and
        /// rather than names, which are the operator's to change (routing written by name in
        /// an older file is re-pointed on load, `output::routeByIds`). An id that matches
        /// nothing on this rig contributes nothing and is *kept* rather than dropped: the
        /// editor says the routing reaches nothing, and nothing is lost if an output with that
        /// id comes back — an import carries its outputs' ids along with its rules.
        std::vector<std::string> outputs;
    };

    explicit Rule(Config config);

    const Config& config() const noexcept { return config_; }
    const std::string& id() const noexcept { return config_.id; }
    bool enabled() const noexcept { return enabled_; }
    void setEnabled(bool on) noexcept { enabled_ = on; }

    /// Whether this rule is **muted** — §5.7's `/ctl/rule/<id>/mute`, added on the operator's
    /// ask of 2026-09-16 for live per-rule control.
    ///
    /// **Muted is not disabled, and the difference is the whole reason both exist.** A
    /// disabled rule does not run: its shuffle bag stands still, its cooldown stops, and when
    /// it comes back it starts again from wherever it left off — which, for a rule that draws
    /// clips or colors, is audibly a restart. A muted rule runs exactly as it would: it
    /// triggers, its conditions are judged, its generators advance and its cooldown ticks.
    /// Only the sending is suppressed. So unmuting rejoins the music in phase rather than
    /// beginning again, which is what an operator dropping a layer out for eight bars means.
    bool muted() const noexcept { return muted_; }
    void setMuted(bool on) noexcept { muted_ = on; }

    /// A multiplier on `Config::every` — §5.7's `double`, `halve` and `rate`.
    ///
    /// Live, and *not* part of the configuration: it is a performance gesture, like the tempo
    /// ÷2 button, so it is not saved, and a set loaded rather than edited starts it at one
    /// (`TriggerEngine::setRules`'s `fresh`). A rule on every four
    /// bars at rate 2 fires every eight; at 0.5, every two. The result is rounded and floored
    /// at one, so halving a rule that is already on every beat leaves it on every beat rather
    /// than turning it off.
    double rate() const noexcept { return rate_; }
    void setRate(double rate) noexcept;
    /// `Config::every` with `rate()` applied — what the trigger actually counts against.
    std::uint32_t effectiveEvery() const noexcept;

    /// Empty when the rule is usable, and otherwise why it is not — for §5.9's rule card to
    /// show. An invalid rule is still a rule: it is held, edited and saved, and only
    /// refuses to fire.
    const std::string& problem() const noexcept { return problem_; }
    bool valid() const noexcept { return problem_.empty(); }

    /// Fresh generators, no cooldown owed, nothing remembered about the tempo or the lock.
    /// **Only the tests call it**: a rule the application edits is built again and takes over
    /// what it was doing (`carryFrom`), which is what an edit means.
    void reset() noexcept;

    /// Takes over what `previous` — the same rule before an edit — was in the middle of, so an
    /// edit changes what it changed and nothing else (the audit's H7):
    ///
    ///   - every generator whose configuration is unchanged, with its bag, its cycle and its
    ///     no-repeat memory, so a clip shuffle carries on mid-bag when the name is edited;
    ///   - the probability stream, the cooldown, the fire count and what `seesChange` last saw;
    ///   - the live gestures, mute and rate;
    ///   - **enabled as it is live** — which a control surface may have changed — unless the
    ///     configuration's own `enabled` is what the edit changed, when the edit wins.
    ///
    /// The routing masks come too, until whoever resolves them does it again.
    void carryFrom(const Rule& previous);

    /// Which outputs this rule's messages carry, as `Message::outputs`.
    ///
    /// Resolved from `Config::outputs` by whoever knows what the outputs *are* — which is
    /// `output::OutputRunner`, and neither this class nor `TriggerEngine`. Set again
    /// whenever the target list changes, or a rule keeps routing to the bit its old
    /// neighbour used to occupy.
    std::uint64_t outputMask() const noexcept { return outputMask_; }
    void setOutputMask(std::uint64_t mask) noexcept { outputMask_ = mask; }

    /// Which fixtures this rule's lighting effects reach, as `Message::fixtures`.
    ///
    /// Resolved from `DmxSend::fixtures` by whoever knows what the patch *is* — which is
    /// `output::OutputRunner`, exactly as `outputMask` is. Set again whenever the patch
    /// changes, or a rule keeps aiming at the bit its old neighbour used to occupy, and one
    /// deleted fixture sends a strobe to the front wash.
    const dmx::FixtureSet& fixtureMask() const noexcept { return fixtureMask_; }
    void setFixtureMask(const dmx::FixtureSet& mask) noexcept { fixtureMask_ = mask; }

    /// Whether `context` is a change of the kind this rule's trigger names — tempo, lock or
    /// intensity — and remembers it either way.
    ///
    /// Per rule rather than per engine because `tempoChangeTolerance` is per rule, and
    /// because what it compares against is the value that last *counted as a change* for
    /// this rule: two rules with different tolerances disagree about when a slow drift
    /// became a change, and both are right.
    ///
    /// **Call this on every round for every rule whose trigger is one of the three, whether
    /// or not the rule is enabled or its conditions hold.** What it remembers is how the
    /// rule tells a change from a value it has already seen, and a rule that stopped
    /// watching while disabled would fire the moment it was switched back on.
    bool seesChange(const Context& context) noexcept;

    /// Whether this may fire now, once its trigger has said so: the cooldown — only where
    /// `takesCooldown(trigger)` — and §5.8's ONLY IF stage, only while `Config::conditionsOn`.
    ///
    /// Not const, and each rule owns the stream its probability is drawn from, for the
    /// reason `Generator::Config::seed` gives: rules sharing one stream would fire in a
    /// pattern neither of them describes. The draw is taken last and only when the
    /// probability is short of certain, so a rule excluded by its confidence gate does not
    /// consume one — nor one with the stage switched off.
    bool conditionsHold(const Context& context) noexcept;

    /// Builds the message this rule sends, advancing its generators and starting its
    /// cooldown. Call only when the trigger and the conditions have both said yes: it is
    /// the *firing* half, and drawing from a shuffle bag for a rule that then does not fire
    /// would put a hole in it.
    ///
    /// Nothing when the message could not be built — which for OSC means the filled address
    /// came out illegal, the one thing validation cannot rule out in advance because it
    /// depends on what the generators produced.
    std::optional<Message> fire(const Context& context);

    /// The messages this rule owes after `fired`, appended to `out` in configuration order.
    ///
    /// §5.6's release half and everything past it: each is the fired message with whatever
    /// the matching `FollowUp` overrides. An entry whose kind does not fit the rule's own —
    /// see `followUpFits` — contributes nothing, so `out` can come back shorter than the
    /// configuration, and the index of an appended message is **not** its index in
    /// `Config::followUps`. `followUpDelay` takes the configuration's index, so the two are
    /// paired by `followUpsFor` returning them together rather than by the caller counting.
    ///
    /// Appended rather than assigned: the scheduler calls this once per fire on the output
    /// thread and reuses one buffer, so a rule that sends two messages allocates nothing
    /// after the first fire.
    /// `context` is read only by the DMX kinds, whose follow-ups carry a *duration* as well as
    /// a delay and so have to be converted against the tempo that was playing — the same rule
    /// `followUpDelay` follows, settled at the fire rather than when the follow-up comes due.
    void followUpsFor(const Context& context, const Message& fired,
                      std::vector<std::pair<std::size_t, Message>>& out) const;

    /// How long follow-up `index` of `Config::followUps` waits, in seconds, for a fire now.
    ///
    /// Resolved here rather than by the scheduler because the answer depends on that entry's
    /// own unit and on the tempo the fire happened at — and it is settled **at the fire**,
    /// not when the follow-up comes due: two beats after a press means two beats of the tempo
    /// that was playing, not of whatever the tracker says a second later.
    ///
    /// Falls back to the millisecond figure when the unit is musical and there is no tempo to
    /// measure against. A follow-up due immediately would be a release sent in the same round
    /// as its press, which reads as a rule that does nothing at all. Zero for an index the
    /// rule has not got.
    double followUpDelay(const Context& context, std::size_t index) const noexcept;

    /// How long a fire now is held before it is sent, in seconds: `Config::delayOn`'s wait,
    /// settled against the tempo in `context` like a follow-up's, and at most
    /// `kMaxFireDelaySeconds`. Zero while the delay is off.
    double fireDelay(const Context& context) const noexcept;

    /// When this rule last fired, on `Context::now`. Negative before it ever has.
    double lastFired() const noexcept { return lastFired_; }
    std::uint64_t fires() const noexcept { return fires_; }

    /// What each generator produced on the last fire, **in the order §5.9's editor draws
    /// the chips**: for OSC the address segments and then the value, the value left out when
    /// `sendValue` is false; for MIDI the number and then the value, each left out where the
    /// kind does not carry one — a pitch bend has no number and a program change no value,
    /// so each of those has a single entry.
    ///
    /// That ordering is the contract, and `slotLayout` is it written down once: the editor
    /// builds its rows from it and finds the generator a row edits through it, and one extra
    /// entry here would move every chip after it onto the wrong generator.
    ///
    /// Empty before the rule has fired, and cleared by a fire that could not build its
    /// message — there is nothing to show for a fire that did not happen.
    std::span<const Value> lastSlots() const noexcept { return lastSlots_; }

private:
    void validate();

    /// Draws the level, color, pan and tilt for one fire and turns `Config::dmx` into the
    /// resolved `dmx::Payload` a message carries.
    dmx::Payload buildPayload(const Context& context);

    Config config_;
    bool enabled_ = true;
    /// See `muted()`. Live state rather than configuration: a rule comes back armed, because
    /// a preset that loaded silent would look exactly like a preset that did not load.
    bool muted_ = false;
    /// See `rate()`. Live for the same reason.
    double rate_ = 1.0;
    std::string problem_;
    std::vector<Generator> segments_;
    Generator value_;
    Generator number_;
    /// The four `DmxSend` generators. Separate from `value_` and `number_` rather than
    /// reusing them, because a rule keeps its MIDI and its lighting configuration side by side
    /// (see `Config::dmx`) and sharing a generator would make switching the send kind quietly
    /// rewrite the other half.
    Generator dmxLevel_;
    Generator dmxColor_;
    /// `ColorMode::Mix`'s three. Separate from `dmxColor_` for the reason the four above are
    /// separate from `value_`: switching the color mode must not rewrite the palette an
    /// operator built, and it does not.
    Generator dmxRed_;
    Generator dmxGreen_;
    Generator dmxBlue_;
    Generator dmxPan_;
    Generator dmxTilt_;
    Generator dmxClip_;
    /// §5.8's probability percentage, on this rule's own stream. See `conditionsHold`.
    tracking::Xoshiro256pp probability_;
    /// See `outputMask`. Everywhere until somebody who knows the outputs says otherwise,
    /// which is the right default for a rule nobody has routed.
    std::uint64_t outputMask_ = ~std::uint64_t{0};
    /// See `fixtureMask`. **Nothing** until somebody who knows the patch says otherwise —
    /// the opposite default, and the safe one: a lighting rule that reached every fixture
    /// because nobody had resolved it yet would swing the whole rig on its first fire.
    dmx::FixtureSet fixtureMask_{};
    /// What `seesChange` last counted as a change. Negative means "nothing seen yet", which
    /// is not a change: a rule must not fire on the first round merely for existing.
    double lastBpmSeen_ = -1.0;
    int lastLockedSeen_ = -1;
    int lastIntensitySeen_ = -1;
    double lastFired_ = -1.0;
    std::uint64_t fires_ = 0;
    /// Reused so building an address does not reallocate on every fire.
    std::string address_;
    std::vector<Value> segmentValues_;
    /// See `lastSlots`. Reused for the same reason as `address_`: the size settles after the
    /// first fire and the output thread stops allocating for it.
    std::vector<Value> lastSlots_;
};

/// How many `{...}` placeholders an address template holds, and where they are. Public
/// because §5.9's editor needs it to draw the chips, and because a rule's validity is
/// mostly this number against the number of generators it was given.
std::size_t countPlaceholders(std::string_view address) noexcept;

/// Fills `address`'s placeholders from `values`, in order, into `out`. False — leaving `out`
/// untouched — when the counts do not match or the result would not be a legal OSC address.
bool fillAddress(std::string_view address, const Value* values, std::size_t count,
                 std::string& out);

/// Which of a rule's generators a slot is: a field of `Rule::Config`, and for `Segment`,
/// which of the address's segments.
enum class SlotRole : std::uint8_t {
    Segment,
    Value,
    Number,
    Level,
    Color,
    Red,
    Green,
    Blue,
    Pan,
    Tilt,
    /// A laser clip, by its place in deck order — `DmxSend::clip`.
    Clip,
};
struct Slot {
    SlotRole role = SlotRole::Value;
    std::size_t segment = 0;
    bool operator==(const Slot&) const = default;
};

/// The generators `config` draws when it fires, in the order `Rule::lastSlots` records what
/// they drew. **The one walk of that order**: §5.9's editor builds its chips from it and
/// finds the generator a chip edits through it, where it used to walk the same conditions
/// twice beside `Rule::fire`'s own, held together by comments saying they must not drift
/// apart (the audit's Low items). Tested against what a fire records, for every kind.
std::vector<Slot> slotLayout(const Rule::Config& config);

/// The generator configuration a slot of `config` stands for. `slot` has to be one
/// `slotLayout(config)` gave.
Generator::Config& slotGenerator(Rule::Config& config, const Slot& slot) noexcept;
const Generator::Config& slotGenerator(const Rule::Config& config, const Slot& slot) noexcept;

} // namespace takt4::trigger
