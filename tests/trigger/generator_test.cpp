#include "core/trigger/generator.hpp"
#include "core/trigger/value.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using Catch::Approx;
using takt4::features::Intensity;
using takt4::trigger::Context;
using takt4::trigger::Generator;
using takt4::trigger::GeneratorKind;
using takt4::trigger::LiveSource;
using takt4::trigger::Value;
using takt4::trigger::WeightedChoice;

namespace {

/// `count` draws, as the integers they are. Every generator but `Live` ignores the context.
std::vector<std::int32_t> draw(Generator& generator, std::size_t count) {
    const Context context;
    std::vector<std::int32_t> drawn;
    drawn.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        drawn.push_back(generator.next(context).asInt());
    }
    return drawn;
}

/// The longest run of one value, which is what an audience actually notices.
std::size_t longestRun(const std::vector<std::int32_t>& values) {
    std::size_t longest = 0;
    std::size_t run = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        run = (i > 0 && values[i] == values[i - 1]) ? run + 1 : 1;
        longest = std::max(longest, run);
    }
    return longest;
}

} // namespace

TEST_CASE("shuffle empties its bag before refilling it", "[trigger][generator]") {
    // §5.8: "Draw from a bag without replacement, reshuffle when empty. **The default.**"
    // And the handoff's reason, which is the one thing to keep in mind when touching this:
    // "Plain random repeats constantly, and every repeat reads to an audience as a bug.
    // Shuffle-without-replacement is the difference between 'random visuals' and 'visuals
    // that look random'."
    Generator::Config config;
    config.low = 1;
    config.high = 8;
    Generator generator(config);
    CHECK(generator.kind() == GeneratorKind::Shuffle); // the default, unset above
    CHECK(generator.choiceCount() == 8);

    const std::vector<std::int32_t> drawn = draw(generator, 8);
    const std::set<std::int32_t> distinct(drawn.begin(), drawn.end());
    CHECK(distinct.size() == 8);
    CHECK(*distinct.begin() == 1);
    CHECK(*distinct.rbegin() == 8);

    SECTION("and every bag after it is a permutation too") {
        for (int bag = 0; bag < 32; ++bag) {
            const std::vector<std::int32_t> next = draw(generator, 8);
            const std::set<std::int32_t> each(next.begin(), next.end());
            INFO("bag " << bag);
            CHECK(each.size() == 8);
        }
    }

    SECTION("it is a shuffle, not a cycle") {
        // A bag that came out in the same order every time would satisfy every check above
        // and be useless. Over sixteen bags at least one has to differ from the first.
        bool differed = false;
        for (int bag = 0; bag < 16 && !differed; ++bag) {
            differed = draw(generator, 8) != drawn;
        }
        CHECK(differed);
    }
}

TEST_CASE("the seam between one shuffled bag and the next does not repeat",
          "[trigger][generator]") {
    // The one hole a bag has, and the reason §5.8 asks for a "no repeat within N guard"
    // alongside it: within a bag nothing can repeat, but the last draw of one bag and the
    // first of the next are independent, so a bag of four repeats at the seam a quarter of
    // the time. That is precisely the repeat an audience reads as a bug.
    Generator::Config config;
    config.low = 1;
    config.high = 4;
    Generator guarded(config); // noRepeatWithin defaults to 1
    REQUIRE(guarded.config().noRepeatWithin == 1);

    const std::vector<std::int32_t> drawn = draw(guarded, 4000);
    CHECK(longestRun(drawn) == 1);

    SECTION("and the guard can be switched off, which is what makes it worth having on") {
        // Not a preference: with it off the seam really does repeat, and this says so, so
        // that a change quietly disabling the guard fails a test rather than a set.
        config.noRepeatWithin = 0;
        Generator bare(config);
        CHECK(bare.config().noRepeatWithin == 0);
        CHECK(longestRun(draw(bare, 4000)) > 1);
    }

    SECTION("a wider guard holds across more than the seam") {
        config.low = 1;
        config.high = 8;
        config.noRepeatWithin = 3;
        Generator wide(config);
        REQUIRE(wide.config().noRepeatWithin == 3);
        const std::vector<std::int32_t> values = draw(wide, 4000);
        for (std::size_t i = 3; i < values.size(); ++i) {
            INFO("draw " << i);
            REQUIRE(values[i] != values[i - 1]);
            REQUIRE(values[i] != values[i - 2]);
            REQUIRE(values[i] != values[i - 3]);
        }
    }

    SECTION("a guard wider than the range is cut to what can be satisfied, not obeyed") {
        // Four distinct values cannot avoid the last four, and a generator that tried would
        // spin. Three is the most that can be asked of a range of four.
        config.noRepeatWithin = 99;
        Generator asking(config);
        CHECK(asking.config().noRepeatWithin == 3);
        CHECK(draw(asking, 200).size() == 200);
    }
}

TEST_CASE("shuffle covers its range evenly", "[trigger][generator]") {
    // Without replacement, so this is exact rather than statistical: 500 bags of 10 is
    // exactly 500 of each. A guard biasing the draw would show here as a count that is not.
    Generator::Config config;
    config.low = 0;
    config.high = 9;
    Generator generator(config);

    std::vector<int> counts(10, 0);
    for (const std::int32_t value : draw(generator, 5000)) {
        REQUIRE(value >= 0);
        REQUIRE(value <= 9);
        ++counts[static_cast<std::size_t>(value)];
    }
    for (std::size_t i = 0; i < counts.size(); ++i) {
        INFO("value " << i);
        CHECK(counts[i] == 500);
    }
}

TEST_CASE("random repeats where shuffle does not, which is why it is not the default",
          "[trigger][generator]") {
    // §5.8: "Uniform random int in range. Will repeat; offered but not default." Both
    // halves are behaviour worth holding: it has to cover the range, and it has to be
    // visibly worse at not repeating than the default is.
    Generator::Config config;
    config.kind = GeneratorKind::Random;
    config.low = 1;
    config.high = 4;
    config.noRepeatWithin = 0; // plain, as §5.8 describes it
    Generator generator(config);

    const std::vector<std::int32_t> drawn = draw(generator, 4000);
    std::vector<int> counts(5, 0);
    for (const std::int32_t value : drawn) {
        REQUIRE(value >= 1);
        REQUIRE(value <= 4);
        ++counts[static_cast<std::size_t>(value)];
    }
    for (int value = 1; value <= 4; ++value) {
        INFO("value " << value);
        // 1000 expected; three sigma is about 82, so this is loose enough never to flake
        // and tight enough to catch a generator that is not uniform.
        CHECK(counts[static_cast<std::size_t>(value)] > 850);
        CHECK(counts[static_cast<std::size_t>(value)] < 1150);
    }
    CHECK(longestRun(drawn) > 1);

    SECTION("the guard applies to it too, and cannot make it stall") {
        config.noRepeatWithin = 1;
        Generator guarded(config);
        CHECK(longestRun(draw(guarded, 4000)) == 1);
    }

    SECTION("a range of one is not a stall either") {
        // Nothing to avoid, so the guard has to give up rather than try forever. The
        // configuration says as much: one distinct value can carry no guard at all.
        config.low = 7;
        config.high = 7;
        config.noRepeatWithin = 4;
        Generator single(config);
        CHECK(single.config().noRepeatWithin == 0);
        CHECK(draw(single, 100) == std::vector<std::int32_t>(100, 7));
    }
}

TEST_CASE("cycle goes round its range in order", "[trigger][generator]") {
    Generator::Config config;
    config.kind = GeneratorKind::Cycle;
    config.low = 3;
    config.high = 6;
    Generator generator(config);
    CHECK(draw(generator, 10) == std::vector<std::int32_t>{3, 4, 5, 6, 3, 4, 5, 6, 3, 4});

    SECTION("and reset puts it back at the start") {
        generator.reset();
        CHECK(draw(generator, 2) == std::vector<std::int32_t>{3, 4});
    }
}

TEST_CASE("weighted draws in proportion, and survives weights that say nothing",
          "[trigger][generator]") {
    Generator::Config config;
    config.kind = GeneratorKind::Weighted;
    config.noRepeatWithin = 0; // the point here is the proportion, not the spacing
    config.choices = {
        {Value::ofInt(10), 1.0},
        {Value::ofInt(20), 3.0},
        {Value::ofInt(30), 0.0}, // zero means never
    };
    Generator generator(config);
    CHECK(generator.choiceCount() == 3);

    std::vector<std::int32_t> drawn = draw(generator, 4000);
    const auto tens = std::count(drawn.begin(), drawn.end(), 10);
    const auto twenties = std::count(drawn.begin(), drawn.end(), 20);
    const auto thirties = std::count(drawn.begin(), drawn.end(), 30);
    CHECK(thirties == 0);
    CHECK(tens + twenties == 4000);
    // 1:3, so a quarter and three quarters. Wide enough never to flake.
    CHECK(tens > 850);
    CHECK(tens < 1150);
    CHECK(twenties > 2850);
    CHECK(twenties < 3150);

    SECTION("all weights zero is a preference of none, not an error") {
        for (WeightedChoice& choice : config.choices) {
            choice.weight = 0.0;
        }
        Generator flat(config);
        drawn = draw(flat, 3000);
        for (const std::int32_t value : {10, 20, 30}) {
            INFO("value " << value);
            CHECK(std::count(drawn.begin(), drawn.end(), value) > 700);
        }
    }

    SECTION("a negative weight is treated as zero rather than subtracting") {
        config.choices = {{Value::ofInt(1), -5.0}, {Value::ofInt(2), 1.0}};
        Generator negative(config);
        CHECK(draw(negative, 200) == std::vector<std::int32_t>(200, 2));
    }

    SECTION("an empty list produces something harmless rather than nothing") {
        config.choices.clear();
        Generator empty(config);
        const Context context;
        CHECK(empty.next(context).asInt() == 0);
        CHECK(empty.choiceCount() == 0);
    }
}

TEST_CASE("fixed sends its literal, whatever type it is", "[trigger][generator]") {
    // §5.8: "Literal int, float, string or bool".
    Generator::Config config;
    config.kind = GeneratorKind::Fixed;
    const Context context;

    config.fixed = Value::ofInt(64);
    CHECK(Generator(config).next(context).asInt() == 64);

    config.fixed = Value::ofFloat(0.25f);
    CHECK(Generator(config).next(context).asFloat() == Approx(0.25f));

    config.fixed = Value::ofBool(true);
    CHECK(Generator(config).next(context).asBool());

    config.fixed = Value::ofText("intro");
    CHECK(Generator(config).next(context).text() == "intro");

    // One value, so no guard can apply to it — a Fixed generator repeats by definition and
    // must not be prevented from doing so.
    config.fixed = Value::ofInt(1);
    config.noRepeatWithin = 4;
    Generator fixed(config);
    CHECK(fixed.config().noRepeatWithin == 0);
    CHECK(draw(fixed, 20) == std::vector<std::int32_t>(20, 1));
}

TEST_CASE("a live generator reads the tracker rather than drawing", "[trigger][generator]") {
    Context context;
    context.bpm = 128.0;
    context.confidence = 0.82;
    context.meter = 4;
    context.beatInBar = 3;
    context.beats = 101;
    context.bars = 25;
    context.intensity = Intensity::Intense;

    Generator::Config config;
    config.kind = GeneratorKind::Live;
    const auto live = [&](LiveSource source) {
        config.source = source;
        return Generator(config).next(context);
    };

    CHECK(live(LiveSource::Bpm).asFloat() == Approx(128.0f));
    CHECK(live(LiveSource::Beat).asInt() == 101);
    CHECK(live(LiveSource::BeatInBar).asInt() == 3);
    CHECK(live(LiveSource::Bar).asInt() == 25);
    CHECK(live(LiveSource::Confidence).asFloat() == Approx(0.82f));
    CHECK(live(LiveSource::Meter).asInt() == 4);
    CHECK(live(LiveSource::Intensity).asInt() == 2); // §5.6's wire value for "intense"

    SECTION("BPM normalised to the host's range, which is the host's and not ours") {
        // §5.6, verified against Resolume: "/composition/tempocontroller/tempo — float,
        // normalised 0-1 across 20-500 BPM". The range is configurable because it belongs
        // to the host; hardcoding these two numbers would make the generator Resolume's.
        config.source = LiveSource::BpmNormalised;
        CHECK(Generator(config).next(context).asFloat() == Approx((128.0f - 20.0f) / 480.0f));

        config.normaliseLow = 60.0;
        config.normaliseHigh = 180.0;
        CHECK(Generator(config).next(context).asFloat() == Approx((128.0f - 60.0f) / 120.0f));

        // Outside the host's range it clamps: a host told 1.4 does something undefined,
        // and every host understands the end of its own range.
        context.bpm = 220.0;
        CHECK(Generator(config).next(context).asFloat() == Approx(1.0f));
        context.bpm = 30.0;
        CHECK(Generator(config).next(context).asFloat() == Approx(0.0f));
    }

    SECTION("an inverted host range falls back rather than dividing by nothing") {
        config.source = LiveSource::BpmNormalised;
        config.normaliseLow = 200.0;
        config.normaliseHigh = 100.0;
        const Generator generator(config);
        CHECK(generator.config().normaliseLow == Approx(20.0));
        CHECK(generator.config().normaliseHigh == Approx(500.0));
    }

    SECTION("nothing tracked yet is published as nothing, not as a guess") {
        const Context idle;
        config.source = LiveSource::Meter;
        CHECK(Generator(config).next(idle).asInt() == 0); // never 4; §5.5's rule
        config.source = LiveSource::Bpm;
        CHECK(Generator(config).next(idle).asFloat() == Approx(0.0f));
    }
}

TEST_CASE("a configuration is clamped to something usable, never refused", "[trigger][generator]") {
    // `settings::load` is documented never to fail — "settings that cannot be parsed must
    // not be the reason an app will not open" — so a rule read from a file with a nonsense
    // range has to come out usable. `config()` is what a UI shows back, so it has to say
    // what was accepted rather than what was typed.
    Generator::Config config;

    SECTION("a range given backwards is read the way round it was meant") {
        config.low = 9;
        config.high = 2;
        const Generator generator(config);
        CHECK(generator.config().low == 2);
        CHECK(generator.config().high == 9);
        CHECK(generator.choiceCount() == 8);
    }

    SECTION("a range too wide to hold a bag is cut from the top") {
        config.low = 1;
        config.high = 1000000;
        const Generator generator(config);
        CHECK(generator.config().low == 1);
        CHECK(generator.config().high == Generator::kMaxRangeSize);
    }

    SECTION("a range spanning the whole of int32 does not overflow while being measured") {
        config.low = -2147483647 - 1;
        config.high = 2147483647;
        Generator generator(config);
        CHECK(generator.choiceCount() == Generator::kMaxRangeSize);
        CHECK(draw(generator, 32).size() == 32);
    }

    SECTION("a guard longer than anything can remember is cut to what it can") {
        config.low = 1;
        config.high = 4096;
        config.noRepeatWithin = 5000;
        const Generator generator(config);
        CHECK(generator.config().noRepeatWithin == Generator::kMaxNoRepeat);
    }
}

TEST_CASE("the same seed gives the same sequence and different seeds do not",
          "[trigger][generator]") {
    // Two rules in a preset must not fire the same clip as each other, which means the rule
    // engine has to hand out distinct seeds — and that only means anything if the seed
    // decides the stream. Reproducibility is also what lets the tests above state what
    // Shuffle does rather than sampling it.
    Generator::Config config;
    config.low = 1;
    config.high = 16;
    config.seed = 7;

    Generator first(config);
    Generator second(config);
    const std::vector<std::int32_t> a = draw(first, 64);
    CHECK(draw(second, 64) == a);

    config.seed = 8;
    Generator other(config);
    CHECK(draw(other, 64) != a);

    SECTION("and reset returns to the beginning of it") {
        first.reset();
        CHECK(draw(first, 64) == a);
    }
}

TEST_CASE("every generator kind and live source has a name that reads back",
          "[trigger][generator]") {
    // The names go into a preset file (Q7) and the labels into §5.9's dropdown, so both
    // have to be complete: a kind added without one would serialise as an empty string and
    // load as something else.
    for (const GeneratorKind kind : takt4::trigger::kGeneratorKinds) {
        INFO("kind " << static_cast<int>(kind));
        CHECK_FALSE(takt4::trigger::nameOf(kind).empty());
        CHECK_FALSE(takt4::trigger::labelOf(kind).empty());
        CHECK(takt4::trigger::generatorKindOf(takt4::trigger::nameOf(kind)) == kind);
    }
    for (const LiveSource source : takt4::trigger::kLiveSources) {
        INFO("source " << static_cast<int>(source));
        CHECK_FALSE(takt4::trigger::nameOf(source).empty());
        CHECK_FALSE(takt4::trigger::labelOf(source).empty());
        CHECK(takt4::trigger::liveSourceOf(takt4::trigger::nameOf(source)) == source);
    }
    CHECK_FALSE(takt4::trigger::generatorKindOf("nonsense").has_value());
    CHECK_FALSE(takt4::trigger::liveSourceOf("").has_value());

    // Intensity is `features::Intensity` — it lives with the classifier that computes it,
    // and `trigger` aliases it so a rule reads as one thing.
    for (const Intensity intensity : {Intensity::Calm, Intensity::Normal, Intensity::Intense}) {
        CHECK_FALSE(takt4::features::labelOf(intensity).empty());
    }
}
