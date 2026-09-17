#include "core/dmx/color.hpp"
#include "core/dmx/dmx_engine.hpp"
#include "core/dmx/effect.hpp"
#include "core/dmx/fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

using takt4::dmx::Color;
using takt4::dmx::Curve;
using takt4::dmx::DmxEngine;
using takt4::dmx::EffectKind;
using takt4::dmx::Fixture;
using takt4::dmx::Payload;
using takt4::dmx::Role;

namespace {

/// Channel `channel` of universe `universe`, 1-based as the patch spells it.
std::uint8_t at(const DmxEngine& engine, takt4::dmx::PortAddress universe, int channel) {
    const std::span<const std::uint8_t> levels = engine.levels(universe);
    REQUIRE(!levels.empty());
    return levels[static_cast<std::size_t>(channel) - 1];
}

Fixture rgb(std::string name, std::uint16_t address) {
    return takt4::dmx::fixtureFromMode(name, 1, 0, address); // RGB (3ch)
}

Fixture rgbw(std::string name, std::uint16_t address) {
    return takt4::dmx::fixtureFromMode(name, 2, 0, address); // RGBW (4ch)
}

Fixture head16(std::string name, std::uint16_t address) {
    return takt4::dmx::fixtureFromMode(name, 6, 0, address); // moving head 16-bit (12ch)
}

Payload level(Role role, std::uint8_t value, double seconds = 0.0) {
    Payload payload;
    payload.kind = EffectKind::Level;
    payload.role = role;
    payload.level = value;
    payload.durationSeconds = static_cast<float>(seconds);
    payload.curve = Curve::Linear;
    return payload;
}

} // namespace

TEST_CASE("a patch lays its parked levels into the universe", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({head16("head", 1)});

    // The shutter is open and the movement is at full speed, so the first rule that touches
    // the dimmer produces light. Channel 7 is Strobe in the 16-bit head map.
    CHECK(at(engine, 0, 7) == 255);
    CHECK(at(engine, 0, 6) == 0);   // dimmer, down
    CHECK(at(engine, 0, 1) == 128); // pan, centred

    SECTION("a universe nothing is patched to has no frame at all") {
        CHECK(engine.levels(9).empty());
        CHECK(engine.revision(9) == 0);
    }
}

TEST_CASE("a level lands at once with no duration, and ramps with one", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({takt4::dmx::fixtureFromMode("dim", 0, 0, 5)}); // dimmer (1ch)

    SECTION("no duration is a snap, in the same round the rule fired") {
        engine.start(0b1, level(Role::Dimmer, 200), 10.0);
        CHECK(at(engine, 0, 5) == 200);
    }

    SECTION("a duration is a fade, and it holds where it lands") {
        engine.start(0b1, level(Role::Dimmer, 255, 2.0), 10.0);
        CHECK(at(engine, 0, 5) == 0);
        engine.tick(11.0);
        CHECK(at(engine, 0, 5) == 128); // half way, linear
        engine.tick(12.0);
        CHECK(at(engine, 0, 5) == 255);

        // The effect is finished and dropped, and the level *stays*. A lighting level is a
        // state: a fade that undid itself when it ended would flicker back on every rule.
        CHECK(engine.running() == 0);
        engine.tick(20.0);
        CHECK(at(engine, 0, 5) == 255);
    }

    SECTION("a fade out starts from wherever the channel actually is") {
        engine.start(0b1, level(Role::Dimmer, 100), 10.0);
        engine.start(0b1, level(Role::Dimmer, 0, 2.0), 10.0);
        engine.tick(11.0);
        CHECK(at(engine, 0, 5) == 50);
    }

    SECTION("a curve shapes the ramp without changing where it starts or ends") {
        Payload payload = level(Role::Dimmer, 255, 2.0);
        payload.curve = Curve::EaseOut;
        engine.start(0b1, payload, 10.0);
        engine.tick(11.0);
        // Ease-out is past half way at half time; linear would be exactly 128.
        CHECK(at(engine, 0, 5) > 150);
        engine.tick(12.0);
        CHECK(at(engine, 0, 5) == 255);
    }
}

TEST_CASE("the last effect to touch a channel wins it", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({takt4::dmx::fixtureFromMode("dim", 0, 0, 1)});

    engine.start(0b1, level(Role::Dimmer, 255, 10.0), 0.0);
    engine.tick(5.0);
    CHECK(at(engine, 0, 1) == 128); // half way up
    CHECK(engine.running() == 1);

    // A second fade on the same channel takes it over, and the first is left with nothing to
    // drive and is dropped rather than fighting.
    engine.start(0b1, level(Role::Dimmer, 0, 2.0), 5.0);
    CHECK(engine.running() == 1);
    engine.tick(6.0);
    // Half way through the *second* fade, starting from wherever the first had got to.
    CHECK(at(engine, 0, 1) == 64);

    SECTION("an effect on a different channel is left alone") {
        engine.setPatch({rgb("par", 1)});
        engine.start(0b1, level(Role::Red, 255, 10.0), 0.0);
        engine.start(0b1, level(Role::Green, 255, 10.0), 0.0);
        CHECK(engine.running() == 2);
    }
}

TEST_CASE("a color is written to the channels the fixture actually has", "[dmx][engine]") {
    Payload payload;
    payload.kind = EffectKind::Color;
    payload.color = Color{255, 32, 64};

    SECTION("a plain RGB par takes all three") {
        DmxEngine engine;
        engine.setPatch({rgb("par", 1)});
        engine.start(0b1, payload, 0.0);
        CHECK(at(engine, 0, 1) == 255);
        CHECK(at(engine, 0, 2) == 32);
        CHECK(at(engine, 0, 3) == 64);
    }

    SECTION("an RGBW fixture has its white driven out of the way of a color") {
        DmxEngine engine;
        engine.setPatch({rgbw("par", 1)});
        engine.start(0b1, level(Role::White, 255), 0.0);
        CHECK(at(engine, 0, 4) == 255);
        engine.start(0b1, payload, 0.0);
        CHECK(at(engine, 0, 4) == 0); // or the color would be that color plus leftover white
    }

    SECTION("white on an RGBW fixture uses the white LED, which is a better white") {
        DmxEngine engine;
        engine.setPatch({rgbw("par", 1)});
        Payload white = payload;
        white.color = Color{255, 255, 255};
        engine.start(0b1, white, 0.0);
        CHECK(at(engine, 0, 1) == 0);
        CHECK(at(engine, 0, 4) == 255);
    }

    SECTION("white on a fixture with no white LED still comes out white") {
        // The regression this guards: routing the level to a channel the fixture has not got
        // and driving its three real ones to zero, so the commonest color an operator tries
        // first makes the par go dark.
        DmxEngine engine;
        engine.setPatch({rgb("par", 1)});
        Payload white = payload;
        white.color = Color{255, 255, 255};
        engine.start(0b1, white, 0.0);
        CHECK(at(engine, 0, 1) == 255);
        CHECK(at(engine, 0, 2) == 255);
        CHECK(at(engine, 0, 3) == 255);
    }

    SECTION("a color fade crosses through RGB rather than round the wheel") {
        DmxEngine engine;
        engine.setPatch({rgb("par", 1)});
        Payload red = payload;
        red.color = Color{255, 0, 0};
        engine.start(0b1, red, 0.0);

        Payload green = payload;
        green.color = Color{0, 255, 0};
        green.durationSeconds = 4.0f;
        green.curve = Curve::Linear;
        engine.start(0b1, green, 0.0);
        engine.tick(1.0);
        // A quarter of the way: both components part way, which is a dark olive. Through HSV
        // this would already be yellow, and that is the difference a hue sweep exists for.
        CHECK(at(engine, 0, 1) == 191);
        CHECK(at(engine, 0, 2) == 64);
    }
}

TEST_CASE("a 16-bit head is driven across both bytes", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({head16("head", 1)});

    Payload payload;
    payload.kind = EffectKind::Position;
    payload.pan = 1.0f;
    payload.tilt = 0.0f;
    engine.start(0b1, payload, 0.0);

    // Pan is channel 1 with its fine byte at 2; full travel is 0xFFFF.
    CHECK(at(engine, 0, 1) == 255);
    CHECK(at(engine, 0, 2) == 255);
    CHECK(at(engine, 0, 3) == 0); // tilt coarse
    CHECK(at(engine, 0, 4) == 0); // tilt fine

    SECTION("a fifth of the way along is a fine byte away from a round coarse one") {
        // 0.2 of 65535 is 13107 = 0x3333: the fine byte carries a third of the coarse step,
        // which is the whole reason a 16-bit head moves smoothly and an 8-bit one steps.
        Payload part = payload;
        part.pan = 0.2f;
        engine.start(0b1, part, 0.0);
        CHECK(at(engine, 0, 1) == 0x33);
        CHECK(at(engine, 0, 2) == 0x33);
    }
}

TEST_CASE("a movement window is a safety limit and every effect stays inside it", "[dmx][engine]") {
    Fixture head = head16("head", 1);
    // The operator has aimed this head at the floor and away from the audience.
    head.panMin = 0.25;
    head.panMax = 0.75;
    head.tiltMin = 0.5;
    head.tiltMax = 0.6;

    DmxEngine engine;
    engine.setPatch({head});

    SECTION("a position is a fraction of the window, not of the head's whole travel") {
        Payload payload;
        payload.kind = EffectKind::Position;
        payload.pan = 1.0f; // "as far as I am allowed", which is 0.75 of full travel
        payload.tilt = 0.0f;
        engine.start(0b1, payload, 0.0);
        CHECK(at(engine, 0, 1) == 191); // 0.75 * 65535 = 49151 = 0xBFFF
        CHECK(at(engine, 0, 3) == 128); // 0.5 * 65535 = 32767 = 0x7FFF
    }

    SECTION("home is the middle of the window, not the middle of the travel") {
        Payload payload;
        payload.kind = EffectKind::Home;
        engine.start(0b1, payload, 0.0);
        CHECK(at(engine, 0, 1) == 128); // 0.5 of full travel, which is this window's centre
        CHECK(at(engine, 0, 3) == 140); // 0.55 of full travel
    }

    SECTION("a path at full size just touches the limits and never crosses them") {
        Payload payload;
        payload.kind = EffectKind::Path;
        payload.shape = takt4::dmx::PathShape::Circle;
        payload.size = 1.0f;
        payload.cycles = 1.0f;
        payload.durationSeconds = 4.0f;
        engine.start(0b1, payload, 0.0);

        for (int step = 0; step <= 40; ++step) {
            engine.tick(step * 0.1);
            const double pan = (at(engine, 0, 1) * 256 + at(engine, 0, 2)) / 65535.0;
            const double tilt = (at(engine, 0, 3) * 256 + at(engine, 0, 4)) / 65535.0;
            CHECK(pan >= 0.25 - 0.005);
            CHECK(pan <= 0.75 + 0.005);
            CHECK(tilt >= 0.5 - 0.005);
            CHECK(tilt <= 0.6 + 0.005);
        }
    }
}

TEST_CASE("a flash starts at the peak and decays", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({takt4::dmx::fixtureFromMode("dim", 0, 0, 1)});

    Payload payload;
    payload.kind = EffectKind::Flash;
    payload.level = 255;
    payload.base = 0;
    payload.durationSeconds = 1.0f;
    payload.curve = Curve::Linear;
    engine.start(0b1, payload, 0.0);

    // The jump is the point: it is at full in the round the rule fired in, not a frame later.
    CHECK(at(engine, 0, 1) == 255);
    engine.tick(0.5);
    CHECK(at(engine, 0, 1) == 128);
    engine.tick(1.0);
    CHECK(at(engine, 0, 1) == 0);
}

TEST_CASE("a pulse breathes and a strobe does not", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({takt4::dmx::fixtureFromMode("dim", 0, 0, 1)});

    SECTION("a pulse starts low, so firing one does not flash") {
        Payload payload;
        payload.kind = EffectKind::Pulse;
        payload.base = 0;
        payload.level = 255;
        payload.cycles = 1.0f;
        payload.durationSeconds = 4.0f;
        engine.start(0b1, payload, 0.0);
        CHECK(at(engine, 0, 1) == 0);
        engine.tick(1.0);
        // Half way up. Not pinned to a single byte: the quarter-cycle lands on a cosine of
        // exactly zero, which in doubles is 6e-17 rather than 0 and rounds either way.
        CHECK(at(engine, 0, 1) >= 126);
        CHECK(at(engine, 0, 1) <= 129);
        engine.tick(2.0);
        CHECK(at(engine, 0, 1) == 255);
        engine.tick(4.0);
        CHECK(at(engine, 0, 1) == 0); // one whole cycle, back where it started
    }

    SECTION("a strobe is on for its duty and off for the rest, and ends off") {
        Payload payload;
        payload.kind = EffectKind::Strobe;
        payload.base = 0;
        payload.level = 255;
        payload.cycles = 4.0f;
        payload.duty = 0.5f;
        payload.durationSeconds = 4.0f;
        engine.start(0b1, payload, 0.0);
        CHECK(at(engine, 0, 1) == 255); // first half of cycle 1
        engine.tick(0.6);
        CHECK(at(engine, 0, 1) == 0); // second half
        engine.tick(1.1);
        CHECK(at(engine, 0, 1) == 255); // cycle 2
        engine.tick(4.0);
        CHECK(at(engine, 0, 1) == 0); // and it leaves the fixture off, not mid-flash
    }
}

TEST_CASE("a hue sweep travels round the wheel", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({rgb("par", 1)});

    Payload payload;
    payload.kind = EffectKind::HueSweep;
    payload.color = Color{255, 0, 0}; // full saturation, full brightness
    payload.hueFrom = 0.0f;
    payload.hueTo = 240.0f;
    payload.durationSeconds = 2.0f;
    payload.curve = Curve::Linear;
    engine.start(0b1, payload, 0.0);

    CHECK(at(engine, 0, 1) == 255); // red
    engine.tick(1.0);
    CHECK(at(engine, 0, 2) == 255); // 120 degrees is green
    CHECK(at(engine, 0, 1) == 0);
    engine.tick(2.0);
    CHECK(at(engine, 0, 3) == 255); // 240 is blue
}

TEST_CASE("a blackout kills the light and leaves the fixture usable", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({head16("head", 1)});
    engine.start(0b1, level(Role::Dimmer, 255), 0.0);
    CHECK(at(engine, 0, 6) == 255);

    Payload payload;
    payload.kind = EffectKind::Blackout;
    engine.start(0b1, payload, 0.0);
    CHECK(at(engine, 0, 6) == 0); // dimmer
    CHECK(at(engine, 0, 8) == 0); // red

    // The shutter stays open. A blackout that closed it would leave the fixture dark when the
    // *next* rule fires, which is the one thing worse than a blackout that does not work.
    CHECK(at(engine, 0, 7) == 255);
    // And it has not been sent home either.
    CHECK(at(engine, 0, 1) == 128);
}

TEST_CASE("an effect aimed at channels a fixture has not got is counted, not an error",
          "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({rgb("par", 1), head16("head", 10)});

    Payload payload;
    payload.kind = EffectKind::Position;
    // Aimed at the par alone, which cannot move.
    engine.start(0b1, payload, 0.0);
    CHECK(engine.missed() == 1);
    CHECK(engine.running() == 0);

    SECTION("aimed at a group holding both, it drives the one that can and says nothing") {
        engine.start(0b11, payload, 0.0);
        CHECK(engine.missed() == 1); // still just the one from before
        CHECK(engine.running() == 1);
    }

    SECTION("a rule routed to no fixture at all reaches nothing") {
        engine.start(0, payload, 0.0);
        CHECK(engine.missed() == 2);
    }
}

TEST_CASE("panic stops the animation and holds the levels", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({takt4::dmx::fixtureFromMode("dim", 0, 0, 1)});
    engine.start(0b1, level(Role::Dimmer, 255, 10.0), 0.0);
    engine.tick(5.0);
    const std::uint8_t held = at(engine, 0, 1);
    CHECK(held > 100);

    engine.cancelAll();
    CHECK(engine.running() == 0);
    engine.tick(6.0);
    // Frozen where it was, per the operator's own call: takt4 may be one source among several
    // and a panic that blacked out the stage would take down lights that were not its to take.
    CHECK(at(engine, 0, 1) == held);
}

TEST_CASE("a revision moves only when a level really moves", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({takt4::dmx::fixtureFromMode("dim", 0, 0, 1)});
    const std::uint64_t afterPatch = engine.revision(0);

    engine.tick(1.0);
    CHECK(engine.revision(0) == afterPatch); // nothing running, nothing sent

    engine.start(0b1, level(Role::Dimmer, 128), 1.0);
    const std::uint64_t afterFire = engine.revision(0);
    CHECK(afterFire > afterPatch);

    // The same level again is not a change, and must not make the publisher send a frame.
    engine.start(0b1, level(Role::Dimmer, 128), 2.0);
    CHECK(engine.revision(0) == afterFire);
}

TEST_CASE("re-patching keeps the rig lit", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({rgb("par", 1)});
    engine.start(0b1, level(Role::Red, 200), 0.0);
    CHECK(at(engine, 0, 1) == 200);

    // Adding a fixture during a set must not reset the ones already lit — re-patching is an
    // edit to the map, not an instruction to the lights.
    engine.setPatch({rgb("par", 1), head16("head", 10)});
    CHECK(at(engine, 0, 1) == 200);
    CHECK(at(engine, 0, 16) == 255); // the new head's shutter, parked open

    SECTION("running effects stop, because their channels may have moved under them") {
        engine.setPatch({rgb("par", 1)});
        engine.start(0b1, level(Role::Red, 0, 10.0), 0.0);
        CHECK(engine.running() == 1);
        engine.setPatch({rgb("par", 1), rgb("par2", 4)});
        CHECK(engine.running() == 0);
    }
}

TEST_CASE("reset puts the rig back where it starts", "[dmx][engine]") {
    DmxEngine engine;
    engine.setPatch({head16("head", 1)});
    engine.start(0b1, level(Role::Dimmer, 255), 0.0);
    CHECK(at(engine, 0, 6) == 255);

    engine.reset();
    CHECK(at(engine, 0, 6) == 0);   // dimmer down
    CHECK(at(engine, 0, 7) == 255); // shutter back open
    CHECK(engine.running() == 0);
}

TEST_CASE("a dimmer aimed at a fixture that has not got one drives its color", "[dmx][engine]") {
    // **The bug that made the commonest rig in the world do nothing.**
    //
    // An RGB par has no master intensity channel: its brightness *is* its color, scaled.
    // `Role::Dimmer` has promised since it was written that "a fixture without one has its
    // color scaled instead" and nothing implemented it, so a rule with the default effect on
    // the default channel aimed at the default fixture reached no channel and the lamp stayed
    // dark. Reported from a rig on 2026-09-16: "Choosing the closest thing available right
    // now, dimmer, does absolutely nothing - the light is not coming on at all."
    DmxEngine engine;
    engine.setPatch({rgb("par", 1)});
    REQUIRE(at(engine, 0, 1) == 0);

    SECTION("a dark fixture comes up white, because there is no color to scale") {
        engine.start(0b1, level(Role::Dimmer, 255), 0.0);
        CHECK(engine.missed() == 0);
        CHECK(at(engine, 0, 1) == 255);
        CHECK(at(engine, 0, 2) == 255);
        CHECK(at(engine, 0, 3) == 255);
    }

    SECTION("and at half it is a half-bright white") {
        engine.start(0b1, level(Role::Dimmer, 128), 0.0);
        CHECK(at(engine, 0, 1) == 128);
        CHECK(at(engine, 0, 2) == 128);
        CHECK(at(engine, 0, 3) == 128);
    }

    SECTION("a lit fixture keeps its hue") {
        Payload color;
        color.kind = EffectKind::Color;
        color.color = Color{255, 64, 0}; // orange
        engine.start(0b1, color, 0.0);
        REQUIRE(at(engine, 0, 1) == 255);
        REQUIRE(at(engine, 0, 2) == 64);

        engine.start(0b1, level(Role::Dimmer, 128), 1.0);
        engine.tick(1.0);
        // Half of the orange, not half of white: the weights are the color it was showing.
        CHECK(at(engine, 0, 1) == 128);
        CHECK(at(engine, 0, 2) == 32);
        CHECK(at(engine, 0, 3) == 0);
    }

    SECTION("two fades in a row do not compound") {
        // The weights are normalised against the brightest channel, or a par already at half
        // would answer "go to full" with a quarter and walk itself down over a set.
        engine.start(0b1, level(Role::Dimmer, 128), 0.0);
        REQUIRE(at(engine, 0, 1) == 128);
        engine.start(0b1, level(Role::Dimmer, 255), 0.0);
        CHECK(at(engine, 0, 1) == 255);
        CHECK(at(engine, 0, 2) == 255);
    }

    SECTION("an RGBW par uses its white LED rather than mixing one") {
        DmxEngine rgbwEngine;
        rgbwEngine.setPatch({rgbw("par", 1)});
        rgbwEngine.start(0b1, level(Role::Dimmer, 255), 0.0);
        CHECK(at(rgbwEngine, 0, 1) == 0);
        CHECK(at(rgbwEngine, 0, 2) == 0);
        CHECK(at(rgbwEngine, 0, 3) == 0);
        CHECK(at(rgbwEngine, 0, 4) == 255);
    }

    SECTION("a fixture that really has a dimmer is untouched by any of this") {
        DmxEngine dimmerEngine;
        dimmerEngine.setPatch({takt4::dmx::fixtureFromMode("par", 3, 0, 1)}); // dimmer + RGB
        dimmerEngine.start(0b1, level(Role::Dimmer, 200), 0.0);
        CHECK(at(dimmerEngine, 0, 1) == 200);
        CHECK(at(dimmerEngine, 0, 2) == 0); // the color is left alone
    }

    SECTION("a strobe on a virtual dimmer strobes the color") {
        Payload strobe;
        strobe.kind = EffectKind::Strobe;
        strobe.role = Role::Dimmer;
        strobe.base = 0;
        strobe.level = 255;
        strobe.cycles = 2.0f;
        strobe.duty = 0.5f;
        strobe.durationSeconds = 1.0f;
        engine.start(0b1, strobe, 0.0);
        CHECK(engine.missed() == 0);
        engine.tick(0.1); // first half of the first cycle: on
        CHECK(at(engine, 0, 1) == 255);
        engine.tick(0.4); // second half: off
        CHECK(at(engine, 0, 1) == 0);
    }

    SECTION("but a pan on a wash still reaches nothing") {
        Payload move;
        move.kind = EffectKind::Position;
        engine.start(0b1, move, 0.0);
        CHECK(engine.missed() == 1);
    }
}

TEST_CASE("a channel test holds one channel and puts it back", "[dmx][engine]") {
    // The patch editor's per-channel TEST — `DmxEngine::holdChannel`. Addressed by channel
    // rather than by role, which is the whole point: what an operator is checking is that
    // channel 3 is this fixture's blue, and a role cannot name a channel nothing aims at.
    DmxEngine engine;
    engine.setPatch({rgb("par", 1)});
    engine.holdChannel(0, 2, 200, 3.0, 0.0);
    CHECK(at(engine, 0, 2) == 200);
    CHECK(at(engine, 0, 1) == 0); // and nothing else on the fixture moved
    CHECK(at(engine, 0, 3) == 0);

    engine.tick(1.5);
    CHECK(at(engine, 0, 2) == 200); // still held half way through

    engine.tick(3.0);
    CHECK(at(engine, 0, 2) == 0); // and back where it was when it is over
    CHECK(engine.running() == 0);

    SECTION("it goes back to the level that was there, not to zero") {
        engine.start(0b1, level(Role::Red, 90), 0.0);
        REQUIRE(at(engine, 0, 1) == 90);
        engine.holdChannel(0, 1, 255, 2.0, 10.0);
        CHECK(at(engine, 0, 1) == 255);
        engine.tick(12.0);
        CHECK(at(engine, 0, 1) == 90);
    }

    SECTION("a channel outside the universe is refused rather than written past the frame") {
        engine.holdChannel(0, 0, 255, 1.0, 0.0);
        engine.holdChannel(0, 513, 255, 1.0, 0.0);
        engine.holdChannel(7, 1, 255, 1.0, 0.0); // a universe the patch does not use
        CHECK(engine.running() == 0);
    }
}
