#include "core/dmx/color.hpp"
#include "core/dmx/dmx_engine.hpp"
#include "core/dmx/effect.hpp"
#include "core/dmx/fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

std::uint8_t at(const DmxEngine& engine, int channel) {
    const std::span<const std::uint8_t> levels = engine.levels(0);
    REQUIRE(!levels.empty());
    return levels[static_cast<std::size_t>(channel) - 1];
}

/// A CMY head as the import makes one: dimmer, shutter, the three flags, a color wheel. The
/// shutter parked open, the flags out — a white beam once the dimmer is up.
Fixture cmyHead(std::uint16_t address) {
    Fixture head;
    head.id = "f-cmy";
    head.name = "spot";
    head.address = address;
    head.channels = {Role::Dimmer,  Role::Strobe, Role::Cyan,
                     Role::Magenta, Role::Yellow, Role::ColorWheel};
    head.parked = {0, 255, 0, 0, 0, 0};
    return head;
}

Payload color(Color value, double seconds = 0.0) {
    Payload payload;
    payload.kind = EffectKind::Color;
    payload.color = value;
    payload.durationSeconds = static_cast<float>(seconds);
    payload.curve = Curve::Linear;
    return payload;
}

} // namespace

TEST_CASE("cyan, magenta and yellow are roles a file keeps by name", "[dmx][fixture][cmy]") {
    CHECK(takt4::dmx::nameOf(Role::Cyan) == "cyan");
    CHECK(takt4::dmx::nameOf(Role::Magenta) == "magenta");
    CHECK(takt4::dmx::nameOf(Role::Yellow) == "yellow");
    CHECK(takt4::dmx::roleOf("magenta") == Role::Magenta);
    // Offered in both dropdowns, after UV, and aimable — a level on cyan is a raw level.
    const auto& roles = takt4::dmx::kRoles;
    const auto uv = std::find(roles.begin(), roles.end(), Role::Uv);
    REQUIRE(uv != roles.end());
    CHECK(*(uv + 1) == Role::Cyan);
    CHECK(*(uv + 2) == Role::Magenta);
    CHECK(*(uv + 3) == Role::Yellow);
    const auto& aimable = takt4::dmx::kAimableRoles;
    CHECK(std::find(aimable.begin(), aimable.end(), Role::Yellow) != aimable.end());
    // They are not emitters: a flag in is a filter, and a CMY head is dark through its dimmer.
    CHECK(std::find(takt4::dmx::kEmitters.begin(), takt4::dmx::kEmitters.end(), Role::Cyan) ==
          takt4::dmx::kEmitters.end());
    CHECK_FALSE(takt4::dmx::emits(cmyHead(1)));
}

TEST_CASE("a color reaches a CMY head as the complement of each component", "[dmx][engine][cmy]") {
    DmxEngine engine;
    engine.setPatch({cmyHead(1)});
    CHECK(at(engine, 3) == 0); // the flags start out of the beam: white
    CHECK(at(engine, 2) == 255);

    engine.start(0b1, color(Color{255, 0, 0}), 0.0);
    // Red is white with the green and the blue taken out: no cyan, full magenta and yellow.
    CHECK(at(engine, 3) == 0);
    CHECK(at(engine, 4) == 255);
    CHECK(at(engine, 5) == 255);
    // A color effect aimed at a fixture with no RGB used to reach nothing and count as missed.
    CHECK(engine.missed() == 0);

    SECTION("white takes every flag out, and black puts every one in") {
        engine.start(0b1, color(Color{255, 255, 255}), 1.0);
        CHECK(at(engine, 3) == 0);
        CHECK(at(engine, 4) == 0);
        CHECK(at(engine, 5) == 0);
        engine.start(0b1, color(Color{0, 0, 0}), 2.0);
        CHECK(at(engine, 3) == 255);
        CHECK(at(engine, 4) == 255);
        CHECK(at(engine, 5) == 255);
    }

    SECTION("a fade runs in a straight line, as the RGB fade beside it does") {
        Fixture par = takt4::dmx::fixtureFromMode("par", 1, 0, 20); // RGB at 20-22
        par.id = "f-par";
        engine.setPatch({cmyHead(1), par});
        engine.start(0b11, color(Color{255, 255, 255}), 0.0);
        engine.start(0b11, color(Color{0, 0, 255}, 2.0), 10.0);
        engine.tick(11.0);
        // Half way from white to blue: the par's red at half, the head's cyan half in. The two
        // add up to one full step either side of rounding, never further apart.
        const int red = at(engine, 20);
        const int cyan = at(engine, 3);
        CHECK(red == 128);
        CHECK(cyan == 128);
        CHECK(at(engine, 5) == 0); // blue stays: no yellow
        engine.tick(12.0);
        CHECK(at(engine, 3) == 255);
        CHECK(at(engine, 4) == 255);
        CHECK(at(engine, 5) == 0);
        CHECK(at(engine, 20) == 0);
        CHECK(at(engine, 22) == 255);
    }

    SECTION("the dimmer is still the dimmer") {
        Payload dimmer;
        dimmer.kind = EffectKind::Level;
        dimmer.role = Role::Dimmer;
        dimmer.level = 200;
        engine.start(0b1, dimmer, 1.0);
        CHECK(at(engine, 1) == 200);
        CHECK(at(engine, 4) == 255); // and the color is left where it was
    }

    SECTION("a level aimed at one flag is a raw level on it") {
        Payload flag;
        flag.kind = EffectKind::Level;
        flag.role = Role::Magenta;
        flag.level = 77;
        engine.start(0b1, flag, 1.0);
        CHECK(at(engine, 4) == 77);
        CHECK(at(engine, 3) == 0);
    }
}

TEST_CASE("a blackout leaves a CMY head's flags where they are", "[dmx][engine][cmy]") {
    DmxEngine engine;
    engine.setPatch({cmyHead(1)});
    engine.start(0b1, color(Color{0, 0, 255}), 0.0); // blue: cyan and magenta in
    Payload dimmer;
    dimmer.kind = EffectKind::Level;
    dimmer.role = Role::Dimmer;
    dimmer.level = 255;
    engine.start(0b1, dimmer, 0.0);
    REQUIRE(at(engine, 1) == 255);

    SECTION("as the Blackout effect") {
        Payload dark;
        dark.kind = EffectKind::Blackout;
        engine.start(0b1, dark, 1.0);
    }
    SECTION("as Stop") {
        engine.blackout(1.0);
    }
    // Dark through the dimmer; the flags are a filter, not a lamp, and the next dimmer rule
    // brings the same blue back.
    CHECK(at(engine, 1) == 0);
    CHECK(at(engine, 3) == 255);
    CHECK(at(engine, 4) == 255);
    CHECK(at(engine, 5) == 0);
    CHECK(at(engine, 2) == 255); // the shutter stays open too
}

TEST_CASE("a hue sweep turns a CMY head's flags round the wheel", "[dmx][engine][cmy]") {
    DmxEngine engine;
    engine.setPatch({cmyHead(1)});
    Payload sweep;
    sweep.kind = EffectKind::HueSweep;
    sweep.color = Color{255, 0, 0};
    sweep.hueFrom = 0.0f;
    sweep.hueTo = 240.0f;
    sweep.durationSeconds = 2.0f;
    sweep.curve = Curve::Linear;
    engine.start(0b1, sweep, 0.0);
    CHECK(engine.missed() == 0);

    // 0 degrees, red: no cyan.
    CHECK(at(engine, 3) == 0);
    CHECK(at(engine, 4) == 255);
    CHECK(at(engine, 5) == 255);
    engine.tick(1.0); // 120, green: no magenta
    CHECK(at(engine, 3) == 255);
    CHECK(at(engine, 4) == 0);
    CHECK(at(engine, 5) == 255);
    engine.tick(2.0); // 240, blue: no yellow
    CHECK(at(engine, 3) == 255);
    CHECK(at(engine, 4) == 255);
    CHECK(at(engine, 5) == 0);
}

TEST_CASE("on an LED fixture with flags too, the virtual dimmer leaves the flags alone",
          "[dmx][engine][cmy]") {
    // No dimmer channel and RGB emitters: brightness is the color (the virtual dimmer). The
    // flags are written by a color and by nothing that dims.
    Fixture odd;
    odd.id = "f-odd";
    odd.name = "odd";
    odd.address = 1;
    odd.channels = {Role::Red, Role::Green, Role::Blue, Role::Cyan, Role::Magenta, Role::Yellow};
    odd.parked = {0, 0, 0, 0, 0, 0};
    DmxEngine engine;
    engine.setPatch({odd});

    engine.start(0b1, color(Color{255, 0, 0}), 0.0);
    CHECK(at(engine, 1) == 255);
    CHECK(at(engine, 4) == 0);
    CHECK(at(engine, 5) == 255);

    Payload fade;
    fade.kind = EffectKind::Level;
    fade.role = Role::Dimmer;
    fade.level = 0;
    engine.start(0b1, fade, 1.0);
    CHECK(at(engine, 1) == 0);   // the red LED dims
    CHECK(at(engine, 5) == 255); // the flag does not move
}
