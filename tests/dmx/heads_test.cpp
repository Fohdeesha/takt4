#include "core/dmx/dmx_engine.hpp"
#include "core/dmx/effect.hpp"
#include "core/dmx/fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

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

/// Two yokes on one base, as an import makes one: each head a pan, a pan fine, a tilt and a tilt
/// fine, the first head's channels before the second's. Parked in the middle.
Fixture twin(std::uint16_t address = 1) {
    Fixture twin;
    twin.id = "f-twin";
    twin.name = "twin";
    twin.address = address;
    twin.channels = {Role::Dimmer, Role::Pan,     Role::PanFine, Role::Tilt,    Role::TiltFine,
                     Role::Pan,    Role::PanFine, Role::Tilt,    Role::TiltFine};
    twin.parked = {0, 128, 0, 128, 0, 128, 0, 128, 0};
    return twin;
}

/// The heads a payload names, from their numbers: `heads(2)` is head 2, `heads(1, 3)` heads 1
/// and 3, and `heads()` every head.
template <typename... Numbers>
std::uint32_t heads(Numbers... numbers) {
    return ((std::uint32_t{1} << (numbers - 1)) | ... | 0u);
}

Payload position(float pan, float tilt, std::uint32_t named = 0) {
    Payload payload;
    payload.kind = EffectKind::Position;
    payload.pan = pan;
    payload.tilt = tilt;
    payload.heads = named;
    return payload;
}

} // namespace

TEST_CASE("a fixture has as many heads as pans or tilts, whichever is more", "[dmx][heads]") {
    CHECK(takt4::dmx::headsOf(takt4::dmx::fixtureFromMode("par", 1, 0, 1)) == 0);
    CHECK(takt4::dmx::headsOf(takt4::dmx::fixtureFromMode("head", 6, 0, 1)) == 1);
    CHECK(takt4::dmx::headsOf(twin()) == 2);
    // A bar of segments that only tilt: every tilt is a head.
    Fixture bar;
    bar.channels = {Role::Dimmer, Role::Tilt, Role::Tilt, Role::Tilt};
    CHECK(takt4::dmx::headsOf(bar) == 3);
    // One pan carrying two tilting bars: two heads, the pan the first's.
    Fixture yoke;
    yoke.channels = {Role::Pan, Role::Tilt, Role::Tilt};
    CHECK(takt4::dmx::headsOf(yoke) == 2);
}

TEST_CASE("a move reaches every head of a fixture with several", "[dmx][engine][heads]") {
    DmxEngine engine;
    engine.setPatch({twin()});

    engine.start(0b1, position(1.0f, 0.0f), 0.0);
    CHECK(engine.missed() == 0);
    // Both heads, each across its own two bytes. Only the first moved, before (2026-10-04).
    CHECK(at(engine, 2) == 255);
    CHECK(at(engine, 3) == 255);
    CHECK(at(engine, 4) == 0);
    CHECK(at(engine, 5) == 0);
    CHECK(at(engine, 6) == 255);
    CHECK(at(engine, 7) == 255);
    CHECK(at(engine, 8) == 0);
    CHECK(at(engine, 9) == 0);

    SECTION("home brings every head back to the middle of the window") {
        Payload home;
        home.kind = EffectKind::Home;
        engine.start(0b1, home, 1.0);
        CHECK(at(engine, 2) == 128);
        CHECK(at(engine, 6) == 128);
        CHECK(at(engine, 8) == 128);
    }
    SECTION("a path orbits every head") {
        Payload path;
        path.kind = EffectKind::Path;
        path.size = 1.0f;
        path.cycles = 1.0f;
        path.durationSeconds = 4.0f;
        engine.start(0b1, path, 1.0);
        engine.tick(2.0); // a quarter of the way round a circle: pan at the middle, tilt at the top
        CHECK(at(engine, 2) == at(engine, 6));
        CHECK(at(engine, 4) == at(engine, 8));
        CHECK(at(engine, 4) == 255);
    }
}

TEST_CASE("a move can name one head, and leaves the others where they are",
          "[dmx][engine][heads]") {
    DmxEngine engine;
    engine.setPatch({twin()});

    engine.start(0b1, position(1.0f, 1.0f, heads(2)), 0.0);
    CHECK(at(engine, 6) == 255); // the second head's pan
    CHECK(at(engine, 8) == 255); // and tilt
    CHECK(at(engine, 2) == 128); // the first, untouched
    CHECK(at(engine, 4) == 128);

    SECTION("two rules on two heads both run: neither takes the other's channels") {
        // A circle on head 1 and a sweep on head 2, started together. Had the second taken the
        // first's place, as it takes the channels it shares, head 1 would stop where it began.
        Payload circle;
        circle.kind = EffectKind::Path;
        circle.shape = takt4::dmx::PathShape::Circle;
        circle.size = 1.0f;
        circle.cycles = 1.0f;
        circle.durationSeconds = 4.0f;
        circle.heads = heads(1);
        Payload sweep = circle;
        sweep.shape = takt4::dmx::PathShape::Sweep;
        sweep.heads = heads(2);
        engine.start(0b1, circle, 1.0);
        engine.start(0b1, sweep, 1.0);
        CHECK(engine.running() == 2); // the two paths; the sweep took head 2 from the snap
        engine.tick(2.0);             // a quarter of the way round
        CHECK(at(engine, 2) == 128);  // head 1: pan at the middle of its circle,
        CHECK(at(engine, 4) == 255);  //         tilt at the top
        CHECK(at(engine, 6) == 255);  // head 2: pan at the end of its sweep,
        CHECK(at(engine, 8) == 128);  //         tilt held at the middle
    }
    SECTION("a head the fixture has not got reaches nothing, and is counted") {
        engine.start(0b1, position(0.0f, 0.0f, heads(3)), 1.0);
        CHECK(engine.missed() == 1);
        CHECK(at(engine, 2) == 128);
        CHECK(at(engine, 6) == 255);
    }
    SECTION("a head names the same head on every fixture aimed at, and passes over one without") {
        Fixture spot = takt4::dmx::fixtureFromMode("spot", 6, 0, 20); // one head, 16-bit, at 20
        spot.id = "f-spot";
        engine.setPatch({twin(), spot});
        engine.start(0b11, position(0.0f, 0.0f, heads(2)), 1.0);
        CHECK(engine.missed() == 0);
        CHECK(at(engine, 6) == 0);
        CHECK(at(engine, 20) == 128); // the spot's pan, parked at the middle, where it was
    }
}

TEST_CASE("a move on one head keeps to that head through a re-patch", "[dmx][engine][heads]") {
    DmxEngine engine;
    engine.setPatch({twin()});
    Payload path;
    path.kind = EffectKind::Path;
    path.size = 1.0f;
    path.cycles = 1.0f;
    path.durationSeconds = 4.0f;
    path.heads = heads(2);
    engine.start(0b1, path, 0.0);

    // Renamed and moved down the universe: the path goes with it, on its second head.
    Fixture moved = twin(101);
    moved.name = "twin, renamed";
    engine.setPatch({moved});
    REQUIRE(engine.running() == 1);
    engine.tick(1.0);
    CHECK(at(engine, 108) == 255); // head 2's tilt, at the top of the circle
    CHECK(at(engine, 104) == 128); // head 1's, parked

    SECTION("and stops when the fixture loses that head") {
        Fixture one = twin(101);
        one.channels.resize(5); // the second yoke's channels taken off
        one.parked.resize(5);
        engine.setPatch({one});
        CHECK(engine.running() == 0);
    }
}

TEST_CASE("a move can name any set of heads", "[dmx][engine][heads]") {
    // A bar of three tilting segments: the outer two, and not the middle.
    Fixture bar;
    bar.id = "f-bar";
    bar.name = "bar";
    bar.address = 1;
    bar.channels = {Role::Tilt, Role::Tilt, Role::Tilt};
    bar.parked = {128, 128, 128};
    DmxEngine engine;
    engine.setPatch({bar});
    engine.start(0b1, position(0.5f, 1.0f, heads(1, 3)), 0.0);
    CHECK(at(engine, 1) == 255);
    CHECK(at(engine, 2) == 128);
    CHECK(at(engine, 3) == 255);
    CHECK(engine.missed() == 0);
}

TEST_CASE("a pan two heads share is the first head's, so two rules never fight over it",
          "[dmx][engine][heads]") {
    // The operator, 2026-10-05: one pan and two tilts "only send stuff to the first one dont let
    // rules fight over it". The Crazy Pocket 8's 9-channel mode: a pan, two tilting bars.
    Fixture pocket;
    pocket.id = "f-pocket";
    pocket.name = "pocket";
    pocket.address = 1;
    pocket.channels = {Role::Pan, Role::Tilt, Role::Tilt};
    pocket.parked = {128, 128, 128};
    DmxEngine engine;
    engine.setPatch({pocket});
    REQUIRE(takt4::dmx::headsOf(pocket) == 2);

    // A path on head 2 runs; a position on head 1 arrives and takes the pan and its tilt only.
    Payload path;
    path.kind = EffectKind::Path;
    path.shape = takt4::dmx::PathShape::Circle;
    path.size = 1.0f;
    path.cycles = 1.0f;
    path.durationSeconds = 4.0f;
    path.heads = heads(2);
    engine.start(0b1, path, 0.0);
    CHECK(at(engine, 1) == 128); // head 2 has no pan: the path leaves it
    engine.start(0b1, position(0.0f, 0.0f, heads(1)), 0.5);
    CHECK(engine.running() == 2); // neither stopped the other
    engine.tick(1.0);             // a quarter round: the bottom bar at the top of its circle
    CHECK(at(engine, 1) == 0);
    CHECK(at(engine, 2) == 0);
    CHECK(at(engine, 3) == 255);
}

TEST_CASE("a spread staggers the heads a move reaches", "[dmx][engine][heads]") {
    DmxEngine engine;
    Fixture bar;
    bar.id = "f-bar";
    bar.name = "bar";
    bar.address = 1;
    bar.channels = {Role::Tilt, Role::Tilt, Role::Tilt, Role::Tilt};
    bar.parked = {0, 0, 0, 0};
    engine.setPatch({bar});

    SECTION("round a path: at full spread, a quarter of a turn apart for four heads") {
        Payload sweep;
        sweep.kind = EffectKind::Path;
        sweep.shape = takt4::dmx::PathShape::Circle;
        sweep.size = 1.0f;
        sweep.cycles = 1.0f;
        sweep.durationSeconds = 4.0f;
        sweep.spread = 1.0f;
        engine.start(0b1, sweep, 0.0);
        engine.tick(1.0); // the first a quarter round: tilt at the top
        CHECK(at(engine, 1) == 255);
        CHECK(at(engine, 2) == 128); // the second a quarter behind: at the start, the middle
        CHECK(at(engine, 3) == 0);   // the third half a turn behind: at the bottom
        // The fourth three quarters behind: the middle again — give or take the step a sine of
        // minus pi, a hair under nothing, rounds to.
        CHECK(std::abs(at(engine, 4) - 128) <= 1);
    }
    SECTION("on a position: each head's move starts that much of its time later") {
        Payload move = position(0.5f, 1.0f);
        move.durationSeconds = 2.0f;
        move.curve = takt4::dmx::Curve::Linear;
        move.spread = 0.5f; // four heads: each an eighth of the move later than the one before
        engine.start(0b1, move, 0.0);
        engine.tick(1.0); // the first half way; the second a quarter second behind; ...
        CHECK(at(engine, 1) == 128);
        CHECK(at(engine, 2) == 96);
        CHECK(at(engine, 3) == 64);
        CHECK(at(engine, 4) == 32);
        engine.tick(2.0);
        CHECK(at(engine, 1) == 255);
        CHECK(engine.running() == 1); // still going: the last arrives at 2 + 3 x 0.25 s
        engine.tick(2.75);
        CHECK(at(engine, 4) == 255);
        CHECK(engine.running() == 0);
    }
    SECTION("at none, the default, they move together") {
        Payload move = position(0.5f, 1.0f);
        move.durationSeconds = 2.0f;
        move.curve = takt4::dmx::Curve::Linear;
        engine.start(0b1, move, 0.0);
        engine.tick(1.0);
        CHECK(at(engine, 1) == at(engine, 4));
    }
}
