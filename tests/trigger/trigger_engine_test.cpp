#include "core/trigger/trigger_engine.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
        // Each press to a different clip, so the releases can be told apart. They were all to
        // the same address, and the check below them could not see an order at all (the audit
        // of 2026-09-25, T13).
        Generator::Config clips;
        clips.kind = GeneratorKind::Cycle;
        clips.pool = takt4::trigger::Pool::List;
        clips.values = {Value::ofInt(3), Value::ofInt(7), Value::ofInt(1), Value::ofInt(12)};
        config.segments = {fixedAt(1), clips};
        engine.setRules({config});
        overlapping.setRules({config});
        for (int i = 0; i < 4; ++i) {
            overlapping.onBeat(beatAt(1, 1, 1, static_cast<double>(i) * 0.5));
        }
        CHECK(overlapping.pending() == 4);
        Context late = beatAt(1, 1, 1, 100.0);
        overlapping.advance(late);
        CHECK(overlapping.pending() == 0);
        REQUIRE(many.sent.size() == 8);
        // Four presses, then four releases: each release waited out a delay longer than the
        // gap between the presses.
        CHECK(many.arguments() == std::vector<std::int32_t>{1, 1, 1, 1, 0, 0, 0, 0});
        // And the releases in the presses' order, each to the clip its own press opened.
        const std::vector<std::string> sent = many.addresses();
        const std::vector<std::string> presses(sent.begin(), sent.begin() + 4);
        const std::vector<std::string> releases(sent.begin() + 4, sent.end());
        CHECK(presses == std::vector<std::string>{"/composition/layers/1/clips/3/connect",
                                                  "/composition/layers/1/clips/7/connect",
                                                  "/composition/layers/1/clips/1/connect",
                                                  "/composition/layers/1/clips/12/connect"});
        CHECK(releases == presses);
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

TEST_CASE("a stopped tracker fires nothing the music would, and what the operator fires still goes",
          "[trigger][engine]") {
    // The audit of 2026-09-25, H3: after STOP the engine's last state stays published, and a rule
    // watching it — or a beat that reaches the rules on its way down — fired after the blackout.
    // Stopped, the rules the music drives are silent; manual and [test] are the operator's, and a
    // rig is built with the tracker stopped.
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config manual = simple("manual", Trigger::Manual);
    manual.followUps.push_back(releaseAfterMs(0, 100.0));
    engine.setRules({simple("beat", Trigger::Beat), simple("onset", Trigger::Onset),
                     simple("bar", Trigger::Bar), simple("tempo", Trigger::TempoChange), manual});
    Context first = beatAt(1, 1, 1, 0.0);
    engine.advance(first); // the tempo rule's first look, which it only remembers
    REQUIRE(sink.sent.empty());

    engine.setListening(false);
    CHECK_FALSE(engine.listening());
    engine.onBeat(beatAt(2, 2, 1, 0.5));
    engine.onOnset(beatAt(2, 2, 1, 0.5));
    engine.onBarDeclared(beatAt(2, 1, 2, 0.5));
    Context moved = beatAt(2, 2, 1, 0.6);
    moved.bpm = 90.0;
    engine.advance(moved);
    CHECK(sink.sent.empty());

    // What the operator fires goes, and its release after it.
    engine.manual(beatAt(2, 2, 1, 1.0));
    CHECK(engine.test("beat", beatAt(2, 2, 1, 1.0)));
    Context later = moved; // the tempo where it moved to, so the tempo rule sees no new change
    later.now = 1.2;
    engine.advance(later);
    CHECK(sink.addresses() ==
          std::vector<std::string>{"/fire/manual", "/fire/beat", "/fire/manual"});

    // Listening again, the music drives them — and the tempo rule did not bank the change it saw
    // while stopped to fire the moment it could.
    sink.sent.clear();
    engine.setListening(true);
    engine.advance(moved);
    CHECK(sink.sent.empty());
    engine.onBeat(beatAt(3, 3, 1, 1.5));
    CHECK(sink.addresses() == std::vector<std::string>{"/fire/beat"});
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
        // On an onset, which is where a cooldown means anything (`trigger::takesCooldown`).
        config.trigger = Trigger::Onset;
        config.value = fixedAt(1);
        config.cooldownSeconds = 10.0;
        engine.setRules({config});
        engine.onOnset(beatAt(1, 1, 1, 0.0));
        REQUIRE(sink.sent.size() == 1);
        config.name = "edited";
        engine.setRules({config});
        engine.onOnset(beatAt(2, 2, 1, 1.0));
        CHECK(sink.sent.size() == 1);
        engine.onOnset(beatAt(3, 3, 1, 11.0));
        CHECK(sink.sent.size() == 2);
    }

    SECTION("but a set that was loaded, not edited, starts afresh whatever its ids") {
        // The audit of 2026-09-25, M11: an IMPORT whose rule shares an id with the show before
        // took that rule's mute, rate, switch and place in its bag.
        config.value = fixedAt(1);
        engine.setRules({config});
        engine.onBeat(beatAt(1, 1, 1, 0.0));
        engine.find("clips")->setMuted(true);
        engine.find("clips")->setRate(2.0);
        engine.find("clips")->setEnabled(false);
        REQUIRE(engine.find("clips")->fires() == 1);
        engine.setRules({config}, /*fresh=*/true);
        const Rule* loaded = engine.find("clips");
        REQUIRE(loaded != nullptr);
        CHECK_FALSE(loaded->muted());
        CHECK(loaded->rate() == 1.0);
        CHECK(loaded->enabled());
        CHECK(loaded->fires() == 0);
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
    config.conditionsOn = true;
    config.conditions.minConfidence = 0.99;
    config.cooldownSeconds = 60.0;
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

TEST_CASE("a bar declared late fires that bar's rules and no beat's", "[trigger][engine]") {
    // The audit's M4. A DOWNBEAT pressed a moment after the beat it means turns that beat
    // into the bar's first — but it has already gone out as beat 3, so the downbeat rules
    // never heard it. They fire when the bar is declared, as that bar's; the beat rules did
    // fire on that beat and must not fire twice.
    Recorder sink;
    TriggerEngine engine(sink);
    engine.setRules({simple("down", Trigger::Downbeat), simple("every-bar", Trigger::Bar),
                     simple("odd-bars", Trigger::Bar, 2), simple("beat", Trigger::Beat)});

    engine.onBarDeclared(beatAt(7, 1, 3, 3.1)); // bar 3, declared
    CHECK(sink.addresses() ==
          std::vector<std::string>{"/fire/down", "/fire/every-bar", "/fire/odd-bars"});
    CHECK(engine.find("beat")->fires() == 0);

    SECTION("an every-2-bars rule counts the declared bar's number, not the press") {
        sink.sent.clear();
        engine.onBarDeclared(beatAt(11, 1, 4, 5.1)); // bar 4: not one of every-2's
        CHECK(sink.addresses() == std::vector<std::string>{"/fire/down", "/fire/every-bar"});
    }

    SECTION("and nothing while panicked") {
        engine.panic(beatAt(11, 1, 5, 5.0));
        sink.sent.clear();
        engine.onBarDeclared(beatAt(11, 1, 5, 5.1));
        CHECK(sink.sent.empty());
    }
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
    Rule::Config config = simple("gated", Trigger::Onset);
    config.conditionsOn = true;
    config.conditions.minConfidence = 0.5;
    config.cooldownSeconds = 1.4; // longer than two onsets half a second apart, shorter than three
    engine.setRules({config});

    for (int i = 0; i < 8; ++i) {
        Context context = beatAt(static_cast<std::uint64_t>(i) + 1, 1, 1, 0.5 * i);
        context.confidence = i < 4 ? 0.1 : 0.9; // below the gate for the first four
        engine.onOnset(context);
    }
    // Onsets at 2.0 and 3.5: the first one over the gate, then the next past the cooldown.
    CHECK(sink.sent.size() == 2);

    SECTION("and with the stage switched off, every onset past the cooldown fires") {
        sink.sent.clear();
        config.id = "ungated";
        config.address = "/fire/ungated";
        config.conditionsOn = false;
        engine.setRules({config});
        for (int i = 0; i < 8; ++i) {
            Context context = beatAt(static_cast<std::uint64_t>(i) + 1, 1, 1, 10.0 + 0.5 * i);
            context.confidence = 0.1;
            engine.onOnset(context);
        }
        // 10.0, 11.5 and 13.0: the gate says nothing, the cooldown still spaces them.
        CHECK(sink.sent.size() == 3);
    }
}

TEST_CASE("a cooldown on a beat or a bar rule changes nothing", "[trigger][engine]") {
    // The consequence accepted with the cooldown's move to A (HANDOFF §0.5, 2026-09-30): the
    // editor shows it switched off on the steady triggers, so a saved "every 1 beat" rule whose
    // cooldown was longer than a beat — quietly thinning it — fires on every beat again. A
    // cooldown on a bar rule never did anything, and still does not.
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config beat = simple("beat", Trigger::Beat);
    beat.cooldownSeconds = 1.4; // longer than two beats at 120 BPM
    Rule::Config euclid = simple("euclid", Trigger::Euclid, 8);
    euclid.pulses = 8; // every step a hit, so the pattern cannot be what thins it
    euclid.cooldownSeconds = 1.4;
    engine.setRules({beat, euclid});
    playBars(engine, 2);
    const std::vector<std::string> sent = sink.addresses();
    CHECK(std::count(sent.begin(), sent.end(), "/fire/beat") == 8);
    CHECK(std::count(sent.begin(), sent.end(), "/fire/euclid") == 8);
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

TEST_CASE("a rule names which beat of the bar it fires on", "[trigger][engine][beat-of-bar]") {
    // The operator, 2026-10-06: "not just fire every two beats, but WHICH two beats. for example
    // I want one trigger firing on the first beat of a bar, another on the second beat, another
    // on the third". Eight bars of 4/4; where each rule's fires landed, as heard.
    Recorder sink;
    TriggerEngine engine(sink);
    const auto onBeat = [](std::string id, Trigger trigger, std::uint32_t every,
                           std::uint32_t beat) {
        Rule::Config config = simple(std::move(id), trigger, every);
        config.onBeat = beat;
        return config;
    };
    engine.setRules({onBeat("first", Trigger::Bar, 1, 1), onBeat("second", Trigger::Bar, 1, 2),
                     onBeat("third", Trigger::Bar, 1, 3), onBeat("backbeat", Trigger::Beat, 2, 2),
                     onBeat("ones", Trigger::Beat, 2, 1), onBeat("pairs", Trigger::Beat, 8, 5),
                     onBeat("every", Trigger::Beat, 1, 3), onBeat("fifth", Trigger::Bar, 1, 5),
                     onBeat("late", Trigger::Bar, 2, 4)});
    using Where = std::vector<std::pair<std::uint64_t, std::uint32_t>>;
    // Which bar, and which beat of it, each rule's fires landed on.
    std::vector<std::pair<std::string, std::pair<std::uint64_t, std::uint32_t>>> heard;
    std::pair<std::uint64_t, std::uint32_t> now{0, 0};
    engine.setFireObserver([&heard, &now](std::string_view id, const Message&, bool,
                                          std::span<const Value>, bool) {
        heard.emplace_back(std::string(id), now);
    });
    std::uint64_t beat = 0;
    std::uint64_t bar = 0;
    for (int i = 0; i < 32; ++i) {
        const std::uint32_t inBar = static_cast<std::uint32_t>(i % 4) + 1;
        ++beat;
        if (inBar == 1) {
            ++bar;
        }
        now = {bar, inBar};
        engine.onBeat(beatAt(beat, inBar, bar, 0.5 * static_cast<double>(i)));
    }
    const auto beatsOf = [&heard](const char* id) {
        Where out;
        for (const auto& [rule, where] : heard) {
            if (rule == id) {
                out.push_back(where);
            }
        }
        return out;
    };
    // Once a bar each, each on its own beat.
    CHECK(beatsOf("first").size() == 8);
    CHECK(beatsOf("second") ==
          Where{{1, 2}, {2, 2}, {3, 2}, {4, 2}, {5, 2}, {6, 2}, {7, 2}, {8, 2}});
    REQUIRE(beatsOf("third").size() == 8);
    CHECK(beatsOf("third").front() == std::pair<std::uint64_t, std::uint32_t>{1, 3});
    // Every two beats from beat 2 is the backbeat; from beat 1, the ones and threes.
    CHECK(beatsOf("backbeat").size() == 16);
    for (const auto& [b, inBar] : beatsOf("backbeat")) {
        CHECK((inBar == 2 || inBar == 4));
    }
    CHECK(beatsOf("ones").size() == 16);
    for (const auto& [b, inBar] : beatsOf("ones")) {
        CHECK((inBar == 1 || inBar == 3));
    }
    // Every eight from beat 5: the downbeat of every second bar, the even ones.
    CHECK(beatsOf("pairs") == Where{{2, 1}, {4, 1}, {6, 1}, {8, 1}});
    // Every single beat is every beat, whichever it is laid from.
    CHECK(beatsOf("every").size() == 32);
    // A bar of four has no fifth beat.
    CHECK(beatsOf("fifth").empty());
    // Every second bar, on its last beat: bars 1, 3, 5 and 7.
    CHECK(beatsOf("late") == Where{{1, 4}, {3, 4}, {5, 4}, {7, 4}});

    SECTION("and before the tracker has a bar, beats count from the first") {
        Recorder quiet;
        TriggerEngine unmetered(quiet);
        unmetered.setRules({onBeat("backbeat", Trigger::Beat, 2, 2)});
        for (std::uint64_t b = 1; b <= 8; ++b) {
            unmetered.onBeat(beatAt(b, 0, 0, 0.5 * static_cast<double>(b)));
        }
        CHECK(quiet.sent.size() == 4); // beats 1, 3, 5, 7: there is no bar to lay it on
    }
    SECTION("a beat before the first downbeat is placed on the grid it leads into") {
        // A run that starts on beat 3: bar 0, beats 3 and 4, then bar 1. The backbeat is still
        // the 2 and the 4, so the first fire is beat 4 of the bar before the first.
        Recorder early;
        TriggerEngine lead(early);
        lead.setRules({onBeat("backbeat", Trigger::Beat, 2, 2)});
        lead.onBeat(beatAt(1, 3, 0, 0.0));
        CHECK(early.sent.empty());
        lead.onBeat(beatAt(2, 4, 0, 0.5));
        CHECK(early.sent.size() == 1);
        lead.onBeat(beatAt(3, 1, 1, 1.0));
        lead.onBeat(beatAt(4, 2, 1, 1.5));
        CHECK(early.sent.size() == 2);
    }
}

TEST_CASE("a rule's wait holds its fire, and its follow-ups after it", "[trigger][engine][delay]") {
    // The other half of the operator's ask of 2026-10-06: "an optional delay setting default to
    // unticked in the when section, where u can set it to static time in ms or beats / bars".
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config rule = simple("late", Trigger::Manual);
    rule.delayOn = true;
    rule.delayUnit = takt4::trigger::DelayUnit::Beats;
    rule.delayBeats = 0.5; // a quarter of a second at 120
    rule.followUps = {releaseAfterMs(0, 100.0)};
    engine.setRules({rule});

    engine.manual(beatAt(1, 1, 1, 10.0));
    CHECK(sink.sent.empty()); // held
    CHECK(engine.heldFires() == 1);
    CHECK(engine.find("late")->fires() == 1); // it fired: its generators and its count moved

    engine.advance(beatAt(1, 1, 1, 10.2));
    CHECK(sink.sent.empty());
    engine.advance(beatAt(1, 1, 1, 10.25));
    REQUIRE(sink.sent.size() == 1);
    CHECK(sink.arguments() == std::vector<std::int32_t>{1});
    // About its moment plus the wait, so every target hears it the wait late.
    CHECK(sink.sent[0].moment == Approx(10.25));
    // Its release a hundred milliseconds after it, not after the trigger.
    engine.advance(beatAt(1, 1, 1, 10.30));
    CHECK(sink.sent.size() == 1);
    engine.advance(beatAt(1, 1, 1, 10.36));
    REQUIRE(sink.sent.size() == 2);
    CHECK(sink.arguments() == std::vector<std::int32_t>{1, 0});
    CHECK(sink.sent[1].moment == Approx(10.35));

    SECTION("in milliseconds and bars too, and not at all while switched off") {
        Rule::Config ms = simple("ms", Trigger::Manual);
        ms.delayOn = true;
        ms.delayUnit = takt4::trigger::DelayUnit::Milliseconds;
        ms.delaySeconds = 0.12;
        Rule::Config bars = simple("bars", Trigger::Manual);
        bars.delayOn = true;
        bars.delayUnit = takt4::trigger::DelayUnit::Bars;
        bars.delayBeats = 1.0; // four beats of 4/4 at 120: two seconds
        Rule::Config off = simple("off", Trigger::Manual);
        off.delayBeats = 4.0; // set, and not switched on: nothing waits
        Recorder three;
        TriggerEngine waits(three);
        waits.setRules({ms, bars, off});
        waits.manual(beatAt(1, 1, 1, 0.0));
        CHECK(three.addresses() == std::vector<std::string>{"/fire/off"});
        waits.advance(beatAt(1, 1, 1, 0.13));
        CHECK(three.addresses() == std::vector<std::string>{"/fire/off", "/fire/ms"});
        waits.advance(beatAt(1, 1, 1, 1.99));
        CHECK(three.sent.size() == 2);
        waits.advance(beatAt(1, 1, 1, 2.0));
        CHECK(three.addresses() ==
              std::vector<std::string>{"/fire/off", "/fire/ms", "/fire/bars"});
    }
    SECTION("PANIC drops a held fire rather than sending it") {
        engine.manual(beatAt(1, 1, 1, 20.0));
        engine.panic(beatAt(1, 1, 1, 20.1));
        CHECK(engine.heldFires() == 0);
        engine.release();
        engine.advance(beatAt(1, 1, 1, 21.0));
        CHECK(sink.sent.size() == 2); // the two from before, and nothing of this fire
    }
    SECTION("so does a stop") {
        engine.manual(beatAt(1, 1, 1, 20.0));
        engine.flushFollowUps();
        engine.advance(beatAt(1, 1, 1, 21.0));
        CHECK(sink.sent.size() == 2);
    }
    SECTION("and muting its rule while it waits") {
        engine.manual(beatAt(1, 1, 1, 20.0));
        engine.find("late")->setMuted(true);
        const std::uint64_t mutedBefore = engine.muted();
        engine.advance(beatAt(1, 1, 1, 21.0));
        CHECK(sink.sent.size() == 2);
        CHECK(engine.muted() == mutedBefore + 1); // a muted fire, and counted as one
    }
    SECTION("and deleting its rule") {
        engine.manual(beatAt(1, 1, 1, 20.0));
        engine.setRules({});
        CHECK(engine.heldFires() == 0);
        engine.advance(beatAt(1, 1, 1, 21.0));
        CHECK(sink.sent.size() == 2);
    }
    SECTION("but not an edit, which keeps the fire it made") {
        engine.manual(beatAt(1, 1, 1, 20.0));
        Rule::Config renamed = rule;
        renamed.name = "renamed";
        engine.setRules({renamed});
        engine.advance(beatAt(1, 1, 1, 21.0));
        CHECK(sink.sent.size() == 3); // it,
        engine.advance(beatAt(1, 1, 1, 21.001));
        CHECK(sink.sent.size() == 4); // and its release, queued as it went
    }
    SECTION("a test does not wait") {
        CHECK(engine.test("late", beatAt(1, 1, 1, 30.0)));
        CHECK(sink.sent.size() == 3);
    }
}

TEST_CASE("a rule let go of sends what it owes now, and no earlier than its press",
          "[trigger][engine][release]") {
    // `releaseRule`: what the output runner calls for a rule muted, switched off or deleted.
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config note = simple("note", Trigger::Manual);
    note.sendKind = Message::Kind::MidiNote;
    note.number = fixedAt(60);
    note.value = fixedAt(100);
    note.followUps = {releaseAfterMs(0, 2000.0)};
    Rule::Config held = simple("held", Trigger::Manual);
    held.delayOn = true;
    held.delayUnit = takt4::trigger::DelayUnit::Milliseconds;
    held.delaySeconds = 1.0;
    engine.setRules({note, held});

    // A press about a moment ahead of the round — a beat fired early on a prediction.
    Context ahead = beatAt(1, 1, 1, 5.0);
    ahead.moment = 5.03;
    engine.manual(ahead);
    REQUIRE(sink.sent.size() == 1); // the note on; the other is held
    REQUIRE(engine.pending() == 2);

    engine.releaseRule("note", 5.01);
    REQUIRE(sink.sent.size() == 2);
    CHECK(sink.sent[1].kind == Message::Kind::MidiNoteOff);
    // Now — but about no earlier a moment than the note on's, which a target may still hold.
    CHECK(sink.sent[1].moment == Approx(5.03));
    CHECK(engine.pending() == 1);

    engine.releaseRule("held", 5.02);
    CHECK(engine.pending() == 0);
    engine.advance(beatAt(1, 1, 1, 7.0));
    CHECK(sink.sent.size() == 2); // the held fire went nowhere
}

TEST_CASE("a test that nothing else would let go of lets go a moment later",
          "[trigger][engine][release]") {
    // A laser clip or a note on, tested from a rule that is muted or switched off — or with the
    // tracker stopped — has nothing to take it off again. It is taken off half a second later,
    // or a beat if that is longer.
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config note = simple("note", Trigger::Manual);
    note.sendKind = Message::Kind::MidiNote;
    note.number = fixedAt(64);
    note.value = fixedAt(90);
    Rule::Config clip = simple("clip", Trigger::Manual);
    clip.sendKind = Message::Kind::Dmx;
    clip.dmx.effect = takt4::dmx::EffectKind::Clip;
    clip.dmx.fixtures = {"zone"};
    clip.dmx.clip = fixedAt(7);
    engine.setRules({note, clip});

    SECTION("muted") {
        engine.find("note")->setMuted(true);
        engine.find("clip")->setMuted(true);
    }
    SECTION("switched off") {
        engine.find("note")->setEnabled(false);
        engine.find("clip")->setEnabled(false);
    }
    SECTION("the tracker stopped") {
        engine.setListening(false);
    }
    Context context = beatAt(1, 1, 1, 3.0);
    context.bpm = 60.0; // a beat is a second: longer than the half
    REQUIRE(engine.test("note", context));
    REQUIRE(engine.test("clip", context));
    REQUIRE(sink.sent.size() == 2);
    engine.advance(beatAt(1, 1, 1, 3.9));
    CHECK(sink.sent.size() == 2);
    engine.advance(beatAt(1, 1, 1, 4.0));
    REQUIRE(sink.sent.size() == 4);
    CHECK(sink.sent[2].kind == Message::Kind::MidiNoteOff);
    CHECK(sink.sent[2].number == 64);
    CHECK(sink.sent[3].kind == Message::Kind::Dmx);
    CHECK(sink.sent[3].payload.kind == takt4::dmx::EffectKind::Clip);
    CHECK(sink.sent[3].payload.clip == 0);
}

TEST_CASE("a test of a rule that will go on sending is left to it", "[trigger][engine][release]") {
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config note = simple("note", Trigger::Manual);
    note.sendKind = Message::Kind::MidiNote;
    note.number = fixedAt(64);
    engine.setRules({note});
    REQUIRE(engine.test("note", beatAt(1, 1, 1, 3.0)));
    engine.advance(beatAt(1, 1, 1, 10.0));
    CHECK(sink.sent.size() == 1); // its own rule lets go of it, or does not, as it was built
}

TEST_CASE("a length in beats is counted in the beats going out, not in the number on screen",
          "[trigger][engine][relock]") {
    // A number left at 188 where a lost lock put it, over a record whose beats go out at 94
    // (`Context::beatSeconds`): "release after one beat" used to last sixty over 188 — half a
    // beat of the music the rule fires on. And a ×2 is the same: it doubles the number, and the
    // beats a rule fires on go on at the record's tempo.
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config config = simple("clip", Trigger::Beat);
    takt4::trigger::FollowUp release = releaseAfterMs(0, 0.0);
    release.unit = takt4::trigger::DelayUnit::Beats;
    release.delayBeats = 1.0;
    config.followUps.push_back(release);
    engine.setRules({config});

    Context context = beatAt(1, 1, 1, 10.0);
    context.bpm = 188.0;
    context.beatSeconds = 60.0 / 94.0;
    engine.onBeat(context);
    REQUIRE(sink.sent.size() == 1);
    context.now = 10.0 + 60.0 / 188.0 + 0.01; // where sixty over the number would have put it
    engine.advance(context);
    CHECK(sink.sent.size() == 1);
    context.now = 10.0 + 60.0 / 94.0 + 0.001;
    engine.advance(context);
    CHECK(sink.sent.size() == 2);

    SECTION("and a bar is the meter's worth of those beats") {
        CHECK(takt4::trigger::musicalSeconds(context, takt4::trigger::DelayUnit::Bars, 0.0, 1.0) ==
              Approx(4.0 * 60.0 / 94.0));
        context.beatSeconds = 0.0; // a context that does not say: the number, as before
        CHECK(takt4::trigger::musicalSeconds(context, takt4::trigger::DelayUnit::Beats, 0.0, 1.0) ==
              Approx(60.0 / 188.0));
    }
}

TEST_CASE("the test button holds a note for a beat of the music, not of the number",
          "[trigger][engine][relock]") {
    // TEST holds a note or a clip for a beat, or half a second if that is longer, so the operator
    // sees it land. The beat is the music's: at 188 on screen and 94 going out, a quarter of a
    // second of hold would have been cut short at the half-second floor.
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config config = simple("note", Trigger::Manual);
    config.sendKind = Message::Kind::MidiNote;
    config.number = fixedAt(36);
    config.value = fixedAt(100);
    config.enabled = false; // being built: nothing else will let go of what TEST sends
    engine.setRules({config});
    REQUIRE(engine.rule(0).valid());

    Context context = beatAt(1, 1, 1, 5.0);
    context.bpm = 188.0;
    context.beatSeconds = 60.0 / 94.0;
    REQUIRE(engine.test("note", context));
    REQUIRE(sink.sent.size() == 1);
    context.now = 5.0 + TriggerEngine::kTestHoldSeconds + 0.01;
    engine.advance(context);
    CHECK(sink.sent.size() == 1); // half a second is less than a beat at 94
    context.now = 5.0 + 60.0 / 94.0 + 0.001;
    engine.advance(context);
    REQUIRE(sink.sent.size() == 2);
    CHECK(sink.sent[1].kind == Message::Kind::MidiNoteOff);
}
