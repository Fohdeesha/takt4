#pragma once

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
    /// Not produced yet: the spectral-flux classifier that will is the next piece of Phase
    /// 6. A rule set to this is evaluated correctly and simply never sees one.
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
/// probability percentage · minimum cooldown in ms"*.
///
/// Every field defaults to "does not exclude anything", so a rule with untouched conditions
/// fires whenever its trigger says to. That is the only sane default for a stage whose
/// entire job is to *stop* a rule firing: an operator adding a rule and seeing nothing
/// happen has no way to tell a broken rule from a condition they did not know was set.
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
    /// §5.8's *"minimum cooldown in ms"*, in seconds, against `Context::now`. Measured from
    /// the last time this rule *fired*, not from the last time it was evaluated.
    double cooldownSeconds = 0.0;

    bool allows(features::Intensity intensity) const noexcept {
        return intensities[static_cast<std::size_t>(intensity)];
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
    /// since 1983". It is not: the operator's laser controller (Pangolin Liberation) holds
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
};

inline constexpr std::array<Message::Kind, 6> kMessageKinds{
    Message::Kind::Osc,    Message::Kind::MidiNote,          Message::Kind::MidiNoteOff,
    Message::Kind::MidiCc, Message::Kind::MidiProgramChange, Message::Kind::MidiPitchBend};

/// True for everything but `Osc` — the test every MIDI-shaped branch wants, written once so
/// adding a seventh kind does not mean finding every `!= Osc` in the tree.
constexpr bool isMidi(Message::Kind kind) noexcept {
    return kind != Message::Kind::Osc;
}

/// Whether the kind puts `Message::number` on the wire. False for pitch bend, which is all
/// value, and for OSC, which has neither.
constexpr bool sendsNumber(Message::Kind kind) noexcept {
    return kind == Message::Kind::MidiNote || kind == Message::Kind::MidiNoteOff ||
           kind == Message::Kind::MidiCc || kind == Message::Kind::MidiProgramChange;
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

    DelayUnit unit = DelayUnit::Milliseconds;
    /// The delay in milliseconds, used when `unit` is `Milliseconds`.
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

/// Whether `kind` can follow a rule that sends `sent` — the OSC/MIDI divide `FollowUp::kind`
/// describes. A release (no kind) always can, since it *is* the fired message.
constexpr bool followUpFits(Message::Kind kind, Message::Kind sent) noexcept {
    return isMidi(kind) == isMidi(sent);
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
        /// one. Ignored where `takesEvery` is false; zero is read as one.
        std::uint32_t every = 1;
        /// How many of `every` steps a `Euclid` pattern hits. Ignored by every other
        /// trigger. Zero is silence and is left as the operator typed it — a pattern being
        /// built up from nothing passes through it.
        std::uint32_t pulses = 3;
        /// How far the published tempo has to move to count as a `TempoChange`, as a
        /// fraction. The published tempo is *refined* continuously once locked — the beat
        /// spacing resolves it to a fraction of a BPM and it moves most beats — so an exact
        /// comparison would make this trigger mean "every beat". Measured against the tempo
        /// that last fired rather than the last one seen, so a slow drift is one change and
        /// not a stream of them.
        double tempoChangeTolerance = 0.02;

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

        /// §5.8's *"optional follow-up value after a delay"*, as many as the rule needs.
        /// Empty is a rule that sends one message and is done. See `FollowUp`, and
        /// `Rule::followUpsFor` for what each entry turns into.
        std::vector<FollowUp> followUps;

        /// Seeds every generator this rule owns, each offset from it, so two rules in a
        /// preset do not fire the same clip as each other. See `Generator::Config::seed`.
        std::uint64_t seed = 1;

        /// §5.6's *"rule subset"*: the names of the outputs this rule sends to. **Empty
        /// means every output**, which is what a rule an operator has not routed means and
        /// what every rule meant before there was more than one target.
        ///
        /// Names rather than indices, because this travels in a preset (Q7) and an index
        /// moves the moment an output above it is deleted. A name that matches nothing on
        /// this rig contributes nothing and is *kept*: a preset written where there was a
        /// "lights" output, opened where there is not, should still say "lights" so that
        /// plugging it back in restores the routing rather than needing it typed again.
        std::vector<std::string> outputs;
    };

    explicit Rule(Config config);

    const Config& config() const noexcept { return config_; }
    const std::string& id() const noexcept { return config_.id; }
    bool enabled() const noexcept { return enabled_; }
    void setEnabled(bool on) noexcept { enabled_ = on; }

    /// Empty when the rule is usable, and otherwise why it is not — for §5.9's rule card to
    /// show. An invalid rule is still a rule: it is held, edited and saved, and only
    /// refuses to fire.
    const std::string& problem() const noexcept { return problem_; }
    bool valid() const noexcept { return problem_.empty(); }

    /// Fresh generators, no cooldown owed, nothing remembered about the tempo or the lock.
    void reset() noexcept;

    /// Which outputs this rule's messages carry, as `Message::outputs`.
    ///
    /// Resolved from `Config::outputs` by whoever knows what the outputs *are* — which is
    /// `output::OutputRunner`, and neither this class nor `TriggerEngine`. Set again
    /// whenever the target list changes, or a rule keeps routing to the bit its old
    /// neighbour used to occupy.
    std::uint64_t outputMask() const noexcept { return outputMask_; }
    void setOutputMask(std::uint64_t mask) noexcept { outputMask_ = mask; }

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

    /// Whether §5.8's ONLY IF stage lets this fire now.
    ///
    /// Not const, and each rule owns the stream its probability is drawn from, for the
    /// reason `Generator::Config::seed` gives: rules sharing one stream would fire in a
    /// pattern neither of them describes. The draw is taken last and only when the
    /// probability is short of certain, so a rule excluded by its confidence gate does not
    /// consume one.
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
    void followUpsFor(const Message& fired,
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

    /// When this rule last fired, on `Context::now`. Negative before it ever has.
    double lastFired() const noexcept { return lastFired_; }
    std::uint64_t fires() const noexcept { return fires_; }

    /// What each generator produced on the last fire, **in the order §5.9's editor draws
    /// the chips**: for OSC the address segments and then the value, the value left out when
    /// `sendValue` is false; for MIDI the number and then the value, each left out where the
    /// kind does not carry one — a pitch bend has no number and a program change no value,
    /// so each of those has a single entry.
    ///
    /// That ordering is the contract. The editor pairs these with its rows by position
    /// (`RulesController::slotConfig` indexes them the same way), and one extra entry moves
    /// every chip after it onto the wrong generator.
    ///
    /// Empty before the rule has fired, and cleared by a fire that could not build its
    /// message — there is nothing to show for a fire that did not happen.
    std::span<const Value> lastSlots() const noexcept { return lastSlots_; }

private:
    void validate();

    Config config_;
    bool enabled_ = true;
    std::string problem_;
    std::vector<Generator> segments_;
    Generator value_;
    Generator number_;
    /// §5.8's probability percentage, on this rule's own stream. See `conditionsHold`.
    tracking::Xoshiro256pp probability_;
    /// See `outputMask`. Everywhere until somebody who knows the outputs says otherwise,
    /// which is the right default for a rule nobody has routed.
    std::uint64_t outputMask_ = ~std::uint64_t{0};
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

} // namespace takt4::trigger
