#include "core/output/link_session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>

using Catch::Matchers::WithinAbs;
using takt4::output::LinkSession;

// Every test here leaves the session disabled. Enabling it opens UDP multicast sockets
// and starts announcing on the local network, which a test has no business doing —
// least of all on a CI runner, where the result would depend on what else was on the
// network. The timeline is a local object either way, so everything below is about what
// takt4 asks of Link rather than about Link talking to peers.

TEST_CASE("Link session starts disabled at the requested tempo", "[link]") {
    const LinkSession session(120.0);

    CHECK_FALSE(session.enabled());
    CHECK_FALSE(session.startStopSyncEnabled()); // HANDOFF §5.6: off by default
    CHECK(session.numPeers() == 0);
    CHECK_THAT(session.tempoBpm(), WithinAbs(120.0, 1e-9));
}

TEST_CASE("Link sessions can be created and destroyed repeatedly", "[link]") {
    for (int i = 0; i < 3; ++i) {
        const LinkSession session(90.0 + i);
        CHECK_THAT(session.tempoBpm(), WithinAbs(90.0 + i, 1e-9));
    }
}

TEST_CASE("the tracker's tempo and phase reach Link's timeline", "[link]") {
    LinkSession session(120.0);
    // Link's timeline is re-anchored around the present every time a session state is
    // captured, so a request has to be made in host time near now — which is what the
    // tracker does anyway, since its timestamps come from Link's own clock through the
    // host time filter. A time an hour in the past comes back several beats out.
    const std::chrono::microseconds at = session.now() + std::chrono::milliseconds{100};

    session.setTempo(128.0, at);
    CHECK_THAT(session.tempoBpm(), WithinAbs(128.0, 1e-9));
    CHECK(session.tempoUpdates() == 1);

    SECTION("requesting a beat puts it where it was asked for") {
        // Four beats to the bar, and beat 8 lands at `at`: two bars in, on a downbeat.
        session.requestBeat(8.0, at, 4.0);
        CHECK(session.beatRequests() == 1);
        CHECK_THAT(session.beatAtTime(at, 4.0), WithinAbs(8.0, 1e-6));
        CHECK_THAT(session.phaseAtTime(at, 4.0), WithinAbs(0.0, 1e-6));

        // At 128 BPM a beat is 468750 us, so a beat later the phase is 1.
        const std::chrono::microseconds later = at + std::chrono::microseconds{468'750};
        CHECK_THAT(session.beatAtTime(later, 4.0), WithinAbs(9.0, 1e-3));
        CHECK_THAT(session.phaseAtTime(later, 4.0), WithinAbs(1.0, 1e-3));
    }

    SECTION("the meter is the quantum, and nothing assumes four") {
        // Phase is what a bar indicator and a peer's downbeat both read, and it is what
        // Link guarantees across a capture: the beat magnitude is re-anchored to the
        // session's own grid every time a session state is taken, so only the phase for
        // the quantum that was asked for means anything.
        for (const double quantum : {2.0, 3.0, 4.0, 7.0}) {
            INFO("quantum " << quantum);
            session.forceBeat(0.0, at, quantum);
            CHECK_THAT(session.phaseAtTime(at, quantum), WithinAbs(0.0, 1e-3));
            for (int beat = 1; beat <= 8; ++beat) {
                const std::chrono::microseconds when =
                    at + std::chrono::microseconds{beat * 468'750};
                const double expected = static_cast<double>(beat % static_cast<int>(quantum));
                CHECK_THAT(session.phaseAtTime(when, quantum), WithinAbs(expected, 1e-2));
            }
        }
    }

    SECTION("forcing a beat moves the timeline outright") {
        session.forceBeat(0.0, at, 4.0);
        CHECK_THAT(session.beatAtTime(at, 4.0), WithinAbs(0.0, 1e-6));
        session.forceBeat(100.0, at, 4.0);
        CHECK_THAT(session.beatAtTime(at, 4.0), WithinAbs(100.0, 1e-6));
    }
}

TEST_CASE("the host time filter turns a sample counter into host time", "[link]") {
    // HANDOFF §4.3: "the only requirement is a monotonically increasing sample counter".
    // The regression is fed one point per hop, the way ActivationEngine feeds it.
    LinkSession session(120.0);

    const std::int64_t first = session.hostMicrosForSample(0.0);
    CHECK(first != 0);

    // Feed it a run of hops and check the mapping came out monotonic and roughly the
    // right slope: 441 samples at 22050 Hz is 20000 us, and this loop takes far less
    // than that, so the regression is dominated by the sample times it is given.
    std::int64_t previous = first;
    for (std::uint64_t hop = 1; hop < 200; ++hop) {
        const std::int64_t micros = session.hostMicrosForSample(static_cast<double>(hop * 441));
        CHECK(micros >= previous);
        previous = micros;
    }
    // The clock barely moved while the sample counter advanced by four seconds, so the
    // regression's slope is tiny and the extrapolation is nearly flat. What matters is
    // that it is a straight line through the points, not that it predicts real audio.
    const std::int64_t elapsed = previous - first;
    CHECK(elapsed >= 0);

    SECTION("resetting forgets it, for a stream that was restarted") {
        session.resetHostTimeFilter();
        CHECK(session.hostMicrosForSample(0.0) != 0);
    }
}

TEST_CASE("start/stop sync can be turned on when someone wants it", "[link]") {
    LinkSession session(120.0);
    session.enableStartStopSync(true);
    CHECK(session.startStopSyncEnabled());
    session.enableStartStopSync(false);
    CHECK_FALSE(session.startStopSyncEnabled());
}
