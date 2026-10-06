#include "core/dmx/dmx_engine.hpp"
#include "core/dmx/effect.hpp"
#include "core/dmx/fixture.hpp"
#include "core/dmx/liberation.hpp"
#include "core/output/rule_sink.hpp"
#include "core/output/transports.hpp"
#include "core/trigger/rule.hpp"
#include "core/trigger/trigger_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace liberation = takt4::dmx::liberation;
using liberation::Clip;
using takt4::dmx::DmxEngine;
using takt4::dmx::EffectKind;
using takt4::dmx::Fixture;
using takt4::dmx::Payload;
using takt4::dmx::Role;

namespace {

std::uint8_t at(const DmxEngine& engine, takt4::dmx::PortAddress universe, int channel) {
    const std::span<const std::uint8_t> levels = engine.levels(universe);
    REQUIRE(!levels.empty());
    return levels[static_cast<std::size_t>(channel) - 1];
}

/// Channel `n` (1-based, as Liberation's document numbers them) of the zone patched at `address`.
std::uint8_t zoneChannel(const DmxEngine& engine, std::uint16_t address, int n) {
    return at(engine, 0, address + n - 1);
}

/// What Liberation would render from the zone at `address`: nothing unless it is armed, lit and
/// has a clip — its document's three conditions — and otherwise the clip it decodes.
std::optional<Clip> rendered(const DmxEngine& engine, std::uint16_t address) {
    if (zoneChannel(engine, address, 1) < 250 || zoneChannel(engine, address, 2) == 0) {
        return std::nullopt;
    }
    return liberation::decode(zoneChannel(engine, address, 3), zoneChannel(engine, address, 4));
}

Payload clip(Clip which, std::uint8_t intensity = 255) {
    Payload payload;
    payload.kind = EffectKind::Clip;
    payload.clip = static_cast<std::uint16_t>(liberation::indexOf(which) + 1);
    payload.level = intensity;
    return payload;
}

Payload blackout(double seconds = 0.0) {
    Payload payload;
    payload.kind = EffectKind::Blackout;
    payload.curve = takt4::dmx::Curve::Linear;
    payload.durationSeconds = static_cast<float>(seconds);
    return payload;
}

} // namespace

TEST_CASE("a clip number is Liberation's deck order", "[dmx][liberation]") {
    // Down the five rows of a column, then on to the next — Liberation's own slot order.
    CHECK(liberation::indexOf({0, 0}) == 0);
    CHECK(liberation::indexOf({0, 4}) == 4);
    CHECK(liberation::indexOf({1, 0}) == 5);
    CHECK(liberation::indexOf({1, 1}) == 6);
    CHECK(liberation::indexOf({21, 1}) == 106);
    CHECK(liberation::clipAt(106) == Clip{21, 1});
    // The operator's own example, 1-1 to 21-1, is every clip between the two in that order.
    CHECK(liberation::indexOf({21, 1}) - liberation::indexOf({1, 1}) + 1 == 101);
}

TEST_CASE("Gobo Bank and Gobo Select decode to the clip they were made from", "[dmx][liberation]") {
    SECTION("the examples in Liberation's own document") {
        // Bank | slot | clip deck position: 0|1 x0 y0, 0|2 x0 y1, 0|5 x0 y4, 0|6 x1 y0, 1|1 x8 y0.
        CHECK(liberation::goboOf(liberation::indexOf({0, 0})).bank == 0);
        CHECK(liberation::decode(0, liberation::goboOf(0).select) == Clip{0, 0});
        CHECK(liberation::decode(0, liberation::goboOf(1).select) == Clip{0, 1});
        CHECK(liberation::decode(0, liberation::goboOf(4).select) == Clip{0, 4});
        CHECK(liberation::decode(0, liberation::goboOf(5).select) == Clip{1, 0});
        const liberation::Gobo eighth = liberation::goboOf(liberation::indexOf({8, 0}));
        CHECK(eighth.bank == 1);
        CHECK(liberation::decode(eighth.bank, eighth.select) == Clip{8, 0});
    }
    SECTION("the middle of each slot's band, as the Chataigne module sends it") {
        CHECK(liberation::goboOf(0).select == 4);    // slot 1
        CHECK(liberation::goboOf(19).select == 125); // slot 20
        CHECK(liberation::goboOf(39).select == 253); // slot 40
    }
    SECTION("every clip of the deck, through Liberation's own decoding") {
        // The whole deck the Gobo Bank channel can reach: each one must come back as itself, or
        // a shuffle would put a neighbouring clip on the laser now and then.
        int wrong = 0;
        for (int index = 0; index <= liberation::kMaxIndex; ++index) {
            const liberation::Gobo gobo = liberation::goboOf(index);
            if (gobo.select == 0 || liberation::decode(gobo.bank, gobo.select) !=
                                        std::optional<Clip>(liberation::clipAt(index))) {
                ++wrong;
            }
        }
        CHECK(wrong == 0);
    }
    SECTION("a Gobo Select of 0 is no clip") {
        CHECK_FALSE(liberation::decode(3, 0).has_value());
    }
}

TEST_CASE("a clip is read and written as Liberation names it", "[dmx][liberation]") {
    CHECK(liberation::format({21, 1}) == "21-1");
    CHECK(liberation::parseClip("21-1") == Clip{21, 1});
    CHECK(liberation::parseClip(" 21 - 1 ") == Clip{21, 1});
    CHECK(liberation::parseClip("21 1") == Clip{21, 1});
    CHECK(liberation::parseClip("21.1") == Clip{21, 1});
    CHECK_FALSE(liberation::parseClip("21-5").has_value()); // five rows, 0 to 4
    CHECK_FALSE(liberation::parseClip("21").has_value());
    CHECK_FALSE(liberation::parseClip("-1-1").has_value());
    CHECK_FALSE(liberation::parseClip("2048-0").has_value()); // past the last bank
    CHECK_FALSE(liberation::parseClip("a-b").has_value());

    const auto range = liberation::parseRange("1-1 to 21-1");
    REQUIRE(range.has_value());
    CHECK(range->first == Clip{1, 1});
    CHECK(range->second == Clip{21, 1});
    CHECK(liberation::parseRange("1-1..21-1").has_value());
    CHECK(liberation::parseRange("1-1 \xE2\x80\x93 21-1").has_value());
    CHECK_FALSE(liberation::parseRange("1-1 21-1").has_value());
    CHECK_FALSE(liberation::parseRange("1-1 to").has_value());
    CHECK(liberation::formatRange(6, 106) == "1-1 to 21-1");
}

TEST_CASE("a Liberation zone is the Extended 32ch profile, parked safe", "[dmx][liberation]") {
    const Fixture zone = liberation::zone("laser 1", 0, 1);
    REQUIRE(zone.channels.size() == 32);
    REQUIRE(zone.parked.size() == 32);
    REQUIRE(zone.labels.size() == 32);
    CHECK(liberation::isZone(zone));
    CHECK(takt4::dmx::problemWith(zone).empty());

    // Disarmed, dark and with no clip until a clip fires.
    CHECK(zone.parked[0] == 0); // Arm
    CHECK(zone.parked[1] == 0); // Intensity
    CHECK(zone.parked[3] == 0); // Gobo Select
    // The clip's own colours, not Liberation's white tint.
    CHECK(zone.parked[7] == 0);
    // At its authored size: a Scale of 128 renders nothing at all.
    CHECK(zone.parked[9] == 255);
    CHECK(zone.parked[10] == 255);
    // Centred and still, as Liberation's own defaults are.
    CHECK(zone.parked[11] == 128);
    CHECK(zone.parked[13] == 128);
    CHECK(zone.parked[15] == 128); // rotation stopped
    CHECK(zone.parked[30] == 0);   // follow Liberation's own tempo

    CHECK(zone.labels[3] == "Gobo Select");
    CHECK(zone.labels[31] == "Tempo override fine");

    // And it is the patch editor's built-in mode of the same name.
    bool found = false;
    for (std::size_t mode = 0; mode < takt4::dmx::builtinModes().size(); ++mode) {
        if (takt4::dmx::builtinModes()[mode].name == liberation::kZoneModeName) {
            const Fixture built = takt4::dmx::fixtureFromMode("z", mode, 0, 1);
            CHECK(liberation::isZone(built));
            CHECK(built.parked == zone.parked);
            CHECK(built.labels == zone.labels);
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("Liberation's universe 1 is takt4's universe 0", "[dmx][liberation]") {
    CHECK(liberation::portAddressOf(1) == 0);
    CHECK(liberation::portAddressOf(2) == 1);
    CHECK(liberation::liberationUniverseOf(0) == 1);
    CHECK(liberation::portAddressOf(0) == 0); // there is no universe 0 in Liberation: clamped
}

TEST_CASE("a clip effect arms the zone, lights it and selects the clip at once",
          "[dmx][liberation]") {
    DmxEngine engine;
    engine.setPatch({liberation::zone("laser 1", 0, 1), liberation::zone("laser 2", 0, 33)});
    CHECK_FALSE(rendered(engine, 1).has_value()); // parked: nothing renders

    engine.start(0b01, clip({21, 1}, 200), 1.0);
    CHECK(zoneChannel(engine, 1, 1) == 255);
    CHECK(zoneChannel(engine, 1, 2) == 200);
    CHECK(rendered(engine, 1) == Clip{21, 1});
    CHECK_FALSE(rendered(engine, 33).has_value()); // the other zone is untouched

    SECTION("a duration does not make it a fade") {
        Payload slow = clip({3, 2});
        slow.durationSeconds = 4.0f;
        engine.start(0b01, slow, 2.0);
        CHECK(rendered(engine, 1) == Clip{3, 2});
        engine.tick(2.5);
        CHECK(rendered(engine, 1) == Clip{3, 2});
    }
    SECTION("no clip disarms it and leaves the intensity") {
        Payload none;
        none.kind = EffectKind::Clip;
        none.clip = 0;
        engine.start(0b01, none, 2.0);
        CHECK(zoneChannel(engine, 1, 1) == 0);
        CHECK(zoneChannel(engine, 1, 3) == 0);
        CHECK(zoneChannel(engine, 1, 4) == 0);
        CHECK(zoneChannel(engine, 1, 2) == 200);
    }
    SECTION("aimed at a lamp it reaches nothing, and is counted") {
        engine.setPatch(
            {liberation::zone("laser 1", 0, 1), takt4::dmx::fixtureFromMode("par", 1, 0, 100)});
        const std::uint64_t missed = engine.missed();
        engine.start(0b10, clip({1, 1}), 3.0);
        CHECK(engine.missed() == missed + 1);
    }
}

TEST_CASE("a blackout disarms a laser zone at the end of its fade", "[dmx][liberation]") {
    DmxEngine engine;
    engine.setPatch({liberation::zone("laser", 0, 1)});
    engine.start(0b1, clip({5, 0}), 0.0);

    // A color, so the blend is raised and there is something to put back.
    Payload red;
    red.kind = EffectKind::Color;
    red.color = takt4::dmx::Color{255, 0, 0};
    engine.start(0b1, red, 0.0);
    CHECK(zoneChannel(engine, 1, 8) == 255);

    SECTION("Stop's blackout, at once") {
        engine.blackout(1.0);
        CHECK(zoneChannel(engine, 1, 1) == 0);
        CHECK(zoneChannel(engine, 1, 2) == 0);
        CHECK(zoneChannel(engine, 1, 4) == 0);
        CHECK(zoneChannel(engine, 1, 8) == 0); // back to the clip's own colours
        CHECK_FALSE(rendered(engine, 1).has_value());
    }
    SECTION("a blackout over two seconds fades, then disarms") {
        engine.start(0b1, blackout(2.0), 1.0);
        engine.tick(2.0);
        CHECK(zoneChannel(engine, 1, 1) == 255); // still armed half way
        CHECK(zoneChannel(engine, 1, 2) > 100);  // fading
        CHECK(zoneChannel(engine, 1, 2) < 155);
        CHECK(rendered(engine, 1) == Clip{5, 0}); // and the clip never passes through another
        engine.tick(3.0);
        CHECK(zoneChannel(engine, 1, 1) == 0);
        CHECK_FALSE(rendered(engine, 1).has_value());
    }
    SECTION("a clip fired during the fade wins the zone back") {
        engine.start(0b1, blackout(2.0), 1.0);
        engine.tick(1.5);
        engine.start(0b1, clip({6, 1}), 1.5);
        engine.tick(4.0);
        CHECK(rendered(engine, 1) == Clip{6, 1});
    }
}

TEST_CASE("disarm takes every laser zone off and nothing else", "[dmx][liberation]") {
    DmxEngine engine;
    engine.setPatch(
        {liberation::zone("laser", 0, 1), takt4::dmx::fixtureFromMode("dim", 0, 0, 100)});
    engine.start(0b01, clip({2, 3}), 0.0);
    Payload lamp;
    lamp.kind = EffectKind::Level;
    lamp.role = Role::Dimmer;
    lamp.level = 180;
    engine.start(0b10, lamp, 0.0);

    engine.disarm(1.0);
    CHECK(zoneChannel(engine, 1, 1) == 0);
    CHECK(zoneChannel(engine, 1, 3) == 0);
    CHECK(zoneChannel(engine, 1, 4) == 0);
    CHECK_FALSE(rendered(engine, 1).has_value());
    CHECK(at(engine, 0, 100) == 180); // the lamp is PANIC's to freeze, and it is still lit

    SECTION("the next clip arms it again") {
        engine.start(0b01, clip({2, 3}), 2.0);
        CHECK(rendered(engine, 1) == Clip{2, 3});
    }
    SECTION("a patch with no laser zone starts nothing") {
        DmxEngine lamps;
        lamps.setPatch({takt4::dmx::fixtureFromMode("dim", 0, 0, 1)});
        lamps.disarm(1.0);
        CHECK(lamps.started() == 0);
        CHECK(lamps.missed() == 0);
    }
}

TEST_CASE("a color tints a laser zone and a level moves its blend", "[dmx][liberation]") {
    DmxEngine engine;
    engine.setPatch({liberation::zone("laser", 0, 1)});
    CHECK(zoneChannel(engine, 1, 8) == 0);

    Payload blue;
    blue.kind = EffectKind::Color;
    blue.color = takt4::dmx::Color{0, 0, 255};
    engine.start(0b1, blue, 0.0);
    CHECK(zoneChannel(engine, 1, 5) == 0);
    CHECK(zoneChannel(engine, 1, 7) == 255);
    CHECK(zoneChannel(engine, 1, 8) == 255);

    Payload half;
    half.kind = EffectKind::Level;
    half.role = Role::ColorBlend;
    half.level = 128;
    engine.start(0b1, half, 1.0);
    CHECK(zoneChannel(engine, 1, 8) == 128);
}

TEST_CASE("a laser zone's position is the pan and tilt movement drives", "[dmx][liberation]") {
    DmxEngine engine;
    Fixture zone = liberation::zone("laser", 0, 1);
    // The preset's amount: 25% of the way out from the centre either side.
    zone.panMin = zone.tiltMin = 0.375;
    zone.panMax = zone.tiltMax = 0.625;
    engine.setPatch({zone});
    const auto word = [&engine](int coarse) {
        return zoneChannel(engine, 1, coarse) * 256 + zoneChannel(engine, 1, coarse + 1);
    };
    CHECK(word(12) == 32768); // parked at Liberation's centre
    CHECK(word(14) == 32768);

    Payload corner;
    corner.kind = EffectKind::Position;
    corner.pan = 1.0f;
    corner.tilt = 0.0f;
    engine.start(0b1, corner, 0.0);
    CHECK(word(12) == 40959); // 0.625 of 65535: a quarter of the way out to +200
    CHECK(word(14) == 24576); // 0.375

    Payload home;
    home.kind = EffectKind::Home;
    engine.start(0b1, home, 1.0);
    CHECK(word(12) == 32768);
    CHECK(word(14) == 32768);
}

// --- a rule that drives a zone ---------------------------------------------------------------

namespace {

using takt4::output::RuleSink;
using takt4::output::Transports;
using takt4::trigger::Context;
using takt4::trigger::Message;
using takt4::trigger::Rule;
using takt4::trigger::TriggerEngine;

Context beat(std::uint64_t beats, double now) {
    Context context;
    context.beats = beats;
    context.beatInBar = static_cast<std::uint32_t>((beats - 1) % 4 + 1);
    context.bars = (beats - 1) / 4 + 1;
    context.now = now;
    context.bpm = 120.0;
    context.meter = 4;
    context.confidence = 1.0;
    context.locked = true;
    return context;
}

Rule::Config clipRule(std::string zone) {
    Rule::Config rule;
    rule.id = "laser";
    rule.sendKind = Message::Kind::Dmx;
    rule.trigger = takt4::trigger::Trigger::Beat;
    rule.dmx.effect = EffectKind::Clip;
    rule.dmx.fixtures = {std::move(zone)};
    rule.seed = 11;
    return rule;
}

} // namespace

TEST_CASE("a clip rule shuffles every clip of its range onto the zone",
          "[dmx][liberation][trigger]") {
    Transports::Config config;
    Fixture zone = liberation::zone("laser 1", 0, 1);
    zone.id = "z1";
    config.patch = {zone};
    Transports transports(config);
    RuleSink sink(transports);
    TriggerEngine engine(sink);

    // Untouched, a clip rule is the operator's example: 1-1 to 21-1 in deck order.
    const Rule::Config rule = clipRule("z1");
    CHECK(rule.dmx.clip.low == liberation::indexOf({1, 1}));
    CHECK(rule.dmx.clip.high == liberation::indexOf({21, 1}));
    const std::vector<takt4::trigger::Slot> slots = takt4::trigger::slotLayout(rule);
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].role == takt4::trigger::SlotRole::Clip);
    CHECK(slots[1].role == takt4::trigger::SlotRole::Level);

    engine.setRules({rule});
    engine.rule(0).setFixtureMask(0b1);

    // One shuffle's bag: every one of the 101 clips once, and nothing outside them.
    std::set<int> seen;
    for (std::uint64_t n = 1; n <= 101; ++n) {
        const double now = static_cast<double>(n) * 0.5;
        sink.setNow(now);
        engine.onBeat(beat(n, now));
        const std::optional<Clip> on = rendered(transports.dmx(), 1);
        REQUIRE(on.has_value());
        const int index = liberation::indexOf(*on);
        CHECK(index >= liberation::indexOf({1, 1}));
        CHECK(index <= liberation::indexOf({21, 1}));
        seen.insert(index);
        // The chip says the clip as Liberation names it, and the intensity.
        REQUIRE(engine.rule(0).lastSlots().size() == 2);
        std::string named;
        engine.rule(0).lastSlots()[0].appendTo(named);
        CHECK(named == liberation::format(*on));
        CHECK(engine.rule(0).lastSlots()[1].asInt() == 255);
    }
    CHECK(seen.size() == 101);

    SECTION("its release takes the clip off") {
        Rule::Config held = clipRule("z1");
        takt4::trigger::FollowUp release;
        release.unit = takt4::trigger::DelayUnit::Beats;
        release.delayBeats = 2.0;
        held.followUps.push_back(release);
        engine.setRules({held});
        engine.rule(0).setFixtureMask(0b1);
        sink.setNow(100.0);
        engine.onBeat(beat(200, 100.0));
        CHECK(rendered(transports.dmx(), 1).has_value());
        sink.setNow(101.1);
        engine.advance(beat(200, 101.1));
        transports.dmx().tick(101.1);
        CHECK_FALSE(rendered(transports.dmx(), 1).has_value());
        CHECK(zoneChannel(transports.dmx(), 1, 1) == 0);
    }
}
