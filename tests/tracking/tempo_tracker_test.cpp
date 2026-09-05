#include "core/io/npy_file.hpp"
#include "core/tracking/particle_filter.hpp"
#include "core/tracking/state_space.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

using Catch::Approx;
using takt4::tracking::BeatEvent;
using takt4::tracking::StateSpaceModel;
using takt4::tracking::TempoTracker;
using takt4::tracking::TrackedFrame;

namespace {

constexpr double kFramePeriod = 0.02; // 50 Hz, as the whole pipeline runs at

/// The tempo of a beat period of `intervalFrames` whole frames. Those are the only
/// tempi madmom's state space holds, so they are the only ones the filter can report:
/// 23 frames is 130.43 BPM, the nearest it gets to a track playing 128.
constexpr double bpmOf(std::uint32_t intervalFrames) {
    return 60.0 / (static_cast<double>(intervalFrames) * kFramePeriod);
}

/// A frame as the particle filter would report it, with nothing emitted.
TrackedFrame frameAt(std::uint64_t index, std::uint32_t intervalFrames, double agreement,
                     std::uint32_t beatsPerBar = 4) {
    TrackedFrame frame;
    frame.frameIndex = index;
    frame.intervalFrames = intervalFrames;
    frame.refinedIntervalFrames = static_cast<double>(intervalFrames);
    frame.bpm = bpmOf(intervalFrames);
    frame.tempoAgreement = agreement;
    frame.beatsPerBar = beatsPerBar;
    return frame;
}

/// Runs `count` frames at one tempo.
void settle(TempoTracker& tracker, std::uint64_t& index, std::uint32_t intervalFrames,
            double agreement, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        (void)tracker.process(frameAt(index++, intervalFrames, agreement));
    }
}

} // namespace

TEST_CASE("the octave fold pulls an estimate into the operator's range", "[tracking][tempo]") {
    TempoTracker::Options options;
    options.minBpm = 70.0;
    options.maxBpm = 140.0;
    TempoTracker tracker(kFramePeriod, options);

    CHECK(tracker.fold(128.0) == Approx(128.0));
    CHECK(tracker.fold(70.0) == Approx(70.0));
    CHECK(tracker.fold(170.0) == Approx(85.0)); // the failure this exists to stop
    CHECK(tracker.fold(200.0) == Approx(bpmOf(30)));
    CHECK(tracker.fold(60.0) == Approx(120.0));
    CHECK(tracker.fold(55.0) == Approx(110.0));
    // 140 is the exclusive end, so it folds down rather than staying put.
    CHECK(tracker.fold(140.0) == Approx(70.0));

    SECTION("a window narrower than an octave takes the nearest reachable octave") {
        options.minBpm = 100.0;
        options.maxBpm = 120.0;
        TempoTracker narrow(kFramePeriod, options);
        CHECK(narrow.fold(110.0) == Approx(110.0));
        CHECK(narrow.fold(130.0) == Approx(130.0)); // 10 over, against 35 under at 65
        CHECK(narrow.fold(190.0) == Approx(95.0));  // 5 under, against 70 over at 190
    }

    SECTION("folding off publishes what the filter says") {
        options.minBpm = 70.0;
        options.maxBpm = 140.0;
        options.octaveFold = false;
        TempoTracker plain(kFramePeriod, options);
        CHECK(plain.fold(170.0) == Approx(170.0));
    }
}

TEST_CASE("the tempo locks only after sustained agreement, and unlocks the same way",
          "[tracking][tempo]") {
    TempoTracker::Options options;
    options.lockAfter = 25;
    options.unlockAfter = 75;
    options.confidenceThreshold = 0.15;
    options.confidenceSmoothing = 5.0; // quick, so the test is about locking not smoothing
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    settle(tracker, index, 23, 0.8, 24);
    CHECK_FALSE(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(bpmOf(23)));
    settle(tracker, index, 23, 0.8, 1);
    CHECK(tracker.state().locked);

    SECTION("one frame of disagreement does not unlock it") {
        settle(tracker, index, 31, 0.8, 1);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23))); // still publishing the locked tempo
        settle(tracker, index, 23, 0.8, 1);
        CHECK(tracker.state().locked);
        // The disagreement counter has to have been cleared, or a stray frame every
        // other frame would eventually unlock it.
        settle(tracker, index, 31, 0.8, 74);
        CHECK(tracker.state().locked);
    }

    SECTION("sustained disagreement unlocks it and takes the new tempo") {
        settle(tracker, index, 31, 0.8, 74);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)));
        settle(tracker, index, 31, 0.8, 1);
        CHECK_FALSE(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(31)));
        // ...and locks again once the new tempo has agreed with itself long enough.
        settle(tracker, index, 31, 0.8, 24);
        CHECK(tracker.state().locked);
    }
}

TEST_CASE("a pinned lock is held up rather than set", "[tracking][tempo]") {
    TempoTracker::Options options;
    options.lockAfter = 25;
    options.unlockAfter = 75;
    options.confidenceThreshold = 0.15;
    options.confidenceSmoothing = 5.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    settle(tracker, index, 23, 0.8, 25);
    REQUIRE(tracker.state().locked);
    REQUIRE_FALSE(tracker.state().pinned);

    SECTION("it survives the disagreement that would otherwise unlock it") {
        tracker.setLockPinned(true);
        CHECK(tracker.state().pinned);
        CHECK(tracker.lockPinned());

        settle(tracker, index, 31, 0.8, options.unlockAfter);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)));
        // Not merely slower to give up: four times the window and it still has not.
        settle(tracker, index, 31, 0.8, options.unlockAfter * 3);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)));

        // The same frames, the same count, on a tracker nobody pinned — without this the
        // check above would pass just as well if the disagreement never arrived.
        TempoTracker loose(kFramePeriod, options);
        std::uint64_t at = 0;
        settle(loose, at, 23, 0.8, 25);
        REQUIRE(loose.state().locked);
        settle(loose, at, 31, 0.8, options.unlockAfter);
        CHECK_FALSE(loose.state().locked);
        CHECK(loose.state().bpm == Approx(bpmOf(31)));
    }

    SECTION("releasing it hands the tempo straight back") {
        tracker.setLockPinned(true);
        settle(tracker, index, 31, 0.8, 200);
        REQUIRE(tracker.state().bpm == Approx(bpmOf(23)));

        tracker.setLockPinned(false);
        CHECK_FALSE(tracker.state().pinned);
        CHECK_FALSE(tracker.state().locked);

        // From what is playing now rather than from what was pinned, and on the very next
        // frame: an operator letting go is saying "look again", not "look again slowly".
        settle(tracker, index, 31, 0.8, 1);
        CHECK(tracker.state().bpm == Approx(bpmOf(31)));
        settle(tracker, index, 31, 0.8, options.lockAfter - 1);
        CHECK(tracker.state().locked);
    }

    SECTION("one release is enough however many times it was pinned") {
        tracker.setLockPinned(true);
        tracker.setLockPinned(true); // a control surface may resend its state at will
        settle(tracker, index, 31, 0.8, 200);
        REQUIRE(tracker.state().locked);

        tracker.setLockPinned(false);
        CHECK_FALSE(tracker.state().locked);
        CHECK_FALSE(tracker.lockPinned());
    }

    SECTION("a reset lets go of it") {
        tracker.setLockPinned(true);
        tracker.reset();
        CHECK_FALSE(tracker.lockPinned());
        CHECK_FALSE(tracker.state().pinned);
        CHECK_FALSE(tracker.state().locked);
    }

    SECTION("pinning locks to whatever is showing") {
        TempoTracker mid(kFramePeriod, options);
        std::uint64_t at = 0;
        settle(mid, at, 31, 0.8, 5); // nowhere near lockAfter
        REQUIRE_FALSE(mid.state().locked);
        const double showing = mid.state().bpm;
        REQUIRE(showing == Approx(bpmOf(31)));

        mid.setLockPinned(true);
        CHECK(mid.state().locked);
        CHECK(mid.state().bpm == Approx(showing));
    }

    SECTION("a pin set before anything is tracked does not invent a lock") {
        TempoTracker fresh(kFramePeriod, options);
        fresh.setLockPinned(true);
        CHECK(fresh.state().pinned);
        CHECK_FALSE(fresh.state().locked); // there is no flag to hold up yet
        CHECK(fresh.state().bpm == 0.0);

        // Acquisition is untouched by the pin: it takes exactly as long as it always does.
        std::uint64_t at = 0;
        settle(fresh, at, 23, 0.8, options.lockAfter - 1);
        CHECK_FALSE(fresh.state().locked);
        settle(fresh, at, 23, 0.8, 1);
        CHECK(fresh.state().locked);

        // And now it holds, which is what remembering it was for.
        settle(fresh, at, 31, 0.8, options.unlockAfter * 2);
        CHECK(fresh.state().locked);
        CHECK(fresh.state().bpm == Approx(bpmOf(23)));
    }
}

TEST_CASE("a pinned lock goes on refining the tempo from the beats", "[tracking][tempo]") {
    // What the pin is actually holding on to. Locked, the published tempo comes from the
    // spacing of the beats and resolves to a fraction of a BPM; unlocked, it falls back to
    // the state space's whole-frame intervals, which here offer 130.43 and 125.00 and
    // nothing between. A pin that kept the word lit and quietly stopped the refinement
    // would have held on to the wrong half of it.
    TempoTracker::Options options;
    options.lockAfter = 5;
    options.unlockAfter = 75;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);

    constexpr double kTrue = 128.0; // what the drum machine plays; no whole frame holds it
    const double period = 60.0 / (kTrue * kFramePeriod);
    std::uint64_t at = 0;
    const auto feedBeats = [&](int from, int count) {
        for (int beat = from; beat < from + count; ++beat) {
            const auto on = static_cast<std::uint64_t>(static_cast<double>(beat) * period + 0.5);
            while (at < on) {
                (void)tracker.process(frameAt(at++, 23, 0.9));
            }
            TrackedFrame frame = frameAt(at++, 23, 0.9);
            frame.emitted = TrackedFrame::Emitted::Beat;
            (void)tracker.process(frame);
        }
    };

    feedBeats(0, 40);
    REQUIRE(tracker.state().locked);
    REQUIRE(tracker.state().refined);
    REQUIRE(tracker.state().bpm == Approx(kTrue).margin(1.0));

    tracker.setLockPinned(true);
    feedBeats(40, 20);
    CHECK(tracker.state().refined);
    CHECK(tracker.state().bpm == Approx(kTrue).margin(1.0));
    // Which is to say it is still the refined number and not the cloud's own, 2.4 BPM out.
    CHECK(tracker.state().bpm != Approx(bpmOf(23)));
}

TEST_CASE("below the confidence gate the last good tempo is held", "[tracking][tempo]") {
    TempoTracker::Options options;
    options.confidenceThreshold = 0.4;
    options.confidenceSmoothing = 5.0;
    options.lockAfter = 10;
    options.unlockAfter = 20;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    settle(tracker, index, 23, 0.9, 40);
    REQUIRE(tracker.state().locked);
    REQUIRE_FALSE(tracker.state().holding);
    CHECK(tracker.state().confidence > 0.4);

    // The cloud falls apart and the filter starts reporting nonsense. Nothing new may be
    // published: HANDOFF §5.5, "silence beats wrong".
    settle(tracker, index, 42, 0.02, 40);
    CHECK(tracker.state().holding);
    CHECK(tracker.state().bpm == Approx(bpmOf(23)));
    CHECK(tracker.state().rawBpm == Approx(bpmOf(42))); // what the filter says is still visible
    CHECK(tracker.state().confidence < 0.4);

    // Confidence returns at the new tempo. The gate lifts within a few frames, but the
    // lock still has to run its hysteresis window out before the number moves.
    settle(tracker, index, 42, 0.9, 10);
    CHECK_FALSE(tracker.state().holding);
    CHECK(tracker.state().bpm == Approx(bpmOf(23)));
    settle(tracker, index, 42, 0.9, 30);
    CHECK(tracker.state().bpm == Approx(bpmOf(42)));

    SECTION("before anything has been believed once, there is nothing to hold") {
        TempoTracker fresh(kFramePeriod, options);
        std::uint64_t at = 0;
        settle(fresh, at, 30, 0.0, 5);
        CHECK_FALSE(fresh.state().holding);
        CHECK(fresh.state().bpm == Approx(bpmOf(30)));
    }
}

TEST_CASE("beats are numbered from the meter the filter reports", "[tracking][tempo]") {
    TempoTracker tracker(kFramePeriod);
    std::uint64_t index = 0;
    const auto beat = [&](TrackedFrame::Emitted kind, std::uint32_t beatsPerBar) {
        TrackedFrame frame = frameAt(index++, 23, 0.9, beatsPerBar);
        frame.emitted = kind;
        return tracker.process(frame);
    };

    // A beat before the first downbeat: the bar phase is not known, and is not guessed.
    std::optional<BeatEvent> event = beat(TrackedFrame::Emitted::Beat, 4);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 0);
    CHECK_FALSE(event->downbeat);

    for (const std::uint32_t expected : {1u, 2u, 3u, 4u, 1u, 2u}) {
        event =
            beat(expected == 1 ? TrackedFrame::Emitted::Downbeat : TrackedFrame::Emitted::Beat, 4);
        REQUIRE(event.has_value());
        CHECK(event->beatInBar == expected);
        CHECK(event->downbeat == (expected == 1));
        CHECK(event->beatsPerBar == 4);
    }
    CHECK(tracker.state().bars == 2);
    CHECK(tracker.state().beats == 7);

    SECTION("nothing hardcodes four") {
        (void)beat(TrackedFrame::Emitted::Downbeat, 3);
        for (const std::uint32_t expected : {2u, 3u, 1u, 2u}) {
            event = beat(
                expected == 1 ? TrackedFrame::Emitted::Downbeat : TrackedFrame::Emitted::Beat, 3);
            REQUIRE(event.has_value());
            CHECK(event->beatInBar == expected);
            CHECK(event->beatsPerBar == 3);
        }
    }

    SECTION("a frame with nothing emitted is not a beat") {
        CHECK_FALSE(tracker.process(frameAt(index++, 23, 0.9)).has_value());
    }

    SECTION("without a meter there is nothing to count within") {
        // The filter always reports one — it reads a meter straight out of the downbeat
        // state space, whose intervals are 2, 3 and 4 — but the bar arithmetic must not
        // depend on that being true, because it divides by it.
        TempoTracker bare(kFramePeriod);
        std::uint64_t at = 0;
        const auto unmetered = [&](TrackedFrame::Emitted kind) {
            TrackedFrame frame = frameAt(at++, 23, 0.9, 0);
            frame.emitted = kind;
            return bare.process(frame);
        };
        CHECK(unmetered(TrackedFrame::Emitted::Beat)->beatInBar == 0);
        CHECK(unmetered(TrackedFrame::Emitted::Downbeat)->beatInBar == 1);
        CHECK(unmetered(TrackedFrame::Emitted::Beat)->beatInBar == 1);
        CHECK(bare.state().bars == 1);
    }
}

// madmom's tempo intervals are whole 20 ms frames, so nothing inside the filter can
// report a tempo between 130.43 BPM (23 frames) and 125.00 (24). The beats it calls are
// spaced 23, 24, 23, 23 frames apart, and their mean is not so limited.
TEST_CASE("a manual downbeat re-anchors the bar and keeps it there", "[tracking][tempo]") {
    // §5.5's manual downbeat, the one it calls non-negotiable: "the best online downbeat
    // tracker in the world scores 56% F1, and being two beats out is worse than one tap".
    TempoTracker tracker(kFramePeriod);
    std::uint64_t index = 0;
    const auto beat = [&](TrackedFrame::Emitted kind) {
        TrackedFrame frame = frameAt(index++, 23, 0.9, 4);
        frame.emitted = kind;
        return tracker.process(frame);
    };

    // Settle into the filter's own bar: 1, 2, 3, 4.
    (void)beat(TrackedFrame::Emitted::Downbeat);
    (void)beat(TrackedFrame::Emitted::Beat);
    REQUIRE(tracker.state().beatInBar == 2);
    const std::uint64_t barsBefore = tracker.state().bars;

    // The operator says the bar starts on the next beat — which the filter calls its
    // third.
    tracker.snapDownbeat();
    std::optional<BeatEvent> event = beat(TrackedFrame::Emitted::Beat);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 1);
    CHECK(event->downbeat);
    CHECK(event->snapped);
    CHECK(tracker.state().bars == barsBefore + 1);

    // Only that one beat is marked snapped; the rest are ordinary.
    event = beat(TrackedFrame::Emitted::Beat);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 2);
    CHECK_FALSE(event->snapped);

    // And the filter's own downbeat, two beats later, no longer starts a bar: the
    // rotation is kept, or a correction the filter undid a bar later would be no
    // correction at all.
    event = beat(TrackedFrame::Emitted::Downbeat);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 3);
    CHECK_FALSE(event->downbeat);
    for (const std::uint32_t expected : {4u, 1u, 2u, 3u, 4u, 1u}) {
        event = beat(expected == 3 ? TrackedFrame::Emitted::Downbeat : TrackedFrame::Emitted::Beat);
        REQUIRE(event.has_value());
        INFO("expected beat " << expected << " of the bar");
        CHECK(event->beatInBar == expected);
        CHECK(event->downbeat == (expected == 1));
    }

    SECTION("a second snap moves it again") {
        tracker.snapDownbeat();
        event = beat(TrackedFrame::Emitted::Beat);
        REQUIRE(event.has_value());
        CHECK(event->beatInBar == 1);
        CHECK(event->snapped);
    }

    SECTION("a reset gives the bar back to the filter") {
        tracker.reset();
        CHECK(tracker.state().bars == 0);
        (void)beat(TrackedFrame::Emitted::Downbeat);
        CHECK(tracker.state().beatInBar == 1);
    }
}

TEST_CASE("a manual downbeat before the filter has found one still starts the bar",
          "[tracking][tempo]") {
    // The operator hits the button as the track drops, seconds before the downbeat stage
    // has settled on anything. There is no filter bar phase to rotate yet, so the bar
    // runs from the tap — and joins up with the filter's when it finally calls one.
    TempoTracker tracker(kFramePeriod);
    std::uint64_t index = 0;
    const auto beat = [&](TrackedFrame::Emitted kind) {
        TrackedFrame frame = frameAt(index++, 23, 0.9, 4);
        frame.emitted = kind;
        return tracker.process(frame);
    };

    tracker.snapDownbeat();
    std::optional<BeatEvent> event = beat(TrackedFrame::Emitted::Beat);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 1);
    CHECK(event->downbeat);
    CHECK(event->snapped);

    for (const std::uint32_t expected : {2u, 3u}) {
        event = beat(TrackedFrame::Emitted::Beat);
        REQUIRE(event.has_value());
        CHECK(event->beatInBar == expected);
    }

    // The filter calls its first downbeat here. The operator's count wins: this is beat 4.
    event = beat(TrackedFrame::Emitted::Downbeat);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 4);
    CHECK_FALSE(event->downbeat);

    for (const std::uint32_t expected : {1u, 2u, 3u, 4u}) {
        event = beat(TrackedFrame::Emitted::Beat);
        REQUIRE(event.has_value());
        INFO("expected beat " << expected << " of the bar");
        CHECK(event->beatInBar == expected);
    }
    CHECK(tracker.state().bars == 2);
}

TEST_CASE("the published tempo comes from the beat spacing once locked", "[tracking][tempo]") {
    TempoTracker::Options options;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);

    // 128 BPM is a beat every 23.4375 frames. Lay beats on the nearest whole frame, the
    // way the tracker can only ever call them, and report the cloud's nearest interval.
    constexpr double kTrue = 128.0;
    const double period = 60.0 / (kTrue * kFramePeriod);
    std::uint64_t next = 0;
    std::optional<BeatEvent> last;
    for (int beat = 0; beat < 40; ++beat) {
        const auto at = static_cast<std::uint64_t>(static_cast<double>(beat) * period + 0.5);
        while (next < at) {
            (void)tracker.process(frameAt(next++, 23, 0.9));
        }
        TrackedFrame frame = frameAt(next++, 23, 0.9);
        frame.refinedIntervalFrames = 23.0; // the cloud sits on one interval, as it does
        frame.emitted =
            beat % 4 == 0 ? TrackedFrame::Emitted::Downbeat : TrackedFrame::Emitted::Beat;
        last = tracker.process(frame);
    }
    REQUIRE(last.has_value());
    REQUIRE(tracker.state().locked);
    CHECK(tracker.state().refined);
    // Without the refinement this would read 130.43, a 2.4 BPM error that no amount of
    // averaging inside the filter could remove.
    CHECK(tracker.state().bpm == Approx(128.0).margin(1.0));
    CHECK(last->bpm == tracker.state().bpm);

    SECTION("a missed beat is thrown out rather than halving the tempo") {
        // Skip one beat: the gap doubles, and a mean over the gaps would drag the tempo
        // down by an eighth.
        std::uint64_t at = next + 200;
        for (int beat = 0; beat < 12; ++beat) {
            TrackedFrame frame = frameAt(at, 23, 0.9);
            frame.refinedIntervalFrames = 23.0;
            frame.emitted = TrackedFrame::Emitted::Beat;
            (void)tracker.process(frame);
            // One gap of two beats among ordinary ones.
            at += beat == 5 ? std::uint64_t{47} : std::uint64_t{23};
        }
        CHECK(tracker.state().refined);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)).margin(0.5));
    }

    SECTION("a refinement that disagrees with the lock is not believed") {
        TempoTracker::Options wide = options;
        wide.refineTempoTolerance = 0.001; // nothing can agree this closely
        TempoTracker strict(kFramePeriod, wide);
        std::uint64_t at = 0;
        for (int beat = 0; beat < 40; ++beat) {
            const auto on = static_cast<std::uint64_t>(static_cast<double>(beat) * period + 0.5);
            while (at < on) {
                (void)strict.process(frameAt(at++, 23, 0.9));
            }
            TrackedFrame frame = frameAt(at++, 23, 0.9);
            frame.refinedIntervalFrames = 23.0;
            frame.emitted = TrackedFrame::Emitted::Beat;
            (void)strict.process(frame);
        }
        REQUIRE(strict.state().locked);
        CHECK_FALSE(strict.state().refined);
        CHECK(strict.state().bpm == Approx(bpmOf(23)));
    }
}

TEST_CASE("the latency offset moves the timestamp and nothing else", "[tracking][tempo]") {
    TempoTracker::Options options;
    options.latencyOffsetSeconds = -0.030; // fire 30 ms early
    TempoTracker tracker(kFramePeriod, options);

    TrackedFrame frame = frameAt(500, 23, 0.9);
    frame.emitted = TrackedFrame::Emitted::Downbeat;
    const std::optional<BeatEvent> event = tracker.process(frame);
    REQUIRE(event.has_value());
    CHECK(event->frameIndex == 500);
    CHECK(event->time == Approx(10.0 - 0.030)); // frame 500 at 50 Hz is 10 s
}

TEST_CASE("the manual octave shift moves the published tempo and keeps the lock",
          "[tracking][tempo]") {
    TempoTracker::Options options;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    settle(tracker, index, 23, 0.9, 20);
    REQUIRE(tracker.state().locked);
    tracker.halve();
    CHECK(tracker.state().bpm == Approx(bpmOf(23) / 2.0));
    settle(tracker, index, 23, 0.9, 20);
    CHECK(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(bpmOf(23) / 2.0));

    tracker.redouble();
    CHECK(tracker.state().bpm == Approx(bpmOf(23)));
    tracker.redouble();
    CHECK(tracker.state().bpm == Approx(bpmOf(23) * 2.0));
}

TEST_CASE("a tapped tempo moves the fold window onto the octave the operator meant",
          "[tracking][tempo]") {
    // The failure §7 deviation 4 measured: under a 70-140 window a Quickstep at 204 BPM
    // is folded to 102 and the operator has no way to say otherwise. Quickstep's
    // octave-tolerant tempo accuracy on Ballroom is 1.000 and its exact accuracy 0.000
    // for exactly this reason. Tapping it is the way out.
    TempoTracker::Options options;
    options.minBpm = 70.0;
    options.maxBpm = 140.0;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    // 14 frames a beat is 214.3 BPM, the fastest the state space holds; folded, 107.1.
    settle(tracker, index, 14, 0.9, 20);
    REQUIRE(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(bpmOf(14) / 2.0));
    CHECK(tracker.state().rawBpm == Approx(bpmOf(14)));

    tracker.seedTempo(bpmOf(14));
    CHECK(tracker.state().bpm == Approx(bpmOf(14)));
    CHECK(tracker.options().minBpm == Approx(bpmOf(14) / std::sqrt(2.0)));
    CHECK(tracker.options().maxBpm == Approx(bpmOf(14) * std::sqrt(2.0)));
    CHECK(tracker.options().octaveFold);
    // The published tempo really moved, so the lock is given up and hunted again.
    CHECK_FALSE(tracker.state().locked);
    settle(tracker, index, 14, 0.9, 10);
    CHECK(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(bpmOf(14)));

    SECTION("a tap that agrees with what is tracked costs nothing") {
        const double bpm = tracker.state().bpm;
        tracker.seedTempo(bpm * 1.002); // as close as a human tap ever gets
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpm));
    }

    SECTION("a tap clears a manual octave shift, so the two cannot fight") {
        tracker.halve();
        REQUIRE(tracker.state().bpm == Approx(bpmOf(14) / 2.0));
        tracker.seedTempo(bpmOf(14));
        CHECK(tracker.state().bpm == Approx(bpmOf(14)));
    }

    SECTION("a tap of nothing is ignored rather than folding into an empty window") {
        const double bpm = tracker.state().bpm;
        const double low = tracker.options().minBpm;
        tracker.seedTempo(0.0);
        tracker.seedTempo(-120.0);
        CHECK(tracker.state().bpm == Approx(bpm));
        CHECK(tracker.options().minBpm == Approx(low));
    }
}

TEST_CASE("changing the fold window only drops the lock when it has to", "[tracking][tempo]") {
    TempoTracker::Options options;
    options.minBpm = 70.0;
    options.maxBpm = 140.0;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;
    settle(tracker, index, 23, 0.9, 20);
    REQUIRE(tracker.state().locked);

    options.maxBpm = 160.0; // 130.43 still fits
    tracker.setOptions(options);
    CHECK(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(bpmOf(23)));

    options.minBpm = 60.0;
    options.maxBpm = 120.0; // 130.43 does not
    tracker.setOptions(options);
    CHECK_FALSE(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(bpmOf(23) / 2.0));
}

TEST_CASE("nonsensical options are refused rather than tracked with", "[tracking][tempo]") {
    TempoTracker::Options options;
    CHECK_THROWS_AS(TempoTracker(0.0, options), std::invalid_argument);
    CHECK_THROWS_AS(TempoTracker(-0.02, options), std::invalid_argument);

    TempoTracker::Options bad = options;
    bad.minBpm = 140.0;
    bad.maxBpm = 70.0;
    CHECK_THROWS_AS(TempoTracker(kFramePeriod, bad), std::invalid_argument);

    bad = options;
    bad.lockAfter = 0;
    CHECK_THROWS_AS(TempoTracker(kFramePeriod, bad), std::invalid_argument);

    bad = options;
    bad.confidenceSmoothing = 0.0;
    CHECK_THROWS_AS(TempoTracker(kFramePeriod, bad), std::invalid_argument);

    bad = options;
    bad.refineNeedsBeats = 1;
    CHECK_THROWS_AS(TempoTracker(kFramePeriod, bad), std::invalid_argument);

    bad = options;
    bad.refineOverBeats = 2;
    bad.refineNeedsBeats = 4;
    CHECK_THROWS_AS(TempoTracker(kFramePeriod, bad), std::invalid_argument);
}

// End to end over real material: the golden synthetic excerpt is a drum machine at
// 128 BPM, and the whole chain — committed activations, particle filter, tempo state
// machine — has to say so and lock onto it.
TEST_CASE("the chain tracks the synthetic excerpt's tempo and locks", "[tracking][tempo]") {
    const StateSpaceModel model =
        StateSpaceModel::fromFile(std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin");
    const takt4::io::NpyMatrix activations = takt4::io::readNpyFloat32(
        std::filesystem::path(TAKT4_TEST_DATA_DIR) / "model" / "generic" / "synthetic.npy");

    takt4::tracking::ParticleFilter filter(model);
    TempoTracker::Options options;
    options.minBpm = 90.0;
    options.maxBpm = 180.0;
    TempoTracker tracker(model.secondsPerFrame(), options);

    std::vector<BeatEvent> beats;
    for (std::size_t f = 0; f < activations.rows; ++f) {
        const TrackedFrame frame = filter.process(activations.at(f, 3), activations.at(f, 4));
        if (const std::optional<BeatEvent> event = tracker.process(frame)) {
            beats.push_back(*event);
        }
    }
    REQUIRE(beats.size() > 15);
    CHECK(tracker.state().locked);
    CHECK_FALSE(tracker.state().holding);
    // The cloud can only say 130.43 here; refining from the beat spacing gets it to the
    // 128 BPM the drum machine actually plays.
    CHECK(tracker.state().refined);
    CHECK(tracker.state().bpm == Approx(128.0).margin(1.0));
    CHECK(tracker.state().beatsPerBar >= 2);
    CHECK(tracker.state().beatsPerBar <= 4);
    CHECK(tracker.state().bars > 2);

    std::size_t downbeats = 0;
    for (const BeatEvent& event : beats) {
        if (event.downbeat) {
            ++downbeats;
        }
        CHECK(event.beatInBar <= event.beatsPerBar);
    }
    CHECK(downbeats > 2);
    CHECK(downbeats < beats.size());
}
