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
    enum class Kind : std::uint8_t { Osc, MidiNote, MidiCc };

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

inline constexpr std::array<Message::Kind, 3> kMessageKinds{
    Message::Kind::Osc, Message::Kind::MidiNote, Message::Kind::MidiCc};

/// What §5.9's SEND dropdown offers, and what a preset spells the choice with.
std::string_view labelOf(Message::Kind kind) noexcept;
std::string_view nameOf(Message::Kind kind) noexcept;
std::optional<Message::Kind> messageKindOf(std::string_view name) noexcept;

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

        /// §5.8's *"optional follow-up value after a delay"* — §5.6's press-then-release.
        bool followUp = false;
        Value followUpValue = Value::ofInt(0);
        double followUpDelaySeconds = 0.05;

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

    /// The follow-up for a message just fired, or nothing when the rule has none. §5.6's
    /// release half: the same address and MIDI target, a different value.
    std::optional<Message> followUpFor(const Message& fired) const;

    /// When this rule last fired, on `Context::now`. Negative before it ever has.
    double lastFired() const noexcept { return lastFired_; }
    std::uint64_t fires() const noexcept { return fires_; }

    /// What each generator produced on the last fire, **in the order §5.9's editor draws
    /// the chips**: the address segments and then the value for OSC, the note or CC number
    /// and then the value for MIDI, with the value left out when the rule sends none.
    ///
    /// That ordering is the contract. The editor pairs these with its rows by position, and
    /// a generator that produced a value nobody can see beside the box that configures it is
    /// a rule an operator has to read the event log to debug.
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
