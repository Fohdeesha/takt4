#include "core/settings/rule_json.hpp"
#include "core/settings/settings.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/rule.hpp"
#include "core/trigger/trigger_engine.hpp"
#include "core/trigger/value.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
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
    rule.conditions.minConfidence = 0.7;
    rule.conditions.intensities = {false, true, true}; // not while it is calm
    rule.conditions.minBpm = 120.0;
    rule.conditions.maxBpm = 140.0;
    rule.conditions.probability = 0.9;
    rule.conditions.cooldownSeconds = 0.5;
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

    CHECK(out.conditions.minConfidence == Approx(0.7));
    CHECK(out.conditions.minBpm == Approx(120.0));
    CHECK(out.conditions.maxBpm == Approx(140.0));
    CHECK(out.conditions.probability == Approx(0.9));
    CHECK(out.conditions.cooldownSeconds == Approx(0.5));
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
