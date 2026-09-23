#pragma once

// Taps a rhythm on a control surface in real time, the way a hand on a button that chatters
// does it: every real press is followed a few milliseconds later by a bounce.
//
// The surfaces stamp a tap with their own steady clock at the moment it arrives
// (`ControlSurface::apply`), so there is no time to hand them and the rhythm has to be played
// for real. What the tempo *should* be is worked out from when the presses actually went,
// not from the period asked for, so a sleep that overshoots does not make the test flaky.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace takt4::testing {

struct TappedRhythm {
    std::vector<double> pressed; // steady-clock seconds of each real press
    std::vector<double> bounced; // and of the bounce after it

    /// The tempo `tracking::TapTempo` makes of three presses: the median of two gaps is their
    /// mean, which is the span over two.
    double tempoOfThree() const { return 120.0 / (pressed[2] - pressed[0]); }

    /// The longest gap between a press and its bounce. A bounce that arrived later than
    /// `TapTempo::Options::bounceSeconds` is a tap, and the test would be measuring the sleep.
    double longestBounce() const {
        double longest = 0.0;
        for (std::size_t i = 0; i < pressed.size() && i < bounced.size(); ++i) {
            longest = std::max(longest, bounced[i] - pressed[i]);
        }
        return longest;
    }
};

/// Presses `press` `count` times, `period` apart, each followed by a second press `bounce`
/// later.
template <typename Press>
TappedRhythm tapWithBounces(Press press, int count, std::chrono::milliseconds period,
                            std::chrono::milliseconds bounce = std::chrono::milliseconds{5}) {
    using Clock = std::chrono::steady_clock;
    const auto seconds = [] {
        return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    };
    const auto start = Clock::now();
    TappedRhythm rhythm;
    for (int n = 0; n < count; ++n) {
        std::this_thread::sleep_until(start + n * period);
        rhythm.pressed.push_back(seconds());
        press();
        std::this_thread::sleep_for(bounce);
        rhythm.bounced.push_back(seconds());
        press();
    }
    return rhythm;
}

} // namespace takt4::testing
