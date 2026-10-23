#pragma once

// Whether a Link session is already announcing on this network — for the [network] tests that
// join Link and must not drag another app's session into what they measure, or move its tempo.
// The rig's network has a Link device of its own; Live or Resolume may be up on this machine.

#include "core/output/link_peers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace takt4::testing {

/// Listens passively for `seconds` — nothing is sent, so nothing is disturbed by asking — and
/// says whether any Link session was heard. False with `problem` set when it cannot listen.
inline bool anyLinkHeard(double seconds, std::string& problem) {
    output::LinkPeerWatch watch;
    if (!watch.open(problem)) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&start] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    while (elapsed() < seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        watch.poll(elapsed());
        if (!watch.peers().empty()) {
            return true;
        }
    }
    return false;
}

/// First thing in a [network] test that switches a Link session on: skipped while another Link
/// session is on this network, or when Link cannot be listened to. Joined, a test's session takes
/// that session's tempo and is counted in it, and whatever the test then sets — a tempo, a bar —
/// is set for everyone in it: on the rig's network, another computer.
inline void skipWhileLinkIsOnTheNetwork() {
    std::string problem;
    const bool other = anyLinkHeard(2.0, problem);
    if (!problem.empty()) {
        SKIP("cannot listen for Link sessions on this network: " + problem);
    }
    if (other) {
        SKIP("a Link session is already on this network, which this test's own would join");
    }
}

} // namespace takt4::testing
