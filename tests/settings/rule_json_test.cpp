#include "core/settings/rule_json.hpp"
#include "core/settings/settings.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/rule.hpp"
#include "core/trigger/trigger_engine.hpp"
#include "core/trigger/value.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using Catch::Approx;
using takt4::features::Intensity;
using takt4::settings::rulesFromJson;
using takt4::settings::rulesToJson;
using takt4::trigger::Generator;
using takt4::trigger::GeneratorKind;
using takt4::trigger::Message;
using takt4::trigger::Pool;
using takt4::trigger::Rule;
using takt4::trigger::Trigger;
using takt4::trigger::Value;

namespace {

/// §5.9's own example, and Phase 6's exit criterion: "a rule that fires a non-repeating
/// random clip on every fourth downbeat". Everything a preset has to carry is in it.
Rule::Config resolumeClip() {
    Rule::Config rule;
    rule.id = "drop";
    rule.name = "Random clip on downbeat";
    rule.trigger = Trigger::Bar;
    rule.every = 4;
    rule.conditionsOn = true;
    rule.conditions.minConfidence = 0.7;
    rule.conditions.intensities = {false, true, true}; // not while it is calm
    rule.conditions.minBpm = 120.0;
    rule.conditions.maxBpm = 140.0;
    rule.conditions.probability = 0.9;
    // Ignored on a bar rule, and kept: switched to an onset, it applies again.
    rule.cooldownSeconds = 0.5;
    rule.address = "/composition/layers/{L}/clips/{C}/connect";

    Generator::Config layer;
    layer.kind = GeneratorKind::Fixed;
    layer.fixed = Value::ofInt(3);
    Generator::Config clip;
    clip.kind = GeneratorKind::Shuffle;
    clip.pool = Pool::List;
    clip.values = {Value::ofInt(3), Value::ofInt(7), Value::ofInt(1), Value::ofInt(12)};
    clip.noRepeatWithin = 2;
    rule.segments = {layer, clip};

    rule.value.kind = GeneratorKind::Fixed;
    rule.value.fixed = Value::ofInt(1);
    // §5.6's press-then-release: without the second half a clip stays latched on.
    takt4::trigger::FollowUp release;
    release.value = Value::ofInt(0);
    release.delaySeconds = 0.05;
    rule.followUps.push_back(release);
    rule.seed = 4242;
    return rule;
}

Rule::Config first(const std::string& text) {
    const std::vector<Rule::Config> back = rulesFromJson(text);
    REQUIRE(back.size() == 1);
    return back.front();
}

} // namespace

TEST_CASE("a rule survives being written down and read back", "[settings][trigger]") {
    const Rule::Config in = resolumeClip();
    const Rule::Config out = first(rulesToJson({in}));

    CHECK(out.id == "drop");
    CHECK(out.name == in.name);
    CHECK(out.enabled);
    CHECK(out.trigger == Trigger::Bar);
    CHECK(out.every == 4);
    CHECK(out.address == in.address);
    CHECK(out.sendKind == Message::Kind::Osc);
    CHECK(out.seed == 4242);

    CHECK(out.conditionsOn);
    CHECK(out.conditions.minConfidence == Approx(0.7));
    CHECK(out.conditions.minBpm == Approx(120.0));
    CHECK(out.conditions.maxBpm == Approx(140.0));
    CHECK(out.conditions.probability == Approx(0.9));
    CHECK(out.cooldownSeconds == Approx(0.5));
    CHECK_FALSE(out.conditions.allows(Intensity::Calm));
    CHECK(out.conditions.allows(Intensity::Normal));
    CHECK(out.conditions.allows(Intensity::Intense));

    REQUIRE(out.segments.size() == 2);
    CHECK(out.segments[0].kind == GeneratorKind::Fixed);
    CHECK(out.segments[0].fixed.asInt() == 3);
    CHECK(out.segments[1].kind == GeneratorKind::Shuffle);
    CHECK(out.segments[1].pool == Pool::List);
    CHECK(out.segments[1].noRepeatWithin == 2);
    REQUIRE(out.segments[1].values.size() == 4);
    CHECK(out.segments[1].values[0].asInt() == 3);
    CHECK(out.segments[1].values[3].asInt() == 12);

    REQUIRE(out.followUps.size() == 1);
    CHECK_FALSE(out.followUps[0].kind.has_value()); // a release, which is what none means
    CHECK(out.followUps[0].value.asInt() == 0);
    CHECK(out.followUps[0].delaySeconds == Approx(0.05));

    SECTION("and what comes back fires the same messages as what went in") {
        // The claim that actually matters. Every field above could round-trip and the rule
        // still behave differently — a seed read as a default, a pool ignored — so this
        // runs both through the real engine and compares what reached the sink.
        struct Recording final : takt4::trigger::Sink {
            void send(const Message& message) override { sent.push_back(message.address); }
            std::vector<std::string> sent;
        };

        const auto fire = [](const Rule::Config& config, Recording& sink) {
            takt4::trigger::TriggerEngine engine(sink);
            engine.setRules({config});
            takt4::trigger::Context context;
            context.bpm = 130.0;
            context.confidence = 1.0;
            context.meter = 4;
            for (int bar = 1; bar <= 40; ++bar) {
                context.bars = static_cast<std::uint64_t>(bar);
                context.beatInBar = 1;
                context.beats = static_cast<std::uint64_t>(bar) * 4;
                context.now = bar * 2.0; // past the cooldown every time
                engine.onBeat(context);
            }
        };

        Recording before;
        Recording after;
        fire(in, before);
        fire(out, after);
        CHECK_FALSE(before.sent.empty());
        CHECK(before.sent == after.sent);
    }
}

TEST_CASE("every kind of value keeps its type through the file", "[settings][trigger]") {
    // Written as the JSON type it is — 7, 0.5, "intro", true — rather than as a tagged
    // object, because a preset is a file a person may hand-write (Q8). The risk that buys
    // is a float coming back as an int, so each is checked for its kind and not its digits.
    Rule::Config rule;
    rule.id = "types";
    rule.value.kind = GeneratorKind::Cycle;
    rule.value.pool = Pool::List;
    rule.value.values = {Value::ofInt(7), Value::ofFloat(0.5f), Value::ofText("intro"),
                         Value::ofBool(true)};

    const Rule::Config out = first(rulesToJson({rule}));
    REQUIRE(out.value.values.size() == 4);
    CHECK(out.value.values[0].kind() == Value::Kind::Int);
    CHECK(out.value.values[0].asInt() == 7);
    CHECK(out.value.values[1].kind() == Value::Kind::Float);
    CHECK(out.value.values[1].asFloat() == Approx(0.5f));
    CHECK(out.value.values[2].kind() == Value::Kind::Text);
    CHECK(out.value.values[2].text() == "intro");
    CHECK(out.value.values[3].kind() == Value::Kind::Bool);
    CHECK(out.value.values[3].asBool());
}

TEST_CASE("a MIDI rule carries what a MIDI rule needs", "[settings][trigger]") {
    Rule::Config rule;
    rule.id = "stab";
    rule.sendKind = Message::Kind::MidiNote;
    rule.channel = 10;
    rule.number.kind = GeneratorKind::Shuffle;
    rule.number.low = 36;
    rule.number.high = 43;
    rule.value.kind = GeneratorKind::Fixed;
    rule.value.fixed = Value::ofInt(127);

    const Rule::Config out = first(rulesToJson({rule}));
    CHECK(out.sendKind == Message::Kind::MidiNote);
    CHECK(out.channel == 10);
    CHECK(out.number.low == 36);
    CHECK(out.number.high == 43);
    CHECK(out.value.fixed.asInt() == 127);
    // And it does not carry an OSC address it would never use.
    CHECK(out.address.empty());
    CHECK(out.segments.empty());
    CHECK(out.numberChosen);
}

TEST_CASE("a MIDI rule still waiting for its number is still waiting after a restart",
          "[settings][trigger]") {
    // The audit's C7. Saved with its generator's default, a half-built rule came back armed on
    // a shuffle over 1 to 8.
    Rule::Config rule;
    rule.id = "stab";
    rule.sendKind = Message::Kind::MidiCc;
    rule.numberChosen = false;

    const std::string written = rulesToJson({rule});
    INFO(written);
    const std::size_t key = written.find("\"number\"");
    REQUIRE(key != std::string::npos);
    const std::size_t value = written.find_first_not_of(": \t\r\n", key + 8);
    CHECK(written.compare(value, 4, "null") == 0);

    const Rule::Config out = first(written);
    CHECK_FALSE(out.numberChosen);
    CHECK(out.number.kind == GeneratorKind::Fixed);
    CHECK(Rule(out).problem() == "choose a controller number");

    SECTION("and a file from before the flag reads as chosen") {
        std::string old = written;
        old.replace(key, 8, "\"unknown\""); // a key this build does not read
        CHECK(first(old).numberChosen);
    }
}

TEST_CASE("a rule switched to another kind and saved keeps the half it is not using",
          "[settings][trigger]") {
    // The audit's M22. `Rule::Config::dmx` promises a rule switched to MIDI and back gets its
    // fixtures and fade back; only the kind in force was written, so a SAVE in between lost
    // the other half for good.
    Rule::Config rule;
    rule.id = "wash";
    rule.sendKind = Message::Kind::Dmx;
    rule.dmx.effect = takt4::dmx::EffectKind::Pulse;
    rule.dmx.fixtures = {"stage left"};
    rule.dmx.cycles = 3.0;

    SECTION("lighting tried as MIDI") {
        rule.sendKind = Message::Kind::MidiNote;
        rule.numberChosen = false; // as the editor leaves a rule just switched to MIDI
        Rule::Config back = first(rulesToJson({rule}));
        CHECK(back.sendKind == Message::Kind::MidiNote);
        CHECK_FALSE(back.numberChosen);
        back.sendKind = Message::Kind::Dmx;
        CHECK(back.dmx.effect == takt4::dmx::EffectKind::Pulse);
        CHECK(back.dmx.fixtures == std::vector<std::string>{"stage left"});
        CHECK(back.dmx.cycles == Approx(3.0));

        SECTION("and a MIDI number nobody chose stays unchosen through OSC too") {
            back.sendKind = Message::Kind::Osc;
            back.address = "/x";
            Rule::Config again = first(rulesToJson({back}));
            CHECK_FALSE(again.numberChosen);
        }
    }
    SECTION("MIDI tried as OSC") {
        Rule::Config midi;
        midi.id = "stab";
        midi.sendKind = Message::Kind::MidiCc;
        midi.channel = 10;
        midi.number.kind = GeneratorKind::Fixed;
        midi.number.fixed = Value::ofInt(74);
        midi.sendKind = Message::Kind::Osc;
        midi.address = "/composition/tempo";
        const Rule::Config back = first(rulesToJson({midi}));
        CHECK(back.address == "/composition/tempo");
        CHECK(back.channel == 10);
        CHECK(back.number.fixed.asInt() == 74);
    }
    SECTION("OSC tried as lighting") {
        Rule::Config osc = resolumeClip();
        osc.sendKind = Message::Kind::Dmx;
        const Rule::Config back = first(rulesToJson({osc}));
        CHECK(back.address == "/composition/layers/{L}/clips/{C}/connect");
        CHECK(back.segments.size() == 2);
    }
}

TEST_CASE("a rule tried as another trigger, effect or generator and saved keeps what it set",
          "[settings][trigger]") {
    // Only what the trigger, the effect or the generator in force read was written: a rule on
    // every 4 bars tried as an onset came back on every bar, a palette tried as a strobe came back
    // white, and a list built for a shuffle tried as a ramp came back empty.
    SECTION("a trigger") {
        Rule::Config rule = resolumeClip();
        rule.trigger = takt4::trigger::Trigger::Bar;
        rule.every = 4;
        rule.onBeat = 3;
        rule.trigger = takt4::trigger::Trigger::Onset;
        const Rule::Config back = first(rulesToJson({rule}));
        CHECK(back.every == 4);
        CHECK(back.onBeat == 3);
    }
    SECTION("an effect") {
        Rule::Config rule;
        rule.id = "wash";
        rule.sendKind = Message::Kind::Dmx;
        rule.dmx.fixtures = {"stage left"};
        rule.dmx.effect = takt4::dmx::EffectKind::Path;
        rule.dmx.shape = takt4::dmx::PathShape::Figure8;
        rule.dmx.size = 0.8;
        rule.dmx.color.kind = GeneratorKind::Cycle;
        rule.dmx.color.pool = takt4::trigger::Pool::List;
        rule.dmx.color.values = {Value::ofText("#ff0000"), Value::ofText("#0000ff")};
        rule.dmx.effect = takt4::dmx::EffectKind::Strobe;
        const Rule::Config back = first(rulesToJson({rule}));
        CHECK(back.dmx.effect == takt4::dmx::EffectKind::Strobe);
        CHECK(back.dmx.shape == takt4::dmx::PathShape::Figure8);
        CHECK(back.dmx.size == Approx(0.8));
        CHECK(back.dmx.color.kind == GeneratorKind::Cycle);
        CHECK(back.dmx.color.values.size() == 2);
    }
    SECTION("a generator") {
        Rule::Config rule = resolumeClip();
        rule.value.kind = GeneratorKind::Shuffle;
        rule.value.pool = takt4::trigger::Pool::List;
        rule.value.values = {Value::ofInt(3), Value::ofInt(7), Value::ofInt(1)};
        rule.value.kind = GeneratorKind::Ramp;
        const Rule::Config back = first(rulesToJson({rule}));
        CHECK(back.value.kind == GeneratorKind::Ramp);
        CHECK(back.value.pool == takt4::trigger::Pool::List);
        CHECK(back.value.values.size() == 3);
    }
}

TEST_CASE("a rule that never used another kind does not write that kind's defaults",
          "[settings][trigger]") {
    // The other side of keeping every half: a preset says what was chosen. An OSC rule that was
    // never anything else writes no lighting block and no MIDI channel.
    const std::string written = rulesToJson({resolumeClip()});
    INFO(written);
    CHECK(written.find("\"dmx\"") == std::string::npos);
    CHECK(written.find("\"channel\"") == std::string::npos);
    CHECK(written.find("\"number\"") == std::string::npos);
}

TEST_CASE("a movement rule keeps the heads it moves and their spread, and writes them only when "
          "it names some",
          "[settings][trigger][heads]") {
    Rule::Config rule;
    rule.id = "left";
    rule.sendKind = Message::Kind::Dmx;
    rule.dmx.effect = takt4::dmx::EffectKind::Path;
    rule.dmx.fixtures = {"f-twin"};
    rule.dmx.heads = 0b101; // heads 1 and 3
    rule.dmx.spread = 0.25;
    const std::string written = rulesToJson({rule});
    INFO(written);
    REQUIRE(written.find("\"heads\"") != std::string::npos);
    const std::vector<Rule::Config> back = rulesFromJson(written);
    REQUIRE(back.size() == 1);
    CHECK(back[0].dmx.heads == 0b101);
    CHECK(back[0].dmx.spread == Approx(0.25));

    SECTION("every head together, the default, writes nothing: a file saves as it did") {
        Rule::Config every = rule;
        every.dmx.heads = 0;
        every.dmx.spread = 0.0;
        const std::string text = rulesToJson({every});
        CHECK(text.find("\"heads\"") == std::string::npos);
        CHECK(text.find("\"spread\"") == std::string::npos);
    }
    SECTION("and a rule tried as an effect that moves nothing keeps them for when it moves again") {
        // Every half an operator filled in is kept, whichever the effect in force (the heads were
        // written only for a movement, so a path tried as a fade and saved lost them).
        Rule::Config fade = rule;
        fade.dmx.effect = takt4::dmx::EffectKind::Level;
        const std::vector<Rule::Config> again = rulesFromJson(rulesToJson({fade}));
        REQUIRE(again.size() == 1);
        CHECK(again[0].dmx.heads == 0b101);
        CHECK(again[0].dmx.spread == Approx(0.25));
    }
    SECTION("a hand-edited list keeps the heads that are heads, and nothing else") {
        // The value after "heads": swapped for what a person might type.
        const std::size_t key = written.find("\"heads\"");
        const std::size_t colon = written.find(':', key);
        const std::size_t close = written.find(']', colon);
        const auto with = [&](const char* typed) {
            const std::vector<Rule::Config> read = rulesFromJson(
                written.substr(0, colon + 1) + " " + typed + written.substr(close + 1));
            REQUIRE(read.size() == 1);
            return read[0].dmx.heads;
        };
        for (const char* typed : {"[1e20]", "[-1, 0, 33]", "[\"2\"]", "[2.5]", "null", "7",
                                  "[18446744073709551615]", "{}"}) {
            INFO("heads: " << typed);
            CHECK(with(typed) == 0);
        }
        CHECK(with("[1, 3, 3]") == 0b101);
        CHECK(with("[32]") == std::uint32_t{1} << 31);
        CHECK(with("[2, \"x\", 4]") == 0b1010);
    }
    SECTION("and a spread that is not one is none, or as far as one goes") {
        const std::size_t key = written.find("\"spread\"");
        const std::size_t colon = written.find(':', key);
        const std::size_t end = written.find_first_of(",\n}", colon);
        const auto with = [&](const char* typed) {
            const std::vector<Rule::Config> read =
                rulesFromJson(written.substr(0, colon + 1) + " " + typed + written.substr(end));
            REQUIRE(read.size() == 1);
            return read[0].dmx.spread;
        };
        CHECK(with("\"half\"") == 0.0);
        CHECK(with("-1") == 0.0);
        CHECK(with("1e20") == 1.0);
        CHECK(with("null") == 0.0);
        CHECK(with("0.5") == Approx(0.5));
    }
}

TEST_CASE("a rule file a person edited still opens", "[settings][trigger]") {
    // The contract `settings::load` sets and this has to keep: reading never fails.
    SECTION("nonsense is an empty rule set, not a throw") {
        for (const char* text : {"", "   ", "not json", "{}", "null", "7", "[[1,2]]"}) {
            INFO("input: " << text);
            CHECK(rulesFromJson(text).empty());
        }
    }

    SECTION("one unreadable entry does not cost the others") {
        const std::vector<Rule::Config> back =
            rulesFromJson(R"([{"id":"a"}, 7, "nope", {"id":"b"}])");
        REQUIRE(back.size() == 2);
        CHECK(back[0].id == "a");
        CHECK(back[1].id == "b");
    }

    SECTION("a field of the wrong type keeps its default") {
        const Rule::Config out = first(R"([{"id":"x","every":"four","enabled":"yes"}])");
        CHECK(out.id == "x");
        CHECK(out.every == 1);
        CHECK(out.enabled);
    }

    SECTION("a name this build does not know keeps its default") {
        // What lets a file written by a later build still load: an unknown trigger is not a
        // reason to drop the rest of the rule.
        const Rule::Config out =
            first(R"([{"id":"x","trigger":"on-eclipse","send":"smoke-signal"}])");
        CHECK(out.id == "x");
        CHECK(out.trigger == Trigger::Bar); // the default
        CHECK(out.sendKind == Message::Kind::Osc);
    }

    SECTION("a rule that will not fire is kept, not dropped") {
        // §5.8's policy, and the opposite of a generator's: an invalid rule is held, shown
        // and refused at fire time. A preset that silently lost the rule an operator is
        // half-way through fixing would be the worst of both.
        const Rule::Config out = first(R"([{"id":"broken","address":"/a/{x}/b"}])");
        CHECK(out.id == "broken");
        const Rule built(out);
        CHECK_FALSE(built.valid()); // one placeholder, no segments
        CHECK_FALSE(built.problem().empty());
    }
}

TEST_CASE("a rule file written before follow-ups were a list still opens", "[settings][trigger]") {
    // An operator's settings file is their rig, and upgrading takt4 must not empty it. Until
    // 2026-09-12 a rule carried one follow-up as four loose fields; it is a list now, and the
    // old spelling is read as the single **release** it always meant.
    SECTION("the old fields become one release") {
        const Rule::Config out = first(R"([{
            "id": "clip",
            "address": "/composition/layers/3/clips/2/connect",
            "followUp": true,
            "followUpValue": 0,
            "followUpDelaySeconds": 0.05,
            "followUpUnit": "beats",
            "followUpDelayBeats": 2.0
        }])");
        REQUIRE(out.followUps.size() == 1);
        CHECK_FALSE(out.followUps[0].kind.has_value()); // a release, which is what none means
        CHECK(out.followUps[0].value.asInt() == 0);
        CHECK(out.followUps[0].delaySeconds == Approx(0.05));
        CHECK(out.followUps[0].unit == takt4::trigger::DelayUnit::Beats);
        CHECK(out.followUps[0].delayBeats == Approx(2.0));
    }

    SECTION("and `followUp: false` is a rule that sends once") {
        CHECK(first(R"([{"id":"x","followUp":false,"followUpValue":0}])").followUps.empty());
    }

    SECTION("a new file is not read twice") {
        // Both spellings in one entry — which only a hand-edit produces — takes the list and
        // leaves the legacy fields alone, or the rule would send its release twice.
        const Rule::Config out = first(R"([{
            "id": "x",
            "followUp": true,
            "followUpValue": 0,
            "followUps": [{"value": 3, "delaySeconds": 0.25}]
        }])");
        REQUIRE(out.followUps.size() == 1);
        CHECK(out.followUps[0].value.asInt() == 3);
    }

    SECTION("an entry that names a kind keeps it, and a number with it") {
        const Rule::Config out = first(R"([{
            "id": "x",
            "send": "midi-note",
            "followUps": [{"send": "midi-cc", "number": 21, "value": 64, "delayBeats": 1}]
        }])");
        REQUIRE(out.followUps.size() == 1);
        REQUIRE(out.followUps[0].kind.has_value());
        CHECK(*out.followUps[0].kind == Message::Kind::MidiCc);
        CHECK(out.followUps[0].number == 21);
    }

    SECTION("a kind this build has never heard of reads as a release, not as a lost row") {
        // A rule that has quietly lost its release is a clip that stays held, which is the
        // worst failure this file has. See `FollowUp::kind`.
        const Rule::Config out =
            first(R"([{"id":"x","followUps":[{"send":"smoke-signal","value":0}]}])");
        REQUIRE(out.followUps.size() == 1);
        CHECK_FALSE(out.followUps[0].kind.has_value());
    }

    SECTION("a hand-edited file cannot make one fire queue thousands of messages") {
        std::string many = R"([{"id":"x","followUps":[)";
        for (int i = 0; i < 400; ++i) {
            many += i == 0 ? "" : ",";
            many += R"({"value":0})";
        }
        many += "]}]";
        CHECK(first(many).followUps.size() == takt4::trigger::kMaxFollowUps);
    }
}

TEST_CASE("a rule file written before B had a switch fires as it did", "[settings][trigger]") {
    // HANDOFF §0.5, agreed 2026-09-30: the ONLY IF stage has an on/off tick now, off for a new
    // rule. A file from before it holds no switch, and upgrading must not change what fires — so
    // a rule that used any of the four conditions loads with them on, and one that used none
    // loads with them off, as a new rule starts. And the cooldown, which was one of them, is
    // read from where that build wrote it.
    SECTION("a rule that used a condition loads with them on") {
        const Rule::Config out = first(R"([{
            "id": "gated",
            "trigger": "onset",
            "conditions": {"minConfidence": 0.7, "intensities": ["calm","normal","intense"],
                           "minBpm": 0, "maxBpm": 1000, "probability": 1,
                           "cooldownSeconds": 0.25}
        }])");
        CHECK(out.conditionsOn);
        CHECK(out.conditions.minConfidence == Approx(0.7));
        CHECK(out.cooldownSeconds == Approx(0.25));
    }

    SECTION("each of the four counts as using one") {
        for (const char* conditions :
             {R"({"intensities": ["normal","intense"]})", R"({"minBpm": 120})",
              R"({"maxBpm": 140})", R"({"probability": 0.5})"}) {
            INFO(conditions);
            CHECK(first(std::string(R"([{"id":"x","conditions":)") + conditions + "}]")
                      .conditionsOn);
        }
    }

    SECTION("a rule that used none loads with them off, its cooldown kept") {
        const Rule::Config out = first(R"([{
            "id": "open",
            "trigger": "manual",
            "conditions": {"minConfidence": 0, "intensities": ["calm","normal","intense"],
                           "minBpm": 0, "maxBpm": 1000, "probability": 1,
                           "cooldownSeconds": 2}
        }])");
        CHECK_FALSE(out.conditionsOn);
        CHECK(out.cooldownSeconds == Approx(2.0));
    }

    SECTION("a file written since keeps the switch it was saved with") {
        // Set for later with B off: the conditions are there and the switch says they do not
        // apply, which the migration must not second-guess.
        Rule::Config later = resolumeClip();
        later.conditionsOn = false;
        const Rule::Config out = first(rulesToJson({later}));
        CHECK_FALSE(out.conditionsOn);
        CHECK(out.conditions.minConfidence == Approx(0.7));
    }

    SECTION("the cooldown is written where an older build looks for it too") {
        // An older takt4 given this file keeps the cooldown on the onset rules it was set for.
        Rule::Config onset = resolumeClip();
        onset.trigger = Trigger::Onset;
        onset.cooldownSeconds = 0.3;
        const std::string text = rulesToJson({onset});
        INFO(text);
        // Twice: at the top of the rule, where this build reads it, and inside the conditions.
        // (Not "after the conditions key": the writer sorts its keys, so the top-level one
        // comes after that too, and a first version of this passed with the second removed.)
        std::size_t written = 0;
        for (std::size_t at = text.find("\"cooldownSeconds\""); at != std::string::npos;
             at = text.find("\"cooldownSeconds\"", at + 1)) {
            ++written;
        }
        CHECK(written == 2);
        CHECK(first(text).cooldownSeconds == Approx(0.3));
    }

    SECTION("a cooldown that is not a length of time is no cooldown") {
        CHECK(first(R"([{"id":"x","cooldownSeconds":-3}])").cooldownSeconds == 0.0);
        CHECK(first(R"([{"id":"x","cooldownSeconds":"soon"}])").cooldownSeconds == 0.0);
    }
}

TEST_CASE("rules travel in the preset half of the settings file", "[settings][trigger]") {
    // Q7: a rule is about the show, not about the box it is running on.
    takt4::settings::Settings in;
    in.preset.rules = {resolumeClip()};

    const std::string text = takt4::settings::toJson(in);
    const std::size_t machine = text.find("\"machine\"");
    const std::size_t preset = text.find("\"preset\"");
    REQUIRE(machine != std::string::npos);
    REQUIRE(preset != std::string::npos);
    CHECK(text.find("\"rules\"", preset) != std::string::npos);
    CHECK(text.find("\"rules\"", machine) > preset);

    const takt4::settings::Settings out = takt4::settings::fromJson(text);
    REQUIRE(out.preset.rules.size() == 1);
    CHECK(out.preset.rules[0].id == "drop");
    CHECK(out.preset.rules[0].segments.size() == 2);

    SECTION("and a settings file with no rules in it is a settings file with no rules") {
        CHECK(takt4::settings::fromJson(R"({"preset":{}})").preset.rules.empty());
    }
}

TEST_CASE("a Liberation clip rule keeps its clips and its intensity through the file",
          "[settings][trigger][liberation]") {
    Rule::Config rule;
    rule.id = "laser1";
    rule.sendKind = Message::Kind::Dmx;
    rule.dmx.effect = takt4::dmx::EffectKind::Clip;
    rule.dmx.fixtures = {"f-1"};
    rule.dmx.clip = takt4::trigger::clipShuffle({2, 0}, {30, 4});
    rule.dmx.clip.noRepeatWithin = 3;
    rule.dmx.level = takt4::trigger::fixedNumber(200);

    const std::string written = rulesToJson({rule});
    const Rule::Config back = first(written);
    CHECK(back.dmx.effect == takt4::dmx::EffectKind::Clip);
    CHECK(back.dmx.clip.kind == GeneratorKind::Shuffle);
    CHECK(back.dmx.clip.low == 10);   // 2-0
    CHECK(back.dmx.clip.high == 154); // 30-4
    CHECK(back.dmx.clip.noRepeatWithin == 3);
    CHECK(back.dmx.level.fixed.asInt() == 200);
    CHECK(back.dmx.fixtures == std::vector<std::string>{"f-1"});
    // Spelled as a file holds it: the effect's own name, which `effectKindOf` reads back.
    CHECK(written.find("\"liberation-clip\"") != std::string::npos);
}

TEST_CASE("a rule keeps which beat of the bar it fires on, and its wait", "[settings][trigger]") {
    Rule::Config rule = resolumeClip();
    rule.trigger = Trigger::Bar;
    rule.every = 2;
    rule.onBeat = 3;
    rule.delayOn = true;
    rule.delayUnit = takt4::trigger::DelayUnit::Milliseconds;
    rule.delaySeconds = 0.12;
    rule.delayBeats = 0.25;
    const std::string text = rulesToJson({rule});
    const Rule::Config out = first(text);
    CHECK(out.onBeat == 3);
    CHECK(out.delayOn);
    CHECK(out.delayUnit == takt4::trigger::DelayUnit::Milliseconds);
    CHECK(out.delaySeconds == Approx(0.12));
    CHECK(out.delayBeats == Approx(0.25)); // the other unit's number, kept for a switch back

    SECTION("a wait switched off is kept, for when it is ticked again") {
        rule.delayOn = false;
        const Rule::Config off = first(rulesToJson({rule}));
        CHECK_FALSE(off.delayOn);
        CHECK(off.delaySeconds == Approx(0.12));
    }
    SECTION("an untouched rule writes neither") {
        Rule::Config plain;
        plain.id = "plain";
        plain.trigger = Trigger::Onset; // no beat of the bar to write
        const std::string written = rulesToJson({plain});
        CHECK(written.find("onBeat") == std::string::npos);
        CHECK(written.find("delay\"") == std::string::npos);
        const Rule::Config back = first(written);
        CHECK(back.onBeat == 1);
        CHECK_FALSE(back.delayOn);
    }
    SECTION("a file from before either loads as it fired") {
        const Rule::Config old = first(R"([{"id":"x","trigger":"bar","every":4}])");
        CHECK(old.onBeat == 1);
        CHECK_FALSE(old.delayOn);
    }
    SECTION("nonsense in a hand-edited file is clamped or refused") {
        const Rule::Config odd = first(
            R"([{"id":"x","trigger":"bar","onBeat":99,"delay":{"on":true,"unit":"fortnights","delaySeconds":-4,"delayBeats":"lots"}}])");
        CHECK(odd.onBeat == takt4::trigger::kMaxBeatOfBar);
        CHECK(odd.delayOn);
        CHECK(odd.delayUnit == takt4::trigger::DelayUnit::Beats); // the default
        CHECK(odd.delaySeconds == Approx(Rule::Config{}.delaySeconds));
        CHECK(odd.delayBeats == Approx(Rule::Config{}.delayBeats));
        const Rule::Config zero = first(R"([{"id":"x","trigger":"bar","onBeat":0}])");
        CHECK(zero.onBeat == 1);
    }
}
