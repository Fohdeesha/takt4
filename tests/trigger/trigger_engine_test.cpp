#include "core/trigger/trigger_engine.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using Catch::Approx;
using takt4::features::Intensity;
using takt4::trigger::Context;
using takt4::trigger::Generator;
using takt4::trigger::GeneratorKind;
using takt4::trigger::Message;
using takt4::trigger::Rule;
using takt4::trigger::Trigger;
using takt4::trigger::TriggerEngine;
using takt4::trigger::Value;

namespace {

/// Everything a rule sent, in order. §5.8 puts the rules on the output thread and hands
/// their messages to the transports; a test needs to say exactly what came out, which is
/// the reason `trigger::Sink` is an interface at all.
class Recorder : public takt4::trigger::Sink {
public:
    void send(const Message& message) override { sent.push_back(message); }

    std::vector<std::string> addresses() const {
        std::vector<std::string> out;
        out.reserve(sent.size());
        for (const Message& message : sent) {
            out.push_back(message.address);
        }
        return out;
    }
    std::vector<std::int32_t> arguments() const {
        std::vector<std::int32_t> out;
        out.reserve(sent.size());
        for (const Message& message : sent) {
            out.push_back(message.argument.asInt());
        }
        return out;
    }

    std::vector<Message> sent;
};

Generator::Config fixedAt(std::int32_t value) {
    Generator::Config config;
    config.kind = GeneratorKind::Fixed;
    config.fixed = Value::ofInt(value);
    return config;
}

/// A release — the fired message again with a different value — after a delay in ms.
takt4::trigger::FollowUp releaseAfterMs(std::int32_t value, double milliseconds) {
    takt4::trigger::FollowUp entry;
    entry.value = Value::ofInt(value);
    entry.unit = takt4::trigger::DelayUnit::Milliseconds;
    entry.delaySeconds = milliseconds / 1000.0;
    return entry;
}

/// A rule that sends one fixed address, so a test can count fires by counting messages.
Rule::Config simple(std::string id, Trigger trigger, std::uint32_t every = 1) {
    Rule::Config config;
    config.id = std::move(id);
    config.address = "/fire/" + config.id;
    config.trigger = trigger;
    config.every = every;
    config.value = fixedAt(1);
    return config;
}

/// The context at beat `beat` of a 4/4 bar, on a settled 120 BPM track.
Context beatAt(std::uint64_t beats, std::uint32_t beatInBar, std::uint64_t bars, double now) {
    Context context;
    context.bpm = 120.0;
    context.confidence = 0.9;
    context.locked = true;
    context.meter = 4;
    context.beatInBar = beatInBar;
    context.beats = beats;
    context.bars = bars;
    context.now = now;
    return context;
}

/// Four bars of 4/4, handed to the engine one beat at a time, half a second apart.
void playBars(TriggerEngine& engine, int bars, double from = 0.0) {
    std::uint64_t beat = 0;
    std::uint64_t bar = 0;
    for (int i = 0; i < bars * 4; ++i) {
        const std::uint32_t inBar = static_cast<std::uint32_t>(i % 4) + 1;
        ++beat;
        if (inBar == 1) {
            ++bar;
        }
        engine.onBeat(beatAt(beat, inBar, bar, from + 0.5 * static_cast<double>(i)));
    }
}

} // namespace

TEST_CASE("a euclidean pattern is the rhythm its two numbers name", "[trigger]") {
    // The user's ask on 2026-09-06 for "cool ... algorithmic settings". A Euclidean pattern
    // is the one rhythm that is neither "every N" nor random, and the classics fall straight
    // out of two numbers — which is what makes it worth a trigger rather than a preset.
    using takt4::trigger::euclidHit;

    const auto pattern = [](std::uint32_t pulses, std::uint32_t steps) {
        std::string bits;
        for (std::uint32_t step = 0; step < steps; ++step) {
            bits += euclidHit(step, pulses, steps) ? 'x' : '.';
        }
        return bits;
    };

    // Rhythms a person can hear, so a change to the distribution fails against something
    // checkable rather than against a number nobody can read.
    CHECK(pattern(3, 8) == "x..x..x."); // the tresillo, exactly as Toussaint prints it
    CHECK(pattern(2, 4) == "x.x.");     // every other
    CHECK(pattern(4, 4) == "xxxx");     // every one
    CHECK(pattern(1, 8) == "x.......");
    // E(5,8) is the cinquillo up to rotation — every rotation of a Euclidean rhythm is one,
    // and this construction fixes the rotation that starts on the beat. See `euclidHit`.
    CHECK(pattern(5, 8) == "x.x.xx.x");
    CHECK(pattern(5, 16) == "x...x..x..x..x..");

    SECTION("every pattern starts on the beat") {
        // The property that is *not* a convention: a pattern is counted from the downbeat,
        // and one starting with a rest would be the same rhythm heard in the wrong place.
        for (std::uint32_t steps = 1; steps <= 32; ++steps) {
            for (std::uint32_t pulses = 1; pulses <= steps; ++pulses) {
                INFO(pulses << " in " << steps);
                REQUIRE(euclidHit(0, pulses, steps));
            }
        }
    }

    SECTION("and is as even as whole steps allow, which is what makes it Euclidean") {
        // The actual definition: the gaps between hits take at most two distinct lengths,
        // and those differ by one. A distribution that merely had the right *count* would
        // pass every other check here and sound like nothing.
        for (std::uint32_t steps = 2; steps <= 32; ++steps) {
            for (std::uint32_t pulses = 1; pulses <= steps; ++pulses) {
                std::vector<std::uint32_t> hits;
                for (std::uint32_t step = 0; step < steps; ++step) {
                    if (euclidHit(step, pulses, steps)) {
                        hits.push_back(step);
                    }
                }
                REQUIRE(hits.size() == pulses);
                std::uint32_t shortest = steps;
                std::uint32_t longest = 0;
                for (std::size_t i = 0; i < hits.size(); ++i) {
                    // Round the loop, so the gap over the end of the pattern counts too.
                    const std::uint32_t next = i + 1 < hits.size() ? hits[i + 1] : hits[0] + steps;
                    const std::uint32_t gap = next - hits[i];
                    shortest = std::min(shortest, gap);
                    longest = std::max(longest, gap);
                }
                INFO(pulses << " in " << steps << ": gaps " << shortest << " to " << longest);
                REQUIRE(longest - shortest <= 1);
            }
        }
    }

    SECTION("every pattern has exactly the number of hits it was asked for") {
        for (std::uint32_t steps = 1; steps <= 32; ++steps) {
            for (std::uint32_t pulses = 0; pulses <= steps; ++pulses) {
                std::uint32_t hits = 0;
                for (std::uint32_t step = 0; step < steps; ++step) {
                    hits += euclidHit(step, pulses, steps) ? 1u : 0u;
                }
                INFO(pulses << " in " << steps);
                CHECK(hits == pulses);
            }
        }
    }

    SECTION("and the degenerate ones are answered rather than refused") {
        // §5.8's clamping policy at the two edges a half-built pattern passes through.
        CHECK_FALSE(euclidHit(0, 0, 8)); // no pulses is silence
        CHECK_FALSE(euclidHit(0, 3, 0)); // no steps is no pattern
        CHECK(euclidHit(3, 12, 4));      // more pulses than places: every step
    }
}

TEST_CASE("a rule on a euclidean pattern fires on its own beats", "[trigger]") {
    // The pattern above, driven through the real engine against real beat counts — which is
    // what says the trigger is wired to the tracker's beats rather than to a clock of its own.
    Recorder sink;
    TriggerEngine engine(sink);

    Rule::Config rule;
    rule.id = "tresillo";
    rule.trigger = Trigger::Euclid;
    rule.every = 8;  // steps
    rule.pulses = 3; // hits
    rule.address = "/hit";
    rule.sendValue = false;
    engine.setRules({rule});

    Context context;
    context.bpm = 128.0;
    context.meter = 4;
    std::vector<std::uint64_t> firedOn;
    for (std::uint64_t beat = 1; beat <= 16; ++beat) {
        const std::size_t before = sink.sent.size();
        context.beats = beat;
        context.beatInBar = static_cast<std::uint32_t>(((beat - 1) % 4) + 1);
        context.bars = ((beat - 1) / 4) + 1;
        context.now = static_cast<double>(beat);
        engine.onBeat(context);
        if (sink.sent.size() > before) {
            firedOn.push_back(beat);
        }
    }

    // "x..x..x." over eight, twice: beats 1, 4, 7 and then 9, 12, 15. Counted from the first
    // beat, like every other N in this layer (§7 deviation 11).
    CHECK(firedOn == std::vector<std::uint64_t>{1, 4, 7, 9, 12, 15});
}

TEST_CASE("the WHEN stage counts beats and bars from the first, not from the modulo",
          "[trigger][engine]") {
    // §5.8's first four: "every beat · every N beats · every bar · every N bars · on
    // downbeat". "Every 4 bars" fires on bars 1, 5 and 9 — an operator counting a phrase in
    // starts at one.
    Recorder sink;
    TriggerEngine engine(sink);
    engine.setRules({simple("beat", Trigger::Beat), simple("four", Trigger::Beat, 4),
                     simple("bar", Trigger::Bar), simple("phrase", Trigger::Bar, 4),
                     simple("down", Trigger::Downbeat)});
    REQUIRE(engine.ruleCount() == 5);

    playBars(engine, 8); // 32 beats

    const std::vector<std::string> fired = sink.addresses();
    const auto count = [&](const char* address) {
        return std::count(fired.begin(), fired.end(), std::string(address));
    };
    CHECK(count("/fire/beat") == 32);
    CHECK(count("/fire/four") == 8);   // beats 1, 5, 9, ... 29
    CHECK(count("/fire/bar") == 8);    // every bar
    CHECK(count("/fire/phrase") == 2); // bars 1 and 5
    CHECK(count("/fire/down") == 8);
    CHECK(engine.sent() == 58);
    CHECK(engine.dropped() == 0);

    SECTION("a bar rule needs a bar, and an unmetered stretch has none") {
        // Before the filter calls a downbeat there is no bar position, and §5.5 publishes
        // that as 0 rather than guessing 1. A bar rule must not fire on every beat there.
        Recorder quiet;
        TriggerEngine unmetered(quiet);
        unmetered.setRules({simple("bar", Trigger::Bar), simple("down", Trigger::Downbeat)});
        for (std::uint64_t beat = 1; beat <= 16; ++beat) {
            unmetered.onBeat(beatAt(beat, 0, 0, 0.5 * static_cast<double>(beat)));
        }
        CHECK(quiet.sent.empty());
    }
}

TEST_CASE("a follow-up arrives after its delay and not before", "[trigger][engine]") {
    // §5.8's "optional follow-up value after a delay", which is §5.6's press-then-release:
    // "/composition/layers/{L}/clips/{C}/connect — int 1 (press) then 0 (release)".
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config config = simple("clip", Trigger::Downbeat);
    config.address = "/composition/layers/{L}/clips/{C}/connect";
    config.segments = {fixedAt(2), fixedAt(5)};
    config.followUps.push_back(releaseAfterMs(0, 50));
    engine.setRules({config});

    engine.onBeat(beatAt(1, 1, 1, 10.0));
    REQUIRE(sink.sent.size() == 1);
    CHECK(sink.sent[0].address == "/composition/layers/2/clips/5/connect");
    CHECK(sink.sent[0].argument.asInt() == 1);
    CHECK(engine.pending() == 1);

    Context context = beatAt(1, 1, 1, 10.04);
    engine.advance(context);
    CHECK(sink.sent.size() == 1); // not yet
    CHECK(engine.pending() == 1);

    context.now = 10.05;
    engine.advance(context);
    REQUIRE(sink.sent.size() == 2);
    CHECK(sink.sent[1].address == sink.sent[0].address);
    CHECK(sink.sent[1].argument.asInt() == 0);
    CHECK(engine.pending() == 0);

    SECTION("and a second round sends nothing again") {
        context.now = 10.5;
        engine.advance(context);
        CHECK(sink.sent.size() == 2);
    }

    SECTION("a delay counted in beats is the same gesture at any tempo") {
        // Asked for on 2026-09-08: "an optional beats/bars box, so you can have it send
        // something after 2 beats for example, regardless of bpm". A device's own lag is a
        // fixed number of milliseconds; a *release* is a length of music, and spelling it in
        // milliseconds means retyping it whenever the record changes.
        Recorder musical;
        TriggerEngine beats(musical);
        config.followUps[0].unit = takt4::trigger::DelayUnit::Beats;
        config.followUps[0].delayBeats = 2.0;
        beats.setRules({config});

        // Two beats at 120 BPM is one second.
        beats.onBeat(beatAt(1, 1, 1, 10.0));
        REQUIRE(musical.sent.size() == 1);
        Context later = beatAt(1, 1, 1, 10.99);
        beats.advance(later);
        CHECK(musical.sent.size() == 1);
        later.now = 11.0;
        beats.advance(later);
        CHECK(musical.sent.size() == 2);

        SECTION("and at a different tempo it is a different number of seconds") {
            Recorder faster;
            TriggerEngine slow(faster);
            slow.setRules({config});
            Context at90 = beatAt(1, 1, 1, 10.0);
            at90.bpm = 90.0; // two beats is 1.333 s
            slow.onBeat(at90);
            REQUIRE(faster.sent.size() == 1);
            at90.now = 11.3;
            slow.advance(at90);
            CHECK(faster.sent.size() == 1);
            at90.now = 11.34;
            slow.advance(at90);
            CHECK(faster.sent.size() == 2);
        }

        SECTION("a bar is the meter the tracker reports, never four") {
            Recorder waltzed;
            TriggerEngine waltz(waltzed);
            config.followUps[0].unit = takt4::trigger::DelayUnit::Bars;
            config.followUps[0].delayBeats = 1.0;
            waltz.setRules({config});
            Context three = beatAt(1, 1, 1, 10.0);
            three.meter = 3; // one bar is three beats, 1.5 s at 120
            waltz.onBeat(three);
            REQUIRE(waltzed.sent.size() == 1);
            three.now = 11.49;
            waltz.advance(three);
            CHECK(waltzed.sent.size() == 1);
            three.now = 11.5;
            waltz.advance(three);
            CHECK(waltzed.sent.size() == 2);
        }

        SECTION("with no tempo tracked it falls back to the milliseconds") {
            // A follow-up due immediately is a release sent in the same round as its press,
            // which reads as a rule that does nothing at all.
            Recorder silent;
            TriggerEngine nothing(silent);
            nothing.setRules({config});
            Context idle = beatAt(1, 1, 1, 10.0);
            idle.bpm = 0.0;
            nothing.onBeat(idle);
            REQUIRE(silent.sent.size() == 1);
            idle.now = 10.04;
            nothing.advance(idle);
            CHECK(silent.sent.size() == 1);
            idle.now = 10.05; // the entry's own delaySeconds
            nothing.advance(idle);
            CHECK(silent.sent.size() == 2);
        }
    }

    SECTION("overlapping follow-ups come out in the order they were queued") {
        Recorder many;
        TriggerEngine overlapping(many);
        config.followUps[0].delaySeconds = 1.2; // longer than the gap between beats
        config.segments = {fixedAt(1), fixedAt(1)};
        engine.setRules({config});
        overlapping.setRules({config});
        for (int i = 0; i < 4; ++i) {
            overlapping.onBeat(beatAt(1, 1, 1, static_cast<double>(i) * 0.5));
        }
        CHECK(overlapping.pending() == 4);
        Context late = beatAt(1, 1, 1, 100.0);
        overlapping.advance(late);
        CHECK(overlapping.pending() == 0);
        CHECK(many.sent.size() == 8);
        // Four presses, then four releases: each release waited out a delay longer than the
        // gap between the presses.
        CHECK(many.arguments() == std::vector<std::int32_t>{1, 1, 1, 1, 0, 0, 0, 0});
    }
}

TEST_CASE("the triggers that do not wait for a beat fire on a round", "[trigger][engine]") {
    Recorder sink;
    TriggerEngine engine(sink);
    engine.setRules({simple("tempo", Trigger::TempoChange), simple("lock", Trigger::LockChange),
                     simple("mood", Trigger::IntensityChange)});

    Context context = beatAt(1, 1, 1, 0.0);
    engine.advance(context); // baselines; nothing has changed yet
    CHECK(sink.sent.empty());
    engine.advance(context);
    CHECK(sink.sent.empty());

    context.bpm = 174.0;
    engine.advance(context);
    CHECK(sink.addresses() == std::vector<std::string>{"/fire/tempo"});

    context.locked = false;
    engine.advance(context);
    CHECK(sink.sent.size() == 2);
    CHECK(sink.sent[1].address == "/fire/lock");

    context.intensity = Intensity::Calm;
    engine.advance(context);
    CHECK(sink.sent.size() == 3);
    CHECK(sink.sent[2].address == "/fire/mood");

    SECTION("and a beat does not fire them a second time") {
        // Every trigger belongs to exactly one of onBeat and advance. A change trigger
        // firing from both would double every message a rule sends.
        const std::size_t before = sink.sent.size();
        engine.onBeat(context);
        CHECK(sink.sent.size() == before);
    }
}

TEST_CASE("a rule disabled while a change happens does not fire when it comes back",
          "[trigger][engine]") {
    // What a rule remembers is how it tells a change from a value it has already seen. One
    // that stopped watching while switched off would fire the moment it was switched on,
    // for a change that happened while it was not listening.
    Recorder sink;
    TriggerEngine engine(sink);
    engine.setRules({simple("lock", Trigger::LockChange)});
    Rule* rule = engine.find("lock");
    REQUIRE(rule != nullptr);

    Context context = beatAt(1, 1, 1, 0.0);
    context.locked = false;
    engine.advance(context); // baseline

    rule->setEnabled(false);
    context.locked = true;
    engine.advance(context);
    CHECK(sink.sent.empty());

    rule->setEnabled(true);
    engine.advance(context);
    CHECK(sink.sent.empty()); // it already knows we are locked

    context.locked = false;
    engine.advance(context);
    CHECK(sink.sent.size() == 1);
}

TEST_CASE("panic halts every rule and hands back what it owes", "[trigger][engine]") {
    // §5.8: "A global halt that stops every rule instantly, reachable from the UI, a
    // keyboard shortcut, OSC and MIDI. Non-negotiable for live use."
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config config = simple("clip", Trigger::Beat);
    config.followUps.push_back(releaseAfterMs(0, 10000)); // a long way off, so panic owes
    engine.setRules({config});

    engine.onBeat(beatAt(1, 1, 1, 0.0));
    REQUIRE(sink.sent.size() == 1);
    REQUIRE(engine.pending() == 1);
    CHECK_FALSE(engine.panicked());

    engine.panic(beatAt(1, 1, 1, 0.5));
    CHECK(engine.panicked());
    // The owed release goes out **now** rather than being dropped. §5.6's shape is
    // press-then-release, so the pending half is what turns a clip off; dropping it would
    // leave the rig latched into the state panic was hit to escape.
    REQUIRE(sink.sent.size() == 2);
    CHECK(sink.sent[1].argument.asInt() == 0);
    CHECK(engine.pending() == 0);

    SECTION("and nothing fires again until it is released") {
        playBars(engine, 4, 1.0);
        CHECK(sink.sent.size() == 2);
        Context context = beatAt(1, 1, 1, 20.0);
        engine.advance(context);
        CHECK(sink.sent.size() == 2);
        CHECK_FALSE(engine.test("clip", context));

        engine.release();
        CHECK_FALSE(engine.panicked());
        engine.onBeat(beatAt(1, 1, 1, 21.0));
        CHECK(sink.sent.size() == 3);
    }
}

TEST_CASE("replacing the rules keeps what the old ones owe until it is due", "[trigger][engine]") {
    // A queued follow-up is a whole message with its own routing, so it is still right when it
    // comes due. Paying it out at the replacement — which every keystroke in the editor is — cut
    // a laser clip or a fade short (the audit's H7). A rule that is gone still gets its release.
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config config = simple("clip", Trigger::Beat);
    config.followUps.push_back(releaseAfterMs(0, 5000));
    engine.setRules({config});
    engine.onBeat(beatAt(1, 1, 1, 0.0));
    REQUIRE(engine.pending() == 1);

    engine.setRules({simple("other", Trigger::Beat)});
    CHECK(engine.pending() == 1);
    CHECK(sink.sent.size() == 1); // the press, and nothing owed paid early
    CHECK(engine.ruleCount() == 1);
    CHECK(engine.find("clip") == nullptr);

    Context later = beatAt(1, 1, 1, 4.9);
    engine.advance(later);
    CHECK(sink.sent.size() == 1);
    later.now = 5.0;
    engine.advance(later);
    REQUIRE(sink.sent.size() == 2);
    CHECK(sink.sent[1].address == "/fire/clip");
    CHECK(sink.sent[1].argument.asInt() == 0);
}

TEST_CASE("an edited rule carries on from where it was", "[trigger][engine]") {
    // The audit's H7: every edit in the editor rebuilt every rule from its configuration, so a
    // clip shuffle replayed its bag from the seed, a cooldown was forgotten and a rule a Stream
    // Deck had switched off came back on.
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config config = simple("clips", Trigger::Beat);
    config.value.kind = GeneratorKind::Shuffle;
    config.value.low = 1;
    config.value.high = 8;

    SECTION("a shuffle keeps drawing from the same bag across an edit elsewhere") {
        engine.setRules({config});
        for (std::uint64_t beat = 1; beat <= 3; ++beat) {
            engine.onBeat(beatAt(beat, 1, 1, static_cast<double>(beat)));
        }
        config.name = "renamed mid-bag";
        engine.setRules({config});
        for (std::uint64_t beat = 4; beat <= 8; ++beat) {
            engine.onBeat(beatAt(beat, 1, 1, static_cast<double>(beat)));
        }
        std::vector<std::int32_t> drawn = sink.arguments();
        REQUIRE(drawn.size() == 8);
        std::sort(drawn.begin(), drawn.end());
        // One bag of eight, drawn without replacement: every value once. A bag restarted from
        // its seed after three draws hands those three out again.
        CHECK(drawn == std::vector<std::int32_t>{1, 2, 3, 4, 5, 6, 7, 8});
    }

    SECTION("but a generator that was itself edited starts again") {
        engine.setRules({config});
        engine.onBeat(beatAt(1, 1, 1, 1.0));
        config.value.high = 4;
        engine.setRules({config});
        for (std::uint64_t beat = 2; beat <= 5; ++beat) {
            engine.onBeat(beatAt(beat, 1, 1, static_cast<double>(beat)));
        }
        const std::vector<std::int32_t> all = sink.arguments();
        REQUIRE(all.size() == 5);
        std::vector<std::int32_t> after(all.begin() + 1, all.end());
        std::sort(after.begin(), after.end());
        CHECK(after == std::vector<std::int32_t>{1, 2, 3, 4});
    }

    SECTION("a cooldown already running is still running") {
        config.value = fixedAt(1);
        config.conditions.cooldownSeconds = 10.0;
        engine.setRules({config});
        engine.onBeat(beatAt(1, 1, 1, 0.0));
        REQUIRE(sink.sent.size() == 1);
        config.name = "edited";
        engine.setRules({config});
        engine.onBeat(beatAt(2, 2, 1, 1.0));
        CHECK(sink.sent.size() == 1);
        engine.onBeat(beatAt(3, 3, 1, 11.0));
        CHECK(sink.sent.size() == 2);
    }

    SECTION("a rule switched off from outside stays off through an edit") {
        engine.setRules({config});
        engine.find("clips")->setEnabled(false); // /ctl/rule/clips/enable 0
        config.name = "edited";
        engine.setRules({config});
        CHECK_FALSE(engine.find("clips")->enabled());
        engine.onBeat(beatAt(1, 1, 1, 0.0));
        CHECK(sink.sent.empty());
    }

    SECTION("unless the edit is the switch itself") {
        config.enabled = false;
        engine.setRules({config});
        config.enabled = true; // the operator ticked the box
        engine.setRules({config});
        CHECK(engine.find("clips")->enabled());
    }

    SECTION("and something still owed goes out when it is due, not at the edit") {
        config.value = fixedAt(1);
        config.followUps.push_back(releaseAfterMs(0, 500));
        engine.setRules({config});
        engine.onBeat(beatAt(1, 1, 1, 0.0));
        REQUIRE(engine.pending() == 1);
        config.name = "edited";
        engine.setRules({config});
        CHECK(engine.pending() == 1);
        CHECK(sink.sent.size() == 1);
    }
}

TEST_CASE("the test button fires one rule past everything that would stop it",
          "[trigger][engine]") {
    // §5.9's per-rule "[test]" button. An operator building a rule needs to see where the
    // message lands without waiting for a downbeat at the right confidence — and the state
    // they are most likely in while building it is "not switched on yet".
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config config = simple("clip", Trigger::Downbeat);
    config.enabled = false;
    config.conditions.minConfidence = 0.99;
    config.conditions.cooldownSeconds = 60.0;
    engine.setRules({config});

    Context context = beatAt(1, 1, 1, 0.0);
    context.confidence = 0.1;
    CHECK(engine.test("clip", context));
    CHECK(sink.addresses() == std::vector<std::string>{"/fire/clip"});
    // The generators and the cooldown advance as they would on a real fire: the point is to
    // see what it will really send.
    CHECK(engine.find("clip")->fires() == 1);
    CHECK(engine.find("clip")->lastFired() == Approx(0.0));

    CHECK_FALSE(engine.test("nothing-by-that-name", context));

    SECTION("but not a rule that is invalid, which has nothing to send") {
        Rule::Config broken = simple("broken", Trigger::Manual);
        broken.address = "/a/{x}"; // no generator for it
        engine.setRules({broken});
        CHECK_FALSE(engine.test("broken", context));
        CHECK(sink.sent.size() == 1); // still just the first
    }
}

TEST_CASE("manual and onset fire only the rules that asked for them", "[trigger][engine]") {
    Recorder sink;
    TriggerEngine engine(sink);
    engine.setRules({simple("hotkey", Trigger::Manual), simple("hit", Trigger::Onset),
                     simple("beat", Trigger::Beat)});

    const Context context = beatAt(1, 1, 1, 0.0);
    engine.manual(context);
    CHECK(sink.addresses() == std::vector<std::string>{"/fire/hotkey"});

    engine.onOnset(context);
    CHECK(sink.addresses() == std::vector<std::string>{"/fire/hotkey", "/fire/hit"});

    // And neither of them is a beat.
    CHECK(engine.find("beat")->fires() == 0);
}

TEST_CASE("a rule whose address cannot be built is counted rather than hidden",
          "[trigger][engine]") {
    // A rule dropping every fire looks exactly like a rule that never triggers, and an
    // operator staring at a card that says "0 fires" has no way to tell them apart.
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config config = simple("clip", Trigger::Beat);
    config.address = "/deck/{name}";
    Generator::Config text;
    text.kind = GeneratorKind::Fixed;
    text.fixed = Value::ofText("a/b"); // a slash would move the message to another address
    config.segments = {text};
    engine.setRules({config});
    REQUIRE(engine.rule(0).valid()); // the template is fine; what fills it is not

    playBars(engine, 1);
    CHECK(sink.sent.empty());
    CHECK(engine.sent() == 0);
    CHECK(engine.dropped() == 4);
}

TEST_CASE("an invalid rule is held and shown rather than dropped", "[trigger][engine]") {
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config broken = simple("broken", Trigger::Beat);
    broken.address = "/a/{x}/{y}";
    broken.segments = {fixedAt(1)};
    engine.setRules({broken, simple("fine", Trigger::Beat)});

    REQUIRE(engine.ruleCount() == 2); // both kept: a UI has to be able to show the mistake
    CHECK_FALSE(engine.rule(0).valid());
    CHECK(engine.rule(1).valid());

    playBars(engine, 1);
    CHECK(sink.addresses().size() == 4);
    for (const std::string& address : sink.addresses()) {
        CHECK(address == "/fire/fine");
    }
    CHECK(engine.dropped() == 0); // it never fired at all, which is not the same as dropping
}

TEST_CASE("the ONLY IF stage is asked on every fire, not only the first", "[trigger][engine]") {
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config config = simple("gated", Trigger::Beat);
    config.conditions.minConfidence = 0.5;
    config.conditions.cooldownSeconds = 1.4; // longer than one beat at 120 BPM, shorter than three
    engine.setRules({config});

    for (int i = 0; i < 8; ++i) {
        Context context = beatAt(static_cast<std::uint64_t>(i) + 1, 1, 1, 0.5 * i);
        context.confidence = i < 4 ? 0.1 : 0.9; // below the gate for the first four
        engine.onBeat(context);
    }
    // Beats at 2.0 and 3.5: the first one over the gate, then the next past the cooldown.
    CHECK(sink.sent.size() == 2);
}

TEST_CASE("an observer is told which values a fire produced, and which sends are releases",
          "[trigger][engine]") {
    // Two things §5.9 draws that had no source until now: the fire count on each rule card,
    // and "what this slot last produced" beside each generator chip. Both come from here,
    // because the rules themselves belong to the output thread and a UI cannot read them.
    Recorder sink;
    TriggerEngine engine(sink);

    Rule::Config config = simple("clip", Trigger::Beat);
    config.address = "/composition/layers/{layer}/clips/{clip}/connect";
    config.segments = {fixedAt(3), fixedAt(7)};
    config.sendValue = true;
    config.value = fixedAt(1);
    // §7.4's release, on a timer — the thing that would double a naive fire count.
    config.followUps.push_back(releaseAfterMs(0, 50));
    engine.setRules({config});

    struct Seen {
        std::string ruleId;
        bool followUp = false;
        std::vector<Value> slots;
    };
    std::vector<Seen> seen;
    engine.setFireObserver([&seen](std::string_view ruleId, const Message&, bool followUp,
                                   std::span<const Value> slots, bool) {
        seen.push_back(
            Seen{std::string(ruleId), followUp, std::vector<Value>(slots.begin(), slots.end())});
    });

    engine.onBeat(beatAt(1, 1, 1, 0.0));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].ruleId == "clip");
    CHECK_FALSE(seen[0].followUp);
    // The layer, the clip and the value — the three chips the editor draws for this rule,
    // in the order it draws them. That ordering is what pairs a value with its own box.
    REQUIRE(seen[0].slots.size() == 3);
    CHECK(seen[0].slots[0].asInt() == 3);
    CHECK(seen[0].slots[1].asInt() == 7);
    CHECK(seen[0].slots[2].asInt() == 1);

    // The release, 50 ms later. `advance` is what drains what is owed — `onBeat` only fires
    // rules — and the output thread calls it every round.
    engine.advance(beatAt(1, 1, 1, 0.06));
    const auto release =
        std::find_if(seen.begin(), seen.end(), [](const Seen& s) { return s.followUp; });
    REQUIRE(release != seen.end());
    CHECK(release->ruleId == "clip");
    CHECK(release->slots.empty());

    SECTION("so counting only the fires counts each fire once") {
        // What the rule card shows. A bar of 4/4 driven the way the output thread drives it
        // — a beat, then the rounds that follow it — so every release lands. Eight messages
        // leave and the number an operator recognises is four.
        for (int i = 0; i < 4; ++i) {
            const double now = 10.0 + 0.5 * static_cast<double>(i);
            const auto beat = static_cast<std::uint64_t>(i + 2); // beat 1 fired above
            engine.onBeat(beatAt(beat, static_cast<std::uint32_t>(i % 4) + 1, 1, now));
            engine.advance(beatAt(beat, static_cast<std::uint32_t>(i % 4) + 1, 1, now + 0.06));
        }
        const auto fires =
            std::count_if(seen.begin(), seen.end(), [](const Seen& s) { return !s.followUp; });
        const auto releases =
            std::count_if(seen.begin(), seen.end(), [](const Seen& s) { return s.followUp; });
        // One from the top of the test and four here, each with exactly one release.
        CHECK(fires == 5);
        CHECK(releases == 5);
        CHECK(sink.sent.size() == 10);
        // And the rule's own counter agrees, which is what the card would show if it could
        // safely be read — it cannot, which is why the observer exists.
        CHECK(engine.rule(0).fires() == 5);
    }

    SECTION("and a fire whose address cannot be built leaves no stale values behind") {
        // A generator that produces something illegal in an OSC address fails the fill, and
        // the chips must not go on showing the values of the last fire that worked.
        Rule::Config broken = config;
        broken.id = "broken";
        broken.segments = {fixedAt(3), Generator::Config{}};
        broken.segments[1].kind = GeneratorKind::Fixed;
        broken.segments[1].fixed = Value::ofText("has space");
        engine.setRules({broken});
        engine.onBeat(beatAt(1, 1, 1, 20.0));
        CHECK(engine.rule(0).lastSlots().empty());
    }
}
