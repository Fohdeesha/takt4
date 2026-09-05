#include "core/output/link_session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

using Catch::Matchers::WithinAbs;
using takt4::output::LinkSession;

namespace {

/// Committing a session state and capturing it again is not one atomic operation:
/// `commitAppSessionState` hands the change to Link's own controller, and a capture
/// taken in the same instant can still return the timeline from before it. Measured on
/// this machine, an immediate re-capture is a fifth of a beat out roughly four times in
/// five. Nothing takt4 does reads a state back the microsecond it wrote it — it commits
/// on a beat and reads on the next UI frame — so the tests wait the way an application
/// would.
void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
}

} // namespace

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
    //
    // The tolerances are hundredths of a beat, which is 4.7 ms at 128 BPM — far finer
    // than anything downstream of a 40 ms feature pipeline could act on, and coarse
    // enough not to be a test of Link's rounding.
    const std::chrono::microseconds at = session.now() + std::chrono::milliseconds{100};

    session.setTempo(128.0, at);
    settle();
    CHECK_THAT(session.tempoBpm(), WithinAbs(128.0, 1e-9));
    CHECK(session.tempoUpdates() == 1);

    SECTION("requesting a beat puts its phase where it was asked for") {
        // Four beats to the bar, and beat 8 is a downbeat: phase 0.
        session.requestBeat(8.0, at, 4.0);
        settle();
        CHECK(session.beatRequests() == 1);
        CHECK_THAT(session.phaseAtTime(at, 4.0), WithinAbs(0.0, 1e-2));

        // At 128 BPM a beat is 468750 us, so a beat later the phase is 1 and four beats
        // later it has come back round.
        const std::chrono::microseconds later = at + std::chrono::microseconds{468'750};
        CHECK_THAT(session.phaseAtTime(later, 4.0), WithinAbs(1.0, 1e-2));
        CHECK_THAT(session.phaseAtTime(at + std::chrono::microseconds{4 * 468'750}, 4.0),
                   WithinAbs(0.0, 1e-2));
    }

    SECTION("the meter is the quantum, and nothing assumes four") {
        // Phase is what a bar indicator and a peer's downbeat both read, and it is what
        // Link guarantees across a capture: the beat magnitude is re-anchored to the
        // session's own grid every time a session state is taken, so only the phase for
        // the quantum that was asked for means anything.
        for (const double quantum : {2.0, 3.0, 4.0, 7.0}) {
            INFO("quantum " << quantum);
            session.forceBeat(0.0, at, quantum);
            settle();
            CHECK_THAT(session.phaseAtTime(at, quantum), WithinAbs(0.0, 1e-3));
            for (int beat = 1; beat <= 8; ++beat) {
                const std::chrono::microseconds when =
                    at + std::chrono::microseconds{beat * 468'750};
                const double expected = static_cast<double>(beat % static_cast<int>(quantum));
                CHECK_THAT(session.phaseAtTime(when, quantum), WithinAbs(expected, 1e-2));
            }
        }
    }

    SECTION("forcing a beat moves the timeline's magnitude, not just its phase") {
        // requestBeat only ever changes where the phase falls. forceBeat also moves the
        // beat count, which is why §5.6 keeps it for the manual downbeat snap alone.
        session.forceBeat(0.0, at, 4.0);
        settle();
        const double from = session.beatAtTime(at, 4.0);
        session.forceBeat(100.0, at, 4.0);
        settle();
        CHECK_THAT(session.beatAtTime(at, 4.0) - from, WithinAbs(100.0, 1e-2));
        CHECK(session.beatRequests() == 2);
    }
}

TEST_CASE("the host time filter stamps a hop with the time it was fed at", "[link]") {
    // HANDOFF §4.3: "the only requirement is a monotonically increasing sample counter".
    // Feed it the way ActivationEngine does — one point per hop, with real time passing
    // between them — and the stamp for the hop being fed is the present. What it must
    // not be given is a burst of points taken at the same instant: the least-squares
    // line through those has no slope worth anything, and asking for one back is asking
    // the regression about noise. That is not a fault in the filter, and nothing in
    // takt4 does it; a hop only exists once its 20 ms of audio has arrived.
    LinkSession session(120.0);

    // Two claims, and only one of them is this machine's to make.
    //
    // Real hops arrive on a hardware clock, evenly. This test fakes them with sleep_for,
    // and the regression fits a straight line through whatever intervals it was actually
    // handed — so one long gap among short ones skews the fit and the prediction with it.
    // That is the test's premise breaking, not the filter: measured on CI 2026-09-05, a
    // descheduled thread turned a 2 ms sleep into most of a second and the stamp came out
    // 716 ms off.
    //
    // So: the stamp being on Link's clock at all — the right epoch, the right unit, a
    // slope that is not inverted — is checked always, because no amount of scheduling
    // noise can fake it. Accuracy is checked only when the hops really did arrive evenly,
    // which is the only condition under which this test is entitled to an opinion on it.
    constexpr std::uint64_t kWarmUp = 5;
    constexpr std::uint64_t kHops = 40;
    constexpr std::chrono::milliseconds kSleep{2};
    /// Wide enough that scheduling cannot reach it, narrow enough that a wrong epoch, a
    /// wrong unit or an inverted slope cannot hide inside it.
    constexpr std::int64_t kOnLinksClock = 5'000'000;
    /// What the filter is held to when the feed was regular.
    constexpr std::int64_t kTolerance = 250'000;
    /// A gap this far past the sleep means the thread was descheduled, and the regression
    /// was fed intervals that §4.3's premise says it never sees.
    constexpr std::int64_t kGapLimit = 50'000;

    std::int64_t worst = 0;
    bool evenlyFed = true;
    for (std::uint64_t hop = 0; hop < kHops; ++hop) {
        const std::int64_t before = session.now().count();
        const std::int64_t micros = session.hostMicrosForSample(static_cast<double>(hop) * 441.0);
        const std::int64_t after = session.now().count();
        if (hop >= kWarmUp) {
            // The stamp is when the regression thinks this hop's samples arrived, and this
            // hop's samples are arriving now.
            CHECK(micros > before - kOnLinksClock);
            CHECK(micros < after + kOnLinksClock);
            // How far outside [before, after] it fell. Inside that bracket the stamp *is*
            // the present, however long the call itself took.
            worst = std::max({worst, before - micros, micros - after});
        }
        std::this_thread::sleep_for(kSleep);
        if (session.now().count() - after > kGapLimit) {
            evenlyFed = false;
        }
    }
    INFO("worst stamp error " << worst << " us outside the bracket");
    if (evenlyFed) {
        CHECK(worst < kTolerance);
    } else {
        WARN("the runner descheduled this thread, so the hops were not evenly fed and the "
             "regression's accuracy was not tested; that the stamp is on Link's clock was");
    }

    SECTION("resetting forgets it, for a stream that was restarted") {
        session.resetHostTimeFilter();
        const std::int64_t before = session.now().count();
        const std::int64_t micros = session.hostMicrosForSample(0.0);
        // One point is a degenerate fit, and Link returns the mean host time rather than
        // dividing by zero, which is still the present.
        CHECK(micros > before - kTolerance);
    }
}

TEST_CASE("start/stop sync can be turned on when someone wants it", "[link]") {
    LinkSession session(120.0);
    session.enableStartStopSync(true);
    CHECK(session.startStopSyncEnabled());
    session.enableStartStopSync(false);
    CHECK_FALSE(session.startStopSyncEnabled());
}
