#include "core/io/npy_file.hpp"
#include "core/tracking/particle_filter.hpp"
#include "core/tracking/state_space.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
/// A beat period for the bar tests: 23 frames, 460 ms, the nearest the state space gets to
/// a track playing 128. The filter emits on one frame in twenty-three, and where inside
/// that gap a downbeat press lands is what decides which beat it means — so the bar tests
/// space their beats out rather than emitting one per frame.
constexpr std::uint32_t kInterval = 23;

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

/// Runs `count` frames at one tempo, the filter calling a beat every period as it does on a
/// steady track: a lock needs beats at the tempo being locked (`Options::lockBeats`).
void settle(TempoTracker& tracker, std::uint64_t& index, std::uint32_t intervalFrames,
            double agreement, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        TrackedFrame frame = frameAt(index, intervalFrames, agreement);
        if (index % intervalFrames == 0) {
            frame.emitted = TrackedFrame::Emitted::Beat;
            frame.beatActivation = 0.7f;
        }
        ++index;
        (void)tracker.process(frame);
    }
}

/// The options the tests below run under: the defaults, except that a lock asks for the fewest
/// beats it can — two, a period apart — so that `lockAfter` and not a bar of beats sets when it
/// arrives, which is what these tests count in frames. `settle` calls the beats. The tests of
/// `lockBeats` itself use the default.
TempoTracker::Options testOptions() {
    TempoTracker::Options options;
    options.lockBeats = 2;
    return options;
}

} // namespace

TEST_CASE("the octave fold pulls an estimate into the operator's range", "[tracking][tempo]") {
    TempoTracker::Options options = testOptions();
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

namespace {

/// Runs `frames` frames at one filter period, calling a beat on every `interval`-th frame,
/// and returns how many beats the tracker actually *published*. The two numbers are the
/// whole of what `Options::foldBeats` is about, and they used to be the same number by
/// construction.
///
/// `meter` beats to the bar, with every `meter`-th call a downbeat, so that the bar can be
/// counted as well as the beats.
struct Published {
    std::size_t beats = 0;
    std::size_t downbeats = 0;
};

Published feed(TempoTracker& tracker, std::uint64_t& index, std::uint32_t interval,
               std::size_t frames, std::uint32_t meter = 4, std::uint64_t* called = nullptr) {
    Published out;
    for (std::size_t f = 0; f < frames; ++f) {
        TrackedFrame frame = frameAt(index, interval, 0.9, meter);
        if (index % interval == 0) {
            const bool downbeat = (index / interval) % meter == 0;
            frame.emitted =
                downbeat ? TrackedFrame::Emitted::Downbeat : TrackedFrame::Emitted::Beat;
            // The network believing this is a beat, split across the two classes as a
            // softmax over beat / downbeat / non-beat really is. Equal on every beat: the
            // fold must not need one sub-grid to be louder than the other, because measured
            // over real audio neither is. See Options::foldSupportFrames.
            frame.beatActivation = downbeat ? 0.2f : 0.7f;
            frame.downbeatActivation = downbeat ? 0.5f : 0.0f;
            if (called != nullptr) {
                ++*called;
            }
        }
        ++index;
        if (const std::optional<BeatEvent> event = tracker.process(frame)) {
            ++out.beats;
            if (event->downbeat) {
                ++out.downbeats;
            }
        }
    }
    return out;
}

/// The same, but with the filter **contradicting itself**: it reports a beat period of
/// `interval` frames and calls its beats `callEvery` frames apart. That is not a state a
/// working filter is in, and it is the state "Pogo - Quantum Field - 08 Moonlake" put it in —
/// interval 15, beats 30 apart. See `Options::beatOctaveBeats`.
Published feedDisagreeing(TempoTracker& tracker, std::uint64_t& index, std::uint32_t interval,
                          std::uint32_t callEvery, std::size_t frames, std::uint32_t meter = 4) {
    Published out;
    for (std::size_t f = 0; f < frames; ++f) {
        TrackedFrame frame = frameAt(index, interval, 0.9, meter);
        if (index % callEvery == 0) {
            const bool downbeat = (index / callEvery) % meter == 0;
            frame.emitted =
                downbeat ? TrackedFrame::Emitted::Downbeat : TrackedFrame::Emitted::Beat;
            frame.beatActivation = downbeat ? 0.2f : 0.7f;
            frame.downbeatActivation = downbeat ? 0.5f : 0.0f;
        }
        ++index;
        if (const std::optional<BeatEvent> event = tracker.process(frame)) {
            ++out.beats;
            if (event->downbeat) {
                ++out.downbeats;
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("the published tempo is the one the beats are on", "[tracking][tempo]") {
    // Reported from a rig on 2026-09-08 about "Pogo - Quantum Field - 08 Moonlake": "the app
    // said locked and displayed 200bpm, but the beat counter dot lights were moving very
    // clearly at 100bpm ... everything pointed towards a correct 100bpm detection but the app
    // said 200bpm". Both numbers were true of what they described — the readout is the cloud's
    // tempo and the dots move on the beats — and the filter was reporting one while emitting
    // the other. Measured over the track: 216 of 402 gaps are 30 frames, the cloud says 15.
    TempoTracker::Options options = testOptions();
    options.octaveFold = false;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    // The cloud says 15 frames — 200 BPM — and the beats arrive 30 apart, which is 100.
    const Published played = feedDisagreeing(tracker, index, 15, 30, 1800);
    REQUIRE(tracker.state().locked);
    // Every beat the filter called is published: nothing here divides the grid, and that is
    // the point — the beats were never wrong.
    CHECK(played.beats == 60);
    CHECK(tracker.state().bpm == Approx(bpmOf(30)).margin(2.0));
    CHECK(tracker.state().rawBpm == Approx(bpmOf(15)).margin(2.0));

    SECTION("and a filter that agrees with itself is left alone") {
        // The guard that makes this safe: the ratio has to be a clean factor of two held for
        // several beats, which a working filter never produces.
        TempoTracker ordinary(kFramePeriod, options);
        std::uint64_t at = 0;
        (void)feed(ordinary, at, 15, 1800);
        REQUIRE(ordinary.state().locked);
        CHECK(ordinary.state().bpm == Approx(bpmOf(15)).margin(2.0));
    }
}

TEST_CASE("the fold and the beat-octave rule do not correct the same octave twice",
          "[tracking][tempo]") {
    // Measured on 2026-09-08 with the 70-140 window on: "02 -
    // Jamie Lidell - Your Sweet Boom" is a 107 BPM track whose cloud sits at 214 and whose
    // beats arrive at 107. The fold halved the cloud's 214 to 107, and then the beat-octave
    // rule halved *that* to 53.5 — for the whole track, at Link, MIDI clock and OSC. With
    // the fold off it read 107, which is how it went unseen. And the `foldSupportFrames`
    // latch, seeing the beats sit on the folded tempo, then divided a grid that was already
    // the music's and threw every other beat away in three windows of the track.
    //
    // The fix is that the fold and the lock are given the tempo the beats are *on*, so
    // there is one correction and nothing left to divide.
    TempoTracker::Options options = testOptions(); // the default 70-140 window, on
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    // The cloud says 14 frames — 214.3 BPM — and the beats arrive 28 apart, which is 107.1.
    // Long enough for the beat-octave rule to settle (eight beats) and then for the latch
    // to have had every chance to fire (foldSupportFrames is 250 frames).
    Published played = feedDisagreeing(tracker, index, 14, 28, 300);
    REQUIRE(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(bpmOf(28)).margin(2.0)); // 107, not 53.5
    // And the lock is carried across the beat-octave rule settling, rather than argued
    // with about a number that did not move: from here on it never lets go.
    for (int chunk = 0; chunk < 100; ++chunk) {
        const Published more = feedDisagreeing(tracker, index, 14, 28, 28);
        played.beats += more.beats;
        played.downbeats += more.downbeats;
        INFO("chunk " << chunk);
        REQUIRE(tracker.state().locked);
        REQUIRE(tracker.state().bpm == Approx(bpmOf(28)).margin(2.0));
    }
    CHECK(tracker.state().rawBpm == Approx(bpmOf(14)).margin(2.0));    // the cloud, still
    CHECK(tracker.state().calledBpm == Approx(bpmOf(28)).margin(2.0)); // the beats
    // Every beat the filter called is published. The grid is the music's already: nothing
    // divides it, and the divisor says so. (A beat on frame 0 and on every 28th after it,
    // so one more than the frames divide into.)
    CHECK(played.beats == (index + 27) / 28);
    CHECK(tracker.state().beatDivisor == 1);

    SECTION("Moonlake, the other track reported: cloud at 200, beats at 100, read 50") {
        TempoTracker moonlake(kFramePeriod, options);
        std::uint64_t at = 0;
        const Published beats = feedDisagreeing(moonlake, at, 15, 30, 1800);
        REQUIRE(moonlake.state().locked);
        CHECK(moonlake.state().bpm == Approx(bpmOf(30)).margin(2.0)); // 100, not 50
        CHECK(beats.beats == 1800 / 30);
        CHECK(moonlake.state().beatDivisor == 1);
    }

    SECTION("a fold that genuinely halves the cloud's grid still does its job") {
        // The other side: a filter that agrees with itself and double-times a 92 BPM track
        // — "03 - Fake Sweat" — must still be folded, and its beats still divided once the
        // cloud has argued for it. This is the test above "the octave fold divides the
        // beats", in short, to show the fix took nothing from it.
        TempoTracker fake(kFramePeriod, testOptions());
        std::uint64_t at = 0;
        (void)feed(fake, at, 32, 400);
        REQUIRE(fake.state().bpm == Approx(bpmOf(32)).margin(1.0));
        std::uint64_t called = 0;
        const Published fast = feed(fake, at, 16, 640, 4, &called);
        CHECK(fake.state().bpm == Approx(bpmOf(32)).margin(2.0));
        CHECK(fast.beats == called / 2);
        CHECK(fake.state().beatDivisor == 2);
    }
}

TEST_CASE("a tap names the octave the beats are on, not the cloud's", "[tracking][tempo]") {
    // With the fold off a tap moves the tempo through the manual shift, measured against
    // what is being tracked. An operator tapping 100 over Moonlake — cloud at 200, beats at
    // 100, readout already 100 — has confirmed what is showing; measured against the cloud
    // it would have halved the readout to 50, which is the double correction of
    // Options::beatOctaveBeats wearing a different hat.
    TempoTracker::Options options = testOptions();
    options.octaveFold = false;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    (void)feedDisagreeing(tracker, index, 15, 30, 900);
    REQUIRE(tracker.state().locked);
    REQUIRE(tracker.state().bpm == Approx(bpmOf(30)).margin(2.0));

    tracker.seedTempo(bpmOf(30));
    CHECK(tracker.state().bpm == Approx(bpmOf(30)).margin(2.0));
    CHECK(tracker.state().locked); // a tap that agrees keeps the lock
    (void)feedDisagreeing(tracker, index, 15, 30, 300);
    CHECK(tracker.state().bpm == Approx(bpmOf(30)).margin(2.0));

    SECTION("and a tap an octave up from the beats doubles the number, as ×2 would") {
        tracker.seedTempo(bpmOf(15));
        CHECK(tracker.state().bpm == Approx(bpmOf(15)).margin(2.0));
    }
}

TEST_CASE("the octave fold divides the beats, not only the number", "[tracking][tempo]") {
    // The user's report of 2026-09-06: "the tool kept detecting the bpm as double, when it
    // has a very obvious kick drum at 93ish bpm ... it kept trying to fold from 186."
    //
    // The fold was folding — the *number* read 92. What went past it untouched were the
    // beats, so OSC, MIDI clock, Link's phase and every trigger rule ran at 186 under a
    // readout that said 92. See Options::foldBeats.
    TempoTracker tracker(kFramePeriod, testOptions()); // the default 70-140 window
    std::uint64_t index = 0;

    // Interval 32 is 93.75 BPM, inside the window. The cloud sitting there is the evidence
    // the fold needs before it will divide anything: Options::foldSupportFrames.
    const Published slow = feed(tracker, index, 32, 400);
    REQUIRE(tracker.state().locked);
    REQUIRE(tracker.state().bpm == Approx(bpmOf(32)).margin(1.0));
    // Nothing divided: the filter and the published grid are the same grid.
    CHECK(slow.beats == 400 / 32 + 1);

    // Now the filter goes to double time — interval 16, 187.5 BPM — which is exactly what it
    // does over 56 % of "03 - Fake Sweat". The tempo must not move, and the beats must halve
    // with it rather than doubling under a number that did not.
    std::uint64_t called = 0;
    const Published fast = feed(tracker, index, 16, 640, 4, &called);
    CHECK(tracker.state().bpm == Approx(bpmOf(32)).margin(2.0));
    CHECK(called == 640 / 16);
    CHECK(fast.beats == called / 2);
    // And the bar with them: a filter bar is four of *its* beats, so under a divisor of two
    // it calls a downbeat twice as often as one is due.
    CHECK(fast.downbeats == called / 8);

    SECTION("switched off, the fold moves the number and nothing else — as it used to") {
        TempoTracker::Options options = testOptions();
        options.foldBeats = false;
        TempoTracker unfolded(kFramePeriod, options);
        std::uint64_t at = 0;
        (void)feed(unfolded, at, 32, 400);
        std::uint64_t calls = 0;
        const Published all = feed(unfolded, at, 16, 640, 4, &calls);
        CHECK(all.beats == calls); // every one of the filter's, at twice the published tempo
        CHECK(unfolded.state().bpm == Approx(bpmOf(32)).margin(2.0));
    }
}

TEST_CASE("a record the cloud never reads slowly keeps every beat", "[tracking][tempo]") {
    // The other half of the same question, and the reason the fold asks the music before it
    // divides anything. A 204 BPM Quickstep under a 70-140 window reaches the fold looking
    // exactly like the case above — the filter is calling twice as many beats as the
    // published tempo — and is its opposite: the filter is right and the window does not fit
    // the record. Halving there throws away every second real beat.
    //
    // Measured over Ballroom, dividing unconditionally cost Quickstep 0.90 beat F-measure to
    // 0.62, Viennese Waltz 0.96 to 0.65 and Jive 0.90 to 0.66. Nothing in the activations
    // separates the two cases; the cloud never settling on the slower octave does. See
    // Options::foldSupportFrames.
    TempoTracker tracker(kFramePeriod, testOptions());
    std::uint64_t index = 0;

    // Straight in at 187.5 BPM and never anywhere else, which is what a genuinely fast
    // record looks like: the cloud has no reason to visit half of it.
    std::uint64_t called = 0;
    const Published fast = feed(tracker, index, 16, 2000, 4, &called);
    CHECK(tracker.state().bpm == Approx(bpmOf(32)).margin(2.0)); // the number still folds
    CHECK(fast.beats == called);                                 // and the beats are all kept
    CHECK(called > 100);
}

TEST_CASE("a manual halving divides the beats without waiting to be convinced",
          "[tracking][tempo]") {
    // Evidence is wanted in place of an instruction, not in spite of one: the operator
    // pressing ÷2 has said which grid they mean. See Options::foldSupportFrames.
    TempoTracker tracker(kFramePeriod, testOptions());
    std::uint64_t index = 0;
    std::uint64_t before = 0;
    const Published kept = feed(tracker, index, 16, 320, 4, &before);
    REQUIRE(kept.beats == before); // nothing has convinced it yet

    tracker.halve();
    std::uint64_t after = 0;
    const Published halved = feed(tracker, index, 16, 640, 4, &after);

    // The press lands on top of a fold that had *already* put the number an octave down
    // without being allowed to move the beats, so the grid divides by four here and the
    // number by two. Asserting either count on its own would be asserting that arithmetic
    // rather than the thing it is for, which is this:
    //
    //   **the beats a tracker publishes are the tempo it publishes.**
    //
    // That is the invariant the whole of `Options::foldBeats` exists to restore, it is what
    // the user's report was a violation of, and it is true here where a ÷2 has been pressed
    // on a fold that was holding back — which is exactly where it is easiest to lose.
    const double publishedPeriodFrames = 60.0 / (tracker.state().bpm * kFramePeriod);
    const double framesPerPublishedBeat = 640.0 / static_cast<double>(halved.beats);
    CHECK(framesPerPublishedBeat == Approx(publishedPeriodFrames).margin(2.0));
    CHECK(tracker.state().bpm == Approx(bpmOf(64)).margin(2.0)); // 187.5, folded then halved
}

TEST_CASE("an estimate wandering across the window's edge does not take the octave with it",
          "[tracking][tempo]") {
    // The failure this exists to stop, measured on references/audio's "01 - Pirates": a
    // 140 BPM track under the default 70-140 window flipped between 136 and 71 fifty times
    // in 213 seconds, because the cloud reports interval 21 or interval 22 — 142.9 BPM or
    // 136.4, the state space holding nothing in between — and a memoryless fold folds one
    // and not the other. See TempoTracker::Options::foldHysteresis.
    TempoTracker::Options options = testOptions();
    options.minBpm = 70.0;
    options.maxBpm = 140.0;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    // Interval 22 is 136.4, inside the window; interval 21 is 142.9, just outside it.
    settle(tracker, index, 22, 0.9, 30); // two beats: a lock needs them
    REQUIRE(tracker.state().locked);
    REQUIRE(tracker.state().bpm == Approx(bpmOf(22)));

    double lowest = tracker.state().bpm;
    for (int round = 0; round < 40; ++round) {
        settle(tracker, index, round % 2 == 0 ? 21 : 22, 0.9, 3);
        lowest = std::min(lowest, tracker.state().bpm);
    }
    // Nothing near half. The published tempo stays up where the material is, and 142.9 is
    // published as 142.9 rather than as 71.4: the window says which octave, not where the
    // fence is.
    CHECK(lowest > 100.0);
    CHECK(tracker.state().bpm > 130.0);

    SECTION("but an estimate genuinely out of range is still folded") {
        // Well outside the window and its hysteresis, so there is nothing ambiguous about
        // it: interval 14 is 214.3 BPM, which is what a Quickstep does under a 70-140
        // window (§7 deviation 4).
        settle(tracker, index, 14, 0.9, 300);
        CHECK(tracker.state().bpm == Approx(bpmOf(14) / 2.0).margin(2.0));
    }
}

TEST_CASE("the tempo locks only after sustained agreement, and unlocks the same way",
          "[tracking][tempo]") {
    TempoTracker::Options options = testOptions();
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

    SECTION("sustained disagreement unlocks it, and the tempo waits for the new one to "
            "earn a lock of its own") {
        settle(tracker, index, 31, 0.8, 74);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)));
        settle(tracker, index, 31, 0.8, 1);
        CHECK_FALSE(tracker.state().locked);
        // An unlock says the tracker is no longer sure; it does not say it has a better
        // answer. What is published meanwhile is still the tempo that last earned a lock.
        CHECK(tracker.state().bpm == Approx(bpmOf(23)));
        // The 75 frames that unwound the lock already count towards whatever replaces it,
        // so a genuine change does not pay for the same evidence twice. This lock was 25
        // frames old, so an eighth of that is under `lockAfter` and it is replaced at the
        // ordinary price — see Options::relockAfter.
        settle(tracker, index, 31, 0.8, 1);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(31)));
    }

    SECTION("a disagreement that resolves never moves the tempo at all") {
        // Two thirds of the way to an unlock, then back. This is the shape the operator's
        // report was about — "as soon as the main beat drops it goes from 140 to 90 in
        // seconds, just because the clap stopped" — and it now costs nothing.
        settle(tracker, index, 31, 0.8, 50);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)));
        settle(tracker, index, 23, 0.8, 1);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)));
        // And the disagreement counter really was cleared, so a second excursion gets the
        // whole window again rather than finishing off the first.
        settle(tracker, index, 31, 0.8, 74);
        CHECK(tracker.state().locked);
    }

    SECTION("a tempo that has held for a while is defended for longer than a new one") {
        // Twelve seconds of lock, so a challenger owes an eighth of that: 75 frames, which
        // is more than `lockAfter` and more than the 75 the unlock itself supplied.
        settle(tracker, index, 23, 0.8, 600);
        REQUIRE(tracker.state().locked);

        settle(tracker, index, 31, 0.8, 75);
        REQUIRE_FALSE(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)));
        // A young lock would have taken it on the next frame; this one does not.
        settle(tracker, index, 31, 0.8, 1);
        CHECK_FALSE(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)));
        // ...but it is a delay, not a refusal: keep arguing and the new tempo wins.
        settle(tracker, index, 31, 0.8, 100);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(31)));
    }

    SECTION("neighbouring tempo intervals are not a disagreement") {
        // 23 frames is 130.43 and 22 is 136.36 — one step of the state space, 4.5 % apart,
        // and a cloud sitting on a 133 BPM track hops between them continually. Counting
        // that as disagreement is why no lock survived a long passage; see
        // Options::sameTempoTolerance.
        for (int round = 0; round < 40; ++round) {
            settle(tracker, index, round % 2 == 0 ? 22 : 23, 0.8, 5);
        }
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)).margin(bpmOf(23) * 0.06));
    }
}

TEST_CASE("a pinned lock is held up rather than set", "[tracking][tempo]") {
    TempoTracker::Options options = testOptions();
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
        // The tempo is held through the unlock itself and moves when the new one locks,
        // which is the frame after — see "sustained disagreement unlocks it" above. What
        // this control has to show is that the disagreement really arrived, and it did.
        settle(loose, at, 31, 0.8, 1);
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
        // And the lock as soon as it is earned again: `lockAfter` frames and two beats, the
        // beat spacing having gone with the release.
        settle(tracker, index, 31, 0.8, 2 * 31);
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

    SECTION("pinning while hunting holds up nothing until a lock is earned") {
        // The audit of 2026-09-25, M4. This used to lock at once to whatever was showing — which
        // before the first lock is the hunt's guess of the moment — and the engine then held the
        // decoder to that guess and Link was fed it as locked: pinning noise.
        TempoTracker mid(kFramePeriod, options);
        std::uint64_t at = 0;
        settle(mid, at, 31, 0.8, 5); // nowhere near lockAfter
        REQUIRE_FALSE(mid.state().locked);
        REQUIRE(mid.state().bpm == Approx(bpmOf(31)));

        mid.setLockPinned(true);
        CHECK(mid.state().pinned);
        CHECK_FALSE(mid.state().locked);
        // The lock comes when it is earned — `lockAfter` frames and two beats — and is then held.
        settle(mid, at, 31, 0.8, 31);
        CHECK(mid.state().locked);
        settle(mid, at, 23, 0.8, options.unlockAfter * 2);
        CHECK(mid.state().locked);
        CHECK(mid.state().bpm == Approx(bpmOf(31)));
    }

    SECTION("pinning after a lock was lost holds up the tempo that was earned") {
        // What is showing through a lost lock is the tempo the last lock was on, held: earned,
        // so a pin may hold it up again.
        settle(tracker, index, 31, 0.8, options.unlockAfter);
        REQUIRE_FALSE(tracker.state().locked);
        REQUIRE(tracker.state().bpm == Approx(bpmOf(23)));
        tracker.setLockPinned(true);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23)));
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
    TempoTracker::Options options = testOptions();
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

    // **And it goes on moving with the beats**, which the two checks above cannot tell from a
    // refinement frozen at the moment of the pin: the record drifts a BPM slower while pinned,
    // and the number follows it (the audit of 2026-09-25, T13). Thirty beats, so the window of
    // gaps is all the new tempo's.
    const double before = tracker.state().bpm;
    constexpr double kDrifted = 127.0;
    const double drifted = 60.0 / (kDrifted * kFramePeriod);
    const double start = static_cast<double>(at);
    for (int beat = 1; beat <= 30; ++beat) {
        const auto on = static_cast<std::uint64_t>(start + static_cast<double>(beat) * drifted + 0.5);
        while (at < on) {
            (void)tracker.process(frameAt(at++, 23, 0.9));
        }
        TrackedFrame frame = frameAt(at++, 23, 0.9);
        frame.emitted = TrackedFrame::Emitted::Beat;
        (void)tracker.process(frame);
    }
    CHECK(tracker.state().refined);
    CHECK(tracker.state().bpm == Approx(kDrifted).margin(0.3));
    CHECK(tracker.state().bpm < before - 0.5);
}

TEST_CASE("below the confidence gate the last good tempo is held", "[tracking][tempo]") {
    TempoTracker::Options options = testOptions();
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
    TempoTracker tracker(kFramePeriod, testOptions());
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
        TempoTracker bare(kFramePeriod, testOptions());
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
    //
    // The beats are 23 frames apart here rather than one per frame, because *where inside
    // that gap* the press lands is now the whole question — see TempoTracker::snapDownbeat.
    TempoTracker tracker(kFramePeriod, testOptions());
    std::uint64_t index = 0;
    // Frames carrying no beat: the gap between two of them, run a piece at a time so a
    // press can be put anywhere in it.
    const auto gap = [&](std::uint32_t frames) {
        for (std::uint32_t i = 0; i < frames; ++i) {
            (void)tracker.process(frameAt(index++, kInterval, 0.9, 4));
        }
    };
    // Whatever is left of the gap, and then the beat.
    const auto beat = [&](TrackedFrame::Emitted kind, std::uint32_t rest = kInterval - 1) {
        gap(rest);
        TrackedFrame frame = frameAt(index++, kInterval, 0.9, 4);
        frame.emitted = kind;
        return tracker.process(frame);
    };

    // Settle into the filter's own bar: 1, 2, 3, 4.
    (void)beat(TrackedFrame::Emitted::Downbeat);
    (void)beat(TrackedFrame::Emitted::Beat);
    REQUIRE(tracker.state().beatInBar == 2);
    const std::uint64_t barsBefore = tracker.state().bars;

    // The operator presses the button on the downbeat they can hear, which is the beat the
    // tracker has just called — its second. That beat becomes the bar's first, and it does
    // so immediately: waiting for the next one is what put the bar a beat behind the music.
    REQUIRE(tracker.state().barsDeclared == 0);
    tracker.snapDownbeat();
    CHECK(tracker.state().beatInBar == 1);
    CHECK(tracker.state().bars == barsBefore + 1);
    // That beat went out as beat 2, so no downbeat rule heard it; the bar is declared, for the
    // output thread to fire its rules late (the audit's M4).
    CHECK(tracker.state().barsDeclared == 1);
    CHECK(tracker.state().declaredBar == barsBefore + 1);

    // So the next beat called is the bar's second, and it is the one that carries the new
    // phase out to the transports — the beat the operator named went out before they
    // pressed anything, and cannot be marked.
    std::optional<BeatEvent> event = beat(TrackedFrame::Emitted::Beat);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 2);
    CHECK_FALSE(event->downbeat);
    CHECK(event->snapped);

    // Only that one beat is marked snapped; the rest are ordinary.
    event = beat(TrackedFrame::Emitted::Beat);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 3);
    CHECK_FALSE(event->snapped);

    // And the filter's own downbeat no longer starts a bar: the rotation is kept, or a
    // correction the filter undid a bar later would be no correction at all.
    event = beat(TrackedFrame::Emitted::Downbeat);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 4);
    CHECK_FALSE(event->downbeat);
    for (const std::uint32_t expected : {1u, 2u, 3u, 4u, 1u, 2u}) {
        event = beat(expected == 4 ? TrackedFrame::Emitted::Downbeat : TrackedFrame::Emitted::Beat);
        REQUIRE(event.has_value());
        INFO("expected beat " << expected << " of the bar");
        CHECK(event->beatInBar == expected);
        CHECK(event->downbeat == (expected == 1));
    }

    SECTION("a second snap moves it again") {
        REQUIRE(tracker.state().beatInBar != 1);
        tracker.snapDownbeat();
        CHECK(tracker.state().beatInBar == 1);
        event = beat(TrackedFrame::Emitted::Beat);
        REQUIRE(event.has_value());
        CHECK(event->beatInBar == 2);
        CHECK(event->snapped);
    }

    SECTION("a press past the middle of the gap means the beat still to come") {
        // The other half of "nearest". Nothing moves on the press itself, because the beat
        // being pointed at has not happened; when it does, it is beat 1. This is what every
        // press used to do, and for a press here it is right.
        constexpr std::uint32_t kLate = kInterval / 2 + 1; // 12 of 23: past the halfway mark
        const std::uint32_t before = tracker.state().beatInBar;
        const std::uint64_t bars = tracker.state().bars;
        gap(kLate);
        tracker.snapDownbeat();
        CHECK(tracker.state().beatInBar == before);
        CHECK(tracker.state().bars == bars);

        event = beat(TrackedFrame::Emitted::Beat, kInterval - 1 - kLate);
        REQUIRE(event.has_value());
        CHECK(event->beatInBar == 1);
        CHECK(event->downbeat);
        CHECK(event->snapped);
        CHECK(tracker.state().bars == bars + 1);
        // This bar's first beat *is* a downbeat when it goes out, so nothing is owed.
        CHECK(tracker.state().barsDeclared == 1); // still only the first snap's
    }

    SECTION("the two halves of the gap divide at the middle of it") {
        // 11 frames after the beat is 220 ms of a 460 ms period and belongs to the beat just
        // gone; 12 is 240 ms and belongs to the next. No constant decides that — the split
        // is the period's own middle, which is the only place it can be without a measured
        // number for how long after a beat a person presses a button.
        gap(kInterval / 2); // 11
        tracker.snapDownbeat();
        CHECK(tracker.state().beatInBar == 1);

        (void)beat(TrackedFrame::Emitted::Beat, kInterval - 1 - kInterval / 2);
        const std::uint32_t after = tracker.state().beatInBar;
        gap(kInterval / 2 + 1); // 12
        tracker.snapDownbeat();
        CHECK(tracker.state().beatInBar == after);
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
    TempoTracker tracker(kFramePeriod, testOptions());
    std::uint64_t index = 0;
    const auto gap = [&](std::uint32_t frames) {
        for (std::uint32_t i = 0; i < frames; ++i) {
            (void)tracker.process(frameAt(index++, kInterval, 0.9, 4));
        }
    };
    const auto beat = [&](TrackedFrame::Emitted kind, std::uint32_t rest = kInterval - 1) {
        gap(rest);
        TrackedFrame frame = frameAt(index++, kInterval, 0.9, 4);
        frame.emitted = kind;
        return tracker.process(frame);
    };

    // Before any beat at all there is no beat behind to name, so the press means the one
    // still to come however early in the gap it lands.
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

TEST_CASE("a manual downbeat on a beat the filter has no bar for still starts one",
          "[tracking][tempo]") {
    // Between the filter's first beat and its first downbeat there is a real stretch —
    // seconds of it on a track that opens without drums — where beats are being called and
    // nothing has an opinion on the bar. A press in there names the beat just gone like any
    // other, but there is no filter position to rotate against, so the count runs from the
    // press and joins the filter's when it finally has one.
    TempoTracker tracker(kFramePeriod, testOptions());
    std::uint64_t index = 0;
    const auto beat = [&](TrackedFrame::Emitted kind) {
        for (std::uint32_t i = 0; i + 1 < kInterval; ++i) {
            (void)tracker.process(frameAt(index++, kInterval, 0.9, 4));
        }
        TrackedFrame frame = frameAt(index++, kInterval, 0.9, 4);
        frame.emitted = kind;
        return tracker.process(frame);
    };

    // Beats, but no downbeat: the bar is unknown and says so.
    for (int i = 0; i < 3; ++i) {
        const std::optional<BeatEvent> event = beat(TrackedFrame::Emitted::Beat);
        REQUIRE(event.has_value());
        CHECK(event->beatInBar == 0);
    }
    CHECK(tracker.state().bars == 0);

    tracker.snapDownbeat();
    CHECK(tracker.state().beatInBar == 1);
    CHECK(tracker.state().bars == 1);

    std::optional<BeatEvent> event = beat(TrackedFrame::Emitted::Beat);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 2);
    CHECK(event->snapped);

    event = beat(TrackedFrame::Emitted::Beat);
    REQUIRE(event.has_value());
    CHECK(event->beatInBar == 3);

    // The filter's first downbeat lands on what the operator is counting as 4, and the
    // count goes on from there rather than being taken back.
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
    TempoTracker::Options options = testOptions();
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

    SECTION("a passage with no beats in it holds the refinement rather than dropping back "
            "to the state space's coarse value") {
        const double refined = tracker.state().bpm;
        REQUIRE(refined == Approx(128.0).margin(1.0));
        // Eight seconds of frames with nothing emitted, which is what a breakdown is. The
        // gaps in `beatFrames_` go stale and stop being counted; before, that fell straight
        // back to `lockedBpm_` — 130.43 here — and alternating between two values 2.4 BPM
        // apart is exactly the jump the operator was reporting.
        for (int i = 0; i < 400; ++i) {
            (void)tracker.process(frameAt(next++, 23, 0.9));
        }
        CHECK(tracker.state().refined);
        CHECK(tracker.state().bpm == Approx(refined));
    }

    SECTION("a missed beat is thrown out rather than halving the tempo") {
        // Skip one beat: the gap doubles, and a mean over the gaps would drag the tempo
        // down to 125. **Twenty-eight beats, and it is the count that makes this a test**
        // (the audit of 2026-09-25, T8): the window holds `refineOverBeats` gaps, 24, so it
        // takes 25 beats to push out the older run's gaps and the 200-frame one, and past 30
        // the doubled gap — the sixth — has left it too. It was forty, so the tempo came out
        // the same with the rejection taken away.
        std::uint64_t at = next + 200;
        for (int beat = 0; beat < 28; ++beat) {
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
    TempoTracker::Options options = testOptions();
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
    TempoTracker::Options options = testOptions();
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    settle(tracker, index, 23, 0.9, 30);
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

namespace {

/// Locks `tracker` on beats laid at 128 BPM while the cloud reports 23 frames, 130.43 — so what
/// is published is the refinement, 2.4 BPM from the filter's own estimate. On real music the two
/// differ by more than the lock's tolerance on most locked frames (the audit of 2026-09-25, H5:
/// 81 %, 85 %, 37 % and 72 % on four excerpts).
void lockRefinedAt128(TempoTracker& tracker, std::uint64_t& next) {
    const double period = 60.0 / (128.0 * kFramePeriod);
    const std::uint64_t first = next;
    for (int beat = 0; beat < 40; ++beat) {
        const auto at = first + static_cast<std::uint64_t>(static_cast<double>(beat) * period + 0.5);
        while (next < at) {
            (void)tracker.process(frameAt(next++, 23, 0.9));
        }
        TrackedFrame frame = frameAt(next++, 23, 0.9);
        frame.emitted = beat % 4 == 0 ? TrackedFrame::Emitted::Downbeat : TrackedFrame::Emitted::Beat;
        (void)tracker.process(frame);
    }
}

} // namespace

TEST_CASE("a tap that agrees, halve and double move what is showing, not the filter's estimate",
          "[tracking][tempo]") {
    // The audit of 2026-09-25, H5. A tap, ÷2 and ×2 all rebuilt the published tempo from the
    // filter's continuous estimate. ÷2 published half of *that*; and a tap whose rebuilt number
    // was more than the lock's half-BPM tolerance from the refined one — most taps, on real
    // music — dropped the lock: LOCKED out for the whole of the tapping, Link sent nothing, then
    // a forced snap at the rough tempo, and the MIDI clock on the rough tempo for nine beats.
    const bool fold = GENERATE(false, true);
    INFO((fold ? "fold on" : "fold off"));
    TempoTracker::Options options = testOptions();
    options.octaveFold = fold;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t next = 0;
    lockRefinedAt128(tracker, next);
    REQUIRE(tracker.state().locked);
    REQUIRE(tracker.state().refined);
    const double shown = tracker.state().bpm;
    REQUIRE(shown == Approx(128.0).margin(1.0));
    REQUIRE(std::abs(tracker.state().calledBpm - shown) > options.lockToleranceBpm);

    SECTION("a tap on the tempo that is showing changes nothing") {
        tracker.seedTempo(shown * 1.01); // a human tap, a per cent out
        CHECK(tracker.state().locked);
        CHECK(tracker.state().refined);
        CHECK(tracker.state().bpm == Approx(shown).epsilon(1e-12));
    }

    SECTION("÷2 is half of what was showing, and ×2 puts it back") {
        tracker.halve();
        CHECK(tracker.state().bpm == Approx(shown / 2.0).epsilon(1e-12));
        CHECK(tracker.state().locked);
        CHECK(tracker.state().refined);
        tracker.redouble();
        CHECK(tracker.state().bpm == Approx(shown).epsilon(1e-12));
        CHECK(tracker.state().locked);
    }

    SECTION("a tap at half time is ÷2") {
        tracker.seedTempo(shown / 2.0 * 0.99);
        CHECK(tracker.state().bpm == Approx(shown / 2.0).epsilon(1e-12));
        CHECK(tracker.state().locked);
    }
}

TEST_CASE("a halving is dropped at the next track even when a pin was let go in between",
          "[tracking][tempo]") {
    // The audit of 2026-09-25, M3: H2's fix, through the pin. Releasing a pin drops the lock and
    // everything learned under it, so the next lock was never "replacing" anything — and only a
    // replacing lock dropped the ÷2. So: ÷2 on a drum-and-bass record, pinned through the mix,
    // released on the house record, and the house record came out at half its tempo with its
    // beats divided.
    TempoTracker::Options options = testOptions();
    options.octaveFold = false; // as a fresh install ships
    options.lockAfter = 5;
    options.unlockAfter = 10;
    options.relockAfter = 10;
    options.confidenceSmoothing = 2.0;

    const auto throughAPin = [&](bool keep, std::uint32_t nextRecord) {
        TempoTracker::Options chosen = options;
        chosen.keepOctaveShift = keep;
        TempoTracker tracker(kFramePeriod, chosen);
        std::uint64_t index = 0;
        settle(tracker, index, 23, 0.9, 30);
        tracker.halve();
        settle(tracker, index, 23, 0.9, 10);
        REQUIRE(tracker.state().bpm == Approx(bpmOf(23) / 2.0));
        tracker.setLockPinned(true);
        settle(tracker, index, nextRecord, 0.9, 60); // the mix, under the pin
        REQUIRE(tracker.state().locked);
        tracker.setLockPinned(false);
        settle(tracker, index, nextRecord, 0.9, 60);
        REQUIRE(tracker.state().locked);
        return tracker.state();
    };

    SECTION("released on another record, the halving goes") {
        const auto state = throughAPin(false, 31);
        CHECK(state.bpm == Approx(bpmOf(31)));
        CHECK(state.beatDivisor == 1);
    }
    SECTION("released on the same record, it is still that record's") {
        const auto state = throughAPin(false, 23);
        CHECK(state.bpm == Approx(bpmOf(23) / 2.0));
        CHECK(state.beatDivisor == 2);
    }
    SECTION("kept, it is kept") {
        const auto state = throughAPin(true, 31);
        CHECK(state.bpm == Approx(bpmOf(31) / 2.0));
        CHECK(state.beatDivisor == 2);
    }
}

TEST_CASE("a manual halving publishes the bar's downbeat, not whichever beat came next",
          "[tracking][tempo]") {
    // The audit of 2026-09-25, M2. After a divide, the first beat the filter called took the
    // published grid, whichever beat of the bar it was — and the evidence never moved it, because
    // the two halves of a bar score within a few per cent of each other. About half the time the
    // published beats were 2 and 4: the lights on the backbeat, bar rules on beat 2. Pressed here
    // with the second beat of a bar the next one due, which is the unlucky half.
    TempoTracker::Options options = testOptions();
    options.octaveFold = false;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t next = 0;
    // One filter beat every 23 frames, a downbeat on every fourth, the network's activation the
    // same on each as `feed` gives it.
    const auto play = [&](int from, int to, std::vector<int>& published) {
        for (int beat = from; beat < to; ++beat) {
            for (int i = 0; i < 22; ++i) {
                (void)tracker.process(frameAt(next++, 23, 0.9));
            }
            TrackedFrame frame = frameAt(next++, 23, 0.9);
            const bool downbeat = beat % 4 == 0;
            frame.emitted = downbeat ? TrackedFrame::Emitted::Downbeat : TrackedFrame::Emitted::Beat;
            frame.beatActivation = downbeat ? 0.2f : 0.7f;
            frame.downbeatActivation = downbeat ? 0.5f : 0.0f;
            if (tracker.process(frame)) {
                published.push_back(beat);
            }
        }
    };
    std::vector<int> before;
    play(0, 13, before); // the last of these is a downbeat, so the next is the bar's second
    REQUIRE(tracker.state().locked);

    tracker.halve();
    std::vector<int> after;
    play(13, 61, after);
    REQUIRE(tracker.state().beatDivisor == 2);
    // From the first downbeat after the press, the published beats are the bar's 1 and 3.
    int downbeatsPublished = 0;
    for (const int beat : after) {
        if (beat >= 16) {
            INFO("filter beat " << beat << " published");
            CHECK(beat % 2 == 0);
            downbeatsPublished += beat % 4 == 0 ? 1 : 0;
        }
    }
    CHECK(downbeatsPublished == (60 - 16) / 4 + 1);

    // And from a divided grid, where the place of the next beat is not known, the first downbeat
    // the filter calls chooses: a second ÷2 publishes the bar's beat 1 and nothing else.
    tracker.halve();
    std::vector<int> quartered;
    play(61, 101, quartered);
    REQUIRE(tracker.state().beatDivisor == 4);
    int downbeatsQuartered = 0;
    for (const int beat : quartered) {
        if (beat >= 64) {
            INFO("filter beat " << beat << " published");
            CHECK(beat % 4 == 0);
            ++downbeatsQuartered;
        }
    }
    CHECK(downbeatsQuartered == (100 - 64) / 4 + 1);
}

TEST_CASE("the manual octave shift stops at two octaves either way", "[tracking][tempo]") {
    // The audit's H1: ÷2 and ×2 were unbounded, and so was the shift a tap turned into with the
    // fold off, so a few presses sent Link and the MIDI clock a tempo of eight BPM or a
    // thousand. Past two octaves it is not an octave preference but a tempo nothing plays.
    TempoTracker::Options options = testOptions();
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;
    settle(tracker, index, 23, 0.9, 30);
    REQUIRE(tracker.state().locked);

    for (int press = 0; press < 5; ++press) {
        tracker.halve();
    }
    CHECK(tracker.state().bpm == Approx(bpmOf(23) / 4.0));
    for (int press = 0; press < 9; ++press) {
        tracker.redouble();
    }
    CHECK(tracker.state().bpm == Approx(bpmOf(23) * 4.0));
    // And one press back from the top is one octave down, not the fifth of the presses above.
    tracker.halve();
    CHECK(tracker.state().bpm == Approx(bpmOf(23) * 2.0));

    SECTION("a tap with the fold off is held to the same two octaves") {
        TempoTracker::Options off = options;
        off.octaveFold = false;
        TempoTracker unfolded(kFramePeriod, off);
        std::uint64_t at = 0;
        settle(unfolded, at, 23, 0.9, 20);
        unfolded.seedTempo(bpmOf(23) / 16.0);
        CHECK(unfolded.state().bpm == Approx(bpmOf(23) / 4.0));
        unfolded.seedTempo(bpmOf(23) * 16.0);
        CHECK(unfolded.state().bpm == Approx(bpmOf(23) * 4.0));
    }
}

TEST_CASE("a halving is dropped at the next track unless the operator asked to keep it",
          "[tracking][tempo]") {
    // The audit's H2, and the operator's call of 2026-09-23: a set is one record after another,
    // and a ÷2 that suited a drum-and-bass record turned the house record after it into half
    // time with the beats divided, until somebody noticed. Dropped at the next track by
    // default; kept when the operator ticks "keep for the next track".
    TempoTracker::Options options = testOptions();
    options.octaveFold = false; // as a fresh install ships
    options.lockAfter = 5;
    options.unlockAfter = 10;
    options.relockAfter = 10;
    options.confidenceSmoothing = 2.0;

    const auto nextTrack = [&](bool keep, bool tapped) {
        TempoTracker::Options chosen = options;
        chosen.keepOctaveShift = keep;
        TempoTracker tracker(kFramePeriod, chosen);
        std::uint64_t index = 0;
        settle(tracker, index, 23, 0.9, 30); // 130.4
        REQUIRE(tracker.state().locked);
        if (tapped) {
            tracker.seedTempo(bpmOf(23) / 2.0); // the operator taps half time
        } else {
            tracker.halve();
        }
        settle(tracker, index, 23, 0.9, 30);
        REQUIRE(tracker.state().locked);
        REQUIRE(tracker.state().bpm == Approx(bpmOf(23) / 2.0));
        REQUIRE(tracker.state().beatDivisor == 2);
        // The next record: 31 frames a beat, 96.8, for long enough to take the lock.
        settle(tracker, index, 31, 0.9, 120);
        REQUIRE(tracker.state().locked);
        return tracker.state();
    };

    SECTION("by default the next track is published at the tempo it is heard at") {
        const auto state = nextTrack(false, false);
        CHECK(state.bpm == Approx(bpmOf(31)));
        CHECK(state.beatDivisor == 1);
    }

    SECTION("kept, the halving goes on to the next track") {
        const auto state = nextTrack(true, false);
        CHECK(state.bpm == Approx(bpmOf(31) / 2.0));
        CHECK(state.beatDivisor == 2);
    }

    SECTION("a tapped octave is never carried, whatever the setting") {
        // A tap names *this* record's tempo, not an octave for the rest of the night.
        const auto state = nextTrack(true, true);
        CHECK(state.bpm == Approx(bpmOf(31)));
        CHECK(state.beatDivisor == 1);
    }

    SECTION("a breakdown and back is not a new track") {
        // The lock let go and came back to the tempo it had: the same record, and the halving
        // the operator pressed for it stays.
        TempoTracker tracker(kFramePeriod, options);
        std::uint64_t index = 0;
        settle(tracker, index, 23, 0.9, 30);
        tracker.halve();
        settle(tracker, index, 23, 0.9, 30);
        settle(tracker, index, 31, 0.9, 10); // long enough to unlock, not to relock
        REQUIRE_FALSE(tracker.state().locked);
        settle(tracker, index, 23, 0.9, 50); // two beats 23 frames apart, then the lock
        REQUIRE(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23) / 2.0));
    }
}

TEST_CASE("a tapped tempo moves the fold window onto the octave the operator meant",
          "[tracking][tempo]") {
    // The failure §7 deviation 4 measured: under a 70-140 window a Quickstep at 204 BPM
    // is folded to 102 and the operator has no way to say otherwise. Quickstep's
    // octave-tolerant tempo accuracy on Ballroom is 1.000 and its exact accuracy 0.000
    // for exactly this reason. Tapping it is the way out.
    TempoTracker::Options options = testOptions();
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
    // The tap named another octave, and the published tempo moved onto it — **keeping the
    // lock**, as ÷2 and ×2 do (the audit of 2026-09-25, H5). It used to be given up and hunted
    // again, which cost Link every beat of the hunt and then a forced snap.
    CHECK(tracker.state().locked);
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

TEST_CASE("a tap does not switch the fold on behind the operator", "[tracking][tempo]") {
    // Reported from a rig on 2026-09-08: "octave fold keeps getting automatically turned on,
    // which then breaks the next track in the mix because it might not need octave folding
    // ... the tool will always be streamed several tracks in a row". `seedTempo` used to set
    // `octaveFold = true`, so one tap left a window behind that halved or doubled every
    // record after it.
    TempoTracker::Options options = testOptions();
    options.octaveFold = false;
    options.minBpm = 70.0;
    options.maxBpm = 140.0;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;

    settle(tracker, index, 14, 0.9, 20);
    REQUIRE(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(bpmOf(14))); // unfolded: 214.3

    // The tap still puts the tempo on the octave it named — through the manual shift, which
    // is what ÷2 and ×2 use and the only thing an octave instruction can mean with no window.
    tracker.seedTempo(bpmOf(28));
    CHECK_FALSE(tracker.options().octaveFold);
    CHECK(tracker.options().minBpm == Approx(70.0)); // and the window is where it was
    CHECK(tracker.options().maxBpm == Approx(140.0));
    CHECK(tracker.state().bpm == Approx(bpmOf(28)));

    SECTION("and the beats go with it, because a shift is an instruction") {
        // `foldDivisor` does not ask for evidence when the operator has given a shift, so
        // the published grid halves along with the number — which is the whole difference
        // between this and a window, where the beats wait on `foldSupportFrames`.
        settle(tracker, index, 14, 0.9, 10);
        CHECK(tracker.state().beatDivisor == 2);
    }
}

TEST_CASE("changing the fold window only drops the lock when it has to", "[tracking][tempo]") {
    TempoTracker::Options options = testOptions();
    options.minBpm = 70.0;
    options.maxBpm = 140.0;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;
    settle(tracker, index, 23, 0.9, 30);
    REQUIRE(tracker.state().locked);

    options.maxBpm = 160.0; // 130.43 still fits
    tracker.setOptions(options);
    CHECK(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(bpmOf(23)));

    options.minBpm = 60.0;
    options.maxBpm = 120.0; // 130.43 is outside the window itself...
    tracker.setOptions(options);
    // ...but it is *not* halved, because 130.43 is inside 120 widened by the fold's
    // hysteresis. The window says which octave the material is in; it is not a fence, and
    // a 130 BPM track under a 60-120 window is a 130 BPM track. An operator who wants the
    // fence sets `foldHysteresis` to 0, and the section below is what that does.
    CHECK(tracker.state().bpm == Approx(bpmOf(23)));
    // And so the lock stays. This used to drop it while publishing the same tempo — a
    // re-lock for nothing, which is the audit's M3.
    CHECK(tracker.state().locked);

    options.maxBpm = 100.0; // and now it is out of reach of the hysteresis too
    tracker.setOptions(options);
    CHECK_FALSE(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(bpmOf(23) / 2.0));

    SECTION("a fold with no hysteresis is the fence it used to be") {
        TempoTracker::Options hard = options;
        hard.minBpm = 70.0;
        hard.maxBpm = 140.0;
        hard.foldHysteresis = 0.0;
        hard.lockAfter = 5;
        hard.confidenceSmoothing = 2.0;
        TempoTracker fenced(kFramePeriod, hard);
        std::uint64_t at = 0;
        settle(fenced, at, 21, 0.9, 40); // 142.9, just over the edge
        CHECK(fenced.state().bpm == Approx(bpmOf(21) / 2.0).margin(1.0));
    }
}

TEST_CASE("a settings change that leaves the window alone never costs the lock",
          "[tracking][tempo]") {
    // The audit's M3. Every `setOptions` tested the locked tempo against the window, so with
    // ÷2 pressed — 65 BPM under a 70-140 window, exactly where the operator put it — moving
    // the latency slider dropped the lock and the hunt started again. The same for a tempo
    // the hysteresis holds just past the window's edge.
    TempoTracker::Options options = testOptions();
    options.minBpm = 70.0;
    options.maxBpm = 140.0;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;
    settle(tracker, index, 23, 0.9, 30);
    REQUIRE(tracker.state().locked);
    tracker.halve();
    REQUIRE(tracker.state().bpm == Approx(bpmOf(23) / 2.0));
    REQUIRE(tracker.state().locked);

    SECTION("the latency slider") {
        options.latencyOffsetSeconds = 0.015;
        tracker.setOptions(options);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23) / 2.0));
    }
    SECTION("the confidence gate") {
        options.confidenceThreshold = 0.3;
        tracker.setOptions(options);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23) / 2.0));
    }
    SECTION("a window that keeps the halved tempo's octave") {
        options.maxBpm = 150.0; // 130.43 still in it, so the halving still stands
        tracker.setOptions(options);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23) / 2.0));
    }
    SECTION("a window that moves the octave still drops it") {
        options.minBpm = 40.0;
        options.maxBpm = 80.0; // 130.43 folds down an octave now, and then ÷2 on top
        tracker.setOptions(options);
        CHECK_FALSE(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(23) / 4.0));
    }
}

TEST_CASE("a tempo the hysteresis holds past the window's edge survives a settings change",
          "[tracking][tempo]") {
    // The other half of M3. A track folded down an octave that then drifts to just past the
    // window's top — 150 under 70-140 — stays in the folded octave, because the hysteresis
    // remembers the octave in force. A settings change that forgot that octave would flip the
    // published tempo up to 150 and drop the lock, on a nudge of the latency slider.
    TempoTracker::Options options = testOptions();
    options.minBpm = 70.0;
    options.maxBpm = 140.0;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;
    settle(tracker, index, 19, 0.9, 25); // 157.9: past the widened window, so folded
    REQUIRE(tracker.state().bpm == Approx(bpmOf(19) / 2.0));
    settle(tracker, index, 20, 0.9, 30); // 150: inside the hysteresis, the octave kept
    REQUIRE(tracker.state().locked);
    const double folded = tracker.state().bpm;
    REQUIRE(folded < 100.0);

    options.latencyOffsetSeconds = 0.015;
    tracker.setOptions(options);
    CHECK(tracker.state().locked);
    CHECK(tracker.state().bpm == Approx(folded));
    settle(tracker, index, 20, 0.9, 5);
    CHECK(tracker.state().locked);
    CHECK(tracker.state().bpm < 100.0);
}

TEST_CASE("with the fold in the decoder a tempo outside the window keeps its lock through an edit",
          "[tracking][tempo]") {
    // The audit of 2026-09-25, L40: M3 on the default decoder. With the fold in the decoder
    // nothing chooses an octave here, so the octave "before" an edit read as 0, and a tempo the
    // beats or a hold kept outside the window was unlocked by any edit of the window at all —
    // while the tempo published, which the tracker does not fold, did not change.
    TempoTracker::Options options = testOptions();
    options.minBpm = 70.0;
    options.maxBpm = 140.0;
    options.foldInDecoder = true;
    options.lockAfter = 5;
    options.confidenceSmoothing = 2.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;
    settle(tracker, index, 19, 0.9, 25); // 157.9: past the window and its hysteresis
    REQUIRE(tracker.state().locked);
    REQUIRE(tracker.state().bpm == Approx(bpmOf(19))); // not folded here: the decoder's job

    SECTION("an edit that still leaves it outside") {
        options.minBpm = 72.0;
        tracker.setOptions(options);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(19)));
    }
    SECTION("an edit that takes it in") {
        options.minBpm = 120.0;
        options.maxBpm = 240.0;
        tracker.setOptions(options);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().bpm == Approx(bpmOf(19)));
    }
    SECTION("an edit that shuts out a tempo it had inside still drops the lock") {
        settle(tracker, index, 23, 0.9, 30); // 130.4, inside 70-140
        REQUIRE(tracker.state().locked);
        options.minBpm = 40.0;
        options.maxBpm = 80.0;
        tracker.setOptions(options);
        CHECK_FALSE(tracker.state().locked);
    }
}

TEST_CASE("frame counts are stated at 50 Hz and scaled to the tracker's own rate",
          "[tracking][tempo]") {
    // A decoder running at 100 fps hands the tracker twice the frames for the same half
    // second, so `lockAfter = 25` has to mean fifty of them there. See Options::lockAfter.
    TempoTracker::Options options = testOptions();
    options.octaveFold = false;
    options.lockAfter = 25;
    options.confidenceSmoothing = 5.0;
    // The smoother is scaled too — ten frames here — so the first frame's confidence is
    // 0.09; below the default gate, and the count would start a frame late. This test is
    // about the lock's count, so the gate is opened.
    options.confidenceThreshold = 0.05;
    TempoTracker fast(0.01, options);
    std::uint64_t index = 0;
    const auto frame = [&] {
        TrackedFrame f;
        f.frameIndex = index++;
        f.intervalFrames = 46; // 130.4 BPM at 100 fps
        f.refinedIntervalFrames = 46.0;
        f.bpm = 60.0 / (46.0 * 0.01);
        f.tempoAgreement = 0.9;
        f.beatsPerBar = 4;
        if (f.frameIndex % 46 == 0) {
            f.emitted = TrackedFrame::Emitted::Beat; // two by frame 50, as a lock needs
        }
        return f;
    };
    for (int i = 0; i < 49; ++i) {
        (void)fast.process(frame());
    }
    CHECK_FALSE(fast.state().locked);
    (void)fast.process(frame());
    CHECK(fast.state().locked);
    CHECK(fast.state().bpm == Approx(60.0 / (46.0 * 0.01)));
    // What was set is what is read back: nothing is scaled twice on the way round.
    CHECK(fast.options().lockAfter == 25);
    fast.setOptions(fast.options());
    CHECK(fast.options().lockAfter == 25);

    SECTION("a sub-frame beat offset moves the beat's time by that much") {
        TrackedFrame f = frame();
        f.emitted = TrackedFrame::Emitted::Beat;
        f.beatOffsetFrames = 0.25;
        const std::optional<BeatEvent> event = fast.process(f);
        REQUIRE(event.has_value());
        CHECK(event->frameIndex == f.frameIndex);
        CHECK(event->time == Approx((static_cast<double>(f.frameIndex) + 0.25) * 0.01));
    }
}

TEST_CASE("nonsensical options are refused rather than tracked with", "[tracking][tempo]") {
    TempoTracker::Options options = testOptions();
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
    TempoTracker::Options options = testOptions();
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

TEST_CASE("a lock needs beats at the tempo being locked", "[tracking][tempo]") {
    // Agreement is a statement about the decoder's posterior, and a posterior can agree with
    // itself about nothing: over digital silence the forward filter put 0.95 of its mass on one
    // tempo and the tracker locked there with no beat ever called (2026-10-03). See
    // Options::lockBeats; these use its default, three.
    TempoTracker tracker(kFramePeriod);
    std::uint64_t index = 0;
    for (int f = 0; f < 600; ++f) {
        (void)tracker.process(frameAt(index++, 23, 0.95));
    }
    REQUIRE(tracker.state().confidence > 0.9);
    CHECK_FALSE(tracker.state().locked); // twelve seconds of agreement and not one beat

    SECTION("three beats a period apart earn it") {
        settle(tracker, index, 23, 0.95, 3 * 23 + 1); // beats on 621, 644 and 667
        CHECK(tracker.state().locked);
        CHECK(tracker.state().acquired);
    }

    SECTION("so do beats an octave above the tempo the decoder reports") {
        // Early in a track the decoder often calls its beats at twice the tempo its cloud
        // reports; `beatOctaveBeats` moves the octave later. Refusing the lock until then put
        // two of the 23 tracks' first lock at 13 and 21 seconds.
        for (int f = 0; f < 3 * 23 + 1; ++f) {
            TrackedFrame frame = frameAt(index, 46, 0.95);
            if (index % 23 == 0) {
                frame.emitted = TrackedFrame::Emitted::Beat;
            }
            ++index;
            (void)tracker.process(frame);
        }
        CHECK(tracker.state().locked);
    }

    SECTION("beats on no grid do not") {
        // Gaps of 0.7 and 1.35 periods: beats, but at no tempo, and none an octave of this one.
        std::uint64_t nextBeat = index;
        bool shortGap = true;
        for (int f = 0; f < 600; ++f) {
            TrackedFrame frame = frameAt(index, 23, 0.95);
            if (index == nextBeat) {
                frame.emitted = TrackedFrame::Emitted::Beat;
                nextBeat += shortGap ? 16 : 31;
                shortGap = !shortGap;
            }
            ++index;
            (void)tracker.process(frame);
        }
        CHECK_FALSE(tracker.state().locked);
    }
}

TEST_CASE("no signal drops the lock, and what comes back is acquired afresh", "[tracking][tempo]") {
    // `BeatEngine` says when the input has had no signal for a few seconds: a deck that stopped,
    // not a breakdown. The lock goes, nothing may fire until one is earned again, and the next
    // record is not a challenger that has to out-argue half a minute of the last one.
    TempoTracker::Options options = testOptions();
    options.confidenceSmoothing = 5.0;
    TempoTracker tracker(kFramePeriod, options);
    std::uint64_t index = 0;
    settle(tracker, index, 23, 0.9, 1500); // thirty seconds: a lock at its full defence
    REQUIRE(tracker.state().locked);
    REQUIRE(tracker.state().acquired);
    const double held = tracker.state().bpm;

    tracker.setNoSignal(true);
    CHECK(tracker.state().noSignal);
    CHECK_FALSE(tracker.state().locked);
    CHECK_FALSE(tracker.state().acquired);
    CHECK(tracker.state().bpm == held); // the last tempo, held

    SECTION("a pin pressed now holds up nothing: the lock was on a deck that stopped") {
        tracker.setLockPinned(true);
        CHECK_FALSE(tracker.state().locked);
    }

    SECTION("what comes back is a new acquisition at the ordinary price") {
        // While there is none the decoder hears nothing and says no tempo.
        for (int f = 0; f < 200; ++f) {
            TrackedFrame frame;
            frame.frameIndex = index++;
            (void)tracker.process(frame);
        }
        CHECK_FALSE(tracker.state().locked);
        CHECK(tracker.state().holding);
        CHECK(tracker.state().bpm == held);

        tracker.setNoSignal(false);
        CHECK_FALSE(tracker.state().noSignal);
        // Another record, at the ordinary price — `lockAfter` and its beats — and not the three
        // seconds (`relockAfter`) a challenger to a thirty-second lock would owe.
        settle(tracker, index, 31, 0.9, 100);
        CHECK(tracker.state().locked);
        CHECK(tracker.state().acquired);
        CHECK(tracker.state().bpm == Approx(bpmOf(31)));
    }
}
