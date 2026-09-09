#include "core/tracking/forward_filter.hpp"

#include "core/io/npy_file.hpp"
#include "core/rt/alloc_guard.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

using Catch::Approx;
using takt4::tracking::BeatEvent;
using takt4::tracking::ForwardFilter;
using takt4::tracking::TempoTracker;
using takt4::tracking::TrackedFrame;

namespace {

const std::filesystem::path kDataDir{TAKT4_TEST_DATA_DIR};

/// The network's 50 Hz activations of one committed excerpt, interpolated to `steps` per
/// activation the way `BeatEngine::track` does it: the first as it is, and between every
/// two the frames in between, linearly.
std::vector<std::pair<float, float>> upsampled(const char* name, std::size_t steps) {
    const takt4::io::NpyMatrix act =
        takt4::io::readNpyFloat32(kDataDir / "model" / "generic" / (std::string(name) + ".npy"));
    REQUIRE(act.cols == 6);
    std::vector<std::pair<float, float>> out;
    for (std::size_t f = 0; f < act.rows; ++f) {
        const float beat = act.at(f, 3);
        const float down = act.at(f, 4);
        if (f > 0) {
            const float previousBeat = act.at(f - 1, 3);
            const float previousDown = act.at(f - 1, 4);
            for (std::size_t k = 1; k < steps; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(steps);
                out.emplace_back(previousBeat + (beat - previousBeat) * t,
                                 previousDown + (down - previousDown) * t);
            }
        }
        out.emplace_back(beat, down);
    }
    return out;
}

struct Run {
    std::vector<BeatEvent> beats;
    std::vector<TrackedFrame> frames;
};

/// The excerpt through a filter and a tracker built at the filter's rate.
Run track(ForwardFilter& filter, const std::vector<std::pair<float, float>>& activations,
          TempoTracker::Options options = {}) {
    TempoTracker tracker(filter.secondsPerFrame(), options);
    Run run;
    for (const auto& [beat, down] : activations) {
        run.frames.push_back(filter.process(beat, down));
        if (const std::optional<BeatEvent> event = tracker.process(run.frames.back())) {
            run.beats.push_back(*event);
        }
    }
    return run;
}

} // namespace

TEST_CASE("the forward filter's state space is madmom's bar-pointer model at 100 fps",
          "[tracking][forward]") {
    // The numbers TRACKING-PROPOSAL.md §2.4 quotes for the prototype: at 55-215 BPM and
    // 100 fps madmom's BeatStateSpace holds every whole-frame interval from 28 to 109, 82
    // of them, 5617 states to a beat; meters 3 and 4 make 7 beats, 39319 states.
    ForwardFilter::Options both;
    both.meters = {3, 4, 0, 0};
    const ForwardFilter filter(both);
    CHECK(filter.secondsPerFrame() == Approx(0.01));
    CHECK(filter.numIntervals() == 82);
    CHECK(filter.intervals().front() == 28);
    CHECK(filter.intervals().back() == 109);
    CHECK(filter.numMeters() == 2);
    CHECK(filter.numStates() == 39319);
    CHECK(filter.bpmOfInterval(0) == Approx(214.2857).epsilon(1e-6));
    CHECK(filter.bpmOfInterval(81) == Approx(55.0459).epsilon(1e-5));
    CHECK(filter.posterior().size() == 39319);

    // Every tempo-change row is a distribution, and staying put is always its likeliest
    // move: exp(-lambda * |ratio - 1|) peaks at a ratio of one.
    for (std::size_t from = 0; from < filter.numIntervals(); ++from) {
        const auto row = filter.tempoTransitions(from);
        REQUIRE(row.size() == 82);
        double sum = 0.0;
        std::size_t likeliest = 0;
        for (std::size_t to = 0; to < row.size(); ++to) {
            sum += row[to];
            if (row[to] > row[likeliest]) {
                likeliest = to;
            }
        }
        CHECK(sum == Approx(1.0).margin(1e-12));
        CHECK(likeliest == from);
    }
    // Fresh, the posterior is uniform.
    for (const double p : filter.posterior()) {
        REQUIRE(p == Approx(1.0 / 39319.0).epsilon(1e-9));
    }

    SECTION("the default is a bar of four alone, as the operator chose on 2026-09-09") {
        const ForwardFilter four;
        CHECK(four.numMeters() == 1);
        CHECK(four.numStates() == 4 * 5617);
        CHECK(four.numIntervals() == 82);
    }

    SECTION("the meters and the rate are the operator's to choose") {
        ForwardFilter::Options options;
        options.fps = 50;
        options.meters = {2, 3, 4, 0};
        const ForwardFilter fifty(options);
        CHECK(fifty.secondsPerFrame() == Approx(0.02));
        CHECK(fifty.numIntervals() == 42); // 14 to 55, as the particle filter's blob
        CHECK(fifty.numMeters() == 3);
        CHECK(fifty.numStates() == 9 * 1449);
    }
}

TEST_CASE("the forward filter tracks the synthetic excerpt at twice the network's rate",
          "[tracking][forward]") {
    // The golden synthetic excerpt is a drum machine at 128 BPM. Through the filter with the
    // activations interpolated to 100 fps, and the tempo state machine at that rate, the
    // chain has to lock onto it and refine it, exactly as the particle filter's own chain
    // test asks.
    const std::vector<std::pair<float, float>> activations = upsampled("synthetic", 2);
    REQUIRE(activations.size() == 999);

    // The prototype's rule first, since it is the one TRACKING-PROPOSAL.md's numbers were
    // measured with; the two others follow in their own sections.
    ForwardFilter::Options mapRule;
    mapRule.emission = ForwardFilter::Emission::MapCrossing;
    ForwardFilter filter(mapRule);
    TempoTracker::Options options;
    options.minBpm = 90.0;
    options.maxBpm = 180.0;
    options.foldInDecoder = true; // as the engine sets it for this decoder
    filter.setTempoWindow(options.minBpm, options.maxBpm, true);
    const Run run = track(filter, activations, options);

    REQUIRE(run.frames.size() == 999);
    REQUIRE(run.beats.size() > 15);
    const TrackedFrame& last = run.frames.back();
    CHECK(last.frameIndex == 998);
    // 128 BPM is 46.875 frames at 100 fps: the MAP interval is 47, and the posterior mean
    // around it is closer than the whole-frame grid can be.
    CHECK(last.intervalFrames == 47);
    CHECK(last.bpm == Approx(128.0).margin(2.0));
    CHECK(last.beatsPerBar >= 3);
    CHECK(last.beatsPerBar <= 4);
    // The posterior mass on the MAP interval and its neighbours, averaged over the second
    // half: about 0.65 here, against the particle filter's 0.5 to 0.6 on the same excerpt.
    // The last frame alone dips to 0.49, which is why this is a mean.
    double agreement = 0.0;
    for (std::size_t f = run.frames.size() / 2; f < run.frames.size(); ++f) {
        agreement += run.frames[f].tempoAgreement;
    }
    agreement /= static_cast<double>(run.frames.size() - run.frames.size() / 2);
    CHECK(agreement > 0.5);
    // Beats a quarter note apart, in time, once the tempo has settled — the first few are
    // acquisition and come at whatever the posterior first believed — and every one on a
    // whole frame under the MAP crossing rule.
    for (std::size_t i = 5; i < run.beats.size(); ++i) {
        const double gap = run.beats[i].time - run.beats[i - 1].time;
        INFO("beat " << i);
        CHECK(gap == Approx(60.0 / 128.0).margin(0.06));
    }
    for (const TrackedFrame& frame : run.frames) {
        CHECK(frame.beatOffsetFrames == 0.0);
    }
    std::size_t downbeats = 0;
    for (const BeatEvent& beat : run.beats) {
        downbeats += beat.downbeat ? 1 : 0;
    }
    CHECK(downbeats > 2);
    CHECK(downbeats < run.beats.size());

    SECTION("and the tracker above it locks and refines, at its own frame rate") {
        TempoTracker tracker(filter.secondsPerFrame(), options);
        ForwardFilter again(mapRule);
        again.setTempoWindow(options.minBpm, options.maxBpm, true);
        for (const auto& [beat, down] : activations) {
            (void)tracker.process(again.process(beat, down));
        }
        CHECK(tracker.state().locked);
        CHECK(tracker.state().refined);
        // Ten seconds is 21 beats, and the refinement averages the last 24 gaps, so the
        // acquisition's first gaps are still in it: within two, where the particle
        // filter's own chain test asks for one over the same excerpt.
        CHECK(tracker.state().bpm == Approx(128.0).margin(2.0));
        CHECK(tracker.state().bars > 2);
    }

    SECTION("the mean-phase rule puts the same beats up to a horizon ahead") {
        ForwardFilter::Options predictive;
        predictive.emission = ForwardFilter::Emission::MeanPhase;
        ForwardFilter ahead(predictive);
        ahead.setTempoWindow(options.minBpm, options.maxBpm, true);
        const Run predicted = track(ahead, activations, options);
        // The same number of beats, give or take a couple at either end.
        CHECK(predicted.beats.size() >= run.beats.size() - 3);
        CHECK(predicted.beats.size() <= run.beats.size() + 3);
        for (const TrackedFrame& frame : predicted.frames) {
            if (frame.emitted != TrackedFrame::Emitted::None) {
                CHECK(frame.beatOffsetFrames >= 0.0);
                CHECK(frame.beatOffsetFrames <= predictive.predictFrames);
            }
        }
        for (std::size_t i = 5; i < predicted.beats.size(); ++i) {
            const double gap = predicted.beats[i].time - predicted.beats[i - 1].time;
            INFO("beat " << i);
            CHECK(gap == Approx(60.0 / 128.0).margin(0.06));
        }
        CHECK(predicted.frames.back().bpm == Approx(128.0).margin(2.0));
    }

    SECTION("the peak rule puts the beats on the activation's peak, a frame late") {
        // The default rule: a freshly built filter is this one.
        ForwardFilter::Options peaks;
        CHECK(peaks.emission == ForwardFilter::Emission::Peak);
        ForwardFilter atPeak(peaks);
        atPeak.setTempoWindow(options.minBpm, options.maxBpm, true);
        const Run peaked = track(atPeak, activations, options);
        CHECK(peaked.beats.size() >= run.beats.size() - 3);
        CHECK(peaked.beats.size() <= run.beats.size() + 3);
        std::size_t onPeak = 0;
        for (const TrackedFrame& frame : peaked.frames) {
            if (frame.emitted != TrackedFrame::Emitted::None) {
                CHECK(frame.beatOffsetFrames <= -1.0);
                onPeak += frame.beatOffsetFrames == -1.0 ? 1 : 0;
            }
        }
        // A drum machine's peaks are unmistakable: nearly every beat is read off one.
        CHECK(onPeak >= peaked.beats.size() - 3);
        for (std::size_t i = 5; i < peaked.beats.size(); ++i) {
            const double gap = peaked.beats[i].time - peaked.beats[i - 1].time;
            INFO("beat " << i);
            CHECK(gap == Approx(60.0 / 128.0).margin(0.06));
        }
    }
}

TEST_CASE("the forward filter is deterministic and resettable", "[tracking][forward]") {
    // No seed, no sampling: two filters over the same activations agree to the bit, and a
    // reset one is a fresh one.
    const std::vector<std::pair<float, float>> activations = upsampled("good-times", 2);
    ForwardFilter a;
    ForwardFilter b;
    for (const auto& [beat, down] : activations) {
        const TrackedFrame first = a.process(beat, down);
        const TrackedFrame second = b.process(beat, down);
        REQUIRE(first.gathering == second.gathering);
        REQUIRE(first.emitted == second.emitted);
        REQUIRE(first.bpm == second.bpm);
    }
    a.reset();
    ForwardFilter fresh;
    for (const auto& [beat, down] : activations) {
        const TrackedFrame again = a.process(beat, down);
        const TrackedFrame first = fresh.process(beat, down);
        REQUIRE(again.gathering == first.gathering);
        REQUIRE(again.emitted == first.emitted);
    }
}

TEST_CASE("a held tempo is tracked in phase only", "[tracking][forward]") {
    // TRACKING-PROPOSAL.md §3.4's tempo hold: the operator pins a tempo and the filter
    // keeps it, whatever the network says. The drum machine plays 128; held at 100 the
    // filter reports 100 — 60 frames at 100 fps — for as long as it is held.
    const std::vector<std::pair<float, float>> activations = upsampled("synthetic", 2);
    ForwardFilter filter;
    CHECK(filter.canHoldTempo());
    CHECK(filter.heldBpm() == 0.0);
    filter.holdTempo(100.0);
    CHECK(filter.heldBpm() == Approx(100.0));
    TrackedFrame last;
    for (std::size_t f = 0; f < activations.size(); ++f) {
        last = filter.process(activations[f].first, activations[f].second);
        if (f >= 200) {
            INFO("frame " << f);
            REQUIRE(last.intervalFrames >= 59);
            REQUIRE(last.intervalFrames <= 61);
        }
    }
    CHECK(last.bpm == Approx(100.0).margin(2.0));

    SECTION("released, the tempo is the network's to argue for again") {
        filter.holdTempo(0.0);
        CHECK(filter.heldBpm() == 0.0);
        // A hold that agrees with the music costs nothing: held at what it plays, the
        // filter calls the same beats an unheld one does and ends on the same tempo.
        ForwardFilter agreeing;
        agreeing.holdTempo(128.0);
        ForwardFilter plain;
        std::size_t heldBeats = 0;
        std::size_t looseBeats = 0;
        TrackedFrame held;
        TrackedFrame loose;
        for (const auto& [beat, down] : activations) {
            held = agreeing.process(beat, down);
            loose = plain.process(beat, down);
            heldBeats += held.emitted != TrackedFrame::Emitted::None ? 1 : 0;
            looseBeats += loose.emitted != TrackedFrame::Emitted::None ? 1 : 0;
        }
        CHECK(heldBeats >= looseBeats - 2);
        CHECK(heldBeats <= looseBeats + 2);
        CHECK(held.intervalFrames == loose.intervalFrames);
    }
}

TEST_CASE("the operator's window is evidence inside the filter", "[tracking][forward]") {
    // §2.6: the window as a per-frame weight on every tempo outside it. The drum machine's
    // quarter notes at 128 are clear enough that the prototype's gentle 0.97 loses the
    // argument to them — that is the window as a preference, and it is right to lose. A
    // heavier weight wins it, which is what shows the plumbing rather than the tuning:
    // under a 50-100 window the filter settles on 64, and switched off it goes back.
    const std::vector<std::pair<float, float>> activations = upsampled("synthetic", 2);
    ForwardFilter::Options options;
    options.windowPenalty = 0.5;
    ForwardFilter filter(options);
    CHECK(filter.honoursTempoWindow());
    filter.setTempoWindow(50.0, 100.0, true);
    TrackedFrame last;
    for (const auto& [beat, down] : activations) {
        last = filter.process(beat, down);
    }
    INFO("under a 50-100 window the filter settled on " << last.bpm << " BPM");
    CHECK(last.bpm >= 50.0);
    CHECK(last.bpm <= 100.0);

    ForwardFilter off(options);
    off.setTempoWindow(50.0, 100.0, false);
    for (const auto& [beat, down] : activations) {
        last = off.process(beat, down);
    }
    CHECK(last.bpm == Approx(128.0).margin(3.0));
}

TEST_CASE("a frame of the forward filter touches no heap", "[tracking][forward][rt]") {
    const std::vector<std::pair<float, float>> activations = upsampled("vic-acid", 2);
    ForwardFilter filter;
    for (std::size_t f = 0; f < 10; ++f) {
        (void)filter.process(activations[f].first, activations[f].second);
    }
    const bool abortWas = takt4::rt::abortOnViolation();
    takt4::rt::setAbortOnViolation(false);
    const std::uint64_t before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope scope;
        for (std::size_t f = 10; f < activations.size(); ++f) {
            (void)filter.process(activations[f].first, activations[f].second);
        }
        filter.setTempoWindow(70.0, 140.0, true);
        filter.holdTempo(120.0);
        filter.holdTempo(0.0);
        filter.reset();
    }
    const std::uint64_t after = takt4::rt::violationCount();
    takt4::rt::setAbortOnViolation(abortWas);
    CHECK(takt4::rt::allocationGuardEnabled());
    CHECK(after == before);
}

TEST_CASE("nonsensical forward filter options are refused", "[tracking][forward]") {
    ForwardFilter::Options bad;
    bad.fps = 0;
    CHECK_THROWS_AS(ForwardFilter(bad), std::invalid_argument);
    bad = {};
    bad.minBpm = 200.0;
    bad.maxBpm = 100.0;
    CHECK_THROWS_AS(ForwardFilter(bad), std::invalid_argument);
    bad = {};
    bad.meters = {0, 0, 0, 0};
    CHECK_THROWS_AS(ForwardFilter(bad), std::invalid_argument);
    bad = {};
    bad.observationLambda = 1;
    CHECK_THROWS_AS(ForwardFilter(bad), std::invalid_argument);
    bad = {};
    bad.windowPenalty = 0.0;
    CHECK_THROWS_AS(ForwardFilter(bad), std::invalid_argument);
    bad = {};
    bad.meterChangeProbability = 1.0;
    CHECK_THROWS_AS(ForwardFilter(bad), std::invalid_argument);
}
