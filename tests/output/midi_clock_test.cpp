#include "core/output/midi_clock.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::output::MidiClock;
using takt4::output::MidiOutput;
using takt4::output::MidiSink;

namespace {

/// Keeps every byte, so a test can say exactly what went down the wire.
class Recorder final : public MidiSink {
public:
    void send(std::span<const unsigned char> message) noexcept override {
        for (const unsigned char byte : message) {
            bytes.push_back(byte);
        }
        ++messages;
    }

    std::size_t count(unsigned char status) const {
        std::size_t found = 0;
        for (const unsigned char byte : bytes) {
            found += byte == status ? 1 : 0;
        }
        return found;
    }

    std::vector<unsigned char> bytes;
    std::size_t messages = 0;
};

} // namespace

TEST_CASE("nothing goes out before the clock is started", "[output][midi]") {
    Recorder recorder;
    MidiClock clock(recorder, 120.0);
    CHECK_FALSE(clock.running());
    CHECK(clock.advance(10.0) == 0);
    CHECK(recorder.bytes.empty());

    CHECK_THROWS_AS(MidiClock(recorder, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(MidiClock(recorder, -120.0), std::invalid_argument);
}

TEST_CASE("the clock runs at 24 pulses per quarter note", "[output][midi]") {
    Recorder recorder;
    MidiClock clock(recorder, 120.0); // two quarter notes a second: 48 ticks
    clock.start(0.0);
    CHECK(recorder.bytes == std::vector<unsigned char>{MidiClock::kStart});
    CHECK(clock.running());

    // Ticks fall on k/48 seconds, starting with k = 0. Advancing to just under a second
    // covers k = 0 to 47 — a whole second's worth, without asking a floating-point
    // comparison to decide whether the tick exactly on 1.0 has come due yet.
    CHECK(clock.advance(0.99) == 48);
    CHECK(clock.ticksSent() == 48);
    CHECK(recorder.count(MidiClock::kTick) == 48);
    CHECK(clock.pulseInQuarter() == 0);

    SECTION("a second second brings the total to two quarter notes a second") {
        CHECK(clock.advance(1.99) == 48);
        CHECK(clock.ticksSent() == 96);
    }

    SECTION("advancing to the same time again emits nothing") {
        CHECK(clock.advance(0.99) == 0);
        CHECK(clock.ticksSent() == 48);
    }

    SECTION("stop sends Stop and silences it") {
        clock.stop();
        CHECK(recorder.bytes.back() == MidiClock::kStop);
        CHECK_FALSE(clock.running());
        CHECK(clock.advance(5.0) == 0);
        CHECK(clock.ticksSent() == 48);

        // A second stop is not a second message.
        const std::size_t was = recorder.bytes.size();
        clock.stop();
        CHECK(recorder.bytes.size() == was);
    }

    SECTION("continue rather than start, for a transport that was paused") {
        clock.stop();
        clock.start(2.0, true);
        CHECK(recorder.bytes.back() == MidiClock::kContinue);
    }
}

TEST_CASE("the tick rate follows the tempo", "[output][midi]") {
    Recorder recorder;
    MidiClock clock(recorder, 60.0); // one quarter note a second: 24 ticks
    clock.start(0.0);
    CHECK(clock.advance(0.99) == 24);

    clock.setTempo(120.0);
    CHECK(clock.tempo() == Catch::Approx(120.0));
    // The tick already scheduled is at 1.0; from there the spacing halves, so the second
    // second holds it plus 47 more.
    CHECK(clock.advance(1.99) == 48);

    SECTION("a nonsensical tempo is ignored rather than dividing by zero") {
        clock.setTempo(0.0);
        CHECK(clock.tempo() == Catch::Approx(120.0));
        clock.setTempo(-5.0);
        CHECK(clock.tempo() == Catch::Approx(120.0));
    }
}

TEST_CASE("syncing to a beat puts a tick on it", "[output][midi]") {
    Recorder recorder;
    MidiClock clock(recorder, 120.0);
    clock.start(0.0);
    (void)clock.advance(0.7);
    REQUIRE(clock.pulseInQuarter() != 0);

    // The tracker calls a beat at 0.7s, a little off where the free-running schedule had
    // got to. The clock has to put pulse 0 there rather than carry on drifting.
    clock.syncToBeat(0.7);
    CHECK(clock.pulseInQuarter() == 0);
    const std::uint64_t before = clock.ticksSent();
    CHECK(clock.advance(0.7) == 1); // the tick due exactly on the beat
    CHECK(clock.ticksSent() == before + 1);
    CHECK(clock.pulseInQuarter() == 1);

    SECTION("syncing while stopped does nothing") {
        clock.stop();
        clock.syncToBeat(1.0);
        CHECK(clock.advance(2.0) == 0);
    }
}

TEST_CASE("a long stall is skipped rather than flooded", "[output][midi]") {
    Recorder recorder;
    MidiClock clock(recorder, 120.0);
    clock.start(0.0);

    // Nothing advanced the clock for a minute. Sending the 2880 ticks that were missed
    // would be worse for the receiver than skipping them.
    const std::size_t emitted = clock.advance(60.0);
    CHECK(emitted == MidiClock::kMaxBurst);
    CHECK(clock.ticksSkipped() > 2000);

    // ...and it carries on from where it gave up, at the right rate.
    const std::uint64_t after = clock.ticksSent();
    CHECK(clock.advance(61.0) == 48);
    CHECK(clock.ticksSent() == after + 48);
    // Nothing was lost: the ticks sent plus the ones skipped are the whole minute.
    CHECK(clock.ticksSent() + clock.ticksSkipped() >= 60 * 48);
}

TEST_CASE("a MIDI port that is not there is named as an error", "[output][midi]") {
    // No machine this runs on is guaranteed to have a MIDI device, so the negative case
    // is the one that can be asserted anywhere. The message lists what was found, which
    // is what an operator with a differently-named interface needs.
    CHECK_THROWS_WITH(MidiOutput("no such port, surely"),
                      ContainsSubstring("no MIDI output port matching"));
    CHECK_THROWS_WITH(MidiOutput("99999"), ContainsSubstring("no MIDI output port matching"));
}
