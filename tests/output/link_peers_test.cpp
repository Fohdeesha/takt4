#include "core/output/link_session.hpp"
#include "core/output/transports.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

using Catch::Matchers::WithinAbs;
using takt4::output::LinkSession;

// The only part of HANDOFF §8 Phase 4's exit criterion that this machine cannot check on
// its own is "Resolume or equivalent follows it": nothing here speaks Link. Two
// LinkSessions in one process is as close as it gets — they discover each other over the
// same UDP multicast a real peer would, so what is checked is that takt4 announces
// itself, that a peer sees the tempo it publishes, and that the phase it requests is the
// phase the peer reads. What is not checked is any third-party implementation.
//
// **[network]**, so the default presets leave them out and the `-all` presets — and CI — run
// them. They open multicast sockets and announce on the local network. They used to be hidden
// (`[.link-network]`) instead, which kept them off the network but also out of every run there
// was: no preset registered them, so the audit's C3 fix — force the phase once, never request
// it — could have been reverted with every test still passing (the audit of 2026-09-25, T6). A
// network that blocks multicast is a skip that says so, not a failure: it is not takt4's fault.
//
// Every other test in tests/output/link_session_test.cpp leaves the session disabled.

namespace {

/// Waits for `predicate` or gives up. Link's discovery is asynchronous and goes over the
/// network, so there is no callback to hang a deterministic test off.
template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds limit = std::chrono::seconds{10}) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return predicate();
}

} // namespace

TEST_CASE("a Link peer sees the tempo and phase takt4 publishes", "[output][link][network]") {
    LinkSession tracker(120.0); // takt4, driving
    LinkSession peer(60.0);     // whatever is listening

    tracker.enable(true);
    peer.enable(true);

    // Discovery. If this fails the network is blocking multicast, not takt4.
    if (!eventually([&] { return tracker.numPeers() >= 1 && peer.numPeers() >= 1; })) {
        SKIP("two Link sessions in one process never found each other: multicast is blocked");
    }
    INFO("tracker sees " << tracker.numPeers() << " peers, peer sees " << peer.numPeers());

    SECTION("the tempo crosses over") {
        // A session that joins adopts the tempo already on the network, so the two agree
        // on something before anything is asked of them.
        REQUIRE(eventually([&] { return std::abs(peer.tempoBpm() - tracker.tempoBpm()) < 1e-6; }));

        tracker.setTempo(128.0, tracker.now());
        REQUIRE(eventually([&] { return std::abs(peer.tempoBpm() - 128.0) < 1e-6; }));
        CHECK_THAT(tracker.tempoBpm(), WithinAbs(128.0, 1e-6));

        // ...and again, so this is not one lucky initial handshake.
        tracker.setTempo(96.0, tracker.now());
        REQUIRE(eventually([&] { return std::abs(peer.tempoBpm() - 96.0) < 1e-6; }));
    }

    SECTION("a peer reads the same bar position, with the meter as the quantum") {
        tracker.setTempo(120.0, tracker.now()); // a beat every half second
        REQUIRE(eventually([&] { return std::abs(peer.tempoBpm() - 120.0) < 1e-6; }));

        // What is *not* asserted: that phase 0 lands on the time asked for.
        // requestBeatAtTime respects the quantum — it moves the request to the next time
        // our phase already matches, which is the whole point of it being the gentle
        // call and can be up to a bar later. §5.6 keeps forceBeatAtTime for the manual
        // snap precisely because only that one places a beat outright.
        //
        // What matters, and what a peer's bar indicator reads, is that the two sides
        // agree on the phase at a given host time and that it advances a beat at a time.
        tracker.requestBeat(0.0, tracker.now() + std::chrono::milliseconds{500}, 4.0);

        const auto beat = std::chrono::microseconds{500'000};
        // A twentieth of a beat of slack: two sessions' clocks and a network round trip,
        // not the arithmetic that link_session_test.cpp checks locally.
        constexpr double kSlack = 0.05;
        const auto agrees = [&] {
            const std::chrono::microseconds when = tracker.now() + std::chrono::seconds{1};
            return std::abs(peer.phaseAtTime(when, 4.0) - tracker.phaseAtTime(when, 4.0)) < kSlack;
        };
        REQUIRE(eventually(agrees));

        const std::chrono::microseconds base = tracker.now() + std::chrono::seconds{1};
        for (int step = 0; step < 8; ++step) {
            const std::chrono::microseconds when = base + step * beat;
            INFO("beat " << step << ", tracker says " << tracker.phaseAtTime(when, 4.0));
            CHECK_THAT(peer.phaseAtTime(when, 4.0),
                       WithinAbs(tracker.phaseAtTime(when, 4.0), kSlack));
        }

        // The phase advances one beat at a time and wraps at the quantum — which is the
        // detected meter, and is not four. Both sides, same grid.
        for (const double quantum : {2.0, 3.0, 4.0, 7.0}) {
            INFO("quantum " << quantum);
            const double from = tracker.phaseAtTime(base, quantum);
            const double next = std::fmod(from + 1.0, quantum);
            CHECK_THAT(tracker.phaseAtTime(base + beat, quantum), WithinAbs(next, kSlack));
            CHECK_THAT(peer.phaseAtTime(base + beat, quantum), WithinAbs(next, kSlack));
        }
    }

    tracker.enable(false);
    peer.enable(false);
    // Leaving the network has to be seen too, or a stopped takt4 would haunt a peer's
    // peer count for as long as it kept running.
    CHECK(eventually([&] { return peer.numPeers() == 0; }));
}

TEST_CASE("a peer that was there first is put on the music's phase", "[output][link][network]") {
    // The audit's C3, as its experiment ran it: Resolume on Link first, takt4 enabling Link
    // after, so takt4's session adopts Resolume's timeline on join — and the music's beats fall
    // 180 ms off it. With a peer present `requestBeatAtTime` does not move the session's phase
    // ("the next time value ... with the same phase"), so takt4 used to change only the tempo:
    // measured, both sessions stayed 180 ms off the music for every beat, the bar 1.6 beats
    // wrong. The first locked beat now forces the phase — the operator's call, snap once and
    // then nudge — and the peer reads the music's bar.
    LinkSession peer(128.0);
    peer.enable(true);
    // The peer's own bar, somewhere the music is not.
    peer.forceBeat(0.0, peer.now() + std::chrono::milliseconds{300}, 4.0);
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    takt4::output::Transports::Config config;
    config.link = true;
    takt4::output::Transports app(config);
    app.startOutputs(0.0);
    LinkSession& ours = app.link();
    if (!eventually([&] { return ours.numPeers() >= 1 && peer.numPeers() >= 1; })) {
        SKIP("two Link sessions in one process never found each other: multicast is blocked");
    }
    // Joined, and on one timeline — whichever side won it, the music is not on it, which is
    // the case a request cannot fix. Brought to the music's tempo first, so the only thing
    // wrong is the phase.
    REQUIRE(eventually([&] { return std::abs(ours.tempoBpm() - peer.tempoBpm()) < 1e-6; }));
    ours.setTempo(128.0, ours.now());
    REQUIRE(eventually([&] { return std::abs(peer.tempoBpm() - 128.0) < 1e-6; }));
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    // The music: 128 BPM, its bar starting 180 ms after the peer's.
    const double beat = 60.0 / 128.0;
    const std::chrono::microseconds peerBar =
        ours.now() + std::chrono::seconds{2} -
        std::chrono::microseconds{static_cast<std::int64_t>(ours.phaseAtTime(
                                                                  ours.now() + std::chrono::seconds{2}, 4.0) *
                                                              beat * 1e6)};
    const std::int64_t musicBar = peerBar.count() + 180'000;
    REQUIRE(std::abs(peer.phaseAtTime(std::chrono::microseconds{musicBar}, 4.0) -
                     0.180 / beat) < 0.05);

    for (std::uint32_t k = 0; k < 8; ++k) {
        takt4::tracking::BeatEvent event;
        event.bpm = 128.0;
        event.locked = true;
        event.confidence = 0.9;
        event.beatsPerBar = 4;
        event.beatInBar = k % 4 + 1;
        event.downbeat = event.beatInBar == 1;
        const std::int64_t at = musicBar + static_cast<std::int64_t>(k * beat * 1e6);
        app.publish(event, at, 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }

    // The peer now reads the music's bar at the music's beats — within a fortieth of a beat,
    // for two sessions' clocks and a network round trip.
    const auto onTheMusic = [&] {
        for (std::uint32_t k = 8; k < 16; ++k) {
            const std::chrono::microseconds when{musicBar + static_cast<std::int64_t>(k * beat * 1e6)};
            const double phase = peer.phaseAtTime(when, 4.0);
            double apart = std::fmod(phase - static_cast<double>(k % 4) + 8.0, 4.0);
            apart = std::min(apart, 4.0 - apart);
            if (apart > 0.025) {
                return false;
            }
        }
        return true;
    };
    INFO("peer phase at the music's bar: "
         << peer.phaseAtTime(std::chrono::microseconds{musicBar + static_cast<std::int64_t>(
                                                          8 * beat * 1e6)},
                             4.0));
    CHECK(eventually(onTheMusic, std::chrono::seconds{5}));

    app.stopOutputs();
    peer.enable(false);
}
