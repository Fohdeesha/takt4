#include "core/trigger/rule.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using Catch::Approx;
using takt4::features::Intensity;
using takt4::trigger::Context;
using takt4::trigger::Generator;
using takt4::trigger::GeneratorKind;
using takt4::trigger::Message;
using takt4::trigger::Rule;
using takt4::trigger::Trigger;
using takt4::trigger::Value;

namespace {

/// A generator that always produces `value`, for a test that is about the rule rather than
/// about the draw.
Generator::Config fixedAt(std::int32_t value) {
    Generator::Config config;
    config.kind = GeneratorKind::Fixed;
    config.fixed = Value::ofInt(value);
    return config;
}

/// §5.6's own Resolume example: "/composition/layers/{L}/clips/{C}/connect — int 1 (press)
/// then 0 (release)".
Rule::Config resolumeClip() {
    Rule::Config config;
    config.id = "clip";
    config.address = "/composition/layers/{L}/clips/{C}/connect";
    config.segments = {fixedAt(2), fixedAt(5)};
    config.value = fixedAt(1);
    config.followUp = true;
    config.followUpValue = Value::ofInt(0);
    config.followUpDelaySeconds = 0.05;
    return config;
}

} // namespace

TEST_CASE("an address template is filled by position", "[trigger][rule]") {
    // §5.9: the address is "an internal representation the user never types", assembled by
    // clicking. §5.6 writes the placeholders as {L} and {C}; what is inside the braces is
    // documentation, and the position is what binds.
    std::string out;
    const std::vector<Value> two{Value::ofInt(2), Value::ofInt(5)};

    CHECK(takt4::trigger::countPlaceholders("/composition/layers/{L}/clips/{C}/connect") == 2);
    REQUIRE(takt4::trigger::fillAddress("/composition/layers/{L}/clips/{C}/connect", two.data(),
                                        two.size(), out));
    CHECK(out == "/composition/layers/2/clips/5/connect");

    SECTION("an address with no placeholders is just an address") {
        CHECK(takt4::trigger::countPlaceholders("/composition/tempocontroller/resync") == 0);
        REQUIRE(
            takt4::trigger::fillAddress("/composition/tempocontroller/resync", nullptr, 0, out));
        CHECK(out == "/composition/tempocontroller/resync");
    }

    SECTION("text and adjacent placeholders both work") {
        const std::vector<Value> named{Value::ofText("intro"), Value::ofInt(7)};
        REQUIRE(takt4::trigger::fillAddress("/deck/{a}{b}", named.data(), named.size(), out));
        CHECK(out == "/deck/intro7");
    }

    SECTION("the wrong number of values is a refusal, not a guess") {
        CHECK_FALSE(takt4::trigger::fillAddress("/a/{x}/b", two.data(), two.size(), out));
        CHECK_FALSE(takt4::trigger::fillAddress("/a/{x}/{y}", two.data(), 1, out));
    }

    SECTION("an unbalanced brace is caught by OSC's own rules") {
        // '{' and '}' are reserved in OSC 1.0, so whatever is left of a malformed template
        // survives into the result and is rejected there. Nothing has to check twice.
        CHECK(takt4::trigger::countPlaceholders("/a/{x") == 0);
        CHECK_FALSE(takt4::trigger::fillAddress("/a/{x", nullptr, 0, out));
        CHECK_FALSE(takt4::trigger::fillAddress("/a/x}", nullptr, 0, out));
    }

    SECTION("a value that would make the address illegal is refused at fill time") {
        // The one failure validating a template cannot rule out: it depends on what the
        // generator produced. A '/' would silently move the message to another address, and
        // a space is not an OSC address at all.
        const std::vector<Value> slash{Value::ofText("a/b")};
        CHECK_FALSE(takt4::trigger::fillAddress("/deck/{x}", slash.data(), 1, out));
        const std::vector<Value> space{Value::ofText("a b")};
        CHECK_FALSE(takt4::trigger::fillAddress("/deck/{x}", space.data(), 1, out));
    }

    SECTION("a template that is not an address at all is refused") {
        CHECK_FALSE(takt4::trigger::fillAddress("composition/{x}", two.data(), 1, out));
        CHECK_FALSE(takt4::trigger::fillAddress("", nullptr, 0, out));
    }
}

// The name stays ASCII: ctest passes a test's name back to the binary as a command-line
// argument, and a non-ASCII character in one breaks that test on Windows and nowhere else.
// See NEXT-SESSION.md's §6. Every "5.6" below is a section of HANDOFF.md.
TEST_CASE("a rule sends what the Resolume example in 5.6 describes", "[trigger][rule]") {
    Rule rule(resolumeClip());
    REQUIRE(rule.valid());
    Context context;
    context.now = 10.0;

    const std::optional<Message> fired = rule.fire(context);
    REQUIRE(fired.has_value());
    CHECK(fired->kind == Message::Kind::Osc);
    CHECK(fired->address == "/composition/layers/2/clips/5/connect");
    CHECK(fired->hasArgument);
    CHECK(fired->argument.asInt() == 1);
    CHECK(rule.fires() == 1);
    CHECK(rule.lastFired() == Approx(10.0));

    // "then 0 (release)" — the same address, a different value.
    const std::optional<Message> follow = rule.followUpFor(*fired);
    REQUIRE(follow.has_value());
    CHECK(follow->address == fired->address);
    CHECK(follow->argument.asInt() == 0);

    SECTION("a rule with no follow-up has none") {
        Rule::Config config = resolumeClip();
        config.followUp = false;
        Rule once(config);
        const std::optional<Message> only = once.fire(context);
        REQUIRE(only.has_value());
        CHECK_FALSE(once.followUpFor(*only).has_value());
    }

    SECTION("an address that takes no argument sends none") {
        Rule::Config config;
        config.id = "resync";
        config.address = "/composition/tempocontroller/resync";
        config.sendValue = false;
        Rule bare(config);
        REQUIRE(bare.valid());
        const std::optional<Message> message = bare.fire(context);
        REQUIRE(message.has_value());
        CHECK_FALSE(message->hasArgument);
    }
}

TEST_CASE("a MIDI rule clamps to what MIDI can carry", "[trigger][rule]") {
    // §5.6: "Also send configurable note or CC messages on beat and downbeat for MIDI-learn
    // targets." A generator can produce anything; seven bits is what fits.
    Rule::Config config;
    config.id = "note";
    config.sendKind = Message::Kind::MidiNote;
    config.channel = 10;
    config.number = fixedAt(300);
    config.value = fixedAt(-4);
    Rule rule(config);
    REQUIRE(rule.valid());

    const Context context;
    const std::optional<Message> fired = rule.fire(context);
    REQUIRE(fired.has_value());
    CHECK(fired->kind == Message::Kind::MidiNote);
    CHECK(fired->channel == 10);
    CHECK(fired->number == 127); // clamped, not wrapped to 44
    CHECK(fired->value == 0);

    SECTION("a follow-up on a note is the release") {
        config.number = fixedAt(36);
        config.value = fixedAt(100);
        config.followUp = true;
        config.followUpValue = Value::ofInt(0);
        Rule pressed(config);
        const std::optional<Message> press = pressed.fire(context);
        REQUIRE(press.has_value());
        CHECK(press->number == 36);
        CHECK(press->value == 100);
        const std::optional<Message> release = pressed.followUpFor(*press);
        REQUIRE(release.has_value());
        CHECK(release->number == 36); // the same note, or it is not a release
        CHECK(release->channel == 10);
        CHECK(release->value == 0);
    }

    SECTION("a channel outside 1 to 16 is a rule that says why rather than one that sends") {
        config.channel = 0;
        const Rule bad(config);
        CHECK_FALSE(bad.valid());
        CHECK_FALSE(bad.problem().empty());
    }
}

TEST_CASE("an invalid rule says why and never fires", "[trigger][rule]") {
    // The opposite of Generator's clamp-everything policy, and deliberately: a generator
    // with a silly range sends a silly number, while a rule with a broken address would
    // send a *different address*, and that arriving at a lighting desk is worse than
    // nothing arriving at all.
    Rule::Config config;
    config.address = "/a/{x}/{y}";
    config.segments = {fixedAt(1)}; // one generator, two placeholders

    const Rule mismatched(config);
    CHECK_FALSE(mismatched.valid());
    CHECK(mismatched.problem().find("2") != std::string::npos);
    CHECK(mismatched.problem().find("1") != std::string::npos);

    SECTION("an empty address") {
        config.address.clear();
        config.segments.clear();
        const Rule blank(config);
        CHECK_FALSE(blank.valid());
    }

    SECTION("an address that is not one") {
        config.address = "composition/go";
        config.segments.clear();
        const Rule relative(config);
        CHECK_FALSE(relative.valid());
    }

    SECTION("an id that could not be addressed over OSC") {
        // §5.7 reaches a rule as `/<prefix>/ctl/rule/<id>/enable`, so the id has to survive
        // being one segment of that.
        config.address = "/a";
        config.segments.clear();
        for (const char* bad : {"", "two words", "with/slash", "star*"}) {
            INFO("id '" << bad << "'");
            config.id = bad;
            const Rule named(config);
            CHECK_FALSE(named.valid());
        }
        config.id = "clip-4";
        CHECK(Rule(config).valid());
    }
}

TEST_CASE("the ONLY IF stage excludes only what it was set to exclude", "[trigger][rule]") {
    // Every condition defaults to admitting everything, because an operator who adds a rule
    // and sees nothing happen cannot tell a broken rule from a condition they did not know
    // was set.
    Rule::Config config;
    config.id = "r";
    config.address = "/a";
    Rule open(config);
    Context context;
    CHECK(open.conditionsHold(context));

    SECTION("confidence above a threshold") {
        config.conditions.minConfidence = 0.7;
        Rule gated(config);
        context.confidence = 0.69;
        CHECK_FALSE(gated.conditionsHold(context));
        context.confidence = 0.7;
        CHECK(gated.conditionsHold(context));
    }

    SECTION("intensity in a set") {
        config.conditions.intensities = {false, false, true}; // intense only
        Rule loud(config);
        context.intensity = Intensity::Calm;
        CHECK_FALSE(loud.conditionsHold(context));
        context.intensity = Intensity::Normal;
        CHECK_FALSE(loud.conditionsHold(context));
        context.intensity = Intensity::Intense;
        CHECK(loud.conditionsHold(context));
    }

    SECTION("BPM in a range, inclusive at both ends") {
        config.conditions.minBpm = 120.0;
        config.conditions.maxBpm = 130.0;
        Rule ranged(config);
        context.bpm = 119.9;
        CHECK_FALSE(ranged.conditionsHold(context));
        context.bpm = 120.0;
        CHECK(ranged.conditionsHold(context));
        context.bpm = 130.0;
        CHECK(ranged.conditionsHold(context));
        context.bpm = 130.1;
        CHECK_FALSE(ranged.conditionsHold(context));
    }

    SECTION("a cooldown, measured from the last fire and not the last look") {
        config.conditions.cooldownSeconds = 0.5;
        Rule cooled(config);
        context.now = 10.0;
        REQUIRE(cooled.conditionsHold(context)); // never fired, so nothing is owed
        (void)cooled.fire(context);

        context.now = 10.4;
        CHECK_FALSE(cooled.conditionsHold(context));
        // Asking and being refused must not restart the clock, or a rule evaluated every
        // millisecond would never come off cooldown.
        context.now = 10.49;
        CHECK_FALSE(cooled.conditionsHold(context));
        context.now = 10.5;
        CHECK(cooled.conditionsHold(context));
    }

    SECTION("a probability, which is a proportion over many looks") {
        config.conditions.probability = 0.25;
        Rule chancy(config);
        int held = 0;
        for (int i = 0; i < 4000; ++i) {
            held += chancy.conditionsHold(context) ? 1 : 0;
        }
        CHECK(held > 900);
        CHECK(held < 1100);
    }

    SECTION("a probability of zero never holds and one always does") {
        config.conditions.probability = 0.0;
        Rule never(config);
        config.conditions.probability = 1.0;
        Rule always(config);
        for (int i = 0; i < 100; ++i) {
            CHECK_FALSE(never.conditionsHold(context));
            CHECK(always.conditionsHold(context));
        }
    }
}

TEST_CASE("a change trigger tells a change from a value it has already seen", "[trigger][rule]") {
    Rule::Config config;
    config.id = "r";
    config.address = "/a";
    config.trigger = Trigger::TempoChange;
    Rule rule(config);
    Context context;

    // Nothing seen yet is not a change: a rule must not fire on its first round merely for
    // existing.
    context.bpm = 128.0;
    CHECK_FALSE(rule.seesChange(context));
    CHECK_FALSE(rule.seesChange(context));

    // The published tempo is refined continuously once locked — the beat spacing resolves
    // it to a fraction of a BPM and it moves most beats — so an exact comparison would make
    // this trigger mean "every beat". The default tolerance is 2%.
    context.bpm = 128.4;
    CHECK_FALSE(rule.seesChange(context));
    context.bpm = 140.0;
    CHECK(rule.seesChange(context));
    CHECK_FALSE(rule.seesChange(context));

    SECTION("a slow drift is one change, not a stream of them") {
        // Compared against the tempo that last *counted* as a change rather than the last
        // one seen, so a hundred steps of a tenth of a percent are one crossing.
        Rule drifting(config);
        Context slow;
        slow.bpm = 100.0;
        REQUIRE_FALSE(drifting.seesChange(slow));
        int changes = 0;
        for (int step = 0; step < 25; ++step) {
            slow.bpm += 0.1; // 0.1%, well inside the tolerance
            changes += drifting.seesChange(slow) ? 1 : 0;
        }
        // 100 to 102.5 passes 2% of 100 once, at 102.1; the baseline moves there and the
        // rest of the drift is nowhere near 2% of *that*.
        CHECK(changes == 1);
        CHECK(slow.bpm == Approx(102.5));
    }

    SECTION("nothing tracked is not a tempo, and coming off it is not a change") {
        Rule fresh(config);
        Context idle;
        idle.bpm = 0.0;
        CHECK_FALSE(fresh.seesChange(idle));
        idle.bpm = 128.0;
        CHECK_FALSE(fresh.seesChange(idle)); // the first real value is a baseline
        idle.bpm = 128.0;
        CHECK_FALSE(fresh.seesChange(idle));
    }

    SECTION("lock and unlock are both changes") {
        config.trigger = Trigger::LockChange;
        Rule locked(config);
        Context state;
        state.locked = false;
        CHECK_FALSE(locked.seesChange(state)); // baseline
        CHECK_FALSE(locked.seesChange(state));
        state.locked = true;
        CHECK(locked.seesChange(state));
        CHECK_FALSE(locked.seesChange(state));
        state.locked = false;
        CHECK(locked.seesChange(state));
    }

    SECTION("intensity, in either direction") {
        config.trigger = Trigger::IntensityChange;
        Rule moody(config);
        Context state;
        CHECK_FALSE(moody.seesChange(state)); // baseline at Normal
        state.intensity = Intensity::Intense;
        CHECK(moody.seesChange(state));
        CHECK_FALSE(moody.seesChange(state));
        state.intensity = Intensity::Calm;
        CHECK(moody.seesChange(state));
    }
}

TEST_CASE("a rule's generators are its own", "[trigger][rule]") {
    // Two rules built from the same configuration must not fire the same clip as each
    // other, or a preset with four layers would light them all identically.
    Rule::Config config;
    config.id = "a";
    config.address = "/clip/{n}";
    Generator::Config shuffled;
    shuffled.low = 1;
    shuffled.high = 64;
    config.segments = {shuffled};
    config.seed = 1;

    Rule first(config);
    config.id = "b";
    config.seed = 2;
    Rule second(config);

    const Context context;
    std::vector<std::string> a;
    std::vector<std::string> b;
    for (int i = 0; i < 16; ++i) {
        a.push_back(first.fire(context)->address);
        b.push_back(second.fire(context)->address);
    }
    CHECK(a != b);

    SECTION("and the same seed is the same rule again, which is what makes a test possible") {
        config.id = "a";
        config.seed = 1;
        Rule again(config);
        std::vector<std::string> repeated;
        for (int i = 0; i < 16; ++i) {
            repeated.push_back(again.fire(context)->address);
        }
        CHECK(repeated == a);
    }

    SECTION("reset puts it back to the beginning") {
        first.reset();
        CHECK(first.fires() == 0);
        CHECK(first.fire(context)->address == a.front());
    }
}

TEST_CASE("every trigger has a name that reads back", "[trigger][rule]") {
    for (const Trigger trigger : takt4::trigger::kTriggers) {
        INFO("trigger " << static_cast<int>(trigger));
        CHECK_FALSE(takt4::trigger::nameOf(trigger).empty());
        CHECK_FALSE(takt4::trigger::labelOf(trigger).empty());
        CHECK(takt4::trigger::triggerOf(takt4::trigger::nameOf(trigger)) == trigger);
    }
    CHECK_FALSE(takt4::trigger::triggerOf("every-third-tuesday").has_value());
    // §5.8 spells two of the ten WHEN entries with an N, and only those two.
    CHECK(takt4::trigger::takesEvery(Trigger::Beat));
    CHECK(takt4::trigger::takesEvery(Trigger::Bar));
    CHECK_FALSE(takt4::trigger::takesEvery(Trigger::Downbeat));
    CHECK_FALSE(takt4::trigger::takesEvery(Trigger::Manual));
}
