#include "core/output/link_session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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
///
/// **Until the capture shows it** (the audit of 2026-09-25, T17): a fixed 50 ms was a guess
/// about how busy the machine was, and under ASan with the suite in parallel it is a guess that
/// can lose. Two seconds is the give-up, which nothing near a healthy machine comes close to.
///
/// **And never less than 50 ms**, because landing is not the whole of it. Link's own thread
/// answers each commit by writing the session's timeline back into the app's copy, and a second
/// commit made before that echo can be moved by it — measured: a beat forced 100 on came out
/// 99.80 about one run in five with ten copies of this test at once, and never once the pause
/// was back. A real beat is half a second after the last, so the pause is what an application
/// does anyway (see `LinkSession::snap`, which exists for the same reason).
template <typename Landed>
void settleUntil(Landed landed) {
    const auto start = std::chrono::steady_clock::now();
    const auto earliest = start + std::chrono::milliseconds{50};
    const auto until = start + std::chrono::seconds{2};
    for (auto now = start; now < until; now = std::chrono::steady_clock::now()) {
        if (now >= earliest && landed()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
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
    settleUntil([&] { return std::abs(session.tempoBpm() - 128.0) < 1e-9; });
    CHECK_THAT(session.tempoBpm(), WithinAbs(128.0, 1e-9));
    CHECK(session.tempoUpdates() == 1);

    SECTION("requesting a beat puts its phase where it was asked for") {
        // Four beats to the bar, and beat 8 is a downbeat: phase 0.
        session.requestBeat(8.0, at, 4.0);
        settleUntil([&] { return std::abs(session.phaseAtTime(at, 4.0)) < 1e-2; });
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
            settleUntil([&] { return std::abs(session.phaseAtTime(at, quantum)) < 1e-3; });
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
        // A moment of its own, next to now. The magnitude is re-anchored around the present at
        // every capture, so a time that has drifted into the past — as the one at the top has,
        // on a machine busy enough — comes back a fraction of a beat out: measured, 99.80 with
        // eight copies of this test running at once.
        const std::chrono::microseconds near = session.now() + std::chrono::milliseconds{100};
        session.forceBeat(0.0, near, 4.0);
        // Until the force shows: beat 0 is phase 0. Not "until two reads agree", which is what
        // this was — two reads of the timeline from *before* the force agree too, and under a
        // loaded machine that is what it saw, so `from` came from the wrong timeline.
        settleUntil([&] {
            const double phase = session.phaseAtTime(near, 4.0);
            return std::abs(phase) < 1e-3 || std::abs(phase - 4.0) < 1e-3;
        });
        const double from = session.beatAtTime(near, 4.0);
        session.forceBeat(100.0, near, 4.0);
        settleUntil(
            [&] { return std::abs(session.beatAtTime(near, 4.0) - from - 100.0) < 1e-2; });
        CHECK_THAT(session.beatAtTime(near, 4.0) - from, WithinAbs(100.0, 1e-2));
        CHECK(session.beatRequests() == 2);
    }
}

TEST_CASE("the host time line stamps a buffer's samples with when they were heard", "[link]") {
    // Each buffer of input is observed once — where its first sample is on the sample clock,
    // and when that sample was heard on the steady clock — and a sample's moment is
    // read off the line through them, on Link's clock. Fed here the way the input does it, a
    // buffer every couple of milliseconds of real time, each heard "now".
    LinkSession session(120.0);
    const auto steadyNow = [] {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

    // Two claims, and only one of them is this machine's to make — as before this was a line
    // fitted to buffers rather than a filter fed hops. Real buffers arrive on a hardware clock,
    // evenly; this test fakes them with sleep_for, and a descheduled thread turns a 2 ms sleep
    // into most of a second (measured on CI 2026-09-05). So the stamp being on Link's clock at
    // all — the right epoch, the right unit, a slope that is not inverted — is checked always,
    // and its accuracy only when the buffers really did arrive evenly.
    constexpr std::uint64_t kBuffers = 40;
    constexpr double kSamplesPerBuffer = 44.1; // 2 ms at 22050 Hz
    constexpr std::chrono::milliseconds kSleep{2};
    constexpr std::int64_t kOnLinksClock = 5'000'000;
    constexpr std::int64_t kTolerance = 250'000;
    constexpr std::int64_t kGapLimit = 50'000;

    CHECK(session.hostMicrosForSample(0.0) == 0); // nothing observed: no host time, not a guess
    std::int64_t worst = 0;
    bool evenlyFed = true;
    for (std::uint64_t buffer = 0; buffer < kBuffers; ++buffer) {
        const double sample = static_cast<double>(buffer) * kSamplesPerBuffer;
        const std::int64_t before = session.now().count();
        session.observe(sample, steadyNow());
        const std::int64_t micros = session.hostMicrosForSample(sample);
        const std::int64_t after = session.now().count();
        CHECK(micros > before - kOnLinksClock);
        CHECK(micros < after + kOnLinksClock);
        worst = std::max({worst, before - micros, micros - after});
        std::this_thread::sleep_for(kSleep);
        if (session.now().count() - after > kGapLimit) {
            evenlyFed = false;
        }
    }
    INFO("worst stamp error " << worst << " us outside the bracket");
    if (evenlyFed) {
        CHECK(worst < kTolerance);
    } else {
        WARN("the runner descheduled this thread, so the buffers were not evenly fed and the "
             "line's accuracy was not tested; that the stamp is on Link's clock was");
    }

    SECTION("resetting forgets it, for a stream that was restarted") {
        session.resetHostTimeFilter();
        CHECK(session.hostMicrosForSample(0.0) == 0);
        const std::int64_t before = session.now().count();
        session.observe(0.0, steadyNow());
        CHECK(session.hostMicrosForSample(0.0) > before - kTolerance);
    }
}

TEST_CASE("start/stop sync can be turned on when someone wants it", "[link]") {
    LinkSession session(120.0);
    session.enableStartStopSync(true);
    CHECK(session.startStopSyncEnabled());
    session.enableStartStopSync(false);
    CHECK_FALSE(session.startStopSyncEnabled());
}
