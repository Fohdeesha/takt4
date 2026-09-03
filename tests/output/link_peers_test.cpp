#include "core/output/link_session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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
// **Hidden by default.** The leading dot in the tag keeps Catch2 from running this unless
// it is asked for by name, because it opens multicast sockets and announces on the local
// network — which a CI runner may block, and which a test suite has no business doing to
// whatever else is on the network. Run it deliberately:
//
//     takt4_tests "[link-network]"
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

TEST_CASE("a Link peer sees the tempo and phase takt4 publishes", "[.link-network]") {
    LinkSession tracker(120.0); // takt4, driving
    LinkSession peer(60.0);     // whatever is listening

    tracker.enable(true);
    peer.enable(true);

    // Discovery. If this fails the network is blocking multicast, not takt4.
    REQUIRE(eventually([&] { return tracker.numPeers() >= 1 && peer.numPeers() >= 1; }));
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
