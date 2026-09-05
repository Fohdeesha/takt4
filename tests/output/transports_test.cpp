#include "core/output/transports.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstdint>

using Catch::Matchers::WithinAbs;
using takt4::output::Transports;
using takt4::tracking::BeatEvent;
using takt4::tracking::TempoState;

namespace {

/// A beat of the shape the tempo state machine produces: on the beat, in a bar of four.
BeatEvent beatAt(double time, std::uint32_t beatInBar, double bpm) {
    BeatEvent event;
    event.time = time;
    event.bpm = bpm;
    event.beatInBar = beatInBar;
    event.beatsPerBar = 4;
    event.downbeat = beatInBar == 1;
    event.locked = true;
    event.confidence = 0.8;
    return event;
}

} // namespace

TEST_CASE("transports with nothing configured send nothing and still count", "[output]") {
    // The state an app is in before anyone has set an output up, which is most of the
    // time it is open. It has to be as harmless as it is uninteresting.
    Transports transports(Transports::Config{});

    CHECK_FALSE(transports.any());
    CHECK(transports.link() == nullptr);
    CHECK(transports.osc() == nullptr);
    CHECK(transports.midiClock() == nullptr);
    CHECK(transports.midiPort() == nullptr);

    transports.startOutputs(0.0);
    for (std::uint32_t beat = 1; beat <= 8; ++beat) {
        transports.publish(beatAt(0.5 * beat, (beat - 1) % 4 + 1, 120.0), 0, 0.5 * beat);
        transports.advance(0.5 * beat, TempoState{});
    }
    transports.stopOutputs();

    CHECK(transports.beats() == 8);
    CHECK(transports.downbeats() == 2); // beats 1 and 5
}

TEST_CASE("the latency offset round-trips and is what the transports fire on", "[output]") {
    // §5.5: the tracker applies the offset to event.time, and this is the same number
    // applied to the host times the transports fire on. The two drifting apart is the
    // failure — a beat reported at one time and sent at another.
    Transports::Config config;
    config.latencySeconds = -0.030;
    Transports transports(config);
    CHECK_THAT(transports.latencySeconds(), WithinAbs(-0.030, 1e-9));

    transports.setLatencySeconds(0.012);
    CHECK_THAT(transports.latencySeconds(), WithinAbs(0.012, 1e-9));
}

TEST_CASE("a beat reaches Link as a tempo and a bar position", "[output][link]") {
    // The wiring only, held to Link's own counters so that nothing here depends on
    // timing. What those calls then *mean* — that only the phase survives a capture,
    // that a request is moved to where the phase already matches — is
    // link_session_test.cpp's business.
    Transports::Config config;
    config.link = true;
    Transports transports(config);
    REQUIRE(transports.link() != nullptr);
    transports.startOutputs(0.0);

    const std::int64_t hostMicros = transports.link()->now().count();

    // Offline, there is no host clock to align to, so Link is deliberately left alone.
    transports.publish(beatAt(0.5, 1, 120.0), 0, 0.5);
    CHECK(transports.link()->tempoUpdates() == 0);
    CHECK(transports.link()->beatRequests() == 0);

    // Live, the same beat is a tempo and a bar position.
    transports.publish(beatAt(1.0, 1, 128.0), hostMicros, 1.0);
    CHECK(transports.link()->tempoUpdates() == 1);
    CHECK(transports.link()->beatRequests() == 1);

    // A tempo that has not moved is not resent: the refined tempo wobbles by hundredths
    // of a BPM on almost every beat, and no peer can act on that.
    transports.publish(beatAt(1.5, 2, 128.001), hostMicros, 1.5);
    CHECK(transports.link()->tempoUpdates() == 1);
    CHECK(transports.link()->beatRequests() == 2);

    // A tempo that really moved is.
    transports.publish(beatAt(2.0, 3, 130.0), hostMicros, 2.0);
    CHECK(transports.link()->tempoUpdates() == 2);
    CHECK(transports.link()->beatRequests() == 3);

    // Before the first downbeat the bar phase is unknown, and publishing a guess would
    // put every peer on the wrong beat of the bar. Nothing goes out for it.
    BeatEvent unphased = beatAt(2.5, 0, 130.0);
    unphased.beatsPerBar = 0;
    transports.publish(unphased, hostMicros, 2.5);
    CHECK(transports.link()->beatRequests() == 3);
    CHECK(transports.link()->tempoUpdates() == 2);

    transports.stopOutputs();
    CHECK(transports.beats() == 5);
}
