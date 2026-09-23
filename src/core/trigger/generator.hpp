#pragma once

#include "core/tracking/random.hpp"
#include "core/trigger/context.hpp"
#include "core/trigger/value.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace takt4::trigger {

/// HANDOFF §5.8's six value generators.
///
/// | Generator | Behaviour |
/// |---|---|
/// | **Shuffle** | Draw from a bag without replacement, reshuffle when empty. **The default.** |
/// | Random | Uniform random int in range. Will repeat; offered but not default. |
/// | Cycle | Round-robin through a range |
/// | Weighted | Weighted pick from an explicit list |
/// | Fixed | Literal int, float, string or bool |
/// | Live | A live value — BPM raw or normalised, beat, bar, confidence, meter, intensity |
///
/// §5.8 spells the last one **Value**, and this does not, because `Value` is already the
/// name of the thing every generator produces — a generator called Value, being the one
/// whose output it does not choose, is the worst possible place for that collision.
/// `labelOf` gives a UI the words a person should see.
enum class GeneratorKind : std::uint8_t {
    Shuffle,
    Random,
    Cycle,
    Weighted,
    Fixed,
    Live,
    /// A value that sweeps its range over a whole number of bars, locked to the downbeat.
    ///
    /// **Not in §5.8's list**, and added on the user's ask for *"algorithmic settings ...
    /// based on the bpm of the song and downbeat"*. It is the one generator whose output is
    /// neither drawn nor read but *computed from where in the music we are*, which is what a
    /// Resolume dashboard parameter, a layer opacity or a clip speed wants — a value that
    /// breathes with the track rather than jumping about.
    ///
    /// Sampled when a rule fires, so its resolution is the rule's trigger: a rule on every
    /// beat gives four steps a bar, one on every sixteenth-note Euclid step gives sixteen.
    /// That is the right resolution — it is a *musical* ramp, not an animation.
    Ramp,
};

/// Every kind, in the order a UI should offer them: §5.8's own order, which puts the
/// default first and the deliberate choice second.
inline constexpr std::array<GeneratorKind, 7> kGeneratorKinds{
    GeneratorKind::Shuffle, GeneratorKind::Random, GeneratorKind::Cycle, GeneratorKind::Weighted,
    GeneratorKind::Fixed,   GeneratorKind::Live,   GeneratorKind::Ramp};

/// The shape a `Ramp` traces over its period.
enum class RampShape : std::uint8_t {
    /// Low to high, then straight back. A build that resets on the downbeat.
    Saw,
    /// Low to high and back down again — the one that does not jump, so it suits anything
    /// an audience watches continuously.
    Triangle,
    /// A triangle with the corners taken off. Slower at the ends, quicker through the
    /// middle, which is what "breathing" actually looks like.
    Sine,
    /// Low for the first half of the period, high for the second. A gate rather than a
    /// sweep, and the cheapest way to make something alternate every N bars.
    Square,
};

inline constexpr std::array<RampShape, 4> kRampShapes{RampShape::Saw, RampShape::Triangle,
                                                      RampShape::Sine, RampShape::Square};

std::string_view labelOf(RampShape shape) noexcept;
std::string_view nameOf(RampShape shape) noexcept;
std::optional<RampShape> rampShapeOf(std::string_view name) noexcept;

/// Where in a `Ramp`'s period the music is, 0 to 1 — the *phase*, before any shape.
///
/// Built from the bar count and the position within the bar, so it is locked to the
/// tracker's own downbeat rather than to a clock of its own: a ramp over four bars restarts
/// on bar 1, 5, 9, and a manual downbeat snap moves it with the music.
///
/// `meter` of zero means the filter has no opinion about the bar yet (§5.5 — nothing assumes
/// four), in which case the beat within the bar cannot be used and the phase advances a whole
/// bar at a time.
double rampPhase(const Context& context, std::uint32_t bars) noexcept;

std::string_view labelOf(GeneratorKind kind) noexcept;
/// The word a settings file spells the kind with, and what `generatorKindOf` reads back.
std::string_view nameOf(GeneratorKind kind) noexcept;
std::optional<GeneratorKind> generatorKindOf(std::string_view name) noexcept;

/// What the `Live` generator can read out of a `Context`.
enum class LiveSource : std::uint8_t {
    Bpm,
    /// §5.6's Resolume tempo: *"float, normalised 0-1 across 20-500 BPM"*. The range is a
    /// property of the *host*, not of takt4, so it is configurable — that address is the
    /// reason this source exists and hardcoding 20-500 would make it Resolume's alone.
    BpmNormalised,
    Beat,
    BeatInBar,
    Bar,
    Confidence,
    Meter,
    Intensity,
};

inline constexpr std::array<LiveSource, 8> kLiveSources{
    LiveSource::Bpm, LiveSource::BpmNormalised, LiveSource::Beat,  LiveSource::BeatInBar,
    LiveSource::Bar, LiveSource::Confidence,    LiveSource::Meter, LiveSource::Intensity};

std::string_view labelOf(LiveSource source) noexcept;
std::string_view nameOf(LiveSource source) noexcept;
std::optional<LiveSource> liveSourceOf(std::string_view name) noexcept;

/// Where `Shuffle`, `Random` and `Cycle` take their values from.
///
/// §5.8 describes those three over a *range* — "uniform random int in range", "round-robin
/// through a range" — which is right for a clip grid and wrong for the other thing an
/// operator wants constantly: a handful of clips they picked, in an order they chose. A
/// range cannot say "3, 7, 1, 12", and neither could anything else here: `Weighted` holds an
/// arbitrary list but draws from it at random, so an *ordered* list had no expression at all.
///
/// **This is a second dimension, not two more kinds.** "Which values" and "in what order"
/// are two questions an operator answers separately — the same four clips can be cycled or
/// shuffled, and the same range can be either — so doubling `GeneratorKind` to cover the
/// pairs would make the UI ask one question where there are two, and would leave
/// `noRepeatWithin` meaning something subtly different in each half.
enum class Pool : std::uint8_t {
    /// Every integer from `low` to `high`. §5.8's own reading, and the default.
    Range,
    /// Exactly `values`, in the order given.
    List,
};

inline constexpr std::array<Pool, 2> kPools{Pool::Range, Pool::List};

std::string_view labelOf(Pool pool) noexcept;
std::string_view nameOf(Pool pool) noexcept;
std::optional<Pool> poolOf(std::string_view name) noexcept;

/// True where a generator's `pool` means anything — the three §5.8 spells with a range.
/// `Weighted` carries its own list, `Fixed` one literal, and `Live` no set at all.
bool takesPool(GeneratorKind kind) noexcept;

/// One entry of a `Weighted` generator's list.
struct WeightedChoice {
    Value value;
    /// Relative, not a probability: the list is normalised by its own total. Zero means
    /// "never", and a list that is all zeroes or all negative is treated as uniform rather
    /// than as an error — see `Generator`'s note on clamping.
    double weight = 1.0;

    bool operator==(const WeightedChoice&) const noexcept = default;
};

/// A generator, as a flat configuration plus whatever state its kind needs.
///
/// One class with a kind tag rather than six types behind an interface, because of what has
/// to be done with these. §5.9 requires the rule syntax to be *"an internal representation
/// the user never types"*, edited by clicking a dropdown and a few fields; and Q7 requires
/// rules to travel in a preset file. Both of those want a struct that can be copied,
/// serialised and re-read, and neither wants a polymorphic hierarchy with a clone method.
///
/// **A configuration is clamped, never rejected.** `settings::load` is documented to never
/// fail — *"settings that cannot be parsed must not be the reason an app will not open"* —
/// so a generator built from a file with an inverted range or an absurd one has to do
/// something sensible rather than throw. `config()` returns what was actually accepted, so
/// a UI shows the operator what they have rather than what they typed.
///
/// `next()` allocates nothing: the bag and the no-repeat ring are sized once here.
class Generator {
public:
    /// The largest range `Shuffle` and `Random` will span. A bag is drawn without
    /// replacement and so has to be held, and a clip grid larger than this is not a thing
    /// anyone has; a range past it is clamped rather than refused.
    static constexpr std::int32_t kMaxRangeSize = 4096;
    /// The most recent draws the no-repeat guard can be asked to remember.
    static constexpr std::size_t kMaxNoRepeat = 64;
    /// How hard `Random` and `Weighted` try to avoid a recent value before giving up and
    /// sending it anyway. A rule that stalls is worse than a rule that repeats, and with a
    /// range of one there is nothing else to send.
    static constexpr int kGuardAttempts = 16;

    struct Config {
        /// §5.8: *"Shuffle ... **The default.**"* — and the handoff is emphatic about why:
        /// *"Plain random repeats constantly, and every repeat reads to an audience as a
        /// bug. Shuffle-without-replacement is the difference between 'random visuals' and
        /// 'visuals that look random'."*
        GeneratorKind kind = GeneratorKind::Shuffle;

        /// Where `Shuffle`, `Random` and `Cycle` draw from. Ignored by the other three.
        Pool pool = Pool::Range;

        /// The range for `pool == Range`, inclusive at both ends. Given backwards it is
        /// swapped; wider than `kMaxRangeSize` it is cut from the top.
        std::int32_t low = 1;
        std::int32_t high = 8;

        /// The values for `pool == List`, in the order an operator put them in — which is
        /// the order `Cycle` sends them in, and the set `Shuffle` draws from.
        ///
        /// `Value`, not `int`, so a sequence can be of anything a rule can send: clip
        /// numbers, but also the text segments of an address (`intro`, `build`, `drop`) or
        /// a set of floats. Truncated to `kMaxRangeSize` for the same reason a range is —
        /// `Shuffle` holds a bag of them.
        ///
        /// **Empty is allowed and draws integer zero**, which is what an unconfigured
        /// generator should send: something harmless, not nothing. It is not filled in with
        /// a placeholder, because an editor mid-way through building a list would then show
        /// an entry the operator did not add.
        std::vector<Value> values;

        /// §5.8's *"no repeat within N guard"*, in draws. **One by default**, which for
        /// `Shuffle` costs nothing and closes the one hole a bag has: within a bag no value
        /// can repeat, but the last draw of one bag and the first of the next are
        /// independent, so a bag of eight repeats at the seam one time in eight. That is
        /// the repeat an audience sees. Clamped to `kMaxNoRepeat`, and to one less than the
        /// number of distinct values, since asking for more than that is unsatisfiable.
        std::size_t noRepeatWithin = 1;

        /// `Weighted`'s list. Empty produces integer zero, which is what an unconfigured
        /// generator should send: something harmless, not nothing.
        std::vector<WeightedChoice> choices;

        /// `Fixed`'s literal.
        Value fixed;

        /// `Live`'s source and, for `BpmNormalised`, the host's range.
        LiveSource source = LiveSource::Bpm;
        double normaliseLow = 20.0;
        double normaliseHigh = 500.0;

        /// `Ramp`'s shape and its period in bars. Four bars is a phrase, which is the unit
        /// an operator counts in and the one a build is written over.
        RampShape shape = RampShape::Triangle;
        std::uint32_t rampBars = 4;
        /// Whether a `Ramp` produces a float or an int.
        ///
        /// A float across `low`-`high` is what a normalised host parameter wants — Resolume
        /// takes 0-1 — and an int is what a clip index or a MIDI value wants. Both come from
        /// the same phase; only the last step differs, and getting it wrong is the difference
        /// between a smooth fade and four steps.
        bool rampFloat = true;

        /// The random stream. Two generators with the same seed and configuration produce
        /// the same sequence, which is what lets a test state what Shuffle does — so the
        /// rule engine has to hand out distinct ones, or every rule in a preset fires the
        /// same clip as every other.
        std::uint64_t seed = 1;

        /// Every field, the seed included: two configurations that compare equal draw the
        /// same sequence, which is what lets an edited rule keep a generator it did not touch
        /// mid-bag (`Rule::carryFrom`).
        bool operator==(const Config&) const noexcept = default;
    };

    /// §5.8's default: Shuffle over 1 to 8, with the seam guard on.
    ///
    /// Two constructors rather than `Config config = {}`, which MSVC accepts and clang
    /// rejects — a default argument is a complete-class context, and a *nested* class's
    /// member initialisers are not available in one. §6 of the handoff lists this exact
    /// shape as a CI-only failure; `tracking::TempoTracker` is split the same way.
    Generator();
    explicit Generator(Config config);

    /// What was accepted, after clamping. Not what was passed in.
    const Config& config() const noexcept { return config_; }
    GeneratorKind kind() const noexcept { return config_.kind; }

    /// A fresh bag, a cycle back at the start, the seed re-applied and nothing remembered.
    void reset() noexcept;

    /// The next value. `context` is read by `Live` and `Ramp`; the others ignore it.
    Value next(const Context& context) noexcept;

    /// How many distinct values this can produce, where that is a finite number a UI can
    /// show — the range size or the list length for `Shuffle`, `Random` and `Cycle`, the
    /// list length for `Weighted`, 1 for `Fixed`, and 0 for `Live` and `Ramp`, whose values
    /// are computed rather than drawn from a set.
    ///
    /// **Honest, so it can be zero**: an empty list has nothing in it, and a UI saying "1"
    /// there would be reporting the harmless zero that gets sent rather than what the
    /// operator has. `drawSize()` is the other number and is never zero.
    std::size_t choiceCount() const noexcept;

private:
    Value nextShuffled() noexcept;
    Value nextRandom() noexcept;
    Value nextCycled() noexcept;
    Value nextWeighted() noexcept;
    Value nextLive(const Context& context) const noexcept;
    Value nextRamp(const Context& context) const noexcept;

    void refillBag() noexcept;
    bool isRecent(const Value& value) const noexcept;
    void remember(const Value& value) noexcept;
    std::int32_t rangeSize() const noexcept { return config_.high - config_.low + 1; }
    /// How many values a draw picks between — `choiceCount()`, floored at one so that an
    /// empty list has something to draw. Every index handed to `poolValue` is below this.
    std::size_t drawSize() const noexcept;
    /// The `index`-th value of whichever pool this draws from. Integer zero past the end of
    /// an empty list, which is the one case `drawSize()`'s floor lets through.
    Value poolValue(std::size_t index) const noexcept;

    Config config_;
    tracking::Xoshiro256pp random_;
    /// The shuffled pool as *indices* into it, and how far through we are. Indices rather
    /// than values so one bag serves a range and a list alike. Sized once.
    std::vector<std::int32_t> bag_;
    std::size_t at_ = 0;
    /// `Cycle`'s position, and the no-repeat guard's ring of recent draws. `recentCount_`
    /// is how much of it is filled, so a fresh generator does not guard against zeroes.
    std::int32_t cycle_ = 0;
    std::vector<Value> recent_;
    std::size_t recentAt_ = 0;
    std::size_t recentCount_ = 0;
};

} // namespace takt4::trigger
