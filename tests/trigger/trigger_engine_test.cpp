#include "core/trigger/trigger_engine.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using Catch::Approx;
using takt4::trigger::Context;
using takt4::trigger::Generator;
using takt4::trigger::GeneratorKind;
using takt4::trigger::Intensity;
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
    config.followUp = true;
    config.followUpValue = Value::ofInt(0);
    config.followUpDelaySeconds = 0.05;
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

    SECTION("overlapping follow-ups come out in the order they were queued") {
        Recorder many;
        TriggerEngine overlapping(many);
        config.followUpDelaySeconds = 1.2; // longer than the gap between beats
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
    config.followUp = true;
    config.followUpValue = Value::ofInt(0);
    config.followUpDelaySeconds = 10.0; // a long way off, so panic has something to owe
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

TEST_CASE("replacing the rules pays out the old ones' follow-ups", "[trigger][engine]") {
    // The same argument panic makes: a clip pressed by the preset being replaced would stay
    // latched on if its release went with the rule that owed it.
    Recorder sink;
    TriggerEngine engine(sink);
    Rule::Config config = simple("clip", Trigger::Beat);
    config.followUp = true;
    config.followUpDelaySeconds = 5.0;
    engine.setRules({config});
    engine.onBeat(beatAt(1, 1, 1, 0.0));
    REQUIRE(engine.pending() == 1);

    engine.setRules({simple("other", Trigger::Beat)});
    CHECK(engine.pending() == 0);
    REQUIRE(sink.sent.size() == 2);
    CHECK(sink.sent[1].argument.asInt() == 0);
    CHECK(engine.ruleCount() == 1);
    CHECK(engine.find("clip") == nullptr);
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
