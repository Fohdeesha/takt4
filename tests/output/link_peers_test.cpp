#include "core/engine/beat_source.hpp"
#include "core/output/link_peers.hpp"
#include "core/output/link_session.hpp"
#include "core/output/transports.hpp"
#include "core/timing/beat_grid.hpp"
#include "core/timing/external_beats.hpp"
#include "core/timing/link_follower.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include "support/link_network.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::output::LinkSession;

// The only part of the design Phase 4's exit criterion that this machine cannot check on
// its own is "Resolume or equivalent follows it": nothing here speaks Link. Two
// LinkSessions in one process is as close as it gets — they discover each other over the
// same UDP multicast a real peer would, so what is checked is that takt4 announces
// itself, that a peer sees the tempo it publishes, and that the phase it requests is the
// phase the peer reads. What is not checked is any third-party implementation.
//
// **[network]**, so the default presets leave them out and the `-all` presets — and CI — run
// them. They open multicast sockets and announce on the local network. They used to be hidden
// (`[.link-network]`) instead, which kept them off the network but also out of every run there
// was: no preset registered them, so an earlier review fix — force the phase once, never request
// it — could have been reverted with every test still passing. A
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

using takt4::testing::anyLinkHeard;

TEST_CASE("two Link sessions for another process to list", "[.link-peer]") {
    // Hidden: run only by window_test.cpp's "SHOW PEERS lists a Link peer in another process", as
    // the peer it lists. That test has made sure no other Link session is on the network before
    // switching takt4's own on, and its own is the session these join; a guarded test in their
    // place heard it and skipped, and SHOW PEERS listed nobody.
    LinkSession first(123.0);
    LinkSession second(97.0);
    first.enable(true);
    second.enable(true);
    std::this_thread::sleep_for(std::chrono::seconds{8});
    first.enable(false);
    second.enable(false);
}

TEST_CASE("a Link peer sees the tempo and phase takt4 publishes", "[output][link][network]") {
    takt4::testing::skipWhileLinkIsOnTheNetwork();
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
        // call and can be up to a bar later. the design keeps forceBeatAtTime for the manual
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
    takt4::testing::skipWhileLinkIsOnTheNetwork();
    // an earlier review, as its experiment ran it: Resolume on Link first, takt4 enabling Link
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
    // **And on one timeline**, which a joined session reaches some time after it agrees on the
    // tempo: waited for rather than assumed after a fixed 300 ms, which now and then was not long
    // enough and left the two a few beats apart before the music had been heard at all.
    REQUIRE(eventually([&] {
        const std::chrono::microseconds at = ours.now() + std::chrono::seconds{2};
        double apart = std::fmod(ours.phaseAtTime(at, 4.0) - peer.phaseAtTime(at, 4.0) + 8.0, 4.0);
        apart = std::min(apart, 4.0 - apart);
        return apart < 0.005;
    }));

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

TEST_CASE("a peer that joins after the snap is put on the music's phase too",
          "[output][link][network]") {
    // an earlier review, coverage gap 7. takt4 snaps the session once and then only nudges
    // it — and a peer that joins later can bring a timeline of its own. So a new peer is a reason
    // to snap again, on the next beat, and only on that one.
    // Measured against takt4's own session: one already on this network — the rig has a Link
    // device of its own — is the older, wins the merge and moves what is measured here.
    {
        std::string problem;
        const bool other = takt4::testing::anyLinkHeard(2.0, problem);
        if (!problem.empty()) {
            SKIP("cannot listen for Link sessions on this network: " + problem);
        }
        if (other) {
            SKIP("a Link session is already on this network, which would take this one over");
        }
    }
    takt4::output::Transports::Config config;
    config.link = true;
    takt4::output::Transports app(config);
    app.startOutputs(0.0);
    LinkSession& ours = app.link();
    std::this_thread::sleep_for(std::chrono::milliseconds{300}); // Link's own first reset

    constexpr double kBpm = 128.0;
    const double beat = 60.0 / kBpm;
    const std::int64_t origin = ours.now().count() + 1'000'000;
    const auto at = [&](std::uint32_t k) {
        return origin + static_cast<std::int64_t>(k * beat * 1e6);
    };
    const auto publish = [&](std::uint32_t k) {
        takt4::tracking::BeatEvent event;
        event.bpm = kBpm;
        event.locked = true;
        event.confidence = 0.9;
        event.beatsPerBar = 4;
        event.beatInBar = k % 4 + 1;
        event.downbeat = event.beatInBar == 1;
        app.publish(event, at(k), 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
    };
    publish(0);
    publish(1);
    REQUIRE(ours.beatRequests() == 1); // the snap, and a beat on its grid

    // A peer whose bar is somewhere else.
    LinkSession peer(kBpm);
    peer.enable(true);
    peer.forceBeat(0.0, peer.now() + std::chrono::milliseconds{170}, 4.0);
    if (!eventually([&] { return ours.numPeers() >= 1 && peer.numPeers() >= 1; })) {
        SKIP("two Link sessions in one process never found each other: multicast is blocked");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    publish(2);
    CHECK(ours.beatRequests() == 2); // snapped again for the newcomer
    publish(3);
    CHECK(ours.beatRequests() == 2); // and once
    // And the peer reads the music's bar at the music's beats.
    const auto onTheMusic = [&] {
        for (std::uint32_t k = 2; k < 8; ++k) {
            double apart = std::fmod(peer.phaseAtTime(std::chrono::microseconds{at(k)}, 4.0) -
                                         static_cast<double>(k % 4) + 8.0,
                                     4.0);
            apart = std::min(apart, 4.0 - apart);
            if (apart > 0.025) {
                return false;
            }
        }
        return true;
    };
    CHECK(eventually(onTheMusic, std::chrono::seconds{5}));

    app.stopOutputs();
    peer.enable(false);
}

TEST_CASE("Link's older session wins when two meet, so a follower left alone takes the DJ's tempo",
          "[output][link][network][follow]") {
    // The rule a Link follower has to be built round, measured on Link itself. Enabling founds a
    // new session (`Controller::updateEnabled`, "always reset when first enabling"), and when two
    // sessions meet the older one wins (`Sessions::sawSessionTimeline`: the larger ghost time,
    // with session-id order only inside half a second). So an app that follows Link by switching
    // it on and waiting is, after a few seconds alone, the older session when the DJ's app
    // arrives — and the DJ's app adopts its timeline: a tempo nobody chose, and its phase.
    //
    // Listened for first, and never run beside another session: any Link app on this network —
    // Live or Resolume here, a device elsewhere — would be a third party to the merge, and would
    // have its own tempo moved by it.
    std::string problem;
    const bool other = anyLinkHeard(2.0, problem);
    if (!problem.empty()) {
        SKIP("cannot listen for Link sessions on this network: " + problem);
    }
    if (other) {
        SKIP("a Link session is already on this network, which would join the merge measured "
             "here and have its tempo moved by it");
    }
    // Link sends a tempo between peers as whole microseconds a beat, so a peer reads 126.5 BPM as
    // 474308 microseconds: 126.50008. Agreeing to a thousandth of a BPM is agreeing.
    constexpr double kSameTempo = 1e-3;

    SECTION("a follower switched on first, and alone, is the older session") {
        LinkSession follower(120.0); // a tempo nobody playing has chosen
        follower.enable(true);
        // Older than the DJ's session by far more than Link's half-second tie band.
        std::this_thread::sleep_for(std::chrono::seconds{3});
        REQUIRE(follower.numPeers() == 0);
        LinkSession dj(126.5);
        dj.enable(true);
        if (!eventually([&] { return follower.numPeers() >= 1 && dj.numPeers() >= 1; })) {
            SKIP("two Link sessions in one process never found each other: multicast is blocked");
        }
        REQUIRE(eventually(
            [&] { return std::abs(dj.tempoBpm() - follower.tempoBpm()) < kSameTempo; }));
        // The DJ's app now runs at the follower's tempo, not its own.
        CHECK_THAT(dj.tempoBpm(), WithinAbs(120.0, kSameTempo));
        dj.enable(false);
        follower.enable(false);
    }

    SECTION("a follower switched on after the DJ's app is the younger, and joins it") {
        LinkSession dj(126.5);
        dj.enable(true);
        // Older than the follower will be by more than the tie band — what waiting to hear a
        // session for a second before switching on buys.
        std::this_thread::sleep_for(std::chrono::seconds{2});
        LinkSession follower(120.0);
        follower.enable(true);
        if (!eventually([&] { return follower.numPeers() >= 1 && dj.numPeers() >= 1; })) {
            SKIP("two Link sessions in one process never found each other: multicast is blocked");
        }
        const bool agreed = eventually(
            [&] { return std::abs(dj.tempoBpm() - follower.tempoBpm()) < kSameTempo; });
        INFO("the DJ's app at " << std::setprecision(10) << dj.tempoBpm() << " BPM, the follower at "
                                << follower.tempoBpm());
        REQUIRE(agreed);
        CHECK_THAT(dj.tempoBpm(), WithinAbs(126.5, kSameTempo));
        CHECK_THAT(follower.tempoBpm(), WithinAbs(126.5, kSameTempo));
        follower.enable(false);
        dj.enable(false);
    }
}

TEST_CASE("a Link follower alone takes no session over, and follows the DJ's when it comes",
          "[output][link][network][follow]") {
    // The answer to the test above, on Link itself. takt4 follows by listening — Link switched off
    // — until a session has been heard for a second, and only then switches on, as the youngest
    // session there is: so the DJ's app keeps its own tempo however long takt4 waited first, and
    // takt4's beats are the DJ's, on its beats and in its bar.
    std::string problem;
    const bool other = anyLinkHeard(2.0, problem);
    if (!problem.empty()) {
        SKIP("cannot listen for Link sessions on this network: " + problem);
    }
    if (other) {
        SKIP("a Link session is already on this network, which the follower would join instead");
    }

    // takt4's session, read by a follower listening to the network. The DJ's app is in this same
    // process here, so nothing is taken for this process's own, as the default would take it.
    LinkSession session(120.0);
    takt4::output::LinkPeerWatch watch([](const std::string&, std::uint16_t) { return false; });
    takt4::timing::LinkFollower follower(session, watch);
    takt4::timing::ExternalBeats beats;
    beats.start(follower, takt4::timing::BeatGrid::Options{});
    // Alone, for longer than a session of its own would need to win: nothing joined, nothing
    // founded.
    std::this_thread::sleep_for(std::chrono::seconds{3});
    CHECK_FALSE(session.enabled());
    CHECK(follower.status().phase == takt4::timing::LinkFollower::Phase::Listening);
    CHECK_FALSE(beats.state().locked);

    LinkSession dj(126.5);
    dj.enable(true);
    if (!eventually(
            [&] {
                return follower.status().phase == takt4::timing::LinkFollower::Phase::Following;
            },
            std::chrono::seconds{10})) {
        dj.enable(false);
        beats.stop();
        SKIP("the follower never joined the session in this process: multicast is blocked");
    }
    REQUIRE(eventually([&] { return beats.state().locked; }));
    // The DJ's tempo, kept: the follower joined it, not the other way round.
    CHECK_THAT(dj.tempoBpm(), WithinAbs(126.5, 1e-6));
    CHECK_THAT(beats.state().bpm, WithinAbs(126.5, 0.01));

    // Every beat on one of the DJ's, in its place in the DJ's bar: the phase at the DJ's own
    // quantum of four — Link's alignment between peers — within a millisecond.
    std::vector<takt4::engine::EngineBeat> taken;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < until) {
        takt4::engine::EngineBeat beat;
        while (beats.popBeat(beat)) {
            taken.push_back(beat);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    REQUIRE(taken.size() >= 5);
    const double millisecond = 126.5 / 60000.0; // in beats
    for (const takt4::engine::EngineBeat& beat : taken) {
        const double phase = dj.phaseAtTime(std::chrono::microseconds{beat.hostMicros}, 4.0);
        double apart = std::fmod(phase - static_cast<double>(beat.event.beatInBar - 1) + 8.0, 4.0);
        apart = std::min(apart, 4.0 - apart);
        INFO("beat " << beat.state.beats << ", " << beat.event.beatInBar << " of 4, at the DJ's "
                     << phase);
        CHECK(apart < millisecond);
    }

    // The DJ's app leaves: Link off at once, and the beats coast and then stop.
    dj.enable(false);
    CHECK(eventually([&] { return !session.enabled(); }, std::chrono::seconds{10}));
    CHECK(eventually([&] { return beats.state().noSignal; }, std::chrono::seconds{10}));
    beats.stop();
    CHECK_FALSE(session.enabled());
}

TEST_CASE("a Link follower's rounds are not held up by the system while it listens",
          "[output][link][network][follow]") {
    // While it listens — before a join, and through a coast after the session goes — the follower
    // reads its listener on the thread that sends the beats, and the listener asked the system for
    // the machine's interfaces (at every open and every two seconds) and for whose socket each
    // node it hears sends from, in milliseconds each. Asked aside now: made a second each here,
    // which a round that waited for one would show. Never joins (it is told to hear for ever), so
    // nothing is sent, whatever is on the network.
    std::string problem;
    (void)anyLinkHeard(0.2, problem);
    if (!problem.empty()) {
        SKIP("cannot listen for Link sessions on this network: " + problem);
    }
    std::atomic<int> listed{0};
    takt4::output::LinkPeerWatch watch(
        [](const std::string& address, std::uint16_t port) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
            return takt4::output::sentFromHere(address, port);
        },
        [&listed] {
            ++listed;
            std::this_thread::sleep_for(std::chrono::seconds{1});
            return takt4::output::interfacesUp();
        });
    LinkSession session(120.0);
    takt4::timing::LinkFollower::Options options;
    options.hearFor = 1e9;
    takt4::timing::LinkFollower follower(session, watch, options);
    takt4::timing::ExternalBeats beats;
    beats.start(follower, takt4::timing::BeatGrid::Options{});
    // The open, and two rejoins at least.
    std::this_thread::sleep_for(std::chrono::milliseconds{5000});
    const double longest = beats.lateness().longestRoundSeconds();
    beats.stop();
    INFO("the longest round: " << longest * 1000.0 << " ms");
    CHECK(longest < 0.25);
    CHECK(listed >= 2);
    CHECK(watch.rejoins() >= 1);
    CHECK_FALSE(session.enabled());
}
