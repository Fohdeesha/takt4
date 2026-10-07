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
    case GeneratorKind::Ramp:
        return "ramp";
    }
    return "";
}

std::string_view labelOf(RampShape shape) noexcept {
    switch (shape) {
    case RampShape::Saw:
        return "saw";
    case RampShape::Triangle:
        return "triangle";
    case RampShape::Sine:
        return "sine";
    case RampShape::Square:
        return "square";
    }
    return "";
}

std::string_view nameOf(RampShape shape) noexcept {
    return labelOf(shape);
}

std::optional<RampShape> rampShapeOf(std::string_view name) noexcept {
    for (const RampShape shape : kRampShapes) {
        if (nameOf(shape) == name) {
            return shape;
        }
    }
    return std::nullopt;
}

double rampPhase(const Context& context, std::uint32_t bars) noexcept {
    const std::uint32_t period = std::max<std::uint32_t>(1, bars);
    // Counted from the first bar, so a four-bar ramp restarts on bars 1, 5, 9 — the same
    // "counting from the first" every other N in this layer uses (§7 deviation 11).
    const std::uint64_t bar = context.bars >= 1 ? context.bars - 1 : 0;
    const double whole = static_cast<double>(bar % period);

    // Where in the bar we are, when the filter has an opinion about the bar at all. §5.5:
    // nothing assumes four, and a meter of zero means it has not decided — in which case the
    // ramp still advances, a whole bar at a time, rather than sitting at zero.
    double within = 0.0;
    if (context.meter > 0 && context.beatInBar >= 1) {
        within = static_cast<double>(context.beatInBar - 1) / static_cast<double>(context.meter);
    }
    return (whole + within) / static_cast<double>(period);
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

std::string_view labelOf(Pool pool) noexcept {
    switch (pool) {
    case Pool::Range:
        return "range";
    case Pool::List:
        return "list";
    }
    return "";
}

std::string_view nameOf(Pool pool) noexcept {
    return labelOf(pool);
}

std::optional<Pool> poolOf(std::string_view name) noexcept {
    for (const Pool pool : kPools) {
        if (nameOf(pool) == name) {
            return pool;
        }
    }
    return std::nullopt;
}

bool takesPool(GeneratorKind kind) noexcept {
    return kind == GeneratorKind::Shuffle || kind == GeneratorKind::Random ||
           kind == GeneratorKind::Cycle;
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
    // A ramp's range has a direction — from `low` to `high` — and one from 100 to 0 falls; it used
    // to be swapped, and rose. For the rest the order is only which end is which.
    if (config_.kind != GeneratorKind::Ramp && config_.high < config_.low) {
        std::swap(config_.low, config_.high);
    }
    // **Only where a bag is held**: `Shuffle` draws without replacement, so it holds its whole
    // range or list. Cut from the top: an operator who typed a range too wide meant the low end;
    // and a list from the end, which is where one who pasted too many meant to stop. Every other
    // kind draws or steps without holding anything, and was cut too — a pitch bend over 0 to
    // 16383 to 0 to 4095 — while the editor went on showing what had been typed.
    if (config_.kind == GeneratorKind::Shuffle) {
        if (rangeSize() > kMaxRangeSize) {
            config_.high = static_cast<std::int32_t>(config_.low + (kMaxRangeSize - 1));
        }
        if (config_.values.size() > static_cast<std::size_t>(kMaxRangeSize)) {
            config_.values.resize(static_cast<std::size_t>(kMaxRangeSize));
        }
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
        bag_.resize(drawSize());
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

std::size_t Generator::drawSize() const noexcept {
    const std::size_t size = choiceCount();
    // An empty list still has to produce something, and integer zero is that something.
    return size > 0 ? size : 1;
}

Value Generator::poolValue(std::size_t index) const noexcept {
    if (config_.pool == Pool::List) {
        // The bound only fails for the empty list `drawSize()` floors at one.
        return index < config_.values.size() ? config_.values[index] : Value{};
    }
    // In 64 bits, for a range spanning the whole of int32; every index is below its size, so the
    // sum is inside it.
    return Value::ofInt(static_cast<std::int32_t>(static_cast<std::int64_t>(config_.low) +
                                                  static_cast<std::int64_t>(index)));
}

std::size_t Generator::choiceCount() const noexcept {
    switch (config_.kind) {
    case GeneratorKind::Shuffle:
    case GeneratorKind::Random:
    case GeneratorKind::Cycle:
        return config_.pool == Pool::List ? config_.values.size()
                                          : static_cast<std::size_t>(rangeSize());
    case GeneratorKind::Weighted:
        return config_.choices.size();
    case GeneratorKind::Fixed:
        return 1;
    case GeneratorKind::Live:
    case GeneratorKind::Ramp:
        // Not drawn from a set: computed from what the tracker is saying.
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
    // Indices into the pool, not the values themselves: that is what lets one bag shuffle a
    // range and a list without knowing which it has.
    std::iota(bag_.begin(), bag_.end(), 0);
    // Fisher-Yates, from the top down, over the tracker's own generator (§5.4's
    // xoshiro256++) so a seeded run is reproducible in a test.
    for (std::size_t i = bag_.size(); i > 1; --i) {
        const auto j = static_cast<std::size_t>(random_.bounded(i));
        std::swap(bag_[i - 1], bag_[j]);
    }
    at_ = 0;
}

Value Generator::nextShuffled() noexcept {
    // Never empty: `drawSize()` is at least one by the time the constructor is done with
    // it, and the bag is sized from that and never resized.
    if (at_ >= bag_.size()) {
        refillBag();
    }
    const auto valueAt = [this](std::size_t slot) {
        return poolValue(static_cast<std::size_t>(bag_[slot]));
    };
    // Within a bag nothing can repeat, so the guard can only bite across the seam between
    // one bag and the next — which is exactly the repeat it exists for. Swapping in a
    // later element keeps the bag a permutation, so the displaced value is still drawn.
    if (isRecent(valueAt(at_))) {
        for (std::size_t j = at_ + 1; j < bag_.size(); ++j) {
            if (!isRecent(valueAt(j))) {
                std::swap(bag_[at_], bag_[j]);
                break;
            }
        }
    }
    const Value drawn = valueAt(at_++);
    remember(drawn);
    return drawn;
}

Value Generator::nextRandom() noexcept {
    const auto size = static_cast<std::uint64_t>(drawSize());
    Value drawn = poolValue(0);
    for (int attempt = 0; attempt < kGuardAttempts; ++attempt) {
        drawn = poolValue(static_cast<std::size_t>(random_.bounded(size)));
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
    // Nothing is remembered: a cycle cannot repeat inside its own pool, and it does not
    // consult the guard, so keeping the ring up to date would be work with no reader.
    //
    // **This is the ordered sequence.** With `Pool::List` it walks the operator's own list
    // in the order they wrote it — 3, 7, 1, 12, 3, 7 — which is the thing a range could
    // never say and the reason `Pool` exists.
    const Value drawn = poolValue(static_cast<std::size_t>(cycle_));
    cycle_ = (cycle_ + 1) % static_cast<std::uint64_t>(drawSize());
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
    case GeneratorKind::Ramp:
        return nextRamp(context);
    }
    return Value{};
}

Value Generator::nextRamp(const Context& context) const noexcept {
    const double phase = rampPhase(context, config_.rampBars);

    // The shape, all on a 0-to-1 phase and all returning 0 to 1.
    double shaped = phase;
    switch (config_.shape) {
    case RampShape::Saw:
        break;
    case RampShape::Triangle:
        shaped = phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0;
        break;
    case RampShape::Sine:
        // A raised cosine: 0 at both ends, 1 in the middle, and flat where it turns — which
        // is the difference between a triangle and something that looks like breathing.
        shaped = 0.5 - 0.5 * std::cos(phase * 2.0 * 3.14159265358979323846);
        break;
    case RampShape::Square:
        shaped = phase < 0.5 ? 0.0 : 1.0;
        break;
    }

    // Onto the operator's own range. **The two kinds map differently, and it is not a
    // rounding detail** — they are answers to different questions.
    //
    // A float *sweeps*: 0 to 1 across the phrase, for a host parameter that is watched
    // continuously. The top is reached only at phase 1, which a saw never quite gets to
    // because phase 1 *is* phase 0 of the next phrase — which is right, and is what makes a
    // saw loop without a hitch in it.
    //
    // An int *steps*: a ramp over 1-4 across four bars should be 1, 2, 3, 4, one to a bar.
    // Sweeping and rounding gives 1, 2, 3, 3 — the top value appearing only at an instant
    // nothing samples — which is the same off-by-one that makes a stepped sequencer feel
    // wrong. So the range is cut into `high - low + 1` equal slices and the phase picks one.
    // From `low` to `high`, which may be downwards: see `Config::low`.
    const double low = static_cast<double>(config_.low);
    const double high = static_cast<double>(config_.high);
    if (config_.rampFloat) {
        return Value::ofFloat(static_cast<float>(low + shaped * (high - low)));
    }
    const double steps = std::abs(high - low) + 1.0;
    const double slice = std::floor(shaped * steps);
    // `shaped` reaches exactly 1 at the top of a triangle or a sine, which would index one
    // slice past the end.
    const double way = high < low ? -1.0 : 1.0;
    const double picked = low + way * std::min(slice, steps - 1.0);
    return Value::ofInt(static_cast<std::int32_t>(picked));
}

} // namespace takt4::trigger
