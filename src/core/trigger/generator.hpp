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
enum class GeneratorKind : std::uint8_t { Shuffle, Random, Cycle, Weighted, Fixed, Live };

/// Every kind, in the order a UI should offer them: §5.8's own order, which puts the
/// default first and the deliberate choice second.
inline constexpr std::array<GeneratorKind, 6> kGeneratorKinds{
    GeneratorKind::Shuffle,  GeneratorKind::Random, GeneratorKind::Cycle,
    GeneratorKind::Weighted, GeneratorKind::Fixed,  GeneratorKind::Live};

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

/// One entry of a `Weighted` generator's list.
struct WeightedChoice {
    Value value;
    /// Relative, not a probability: the list is normalised by its own total. Zero means
    /// "never", and a list that is all zeroes or all negative is treated as uniform rather
    /// than as an error — see `Generator`'s note on clamping.
    double weight = 1.0;
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

        /// The range for `Shuffle`, `Random` and `Cycle`, inclusive at both ends. Given
        /// backwards it is swapped; wider than `kMaxRangeSize` it is cut from the top.
        std::int32_t low = 1;
        std::int32_t high = 8;

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

        /// The random stream. Two generators with the same seed and configuration produce
        /// the same sequence, which is what lets a test state what Shuffle does — so the
        /// rule engine has to hand out distinct ones, or every rule in a preset fires the
        /// same clip as every other.
        std::uint64_t seed = 1;
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

    /// The next value. `context` is read only by `Live`; the others ignore it.
    Value next(const Context& context) noexcept;

    /// How many distinct values this can produce, where that is a finite number a UI can
    /// show — the range size for `Shuffle`, `Random` and `Cycle`, the list length for
    /// `Weighted`, 1 for `Fixed`, and 0 for `Live`, whose values are not drawn from a set.
    std::size_t choiceCount() const noexcept;

private:
    Value nextShuffled() noexcept;
    Value nextRandom() noexcept;
    Value nextCycled() noexcept;
    Value nextWeighted() noexcept;
    Value nextLive(const Context& context) const noexcept;

    void refillBag() noexcept;
    bool isRecent(const Value& value) const noexcept;
    void remember(const Value& value) noexcept;
    std::int32_t rangeSize() const noexcept { return config_.high - config_.low + 1; }

    Config config_;
    tracking::Xoshiro256pp random_;
    /// The shuffled range, and how far through it we are. Sized once.
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
