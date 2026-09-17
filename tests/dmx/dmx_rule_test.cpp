// The seam between §5.8's rules and the lighting engine: a rule fires, an effect starts, and
// the levels move. Everything either side of that seam has tests of its own — this one is
// about the join, which is where routing by *fixture* rather than by output lives and where a
// musical duration becomes seconds.

#include "core/dmx/dmx_engine.hpp"
#include "core/dmx/fixture.hpp"
#include "core/output/rule_sink.hpp"
#include "core/output/transports.hpp"
#include "core/trigger/rule.hpp"
#include "core/trigger/trigger_engine.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

using Catch::Approx;
using takt4::dmx::Color;
using takt4::dmx::EffectKind;
using takt4::dmx::Fixture;
using takt4::dmx::Role;
using takt4::output::RuleSink;
using takt4::output::Transports;
using takt4::trigger::Context;
using takt4::trigger::Generator;
using takt4::trigger::GeneratorKind;
using takt4::trigger::Message;
using takt4::trigger::Rule;
using takt4::trigger::TriggerEngine;
using takt4::trigger::Value;

namespace {

Fixture rgb(std::string name, std::uint16_t address) {
    return takt4::dmx::fixtureFromMode(name, 1, 0, address);
}

Fixture head(std::string name, std::uint16_t address) {
    return takt4::dmx::fixtureFromMode(name, 6, 0, address);
}

Generator::Config fixedInt(std::int32_t value) {
    Generator::Config config;
    config.kind = GeneratorKind::Fixed;
    config.fixed = Value::ofInt(value);
    return config;
}

Generator::Config fixedText(std::string text) {
    Generator::Config config;
    config.kind = GeneratorKind::Fixed;
    config.fixed = Value::ofText(text);
    return config;
}

/// A rule that fades its fixtures to `level` over `beats`.
Rule::Config fadeRule(std::string id, std::vector<std::string> fixtures, int level, double beats) {
    Rule::Config config;
    config.id = std::move(id);
    config.sendKind = Message::Kind::Dmx;
    config.trigger = takt4::trigger::Trigger::Beat;
    config.dmx.fixtures = std::move(fixtures);
    config.dmx.effect = EffectKind::Level;
    config.dmx.role = Role::Dimmer;
    config.dmx.level = fixedInt(level);
    config.dmx.curve = takt4::dmx::Curve::Linear;
    config.dmx.unit = takt4::trigger::DelayUnit::Beats;
    config.dmx.durationBeats = beats;
    return config;
}

Context beatAt(std::uint64_t beats, std::uint32_t beatInBar, std::uint64_t bars, double now,
               double bpm = 120.0) {
    Context context;
    context.beats = beats;
    context.beatInBar = beatInBar;
    context.bars = bars;
    context.now = now;
    context.bpm = bpm;
    context.meter = 4;
    context.confidence = 1.0;
    context.locked = true;
    return context;
}

std::uint8_t at(const Transports& transports, int channel) {
    const std::span<const std::uint8_t> levels = transports.dmx().levels(0);
    REQUIRE(!levels.empty());
    return levels[static_cast<std::size_t>(channel) - 1];
}

} // namespace

TEST_CASE("a DMX rule reaches the fixtures it names and no others", "[dmx][trigger]") {
    Transports::Config config;
    config.patch = {rgb("wash L", 1), rgb("wash R", 4), head("head 1", 11)};
    config.patch[0].group = "washes";
    config.patch[1].group = "washes";
    Transports transports(config);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    Rule::Config rule = fadeRule("washes-up", {"washes"}, 255, 0.0);
    rule.dmx.effect = EffectKind::Color;
    rule.dmx.color = fixedText("#ff2040");
    engine.setRules({rule});
    // The runner is what normally resolves this; here the test plays that part, which is the
    // whole point of the mask being set from outside — see `Rule::fixtureMask`.
    engine.rule(0).setFixtureMask(takt4::dmx::resolveFixtures(transports.patch(), {"washes"}));
    sink.setNow(0.0);

    engine.onBeat(beatAt(1, 1, 1, 0.0));

    CHECK(at(transports, 1) == 255); // wash L red
    CHECK(at(transports, 2) == 32);
    CHECK(at(transports, 4) == 255); // wash R red
    // The head is in the patch, in the same universe, and was not named. Its color channels
    // start at 18 in the 16-bit map (address 11 + 7).
    CHECK(at(transports, 18) == 0);
    CHECK(sink.delivered() == 1);
}

TEST_CASE("a DMX rule that names no fixture is invalid rather than aimed at everything",
          "[dmx][trigger]") {
    // The asymmetry with `outputs` is deliberate and this is where it bites: an unrouted OSC
    // rule goes everywhere because that is harmless, and an unrouted lighting rule that went
    // everywhere would swing the whole rig the first time somebody added one.
    Rule::Config config = fadeRule("nowhere", {}, 255, 1.0);
    Rule rule(config);
    CHECK_FALSE(rule.valid());
    CHECK(rule.problem().find("fixture") != std::string::npos);
    CHECK(rule.fixtureMask() == 0);
}

TEST_CASE("a fade's duration is measured in the tempo that was playing", "[dmx][trigger]") {
    Transports::Config config;
    config.patch = {rgb("par", 1)};
    Transports transports(config);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    // Two beats at 120 BPM is one second.
    Rule::Config rule = fadeRule("up", {"par"}, 255, 2.0);
    rule.dmx.role = Role::Red;
    engine.setRules({rule});
    engine.rule(0).setFixtureMask(0b1);
    sink.setNow(10.0);
    engine.onBeat(beatAt(1, 1, 1, 10.0, 120.0));

    CHECK(at(transports, 1) == 0);
    transports.dmx().tick(10.5);
    CHECK(at(transports, 1) == 128);
    transports.dmx().tick(11.0);
    CHECK(at(transports, 1) == 255);

    SECTION("at half the tempo the same two beats take twice as long") {
        transports.dmx().reset();
        sink.setNow(20.0);
        engine.onBeat(beatAt(2, 2, 1, 20.0, 60.0));
        transports.dmx().tick(21.0);
        CHECK(at(transports, 1) == 128); // half way through two seconds
        transports.dmx().tick(22.0);
        CHECK(at(transports, 1) == 255);
    }
}

// ASCII in the name, like every other test in the suite: ctest passes a name back to the
// binary as a filter through the command line, and a character the console codepage cannot
// carry comes back mangled — so the test "fails" by matching nothing at all. It passes when
// run by wildcard, which is the giveaway.
TEST_CASE("fade in, then fade out: one rule with a follow-up", "[dmx][trigger]") {
    // The operator's own first example. The follow-up's *delay* is when it starts and its
    // *duration* is how long it runs, and the two are different numbers: "fade up over a beat,
    // then two beats later fade down over a beat".
    Transports::Config config;
    config.patch = {rgb("par", 1)};
    Transports transports(config);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    Rule::Config rule = fadeRule("breathe", {"par"}, 255, 1.0);
    rule.dmx.role = Role::Red;

    takt4::trigger::FollowUp down;
    down.unit = takt4::trigger::DelayUnit::Beats;
    down.delayBeats = 2.0; // starts two beats after the fire
    takt4::trigger::DmxFollow fade;
    fade.effect = EffectKind::Level;
    fade.curve = takt4::dmx::Curve::Linear;
    fade.unit = takt4::trigger::DelayUnit::Beats;
    fade.durationBeats = 1.0; // and runs for one
    down.dmx = fade;
    down.value = Value::ofInt(0);
    rule.followUps.push_back(down);

    engine.setRules({rule});
    engine.rule(0).setFixtureMask(0b1);
    sink.setNow(0.0);
    engine.onBeat(beatAt(1, 1, 1, 0.0, 120.0)); // a beat is 0.5 s

    transports.dmx().tick(0.5);
    CHECK(at(transports, 1) == 255); // up over one beat

    // The release is owed at one second (two beats) and `advance` is what drains it.
    sink.setNow(1.0);
    engine.advance(beatAt(1, 1, 1, 1.0, 120.0));
    transports.dmx().tick(1.25);
    CHECK(at(transports, 1) == 128); // half way down
    transports.dmx().tick(1.5);
    CHECK(at(transports, 1) == 0);
}

TEST_CASE("a release with no effect of its own dims what fired", "[dmx][trigger]") {
    Transports::Config config;
    config.patch = {rgb("par", 1)};
    Transports transports(config);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    Rule::Config rule = fadeRule("hit", {"par"}, 255, 0.0);
    rule.dmx.effect = EffectKind::Color;
    rule.dmx.color = fixedText("#ff8000");
    takt4::trigger::FollowUp release;
    release.unit = takt4::trigger::DelayUnit::Milliseconds;
    release.delaySeconds = 0.05;
    release.value = Value::ofInt(0);
    rule.followUps.push_back(release);

    engine.setRules({rule});
    engine.rule(0).setFixtureMask(0b1);
    sink.setNow(0.0);
    engine.onBeat(beatAt(1, 1, 1, 0.0, 120.0));
    CHECK(at(transports, 1) == 255);
    CHECK(at(transports, 2) == 128);

    sink.setNow(0.06);
    engine.advance(beatAt(1, 1, 1, 0.06, 120.0));
    transports.dmx().tick(0.06);
    // Black: the same color scaled to zero. A release of a light is a dim, which is what
    // "let go" means for something that is already lit.
    CHECK(at(transports, 1) == 0);
    CHECK(at(transports, 2) == 0);

    SECTION("a release at half brightness keeps the color and loses the level") {
        transports.dmx().reset();
        rule.followUps[0].value = Value::ofInt(128);
        engine.setRules({rule});
        engine.rule(0).setFixtureMask(0b1);
        sink.setNow(1.0);
        engine.onBeat(beatAt(1, 1, 1, 1.0, 120.0));
        sink.setNow(1.06);
        engine.advance(beatAt(1, 1, 1, 1.06, 120.0));
        transports.dmx().tick(1.06);
        CHECK(at(transports, 1) == 128); // 255 * 128/255
        CHECK(at(transports, 2) == 64);  // 128 * 128/255
    }
}

TEST_CASE("a release of a movement effect is skipped rather than invented", "[dmx][trigger]") {
    // There is no sensible "let go" of a pan. Sending the head home would be a gesture nobody
    // asked for, and re-firing the move would make one rule fight itself.
    Rule::Config config = fadeRule("swing", {"head"}, 255, 1.0);
    config.dmx.effect = EffectKind::Position;
    config.followUps.push_back(takt4::trigger::FollowUp{});
    Rule rule(config);

    Message fired;
    fired.kind = Message::Kind::Dmx;
    fired.payload.kind = EffectKind::Position;
    std::vector<std::pair<std::size_t, Message>> owed;
    rule.followUpsFor(Context{}, fired, owed);
    CHECK(owed.empty());
}

TEST_CASE("a muted rule runs and does not send", "[dmx][trigger]") {
    Transports::Config config;
    config.patch = {rgb("par", 1)};
    Transports transports(config);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    Rule::Config rule = fadeRule("pulse", {"par"}, 255, 0.0);
    rule.dmx.role = Role::Red;
    // A shuffle, so that "the generators advanced" is observable rather than asserted.
    rule.dmx.level.kind = GeneratorKind::Cycle;
    rule.dmx.level.pool = takt4::trigger::Pool::List;
    rule.dmx.level.values = {Value::ofInt(10), Value::ofInt(20), Value::ofInt(30)};
    engine.setRules({rule});
    engine.rule(0).setFixtureMask(0b1);

    sink.setNow(0.0);
    engine.onBeat(beatAt(1, 1, 1, 0.0));
    CHECK(at(transports, 1) == 10);

    engine.rule(0).setMuted(true);
    sink.setNow(1.0);
    engine.onBeat(beatAt(2, 2, 1, 1.0));
    CHECK(at(transports, 1) == 10); // nothing sent
    CHECK(engine.muted() == 1);

    // **The rule ran.** Unmuting rejoins the music where it is rather than restarting the
    // sequence, which is the entire difference from disabling it.
    engine.rule(0).setMuted(false);
    sink.setNow(2.0);
    engine.onBeat(beatAt(3, 3, 1, 2.0));
    CHECK(at(transports, 1) == 30); // 20 was drawn while muted

    SECTION("a disabled rule does not run, so it picks up where it left off") {
        engine.rule(0).setEnabled(false);
        sink.setNow(3.0);
        engine.onBeat(beatAt(4, 4, 1, 3.0));
        engine.rule(0).setEnabled(true);
        sink.setNow(4.0);
        engine.onBeat(beatAt(5, 1, 2, 4.0));
        CHECK(at(transports, 1) == 10); // back round the cycle, nothing consumed while off
    }
}

TEST_CASE("a muted rule's follow-ups are suppressed with the press", "[dmx][trigger]") {
    // A press that never left must not be followed by a release that does — on OSC that is a
    // clip switched off that was never switched on, and on DMX a fade to black out of nowhere.
    Transports::Config config;
    config.patch = {rgb("par", 1)};
    Transports transports(config);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    Rule::Config rule = fadeRule("hit", {"par"}, 255, 0.0);
    rule.dmx.role = Role::Red;
    takt4::trigger::FollowUp release;
    release.delaySeconds = 0.01;
    release.value = Value::ofInt(0);
    rule.followUps.push_back(release);
    engine.setRules({rule});
    engine.rule(0).setFixtureMask(0b1);

    // Lit by hand first, so a stray release would be visible as a change.
    sink.setNow(0.0);
    engine.onBeat(beatAt(1, 1, 1, 0.0));
    CHECK(at(transports, 1) == 255);
    sink.setNow(0.02);
    engine.advance(beatAt(1, 1, 1, 0.02));
    CHECK(at(transports, 1) == 0);

    engine.rule(0).setMuted(true);
    sink.setNow(1.0);
    engine.onBeat(beatAt(2, 2, 1, 1.0));
    CHECK(engine.pending() == 0); // nothing owed, because nothing was pressed
}

TEST_CASE("a rule's rate multiplies how often it fires", "[trigger]") {
    Transports::Config config;
    config.patch = {rgb("par", 1)};
    Transports transports(config);
    RuleSink sink(transports);

    /// Which of the first nine bars a rule on every `every` bars fires on, at `rate`.
    ///
    /// A fresh engine each time, because the clock may only ever go **forwards**: a rule's
    /// cooldown is measured against `Context::now`, so replaying bar 1 after bar 9 on one
    /// engine is a rule being asked about a moment it has already lived through, and it
    /// correctly declines. Real time does not do that; a test must not either.
    const auto barsThatFire = [&](std::uint32_t every, double rate) {
        TriggerEngine engine(sink);
        Rule::Config rule = fadeRule("bars", {"par"}, 255, 0.0);
        rule.trigger = takt4::trigger::Trigger::Bar;
        rule.every = every;
        rule.dmx.role = Role::Red;
        engine.setRules({rule});
        engine.rule(0).setFixtureMask(0b1);
        engine.rule(0).setRate(rate);

        std::vector<int> fired;
        for (int bar = 1; bar <= 9; ++bar) {
            const std::uint64_t before = engine.sent();
            sink.setNow(bar);
            engine.onBeat(beatAt(static_cast<std::uint64_t>(bar) * 4 - 3, 1,
                                 static_cast<std::uint64_t>(bar), bar));
            if (engine.sent() != before) {
                fired.push_back(bar);
            }
        }
        return fired;
    };

    CHECK(barsThatFire(4, 1.0) == std::vector<int>{1, 5, 9});

    SECTION("doubling makes it half as often, still counted from the first bar") {
        // Counted from bar 1 rather than from when the gesture was made, so a rule taken to
        // every eight bars still lands on the downbeat of the phrase.
        CHECK(barsThatFire(4, 2.0) == std::vector<int>{1, 9});
    }

    SECTION("halving makes it twice as often") {
        CHECK(barsThatFire(4, 0.5) == std::vector<int>{1, 3, 5, 7, 9});
    }

    SECTION("the multiplier is what the engine counts against") {
        Rule rule(fadeRule("bars", {"par"}, 255, 0.0));
        CHECK(rule.effectiveEvery() == 1);
        Rule::Config config4 = fadeRule("bars", {"par"}, 255, 0.0);
        config4.every = 4;
        Rule four(config4);
        CHECK(four.effectiveEvery() == 4);
        four.setRate(2.0);
        CHECK(four.effectiveEvery() == 8);
        four.setRate(0.5);
        CHECK(four.effectiveEvery() == 2);
        four.setRate(1.0);
        CHECK(four.effectiveEvery() == 4); // a reset puts back what the operator wrote
    }

    SECTION("halving a rule already on every bar leaves it there rather than switching it off") {
        // A rule that fired every zero bars would be a division by nothing or a rule that
        // fired constantly, depending on where the arithmetic landed. Both are worse than a
        // floor, and an operator pressing "twice as often" would have turned it off.
        CHECK(barsThatFire(1, 0.25) == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9});
    }

    SECTION("a preset load puts the live gestures back, because a preset is the show") {
        Rule::Config config4 = fadeRule("bars", {"par"}, 255, 0.0);
        config4.every = 4;
        Rule rule(config4);
        rule.setRate(2.0);
        rule.setMuted(true);
        rule.reset();
        CHECK(rule.effectiveEvery() == 4);
        CHECK_FALSE(rule.muted());
    }
}

TEST_CASE("a random position draws once per fire and the engine executes a number",
          "[dmx][trigger]") {
    // Where the randomness lives matters: drawn at the fire, so the same rule aimed at six
    // heads sends all six to the *same* place, and so a test can say what happened.
    Transports::Config config;
    config.patch = {head("head 1", 1), head("head 2", 20)};
    config.patch[0].group = "heads";
    config.patch[1].group = "heads";
    Transports transports(config);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    Rule::Config rule = fadeRule("swing", {"heads"}, 0, 0.0);
    rule.dmx.effect = EffectKind::Position;
    rule.dmx.pan.kind = GeneratorKind::Random;
    rule.dmx.pan.low = 0;
    rule.dmx.pan.high = 100;
    rule.dmx.tilt = fixedInt(25);
    engine.setRules({rule});
    engine.rule(0).setFixtureMask(0b11);
    sink.setNow(0.0);
    engine.onBeat(beatAt(1, 1, 1, 0.0));

    // Both heads land on the same pan, because the number was drawn once.
    CHECK(at(transports, 1) == at(transports, 20));
    // And tilt is a quarter of the way through the window, which with the default full window
    // is a quarter of full travel: 0.25 * 65535 rounds to 16384 = 0x4000.
    CHECK(at(transports, 3) == 0x40);
    CHECK(at(transports, 4) == 0x00);

    SECTION("the movement window clamps what a random draw can reach") {
        Transports::Config limited;
        limited.patch = {head("head 1", 1)};
        limited.patch[0].panMin = 0.4;
        limited.patch[0].panMax = 0.6;
        Transports narrow(limited);
        RuleSink narrowSink(narrow);
        TriggerEngine narrowEngine(narrowSink);
        Rule::Config swing = rule;
        swing.dmx.pan = fixedInt(100); // "as far as I am allowed"
        narrowEngine.setRules({swing});
        narrowEngine.rule(0).setFixtureMask(0b1);
        narrowSink.setNow(0.0);
        narrowEngine.onBeat(beatAt(1, 1, 1, 0.0));
        const std::span<const std::uint8_t> levels = narrow.dmx().levels(0);
        const double pan = (levels[0] * 256 + levels[1]) / 65535.0;
        CHECK(pan == Approx(0.6).margin(0.001));
    }
}

TEST_CASE("an effect aimed at fixtures that cannot do it is undeliverable, not an error",
          "[dmx][trigger]") {
    Transports::Config config;
    config.patch = {rgb("par", 1)};
    Transports transports(config);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    Rule::Config rule = fadeRule("swing", {"par"}, 0, 0.0);
    rule.dmx.effect = EffectKind::Position;
    engine.setRules({rule});
    engine.rule(0).setFixtureMask(0b1);
    sink.setNow(0.0);
    engine.onBeat(beatAt(1, 1, 1, 0.0));

    CHECK(sink.undeliverable() == 1);
    CHECK(sink.delivered() == 0);
}

TEST_CASE("a rule mixes a random color within the limits set per component",
          "[dmx][trigger]") {
    // §5.8's generators doing color work — `trigger::ColorMode::Mix`. Asked for on
    // 2026-09-16: "any random combination of rgb, within limits I set for r g and b channels
    // specifically on that trigger". Three generators rather than a "random color" tick box,
    // because a limit is a `Random` over a range and that already exists.
    Transports::Config rig;
    rig.patch = {rgb("par", 1)};
    Transports transports(rig);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    Rule::Config config;
    config.id = "mix";
    config.sendKind = Message::Kind::Dmx;
    config.trigger = takt4::trigger::Trigger::Beat;
    config.dmx.fixtures = {"par"};
    config.dmx.effect = EffectKind::Color;
    config.dmx.colorMode = takt4::trigger::ColorMode::Mix;
    config.dmx.unit = takt4::trigger::DelayUnit::Milliseconds;
    config.dmx.durationSeconds = 0.0;
    // Red anywhere, green kept out of it, blue kept high — the instruction a palette cannot
    // give without listing every color that satisfies it.
    config.dmx.red = takt4::trigger::componentMix();
    config.dmx.green = takt4::trigger::componentMix();
    config.dmx.green.high = 40;
    config.dmx.blue = takt4::trigger::componentMix();
    config.dmx.blue.low = 200;
    config.seed = 99;

    engine.setRules({config});
    engine.rule(0).setFixtureMask(takt4::dmx::resolveFixtures(transports.patch(), {"par"}));

    std::set<int> reds;
    for (int i = 0; i < 40; ++i) {
        const double now = static_cast<double>(i) * 0.5;
        sink.setNow(now);
        engine.onBeat(beatAt(static_cast<std::uint64_t>(i) + 1, 1, 1, now));
        const int red = at(transports, 1);
        const int green = at(transports, 2);
        const int blue = at(transports, 3);
        INFO("fire " << i << ": " << red << ", " << green << ", " << blue);
        CHECK(green <= 40);
        CHECK(blue >= 200);
        reds.insert(red);
    }
    // And it really is drawing rather than sending one color over and over: three independent
    // streams, because three generators on one seed would draw the same number and every
    // "random color" would be a grey.
    CHECK(reds.size() > 20);
}

TEST_CASE("a rule shuffles a palette of colors", "[dmx][trigger]") {
    // The other half of what a rig asked for: "I should be able to make it shuffle/randomize
    // through colors, a list of colors I pick from a color picker". A palette is a
    // `Shuffle` over a `List` of text values and no code here knows that is what it is
    // looking at — which is the whole argument for `DmxSend::color` being a generator.
    Transports::Config rig;
    rig.patch = {rgb("par", 1)};
    Transports transports(rig);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    Rule::Config config;
    config.id = "palette";
    config.sendKind = Message::Kind::Dmx;
    config.trigger = takt4::trigger::Trigger::Beat;
    config.dmx.fixtures = {"par"};
    config.dmx.effect = EffectKind::Color;
    config.dmx.colorMode = takt4::trigger::ColorMode::Palette;
    config.dmx.color.kind = GeneratorKind::Shuffle;
    config.dmx.color.pool = takt4::trigger::Pool::List;
    config.dmx.color.values = takt4::trigger::defaultPalette();
    config.dmx.unit = takt4::trigger::DelayUnit::Milliseconds;
    config.dmx.durationSeconds = 0.0;
    config.seed = 7;

    engine.setRules({config});
    engine.rule(0).setFixtureMask(takt4::dmx::resolveFixtures(transports.patch(), {"par"}));

    std::set<std::string> seen;
    for (int i = 0; i < 24; ++i) {
        const double now = static_cast<double>(i) * 0.5;
        sink.setNow(now);
        engine.onBeat(beatAt(static_cast<std::uint64_t>(i) + 1, 1, 1, now));
        const Color drawn{at(transports, 1), at(transports, 2), at(transports, 3)};
        seen.insert(takt4::dmx::formatColor(drawn));
    }
    // Every color of the palette, and nothing that is not in it — a draw that would not parse
    // falls back to white, which is what the integers 1 to 8 used to produce on every fire.
    CHECK(seen.size() == takt4::trigger::defaultPalette().size());
    for (const takt4::trigger::Value& value : takt4::trigger::defaultPalette()) {
        std::string text;
        value.appendTo(text);
        INFO("palette entry " << text);
        CHECK(seen.count(text) == 1);
    }
    CHECK(seen.count("#ffffff") == 0);
}

TEST_CASE("the rig a new operator builds actually lights up", "[dmx][trigger]") {
    // **The exact rig that reported "the light is not coming on at all" on 2026-09-16**, and
    // the whole of what was wrong with it. One RGB par, universe 5, address 70 — the numbers
    // off the back of a fixture — and a lighting rule left at its defaults: a fade, aimed at
    // "dimmer", over a beat.
    //
    // Two faults, either of which was enough on its own. The par has no dimmer channel, so
    // the effect reached nothing at all (`dmx::aims`). And `Generator::Config` defaults to
    // shuffling the integers 1 to 8, so even once it reached the colour it would have driven
    // it to between 0.4 % and 3 % of full, which on a lamp is indistinguishable from the rule
    // not having fired (`trigger::fixedNumber`).
    Transports::Config rig;
    rig.patch = {takt4::dmx::fixtureFromMode("Bedroom RGB", 1, 5, 70)};
    Transports transports(rig);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    // Everything here is a default except the three things the operator actually chose: the
    // send kind, the fixture and the duration.
    Rule::Config config;
    config.id = "bedroom";
    config.sendKind = Message::Kind::Dmx;
    config.trigger = takt4::trigger::Trigger::Beat;
    config.dmx.fixtures = {"Bedroom RGB"};
    config.dmx.unit = takt4::trigger::DelayUnit::Beats;
    config.dmx.durationBeats = 1.0;
    REQUIRE(config.dmx.effect == EffectKind::Level);
    REQUIRE(config.dmx.role == Role::Dimmer);

    engine.setRules({config});
    engine.rule(0).setFixtureMask(
        takt4::dmx::resolveFixtures(transports.patch(), {"Bedroom RGB"}));
    REQUIRE(engine.rule(0).valid());

    sink.setNow(0.0);
    engine.onBeat(beatAt(1, 1, 1, 0.0));
    CHECK(sink.delivered() == 1);

    const std::span<const std::uint8_t> levels = transports.dmx().levels(5);
    REQUIRE(!levels.empty());
    // Half a beat at 120 BPM into a one-beat fade, so it is on its way up rather than at full.
    transports.dmx().tick(0.25);
    CHECK(levels[69] > 0);
    // And at the end of the fade it is full white, which is what "fade the dimmer up" means on
    // a fixture whose brightness is its colour.
    transports.dmx().tick(1.0);
    CHECK(levels[69] == 255);
    CHECK(levels[70] == 255);
    CHECK(levels[71] == 255);
}
