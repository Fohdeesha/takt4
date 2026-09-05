#include "core/trigger/generator.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace takt4::trigger {

namespace {

/// A 64-bit counter as an int32 a downstream host will accept. Wrapping into the
/// non-negative half rather than saturating, because the useful thing about a beat or bar
/// number is that it keeps moving — a counter pinned at INT32_MAX after nine months of
/// uptime would be worse than one that rolls over, and either way nothing downstream is
/// tracking absolute beats across a 2^31 boundary.
std::int32_t counterAsInt(std::uint64_t value) noexcept {
    return static_cast<std::int32_t>(value & 0x7FFFFFFFull);
}

} // namespace

std::string_view labelOf(GeneratorKind kind) noexcept {
    switch (kind) {
    case GeneratorKind::Shuffle:
        return "shuffle";
    case GeneratorKind::Random:
        return "random";
    case GeneratorKind::Cycle:
        return "cycle";
    case GeneratorKind::Weighted:
        return "weighted";
    case GeneratorKind::Fixed:
        return "fixed";
    case GeneratorKind::Live:
        return "live value";
    }
    return "";
}

std::string_view nameOf(GeneratorKind kind) noexcept {
    // The same words as the labels, minus the space in "live value", so a settings file
    // reads the way the UI does. See control::verbOf, which does this for §5.7's actions.
    return kind == GeneratorKind::Live ? "live" : labelOf(kind);
}

std::optional<GeneratorKind> generatorKindOf(std::string_view name) noexcept {
    for (const GeneratorKind kind : kGeneratorKinds) {
        if (nameOf(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string_view labelOf(LiveSource source) noexcept {
    switch (source) {
    case LiveSource::Bpm:
        return "BPM";
    case LiveSource::BpmNormalised:
        return "BPM normalised";
    case LiveSource::Beat:
        return "beat count";
    case LiveSource::BeatInBar:
        return "beat in bar";
    case LiveSource::Bar:
        return "bar count";
    case LiveSource::Confidence:
        return "confidence";
    case LiveSource::Meter:
        return "meter";
    case LiveSource::Intensity:
        return "intensity";
    }
    return "";
}

std::string_view nameOf(LiveSource source) noexcept {
    switch (source) {
    case LiveSource::Bpm:
        return "bpm";
    case LiveSource::BpmNormalised:
        return "bpm-normalised";
    case LiveSource::Beat:
        return "beat";
    case LiveSource::BeatInBar:
        return "beat-in-bar";
    case LiveSource::Bar:
        return "bar";
    case LiveSource::Confidence:
        return "confidence";
    case LiveSource::Meter:
        return "meter";
    case LiveSource::Intensity:
        return "intensity";
    }
    return "";
}

std::optional<LiveSource> liveSourceOf(std::string_view name) noexcept {
    for (const LiveSource source : kLiveSources) {
        if (nameOf(source) == name) {
            return source;
        }
    }
    return std::nullopt;
}

Generator::Generator() : Generator(Config{}) {}

Generator::Generator(Config config) : config_(std::move(config)) {
    // Clamped, never rejected — see the class note. Everything here has to leave a
    // configuration that `next()` can serve without checking anything again.
    if (config_.high < config_.low) {
        std::swap(config_.low, config_.high);
    }
    // In 64 bits so a range spanning the whole of int32 does not overflow while being
    // measured. Cut from the top: an operator who typed a range too wide meant the low end.
    const std::int64_t span =
        static_cast<std::int64_t>(config_.high) - static_cast<std::int64_t>(config_.low) + 1;
    if (span > kMaxRangeSize) {
        config_.high = static_cast<std::int32_t>(config_.low + (kMaxRangeSize - 1));
    }
    if (!(config_.normaliseHigh > config_.normaliseLow)) {
        // A degenerate host range would divide by zero, and a normalised value with no
        // range to normalise against says nothing. §5.6's Resolume figures are the sane
        // fallback because they are the only ones the handoff states.
        config_.normaliseLow = 20.0;
        config_.normaliseHigh = 500.0;
    }
    // More than one less than the number of distinct values cannot be satisfied, so asking
    // for it would only make the guard spin.
    const std::size_t distinct = choiceCount();
    const std::size_t guardable = distinct > 1 ? distinct - 1 : 0;
    config_.noRepeatWithin = std::min({config_.noRepeatWithin, kMaxNoRepeat, guardable});

    if (config_.kind == GeneratorKind::Shuffle) {
        bag_.resize(static_cast<std::size_t>(rangeSize()));
    }
    recent_.resize(config_.noRepeatWithin);
    reset();
}

void Generator::reset() noexcept {
    random_.reseed(config_.seed);
    at_ = bag_.size(); // empty, so the first draw fills it
    cycle_ = 0;
    recentAt_ = 0;
    recentCount_ = 0;
}

std::size_t Generator::choiceCount() const noexcept {
    switch (config_.kind) {
    case GeneratorKind::Shuffle:
    case GeneratorKind::Random:
    case GeneratorKind::Cycle:
        return static_cast<std::size_t>(rangeSize());
    case GeneratorKind::Weighted:
        return config_.choices.size();
    case GeneratorKind::Fixed:
        return 1;
    case GeneratorKind::Live:
        // Not drawn from a set: it is whatever the tracker is saying.
        return 0;
    }
    return 0;
}

bool Generator::isRecent(const Value& value) const noexcept {
    for (std::size_t i = 0; i < recentCount_; ++i) {
        if (recent_[i] == value) {
            return true;
        }
    }
    return false;
}

void Generator::remember(const Value& value) noexcept {
    if (recent_.empty()) {
        return;
    }
    recent_[recentAt_] = value;
    recentAt_ = (recentAt_ + 1) % recent_.size();
    recentCount_ = std::min(recentCount_ + 1, recent_.size());
}

void Generator::refillBag() noexcept {
    std::iota(bag_.begin(), bag_.end(), config_.low);
    // Fisher-Yates, from the top down, over the tracker's own generator (§5.4's
    // xoshiro256++) so a seeded run is reproducible in a test.
    for (std::size_t i = bag_.size(); i > 1; --i) {
        const auto j = static_cast<std::size_t>(random_.bounded(i));
        std::swap(bag_[i - 1], bag_[j]);
    }
    at_ = 0;
}

Value Generator::nextShuffled() noexcept {
    // Never empty: the range is at least one wide by the time the constructor is done with
    // it, and the bag is sized from that and never resized.
    if (at_ >= bag_.size()) {
        refillBag();
    }
    // Within a bag nothing can repeat, so the guard can only bite across the seam between
    // one bag and the next — which is exactly the repeat it exists for. Swapping in a
    // later element keeps the bag a permutation, so the displaced value is still drawn.
    if (isRecent(Value::ofInt(bag_[at_]))) {
        for (std::size_t j = at_ + 1; j < bag_.size(); ++j) {
            if (!isRecent(Value::ofInt(bag_[j]))) {
                std::swap(bag_[at_], bag_[j]);
                break;
            }
        }
    }
    const Value drawn = Value::ofInt(bag_[at_++]);
    remember(drawn);
    return drawn;
}

Value Generator::nextRandom() noexcept {
    const auto size = static_cast<std::uint64_t>(rangeSize());
    Value drawn = Value::ofInt(config_.low);
    for (int attempt = 0; attempt < kGuardAttempts; ++attempt) {
        drawn = Value::ofInt(config_.low + static_cast<std::int32_t>(random_.bounded(size)));
        if (!isRecent(drawn)) {
            break;
        }
    }
    // Whatever the last attempt produced. §5.8 calls this one out as the generator that
    // "will repeat"; the guard makes it repeat less, and is not allowed to make it stall.
    remember(drawn);
    return drawn;
}

Value Generator::nextCycled() noexcept {
    // Nothing is remembered: a cycle cannot repeat inside its own range, and it does not
    // consult the guard, so keeping the ring up to date would be work with no reader.
    const Value drawn = Value::ofInt(config_.low + cycle_);
    cycle_ = (cycle_ + 1) % rangeSize();
    return drawn;
}

Value Generator::nextWeighted() noexcept {
    if (config_.choices.empty()) {
        return Value{};
    }
    double total = 0.0;
    for (const WeightedChoice& choice : config_.choices) {
        total += std::max(0.0, choice.weight);
    }
    Value drawn = config_.choices.front().value;
    for (int attempt = 0; attempt < kGuardAttempts; ++attempt) {
        if (total <= 0.0) {
            // Every weight zero or negative. Uniform rather than an error: an operator who
            // cleared the weights meant "no preference", and refusing to fire would be a
            // rule that silently stopped working.
            const auto index = static_cast<std::size_t>(random_.bounded(config_.choices.size()));
            drawn = config_.choices[index].value;
        } else {
            double ticket = random_.nextDouble() * total;
            drawn = config_.choices.back().value;
            for (const WeightedChoice& choice : config_.choices) {
                ticket -= std::max(0.0, choice.weight);
                if (ticket < 0.0) {
                    drawn = choice.value;
                    break;
                }
            }
        }
        if (!isRecent(drawn)) {
            break;
        }
    }
    remember(drawn);
    return drawn;
}

Value Generator::nextLive(const Context& context) const noexcept {
    switch (config_.source) {
    case LiveSource::Bpm:
        return Value::ofFloat(static_cast<float>(context.bpm));
    case LiveSource::BpmNormalised: {
        const double span = config_.normaliseHigh - config_.normaliseLow;
        const double scaled = (context.bpm - config_.normaliseLow) / span;
        return Value::ofFloat(static_cast<float>(std::clamp(scaled, 0.0, 1.0)));
    }
    case LiveSource::Beat:
        return Value::ofInt(counterAsInt(context.beats));
    case LiveSource::BeatInBar:
        return Value::ofInt(static_cast<std::int32_t>(context.beatInBar));
    case LiveSource::Bar:
        return Value::ofInt(counterAsInt(context.bars));
    case LiveSource::Confidence:
        return Value::ofFloat(static_cast<float>(context.confidence));
    case LiveSource::Meter:
        return Value::ofInt(static_cast<std::int32_t>(context.meter));
    case LiveSource::Intensity:
        return Value::ofInt(static_cast<std::int32_t>(context.intensity));
    }
    return Value{};
}

Value Generator::next(const Context& context) noexcept {
    switch (config_.kind) {
    case GeneratorKind::Shuffle:
        return nextShuffled();
    case GeneratorKind::Random:
        return nextRandom();
    case GeneratorKind::Cycle:
        return nextCycled();
    case GeneratorKind::Weighted:
        return nextWeighted();
    case GeneratorKind::Fixed:
        return config_.fixed;
    case GeneratorKind::Live:
        return nextLive(context);
    }
    return Value{};
}

} // namespace takt4::trigger
