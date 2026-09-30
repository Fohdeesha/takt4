#include "core/trigger/rule.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
using takt4::features::Intensity;
using takt4::trigger::Context;
using takt4::trigger::DelayUnit;
using takt4::trigger::FollowUp;
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

/// A release — the fired message again with a different value — after a delay in ms.
FollowUp releaseAfterMs(std::int32_t value, double milliseconds) {
    FollowUp entry;
    entry.value = Value::ofInt(value);
    entry.unit = DelayUnit::Milliseconds;
    entry.delaySeconds = milliseconds / 1000.0;
    return entry;
}

/// What a rule owes after `fired`, as the messages alone. `Rule::followUpsFor` pairs each
/// with the index of the `FollowUp` that produced it — which the scheduler needs and a test
/// asserting what went on the wire does not.
std::vector<Message> followUps(const Rule& rule, const Message& fired) {
    std::vector<std::pair<std::size_t, Message>> owed;
    // The context is read only by the DMX kinds, whose follow-ups carry a duration as well as
    // a delay. A default one is the right thing for the OSC and MIDI cases here.
    rule.followUpsFor(takt4::trigger::Context{}, fired, owed);
    std::vector<Message> messages;
    messages.reserve(owed.size());
    for (auto& [index, message] : owed) {
        (void)index;
        messages.push_back(std::move(message));
    }
    return messages;
}

/// §5.6's own Resolume example: "/composition/layers/{L}/clips/{C}/connect — int 1 (press)
/// then 0 (release)".
Rule::Config resolumeClip() {
    Rule::Config config;
    config.id = "clip";
    config.address = "/composition/layers/{L}/clips/{C}/connect";
    config.segments = {fixedAt(2), fixedAt(5)};
    config.value = fixedAt(1);
    config.followUps.push_back(releaseAfterMs(0, 50));
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
    const std::vector<Message> follow = followUps(rule, *fired);
    REQUIRE(follow.size() == 1);
    CHECK(follow[0].address == fired->address);
    CHECK(follow[0].argument.asInt() == 0);

    SECTION("a rule with no follow-up has none") {
        Rule::Config config = resolumeClip();
        config.followUps.clear();
        Rule once(config);
        const std::optional<Message> only = once.fire(context);
        REQUIRE(only.has_value());
        CHECK(followUps(once, *only).empty());
    }

    SECTION("an OSC rule cannot be given a MIDI follow-up") {
        // A MIDI follow-up to an OSC rule has no channel or note to inherit, so it is
        // skipped rather than sent as whatever the defaults happen to be. The editor does
        // not offer one; a hand-edited preset can still hold one.
        Rule::Config config = resolumeClip();
        config.followUps.front().kind = Message::Kind::MidiNote;
        Rule mixed(config);
        const std::optional<Message> only = mixed.fire(context);
        REQUIRE(only.has_value());
        CHECK(followUps(mixed, *only).empty());
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

    SECTION("a follow-up on a note is a real note off, on the note that fired") {
        // **Not the same note on with velocity zero.** That convention is widely understood
        // and not universal: the operator's laser controller holds its clip until 0x80
        // arrives, so a rig built on it never released. See `trigger::Message::Kind`.
        config.number = fixedAt(36);
        config.value = fixedAt(100);
        config.followUps.push_back(releaseAfterMs(0, 50));
        Rule pressed(config);
        const std::optional<Message> press = pressed.fire(context);
        REQUIRE(press.has_value());
        CHECK(press->number == 36);
        CHECK(press->value == 100);
        const std::vector<Message> release = followUps(pressed, *press);
        REQUIRE(release.size() == 1);
        CHECK(release[0].kind == Message::Kind::MidiNoteOff);
        CHECK(release[0].number == 36); // the same note, or it is not a release
        CHECK(release[0].channel == 10);
        CHECK(release[0].value == 0);
    }

    SECTION("a shuffled note is released on the note that was drawn") {
        // The whole point of a release inheriting the fired message's number: nobody can
        // type in advance which note a shuffle will pick.
        config.number.kind = GeneratorKind::Shuffle;
        config.number.low = 36;
        config.number.high = 43;
        config.value = fixedAt(100);
        config.followUps.push_back(releaseAfterMs(0, 80));
        Rule shuffled(config);
        const std::optional<Message> press = shuffled.fire(context);
        REQUIRE(press.has_value());
        const std::vector<Message> release = followUps(shuffled, *press);
        REQUIRE(release.size() == 1);
        CHECK(release[0].number == press->number);
    }

    SECTION("a rule can send several things, each timed from the fire") {
        // §5.8's follow-up grown into a list: a note held for a beat, and a CC a beat after
        // that. Every delay is measured from the fire, never from the entry above it.
        config.number = fixedAt(48);
        config.value = fixedAt(100);
        FollowUp release;
        release.unit = DelayUnit::Beats;
        release.delayBeats = 1.0;
        FollowUp cc;
        cc.kind = Message::Kind::MidiCc;
        cc.number = 21;
        cc.value = Value::ofInt(64);
        cc.unit = DelayUnit::Beats;
        cc.delayBeats = 2.0;
        config.followUps = {release, cc};
        Rule several(config);
        const std::optional<Message> press = several.fire(context);
        REQUIRE(press.has_value());

        const std::vector<Message> owed = followUps(several, *press);
        REQUIRE(owed.size() == 2);
        CHECK(owed[0].kind == Message::Kind::MidiNoteOff);
        CHECK(owed[0].number == 48); // the release inherits the note that fired
        CHECK(owed[1].kind == Message::Kind::MidiCc);
        CHECK(owed[1].number == 21); // an explicit kind brings its own
        CHECK(owed[1].value == 64);
        CHECK(owed[1].channel == 10); // and keeps the channel the note went to

        Context playing;
        playing.bpm = 120.0; // a beat is half a second
        CHECK(several.followUpDelay(playing, 0) == Approx(0.5));
        CHECK(several.followUpDelay(playing, 1) == Approx(1.0));
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
    config.conditionsOn = true; // the stage switched on: see the test after this one for off
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

TEST_CASE("the ONLY IF stage applies only while it is switched on", "[trigger][rule]") {
    // The tick on the editor's B heading (HANDOFF §0.5, locked 2026-09-30). Off — as a new rule
    // starts — the rule fires every time its trigger comes round, whatever the conditions hold;
    // they are kept, set for later, and switching B on is what makes them apply.
    Rule::Config config;
    config.id = "r";
    config.address = "/a";
    config.conditions.minConfidence = 0.9;
    config.conditions.intensities = {false, false, true};
    config.conditions.minBpm = 150.0;
    config.conditions.maxBpm = 160.0;
    config.conditions.probability = 0.0;
    Context context;
    context.confidence = 0.1;
    context.intensity = Intensity::Calm;
    context.bpm = 120.0;
    REQUIRE(config.conditions.excludesAnything());

    CHECK_FALSE(Rule::Config{}.conditionsOn); // a rule starts with B off
    CHECK(Rule(config).conditionsHold(context));

    config.conditionsOn = true;
    CHECK_FALSE(Rule(config).conditionsHold(context));

    SECTION("and each of the four is what excludes it, once B is on") {
        // One at a time back to admitting, so each is seen to be the one that refused.
        config.conditions.probability = 1.0;
        config.conditions.minConfidence = 0.0;
        config.conditions.intensities = {true, true, true};
        CHECK_FALSE(Rule(config).conditionsHold(context)); // the BPM range still refuses
        config.conditions.minBpm = 0.0;
        config.conditions.maxBpm = 1000.0;
        CHECK(Rule(config).conditionsHold(context));
        CHECK_FALSE(config.conditions.excludesAnything());
    }
}

TEST_CASE("a cooldown holds a trigger that comes in bursts, and no other", "[trigger][rule]") {
    // The cooldown is A's since 2026-09-30 (HANDOFF §0.5), and the editor shows it switched off
    // for beats, bars, the downbeat and a Euclidean pattern — so the engine has to ignore it
    // there, or a control shown switched off would still be thinning a pattern.
    Rule::Config config;
    config.id = "r";
    config.address = "/a";
    config.cooldownSeconds = 0.5;
    Context context;

    for (const Trigger bursty : {Trigger::Onset, Trigger::TempoChange, Trigger::LockChange,
                                 Trigger::IntensityChange, Trigger::Manual}) {
        INFO("trigger " << takt4::trigger::labelOf(bursty));
        CHECK(takt4::trigger::takesCooldown(bursty));
        config.trigger = bursty;
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

    for (const Trigger steady : {Trigger::Beat, Trigger::Bar, Trigger::Downbeat, Trigger::Euclid}) {
        INFO("trigger " << takt4::trigger::labelOf(steady));
        CHECK_FALSE(takt4::trigger::takesCooldown(steady));
        config.trigger = steady;
        Rule steadyRule(config);
        context.now = 10.0;
        (void)steadyRule.fire(context);
        context.now = 10.1;
        CHECK(steadyRule.conditionsHold(context)); // kept, and ignored
    }

    SECTION("whether or not the ONLY IF stage is switched on") {
        config.trigger = Trigger::Onset;
        for (const bool on : {false, true}) {
            config.conditionsOn = on;
            Rule cooled(config);
            context.now = 10.0;
            (void)cooled.fire(context);
            context.now = 10.2;
            CHECK_FALSE(cooled.conditionsHold(context));
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

TEST_CASE("what a fire records is what the kind actually sends", "[trigger][rule]") {
    // `lastSlots` is paired with §5.9's generator chips **by position**, and the editor
    // builds a chip only for a field the kind puts on the wire — `RulesController::
    // slotConfig` indexes them the same way. One extra entry moves every chip after it onto
    // the wrong generator, which is not a cosmetic difference: it is a readout naming a
    // number that was never sent.
    Context context;

    SECTION("a pitch bend is all value and has no number") {
        // The one that was wrong. `sendsNumber` is false for a bend, so the editor draws a
        // single chip — the bend — and slot 0 is it. Recording the number generator's draw
        // first put a value nothing had sent under that chip: a bend of 12000 read as 9.
        Rule::Config config;
        config.id = "bend";
        config.sendKind = Message::Kind::MidiPitchBend;
        config.number = fixedAt(9);    // never sent: a bend is two data bytes of value
        config.value = fixedAt(12000); // 14-bit, so far past a data byte's ceiling
        Rule rule(config);
        REQUIRE(rule.valid());

        const std::optional<Message> sent = rule.fire(context);
        REQUIRE(sent.has_value());
        CHECK(sent->value == 12000);
        REQUIRE(rule.lastSlots().size() == 1);
        CHECK(rule.lastSlots()[0].asInt() == 12000);
    }

    SECTION("a program change is all number and has no value") {
        Rule::Config config;
        config.id = "program";
        config.sendKind = Message::Kind::MidiProgramChange;
        config.number = fixedAt(42);
        config.value = fixedAt(100); // never sent: it is a two-byte message
        Rule rule(config);
        REQUIRE(rule.valid());

        const std::optional<Message> sent = rule.fire(context);
        REQUIRE(sent.has_value());
        CHECK(sent->number == 42);
        REQUIRE(rule.lastSlots().size() == 1);
        CHECK(rule.lastSlots()[0].asInt() == 42);
    }

    SECTION("a note has both, in the order the chips are drawn") {
        Rule::Config config;
        config.id = "note";
        config.sendKind = Message::Kind::MidiNote;
        config.number = fixedAt(36);
        config.value = fixedAt(100);
        Rule rule(config);
        const std::optional<Message> sent = rule.fire(context);
        REQUIRE(sent.has_value());
        REQUIRE(rule.lastSlots().size() == 2);
        CHECK(rule.lastSlots()[0].asInt() == 36);
        CHECK(rule.lastSlots()[1].asInt() == 100);
    }

    SECTION("and an OSC rule is its segments, then its argument") {
        Rule::Config config;
        config.id = "clip";
        config.address = "/deck/{a}/{b}";
        config.segments = {fixedAt(2), fixedAt(5)};
        config.value = fixedAt(1);
        Rule rule(config);
        const std::optional<Message> sent = rule.fire(context);
        REQUIRE(sent.has_value());
        REQUIRE(rule.lastSlots().size() == 3);
        CHECK(rule.lastSlots()[0].asInt() == 2);
        CHECK(rule.lastSlots()[1].asInt() == 5);
        CHECK(rule.lastSlots()[2].asInt() == 1);

        // And nothing at all when the address takes no argument, which is the other half of
        // `sendValue` and the only case where an OSC rule has fewer chips than placeholders
        // plus one.
        config.sendValue = false;
        Rule bare(config);
        REQUIRE(bare.fire(context).has_value());
        CHECK(bare.lastSlots().size() == 2);
    }
}

TEST_CASE("a MIDI rule waits for a number somebody chose", "[trigger][rule]") {
    // The audit's C7: a rule switched to MIDI CC fired CC 1 to 8 at a value of one on every bar,
    // to every MIDI output — CC 7 at one is a synth's volume gone. The operator's call: the rule
    // stays armed, does nothing, and says what it is waiting for.
    Rule::Config config;
    config.id = "stab";
    config.number = fixedAt(60);
    config.numberChosen = false;
    Context context;
    context.bpm = 120.0;
    context.confidence = 1.0;
    context.locked = true;

    const std::pair<Message::Kind, const char*> kinds[] = {
        {Message::Kind::MidiNote, "choose a note number"},
        {Message::Kind::MidiNoteOff, "choose a note number"},
        {Message::Kind::MidiCc, "choose a controller number"},
        {Message::Kind::MidiProgramChange, "choose a program number"},
    };
    for (const auto& [kind, problem] : kinds) {
        config.sendKind = kind;
        Rule waiting(config);
        INFO(problem);
        CHECK_FALSE(waiting.valid());
        CHECK(waiting.problem() == problem);

        config.numberChosen = true;
        Rule chosen(config);
        CHECK(chosen.valid());
        REQUIRE(chosen.fire(context).has_value());
        CHECK(chosen.fire(context)->number == 60);
        config.numberChosen = false;
    }

    SECTION("pitch bend has no number to wait for") {
        config.sendKind = Message::Kind::MidiPitchBend;
        CHECK(Rule(config).valid());
    }
    SECTION("and nor do OSC and lighting, whatever the flag says") {
        config.sendKind = Message::Kind::Osc;
        config.address = "/go";
        CHECK(Rule(config).valid());
    }
}

TEST_CASE("a note is a note on or off, and nothing else is the same number", "[trigger][rule]") {
    using takt4::trigger::sameNumber;
    CHECK(sameNumber(Message::Kind::MidiNote, Message::Kind::MidiNoteOff));
    CHECK(sameNumber(Message::Kind::MidiNoteOff, Message::Kind::MidiNote));
    CHECK(sameNumber(Message::Kind::MidiCc, Message::Kind::MidiCc));
    CHECK_FALSE(sameNumber(Message::Kind::MidiNote, Message::Kind::MidiCc));
    CHECK_FALSE(sameNumber(Message::Kind::MidiCc, Message::Kind::MidiProgramChange));
    CHECK_FALSE(sameNumber(Message::Kind::Osc, Message::Kind::MidiNote));
    CHECK_FALSE(sameNumber(Message::Kind::MidiPitchBend, Message::Kind::MidiPitchBend));
    CHECK_FALSE(sameNumber(Message::Kind::Dmx, Message::Kind::MidiCc));
}

TEST_CASE("the slot layout is the order a fire records its slots in, for every kind",
          "[trigger]") {
    // The audit's Low items. The editor walked the order of a rule's generators twice — once
    // to build its chips, once to find the generator a chip edits — beside `Rule::fire`'s own,
    // held together by comments saying the three must not drift apart. There is one walk now,
    // `slotLayout`, and this holds it to what a fire records: every slot given its own number,
    // fired, and each recorded value checked against the slot it came from.
    std::vector<Rule::Config> configs;
    Rule::Config osc;
    osc.address = "/a/{}/b/{}";
    osc.segments = {Generator::Config{}, Generator::Config{}};
    osc.sendValue = true;
    configs.push_back(osc);
    osc.sendValue = false;
    configs.push_back(osc);
    for (const Message::Kind kind : {Message::Kind::MidiNote, Message::Kind::MidiCc,
                                     Message::Kind::MidiProgramChange,
                                     Message::Kind::MidiPitchBend}) {
        Rule::Config midi;
        midi.sendKind = kind;
        midi.numberChosen = true;
        configs.push_back(midi);
    }
    for (const takt4::dmx::EffectKind effect : takt4::dmx::kEffectKinds) {
        for (const takt4::trigger::ColorMode mode : takt4::trigger::kColorModes) {
            Rule::Config lighting;
            lighting.sendKind = Message::Kind::Dmx;
            lighting.dmx.effect = effect;
            lighting.dmx.colorMode = mode;
            lighting.dmx.fixtures = {"par"}; // aimed at something, or it is not valid
            configs.push_back(lighting);
        }
    }

    const std::string colorText = "#0a0b0c";
    const std::string colorRecorded =
        takt4::dmx::formatColor(takt4::dmx::parseColor(colorText).value());
    std::size_t checked = 0;
    for (std::size_t c = 0; c < configs.size(); ++c) {
        Rule::Config config = configs[c];
        const std::vector<takt4::trigger::Slot> layout = takt4::trigger::slotLayout(config);
        std::vector<Value> expected;
        for (std::size_t i = 0; i < layout.size(); ++i) {
            Generator::Config& generator = takt4::trigger::slotGenerator(config, layout[i]);
            generator = Generator::Config{};
            generator.kind = GeneratorKind::Fixed;
            if (layout[i].role == takt4::trigger::SlotRole::Color) {
                generator.fixed = Value::ofText(colorText);
                expected.push_back(Value::ofText(colorRecorded));
            } else {
                // Distinct, and inside every range a slot clamps to: a percentage, a DMX byte,
                // a MIDI number or value.
                const int number = 11 + static_cast<int>(i) * 7;
                generator.fixed = Value::ofInt(number);
                expected.push_back(Value::ofInt(number));
            }
        }
        Rule rule(config);
        INFO("config " << c << ": " << rule.problem());
        REQUIRE(rule.valid());
        Context context;
        REQUIRE(rule.fire(context).has_value());
        const std::span<const Value> recorded = rule.lastSlots();
        REQUIRE(recorded.size() == layout.size());
        for (std::size_t i = 0; i < layout.size(); ++i) {
            INFO("slot " << i);
            CHECK(recorded[i] == expected[i]);
        }
        checked += layout.size();
    }
    // The walk covered something: segments, values, numbers, levels, colors and positions.
    CHECK(checked > 30);
}
