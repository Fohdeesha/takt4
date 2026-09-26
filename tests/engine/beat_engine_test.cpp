#include "core/audio/host_time.hpp"
#include "core/audio/rates.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/control.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/tracking/particle_filter.hpp"
#include "core/tracking/state_space.hpp"
#include "core/tracking/tap_tempo.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using Catch::Approx;
using takt4::audio::kHopSize;
using takt4::engine::BeatEngine;
using takt4::engine::Command;
using takt4::engine::EngineBeat;
using takt4::engine::EngineFrame;
using takt4::model::ModelWeights;
using takt4::tracking::StateSpaceModel;

namespace {

const std::filesystem::path kTestData{TAKT4_TEST_DATA_DIR};
const std::filesystem::path kWeightsDir{TAKT4_WEIGHTS_DIR};
const std::filesystem::path kBlob = std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin";

const StateSpaceModel& stateSpace() {
    static const StateSpaceModel loaded = StateSpaceModel::fromFile(kBlob);
    return loaded;
}

const ModelWeights& weights() {
    static const ModelWeights loaded = ModelWeights::fromFile(kWeightsDir / "generic.bin");
    return loaded;
}

std::vector<float> excerpt(const char* name) {
    const takt4::io::WavData audio = takt4::io::readWavFile(kTestData / "features" / name);
    REQUIRE(audio.channels == 1);
    REQUIRE(audio.samples.size() % kHopSize == 0);
    return audio.samples;
}

std::unique_ptr<BeatEngine> makeEngine(BeatEngine::Options options = {}) {
    return std::make_unique<BeatEngine>(weights(), stateSpace(), options);
}

/// An engine on the particle filter, for the tests that are about it: the Phase 4 parity
/// with `tools/pf_reference.py`, its meter, its frame count. The default engine is the
/// forward filter since 2026-09-08 (`tracking::Decoder`), and the rest run on that.
std::unique_ptr<BeatEngine> makeParticleEngine(BeatEngine::Options options = {}) {
    options.decoder = takt4::tracking::Decoder::ParticleFilter;
    return makeEngine(options);
}

} // namespace

TEST_CASE("the meter is steady enough to read", "[engine][meter]") {
    // The user's report, 2026-09-05: *"the time signature detected constantly changes and
    // jumps almost every single beat/bar on any song I test, so it's impossible to set the
    // downbeat consistently"*. Measured over these eighteen real-music excerpts it was
    // **166 changes**, up to 1.8 a second — the meter was the bar length of whichever
    // single downbeat particle happened to be commonest that frame, so a cloud split
    // between three and four to the bar crossed over whenever one particle moved.
    //
    // `ParticleFilter::meterOf` now reads the cloud's mass and remembers it. This holds
    // that to a number, because "steadier" is not a claim a comment can keep: without it
    // the next change to the downbeat stage could quietly put the flicker back and every
    // other test would still pass.
    static constexpr const char* kExcerpts[] = {"big-brown-beaver.wav",
                                                "complicated-geometry.wav",
                                                "good-times.wav",
                                                "hell-of-a-ride.wav",
                                                "in-yer-face.wav",
                                                "jack-yourself.wav",
                                                "keman-rhythm.wav",
                                                "model-2029.wav",
                                                "outside-plume.wav",
                                                "pirates.wav",
                                                "rale.wav",
                                                "synthetic.wav",
                                                "the-galaxist.wav",
                                                "trigger-finger.wav",
                                                "true-believer.wav",
                                                "vic-acid.wav",
                                                "winter-now.wav",
                                                "your-sweet-boom.wav"};

    std::size_t total = 0;
    for (const char* name : kExcerpts) {
        auto engine = makeParticleEngine();
        const std::vector<float> samples = excerpt(name);
        const std::size_t hops = samples.size() / kHopSize;
        std::size_t frames = 0;
        std::size_t changes = 0;
        std::uint32_t last = 0;
        EngineFrame frame;
        for (std::size_t hop = 0; hop < hops; ++hop) {
            engine->processHop(samples.data() + hop * kHopSize, hop);
            (void)engine->step();
            while (engine->popFrame(frame)) {
                ++frames;
                if (frames > 1 && frame.state.beatsPerBar != last) {
                    ++changes;
                }
                last = frame.state.beatsPerBar;
            }
        }
        INFO(name << ": " << changes << " changes over " << frames << " frames, settled on "
                  << last);
        // No single excerpt may go back to flickering, however good the total looks.
        CHECK(changes <= 12);
        total += changes;
    }

    // 47 as measured; the ceiling leaves room for a platform's floating-point to land a
    // tie differently without leaving room for the old behaviour, which was 166.
    INFO("total meter changes across " << std::size(kExcerpts) << " excerpts: " << total);
    CHECK(total <= 70);
}

TEST_CASE("a manual downbeat moves the bar under the real filter", "[engine]") {
    // §5.5's non-negotiable control, through the whole chain rather than through synthetic
    // frames: real activations, the real particle filter, the real meter it reports.
    //
    // tests/tracking covers the bar arithmetic by handing `TempoTracker` frames that emit a
    // beat every time at a fixed 4/4. That is the right way to test the arithmetic and the
    // wrong way to believe the feature works: the real filter emits on perhaps one frame in
    // twenty-three, calls its own downbeats, and changes its mind about the meter early on.
    // This is what an operator's press actually does.
    auto engine = makeEngine();
    const std::vector<float> samples = excerpt("synthetic.wav");
    const std::size_t hops = samples.size() / kHopSize;

    std::vector<std::uint32_t> positions; // beat-in-bar, one per beat called
    std::size_t snapAt = 0;               // how many beats had been called when it was posted
    std::uint32_t pressedOn = 0;          // where in the bar the beat under the press was
    std::uint32_t carriedOn = 0;          // where the count would have gone without it
    bool posted = false;
    bool sawSnapped = false;
    EngineBeat beat;

    for (std::size_t hop = 0; hop < hops; ++hop) {
        engine->processHop(samples.data() + hop * kHopSize, hop);
        (void)engine->step();
        while (engine->popBeat(beat)) {
            positions.push_back(beat.event.beatInBar);
            if (beat.event.snapped) {
                sawSnapped = true;
                // Exactly one beat carries it, and it is the first after the post — which
                // is the beat *after* the one the press named, so it is the bar's second.
                // The beat that was named went out before the press and cannot be marked.
                CHECK(positions.size() == snapAt + 1);
                CHECK(beat.event.beatInBar == 2);
                CHECK_FALSE(beat.event.downbeat);
            }
            // Press once the bar is genuinely running, and on a beat that was not already
            // its first — a press there agrees with the filter and moves nothing, which
            // would make the checks below prove nothing.
            const bool settled = beat.event.beatsPerBar >= 2 && beat.event.beatInBar >= 1;
            if (!posted && settled && positions.size() >= 8 && beat.event.beatInBar != 1) {
                snapAt = positions.size();
                pressedOn = beat.event.beatInBar;
                carriedOn = pressedOn % beat.event.beatsPerBar + 1;
                REQUIRE(engine->post(Command::snapDownbeat()));
                posted = true;

                // Drain the command with no new audio behind it, so what the tracker says
                // next is what the press alone did. The bar moves *here*, on the press, and
                // not a beat later: an operator pressing the button on the downbeat they
                // can hear is naming the beat the tracker has just called.
                const std::uint64_t before = engine->state().beats;
                (void)engine->step();
                CHECK(engine->state().beats == before);
                CHECK(engine->state().beatInBar == 1);
            }
        }
    }

    REQUIRE(posted);
    REQUIRE(sawSnapped);
    REQUIRE(positions.size() > snapAt + 1);

    // Had the press done nothing, the count would have carried on from the beat under it.
    // It did not: the bar restarted on that beat, so the next one is the bar's second.
    CHECK(carriedOn != 2u);
    CHECK(positions[snapAt] == 2);
    CHECK(engine->commandsDropped() == 0);
}

TEST_CASE("the engine turns hops into tracked frames and beats", "[engine]") {
    // On the particle filter, whose frame is the network's: one tracked frame per
    // activation. The forward filter's two per activation are the test of its own below.
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeParticleEngine();

    // Stepped by hand, without either thread: the same code path, deterministic.
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
    }

    // A frame needs the hop after it, so N hops give N - 1 of them.
    CHECK(engine->framesTracked() == hops - 1);
    CHECK(engine->framesDropped() == 0);
    CHECK(engine->beatsDropped() == 0);

    EngineFrame frame;
    std::uint64_t expected = 0;
    std::size_t beatFrames = 0;
    double loudest = 0.0;
    while (engine->popFrame(frame)) {
        CHECK(frame.activation.frameIndex == expected);
        CHECK(frame.tracked.frameIndex == expected);
        CHECK(frame.tracked.bpm >= 54.0);
        CHECK(frame.tracked.bpm <= 215.0);
        CHECK(frame.state.beatsPerBar >= 2);
        CHECK(frame.state.beatsPerBar <= 4);
        if (frame.beat) {
            ++beatFrames;
        }
        loudest = std::max(loudest, static_cast<double>(frame.activation.beat));
        ++expected;
    }
    CHECK(expected == hops - 1);
    CHECK(loudest > 0.5); // a drum machine at 128 BPM

    std::vector<EngineBeat> beats;
    EngineBeat beat;
    while (engine->popBeat(beat)) {
        beats.push_back(beat);
    }
    // One beat on the beat ring for every frame that said it called one.
    CHECK(beats.size() == beatFrames);
    CHECK(beats.size() == engine->beatsCalled());
    CHECK(beats.size() > 15);

    // The state left behind is the last frame's, and it agrees with what was drained.
    CHECK(engine->state().locked);
    CHECK(engine->state().bpm == Approx(128.0).margin(4.0));
}

TEST_CASE("the engine gives what composing the parts by hand gives", "[engine]") {
    // The facade must not change the answer. Anything else and the exact parity gate in
    // tests/tracking/ would be testing a filter the application no longer runs.
    const std::vector<float> signal = excerpt("rale.wav");
    const std::size_t hops = signal.size() / kHopSize;

    const std::unique_ptr<BeatEngine> engine = makeParticleEngine();
    std::vector<EngineFrame> viaEngine;
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
        EngineFrame frame;
        while (engine->popFrame(frame)) {
            viaEngine.push_back(frame);
        }
    }
    REQUIRE(viaEngine.size() == hops - 1);

    // The same activations through a filter and a tracker built the same way.
    takt4::tracking::ParticleFilter filter(stateSpace());
    takt4::tracking::TempoTracker tempo(stateSpace().secondsPerFrame());
    for (const EngineFrame& frame : viaEngine) {
        const takt4::tracking::TrackedFrame tracked =
            filter.process(frame.activation.beat, frame.activation.downbeat);
        const std::optional<takt4::tracking::BeatEvent> event = tempo.process(tracked);
        INFO("frame " << frame.activation.frameIndex);
        REQUIRE(tracked.gathering == frame.tracked.gathering);
        REQUIRE(tracked.downMax == frame.tracked.downMax);
        REQUIRE(tracked.emitted == frame.tracked.emitted);
        REQUIRE(tracked.intervalFrames == frame.tracked.intervalFrames);
        REQUIRE(event.has_value() == frame.beat);
        REQUIRE(tempo.state().bpm == frame.state.bpm);
        REQUIRE(tempo.state().locked == frame.state.locked);
    }
}

TEST_CASE("the threads give what stepping by hand gives", "[engine]") {
    const std::vector<float> signal = excerpt("good-times.wav");
    const std::size_t hops = signal.size() / kHopSize;

    const std::unique_ptr<BeatEngine> stepped = makeEngine();
    std::vector<EngineFrame> byHand;
    for (std::size_t h = 0; h < hops; ++h) {
        stepped->processHop(signal.data() + h * kHopSize, h);
        (void)stepped->step();
        EngineFrame frame;
        while (stepped->popFrame(frame)) {
            byHand.push_back(frame);
        }
    }
    // N hops give N - 1 activations; the first is one decoder frame and every later one
    // `stepsPerActivation()` of them — one for the particle filter, two for the forward
    // filter, which is the default this runs on.
    REQUIRE(byHand.size() == 1 + (hops - 2) * stepped->stepsPerActivation());

    const std::unique_ptr<BeatEngine> threaded = makeEngine();
    threaded->start();
    CHECK(threaded->running());
    std::vector<EngineFrame> fromThreads;
    std::vector<EngineBeat> beats;
    const auto drain = [&] {
        EngineFrame frame;
        while (threaded->popFrame(frame)) {
            fromThreads.push_back(frame);
        }
        EngineBeat beat;
        while (threaded->popBeat(beat)) {
            beats.push_back(beat);
        }
    };
    for (std::size_t h = 0; h < hops; ++h) {
        threaded->processHop(signal.data() + h * kHopSize, h);
        // Faster than real time on purpose, but never far enough ahead to overflow the
        // model worker's 64-hop queue.
        while (threaded->activations().hopsPending() >
               takt4::model::ActivationEngine::kHopQueueCapacity / 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        drain();
    }
    // The throttle above watches the model worker's queue, not the inference thread's: a
    // decoder slower than the network can still be a few hundred activations behind when
    // the last hop has been handed over, and stop()'s final sweep would then put every
    // frame it had left onto the ring at once, with nobody draining. Keep draining until
    // the inference thread has caught up, as a live consumer on a timer does.
    for (int waited = 0; waited < 5000 && fromThreads.size() < byHand.size(); ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        drain();
    }
    threaded->stop(); // drains what is left before joining
    {
        EngineFrame frame;
        while (threaded->popFrame(frame)) {
            fromThreads.push_back(frame);
        }
        EngineBeat beat;
        while (threaded->popBeat(beat)) {
            beats.push_back(beat);
        }
    }
    CHECK_FALSE(threaded->running());
    CHECK(threaded->framesDropped() == 0);
    CHECK(threaded->beatsDropped() == 0);

    REQUIRE(fromThreads.size() == byHand.size());
    std::size_t beatFrames = 0;
    for (std::size_t f = 0; f < byHand.size(); ++f) {
        INFO("frame " << f);
        CHECK(fromThreads[f].activation.frameIndex == byHand[f].activation.frameIndex);
        CHECK(fromThreads[f].activation.beat == byHand[f].activation.beat);
        CHECK(fromThreads[f].tracked.gathering == byHand[f].tracked.gathering);
        CHECK(fromThreads[f].tracked.emitted == byHand[f].tracked.emitted);
        CHECK(fromThreads[f].state.bpm == byHand[f].state.bpm);
        CHECK(fromThreads[f].state.locked == byHand[f].state.locked);
        CHECK(fromThreads[f].beat == byHand[f].beat);
        if (byHand[f].beat) {
            ++beatFrames;
        }
    }
    CHECK(beats.size() == beatFrames);

    // §4.2's reason for the thread: one frame is 20 ms of audio and the filter has to be
    // well inside that. The mean is the claim, as in the model worker's own test — a
    // shared runner will preempt one frame no matter how fast the code is.
    INFO("mean frame " << threaded->meanFrameMicros() << " us, worst "
                       << threaded->worstFrameMicros() << " us, of 20000 us of audio");
    CHECK(threaded->meanFrameMicros() > 0.0);
#ifdef NDEBUG
    CHECK(threaded->meanFrameMicros() < 20000.0);
#endif
}

TEST_CASE("start() clears everything and reseeds the filter", "[engine]") {
    const std::vector<float> signal = excerpt("vic-acid.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeParticleEngine();

    const auto runOnce = [&] {
        std::vector<std::uint32_t> gatherings;
        for (std::size_t h = 0; h < hops; ++h) {
            engine->processHop(signal.data() + h * kHopSize, h);
            (void)engine->step();
            EngineFrame frame;
            while (engine->popFrame(frame)) {
                gatherings.push_back(frame.tracked.gathering);
            }
        }
        return gatherings;
    };

    const std::vector<std::uint32_t> first = runOnce();
    REQUIRE(first.size() == hops - 1);
    const auto beatsFirst = engine->state().beats;
    REQUIRE(beatsFirst > 0);

    // Without a reset the second run would start with the filter and the model where the
    // first left them, and a stream stopped and started would not be the same tracker.
    engine->start();
    engine->stop();
    const std::vector<std::uint32_t> second = runOnce();
    CHECK(second == first);
    // And the state a reader sees is the second run's, counted from nothing again: a count
    // carried over from the first run would read twice as many beats.
    CHECK(engine->state().beats == beatsFirst);
}

TEST_CASE("a consumer that never drains loses frames instead of blocking", "[engine]") {
    // On the particle filter, whose 499 frames over this excerpt fit the ring once.
    const std::vector<float> signal = excerpt("winter-now.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeParticleEngine();

    // 499 frames into a 512-slot ring fits; nothing is dropped by accident.
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
    }
    CHECK(engine->framesDropped() == 0);

    // Round again without draining and the ring fills. The engine must keep tracking —
    // §7.5's rule is that a slow consumer loses data, never that it stalls the producer.
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, hops + h);
        (void)engine->step();
    }
    CHECK(engine->framesDropped() > 0);
    CHECK(engine->framesTracked() == BeatEngine::kFrameQueueCapacity);
    // ...and the published state is still current, because it is not on the ring.
    CHECK(engine->state().beats > 0);
}

TEST_CASE("a command posted while stopped applies to the run that follows", "[engine]") {
    // A settings screen is used before the stream is started, and what it set has to
    // survive start()'s reset. The reset clears tracking state, never options.
    const std::unique_ptr<BeatEngine> engine = makeEngine();
    REQUIRE(engine->tempo().options().minBpm == Approx(70.0));

    takt4::tracking::TempoTracker::Options options = engine->tempo().options();
    options.minBpm = 100.0;
    options.maxBpm = 200.0;
    options.latencyOffsetSeconds = -0.03;
    CHECK(engine->post(Command::setTempoOptions(options)));
    // Not before something drains the queue, though.
    CHECK(engine->tempo().options().minBpm == Approx(70.0));

    engine->start();
    engine->stop();
    CHECK(engine->tempo().options().minBpm == Approx(100.0));
    CHECK(engine->tempo().options().maxBpm == Approx(200.0));
    CHECK(engine->tempo().options().latencyOffsetSeconds == Approx(-0.03));
    CHECK(engine->tempo().fold(400.0) == Approx(100.0));
    CHECK(engine->commandsDropped() == 0);
}

// Test names stay ASCII: ctest passes each one back to the binary as a command-line
// argument, and a non-ASCII name comes through the console codepage mangled, so nothing
// matches and the test fails without ever running. "÷2" cost exactly that.
TEST_CASE("halving and doubling reach a tracking engine without dropping the lock", "[engine]") {
    // §5.5's octave buttons. The point of the queue is that this costs nothing: the
    // filter is not reseeded, the lock is not given up, and no audio is missed.
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeEngine();

    std::size_t h = 0;
    for (; h < hops && !engine->state().locked; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
    }
    REQUIRE(engine->state().locked);
    REQUIRE(h < hops);
    const std::uint64_t beatsBefore = engine->state().beats;
    const double shown = engine->state().bpm;
    CHECK(shown == Approx(128.0).margin(4.0));

    // Nothing is pending, so this step tracks no frame at all — it only applies the
    // command, which is how a button pressed during a silence still does something.
    //
    // **Half of what was showing**, not half of the filter's continuous estimate, which is
    // what this asserted until the audit of 2026-09-25 (H5): 61.2 on this excerpt against 64.15.
    REQUIRE(engine->post(Command::halve()));
    CHECK(engine->step() == 0);
    CHECK(engine->state().bpm == Approx(shown / 2.0).epsilon(1e-12));
    CHECK(engine->state().locked);
    CHECK(engine->state().beats == beatsBefore);

    // And it stays there: the shift outlives the frames that follow it, so the operator
    // does not have to hold the button down.
    for (; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
    }
    CHECK(engine->state().bpm == Approx(64.0).margin(3.0));
    CHECK(engine->state().locked);

    // ×2 undoes it, back to the octave the tracker chose for itself.
    REQUIRE(engine->post(Command::redouble()));
    CHECK(engine->step() == 0);
    CHECK(engine->state().bpm == Approx(128.0).margin(4.0));
    CHECK(engine->state().locked);
    CHECK(engine->commandsDropped() == 0);
}

TEST_CASE("the settings a caller reads back follow the ones a tap moved", "[engine]") {
    // A caller that kept the options it last posted, rather than reading these, would
    // hold a window from before the tap — and the next settings change it sent would
    // quietly put the tracker back on the octave the operator had just corrected.
    const std::unique_ptr<BeatEngine> engine = makeEngine();
    REQUIRE(engine->tempoOptions().minBpm == Approx(70.0));
    REQUIRE(engine->tempoOptions().maxBpm == Approx(140.0));

    REQUIRE(engine->post(Command::seedTempo(200.0)));
    CHECK(engine->step() == 0);
    const takt4::tracking::TempoTracker::Options seeded = engine->tempoOptions();
    CHECK(seeded.minBpm > 140.0);
    CHECK(seeded.maxBpm > seeded.minBpm);
    CHECK(seeded.minBpm < 200.0);
    CHECK(seeded.maxBpm > 200.0);
    CHECK(seeded.octaveFold);

    // Editing what was read back and posting it changes only what was edited.
    takt4::tracking::TempoTracker::Options edited = seeded;
    edited.latencyOffsetSeconds = -0.02;
    REQUIRE(engine->post(Command::setTempoOptions(edited)));
    CHECK(engine->step() == 0);
    CHECK(engine->tempoOptions().latencyOffsetSeconds == Approx(-0.02));
    CHECK(engine->tempoOptions().minBpm == Approx(seeded.minBpm));
    CHECK(engine->tempoOptions().maxBpm == Approx(seeded.maxBpm));
}

TEST_CASE("tapping a tempo out on a clock moves what the engine publishes", "[engine]") {
    // The whole of what the console's space key does, minus the keypress: taps counted on
    // the thread they arrive on, the tempo they work out posted as a command, the tracker
    // moved onto the operator's octave. Nothing about a wall clock reaches the engine.
    //
    // On the particle filter, whose tap is `TempoTracker`'s fold: the window moves and the
    // number is relabelled into it at once. The forward filter's tap is a prior inside the
    // decoder and is the test after next.
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeParticleEngine();

    std::size_t h = 0;
    for (; h < hops && !engine->state().locked; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
    }
    REQUIRE(engine->state().locked);
    REQUIRE(engine->state().bpm == Approx(128.0).margin(4.0));

    // Four taps at half time, as an operator hearing 64 would give them — imperfectly.
    takt4::tracking::TapTempo taps;
    const double period = 60.0 / 64.0;
    const double wobble[] = {0.0, 0.011, -0.008, 0.006};
    std::optional<double> tapped;
    for (int n = 0; n < 4; ++n) {
        tapped = taps.tap(100.0 + static_cast<double>(n) * period + wobble[n]);
    }
    REQUIRE(tapped.has_value());
    CHECK(*tapped == Approx(64.0).margin(1.0));

    REQUIRE(engine->post(Command::seedTempo(*tapped)));
    CHECK(engine->step() == 0);
    CHECK(engine->state().bpm == Approx(64.0).margin(2.0));

    for (; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
    }
    CHECK(engine->state().bpm == Approx(64.0).margin(2.0));
    CHECK(engine->state().locked);
}

TEST_CASE("a tapped tempo reaches the tracker and moves the window", "[engine]") {
    // The other end of tracking::TapTempo: taps are counted wherever they arrive and only
    // the tempo travels, so nothing about a wall clock reaches the inference thread. On the
    // particle filter, as the test above.
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeParticleEngine();

    std::size_t h = 0;
    for (; h < hops && !engine->state().locked; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
    }
    REQUIRE(engine->state().locked);
    REQUIRE(engine->state().bpm == Approx(128.0).margin(4.0));

    // An operator who hears the half-time and taps it. The window follows the tap, so
    // the readout and the setting agree on why the tempo is what it is.
    REQUIRE(engine->post(Command::seedTempo(64.0)));
    CHECK(engine->step() == 0);
    CHECK(engine->state().bpm == Approx(64.0).margin(2.0));
    CHECK(engine->tempo().options().minBpm < 64.0);
    CHECK(engine->tempo().options().maxBpm > 64.0);

    for (; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
    }
    CHECK(engine->state().bpm == Approx(64.0).margin(2.0));
    CHECK(engine->state().locked);
    CHECK(engine->commandsDropped() == 0);
}

TEST_CASE("under the forward filter a tap is a prior, and the number stays with the beats",
          "[engine][forward]") {
    // TRACKING-PROPOSAL.md §2.6: with the window inside the decoder, a tap moves the window
    // and the decoder weighs it as evidence — an octave outside it has to keep out-arguing
    // a per-frame penalty. On a track whose octave is ambiguous that decides it; on a drum
    // machine playing plain quarter notes at 128 the evidence for 128 outweighs a window
    // asking for 64, and the readout *stays* at 128, with the beats. What is never
    // published is the particle filter's old state of a number relabelled to 64 over beats
    // still firing at 128. The operator who wants half-time beats presses ÷2, which is an
    // instruction and divides the grid; the operator who wants a tempo held pins the lock.
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeEngine();
    REQUIRE(engine->decoderKind() == takt4::tracking::Decoder::Forward);

    std::size_t h = 0;
    for (; h < hops && !engine->state().locked; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
    }
    REQUIRE(engine->state().locked);
    REQUIRE(engine->state().bpm == Approx(128.0).margin(4.0));

    REQUIRE(engine->post(Command::seedTempo(64.0)));
    CHECK(engine->step() == 0);
    // The window moved, and it is the decoder's now.
    CHECK(engine->tempoOptions().minBpm < 64.0);
    CHECK(engine->tempoOptions().maxBpm > 64.0);
    CHECK(engine->tempoOptions().octaveFold);
    CHECK(engine->tempo().options().foldInDecoder);

    std::vector<EngineBeat> beats;
    EngineBeat beat;
    for (; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
        while (engine->popBeat(beat)) {
            beats.push_back(beat);
        }
    }
    // The evidence won: 128, and the beats a quarter note apart under it.
    CHECK(engine->state().bpm == Approx(128.0).margin(4.0));
    REQUIRE(beats.size() > 8);
    for (std::size_t i = 1; i < beats.size(); ++i) {
        INFO("beat " << i);
        CHECK(beats[i].event.time - beats[i - 1].event.time == Approx(60.0 / 128.0).margin(0.08));
    }

    SECTION("the instruction that does halve the grid is the octave shift") {
        REQUIRE(engine->post(Command::halve()));
        CHECK(engine->step() == 0);
        CHECK(engine->state().bpm == Approx(64.0).margin(3.0));
        CHECK(engine->state().beatDivisor == 2);
    }
}

TEST_CASE("a manual downbeat reaches the tracker and moves the bar", "[engine]") {
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeEngine();

    // Far enough in that the filter has settled on a bar of its own to disagree with, and
    // stopped on a beat that is not already the bar's first: pressing the button there
    // agrees with the filter and moves nothing.
    std::size_t h = 0;
    EngineBeat beat;
    bool ready = false;
    for (; h < hops && !ready; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
        while (engine->popBeat(beat)) {
            ready = engine->state().bars >= 2 && beat.event.beatInBar > 1;
        }
    }
    REQUIRE(ready);
    while (engine->popBeat(beat)) {
    }

    REQUIRE(engine->post(Command::snapDownbeat()));
    // Drained with no new audio behind it: the bar moves on the press, naming the beat the
    // tracker has just called rather than waiting for the next one.
    const std::uint64_t beatsBefore = engine->state().beats;
    (void)engine->step();
    CHECK(engine->state().beats == beatsBefore);
    CHECK(engine->state().beatInBar == 1);

    std::optional<EngineBeat> snapped;
    for (; h < hops && !snapped; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
        while (engine->popBeat(beat)) {
            if (!snapped) {
                snapped = beat;
            }
        }
    }
    REQUIRE(snapped.has_value());

    // The very next beat carries the new phase out to the transports. It is the bar's
    // second, because its first was the beat under the operator's finger.
    CHECK(snapped->event.snapped);
    CHECK_FALSE(snapped->event.downbeat);
    CHECK(snapped->event.beatInBar == 2);
    CHECK(snapped->state.beatInBar == 2);

    // The bar then runs from there, and the filter's own downbeats do not take it back.
    std::vector<EngineBeat> after;
    for (; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
        while (engine->popBeat(beat)) {
            after.push_back(beat);
        }
    }
    REQUIRE(after.size() > 4);
    std::uint32_t expected = snapped->event.beatInBar;
    for (const EngineBeat& next : after) {
        expected = expected % next.event.beatsPerBar + 1;
        INFO("beat at frame " << next.event.frameIndex);
        CHECK(next.event.beatInBar == expected);
        CHECK_FALSE(next.event.snapped);
    }
    CHECK(engine->commandsDropped() == 0);
}

TEST_CASE("a command crosses from another thread into the inference thread", "[engine]") {
    // The same ÷2, posted from the thread feeding audio while the engine's own threads
    // are running — which is what a UI button, an inbound OSC message (§5.7) or a
    // keyboard shortcut actually is.
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeEngine();
    engine->start();

    bool posted = false;
    const auto drain = [&] {
        EngineFrame frame;
        while (engine->popFrame(frame)) {
        }
        EngineBeat beat;
        while (engine->popBeat(beat)) {
        }
    };
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        while (engine->activations().hopsPending() >
               takt4::model::ActivationEngine::kHopQueueCapacity / 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        drain();
        if (!posted && engine->state().locked) {
            REQUIRE(engine->post(Command::halve()));
            posted = true;
        }
    }
    // The feed above is throttled on the model worker's queue and not on the inference
    // thread's, so a decoder slower than the network can still be tracking the excerpt when
    // the last hop has been handed over. Give it the time it is owed.
    for (int waited = 0; !posted && waited < 2000; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        drain();
        if (engine->state().locked) {
            REQUIRE(engine->post(Command::halve()));
            posted = true;
        }
    }
    // And for the command itself to be applied before the threads are joined.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    engine->stop();

    REQUIRE(posted);
    CHECK(engine->state().bpm == Approx(64.0).margin(4.0));
    CHECK(engine->state().locked);
    CHECK(engine->commandsDropped() == 0);
}

TEST_CASE("the forward decoder runs at twice the network's rate and tracks the excerpt",
          "[engine][forward]") {
    // TRACKING-PROPOSAL.md §2.4 and §2.5 through the whole engine: the forward filter at
    // 100 fps, fed the network's activations with the frames between them interpolated,
    // and the tempo state machine at that rate. Everything a consumer sees is in seconds
    // or flagged, so nothing above the engine has to know.
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    BeatEngine::Options options;
    options.decoder = takt4::tracking::Decoder::Forward;
    const std::unique_ptr<BeatEngine> engine = makeEngine(options);
    CHECK(engine->decoderKind() == takt4::tracking::Decoder::Forward);
    CHECK(makeEngine()->decoderKind() == takt4::tracking::Decoder::Forward); // the default
    CHECK(engine->secondsPerFrame() == Approx(0.01));
    CHECK(engine->stepsPerActivation() == 2);
    // The window is the decoder's now, not the tracker's.
    CHECK(engine->tempo().options().foldInDecoder);
    CHECK_FALSE(makeParticleEngine()->tempo().options().foldInDecoder);
    CHECK(makeParticleEngine()->secondsPerFrame() == Approx(0.02));

    // Drained as it goes: twice the frames of the particle filter is more than the ring
    // holds over this excerpt, and a frame left on it is a frame counted as dropped.
    EngineFrame frame;
    std::uint64_t expected = 0;
    std::size_t beatFrames = 0;
    std::size_t interpolated = 0;
    std::vector<EngineBeat> beats;
    EngineBeat beat;
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
        while (engine->popFrame(frame)) {
            CHECK(frame.tracked.frameIndex == expected);
            // Frame 0 is the first activation; from there the odd frames are in between.
            CHECK(frame.interpolated == (expected % 2 == 1));
            interpolated += frame.interpolated ? 1 : 0;
            beatFrames += frame.beat ? 1 : 0;
            ++expected;
        }
        while (engine->popBeat(beat)) {
            beats.push_back(beat);
        }
    }
    // N hops give N - 1 activations; the first yields one frame and every later one two.
    const std::size_t activations = hops - 1;
    CHECK(engine->framesTracked() == 2 * activations - 1);
    CHECK(engine->framesDropped() == 0);
    CHECK(expected == 2 * activations - 1);
    CHECK(interpolated == activations - 1);
    CHECK(beats.size() == beatFrames);
    CHECK(beats.size() > 15);
    // A quarter note apart once locked; before that the posterior is still choosing.
    std::size_t lockedGaps = 0;
    for (std::size_t i = 1; i < beats.size(); ++i) {
        INFO("beat " << i);
        CHECK(beats[i].event.time > beats[i - 1].event.time);
        if (beats[i].state.locked && beats[i - 1].state.locked) {
            CHECK(beats[i].event.time - beats[i - 1].event.time ==
                  Approx(60.0 / 128.0).margin(0.06));
            ++lockedGaps;
        }
    }
    CHECK(lockedGaps > 10);
    CHECK(engine->state().locked);
    CHECK(engine->state().bpm == Approx(128.0).margin(2.0));

    SECTION("pinning the lock holds the decoder's tempo, and releasing it lets go") {
        const auto* forward = dynamic_cast<const takt4::tracking::ForwardFilter*>(&engine->decoder());
        REQUIRE(forward != nullptr);
        CHECK(forward->heldBpm() == 0.0);
        REQUIRE(engine->post(Command::setLockPinned(true)));
        CHECK(engine->step() == 0);
        CHECK(engine->state().pinned);
        // The published tempo in the decoder's own terms, which with no ÷2 or ×2 pressed is
        // the published tempo itself.
        CHECK(forward->heldBpm() == Approx(engine->tempo().decoderBpm()));
        CHECK(forward->heldBpm() == Approx(engine->state().bpm));
        REQUIRE(engine->post(Command::setLockPinned(false)));
        CHECK(engine->step() == 0);
        CHECK(forward->heldBpm() == 0.0);
        // And the direct command, which a control surface may send by name.
        REQUIRE(engine->post(Command::holdTempo(120.0)));
        CHECK(engine->step() == 0);
        CHECK(forward->heldBpm() == Approx(120.0));
    }

    SECTION("pinned while halved holds the filter at its real tempo, not half of it") {
        // The audit's H3: the hold was given the published tempo, so a pin under ÷2 held the
        // decoder at 64 against a 128 BPM track and the tracker was penalised off the music.
        const auto* forward = dynamic_cast<const takt4::tracking::ForwardFilter*>(&engine->decoder());
        REQUIRE(forward != nullptr);
        REQUIRE(engine->post(Command::halve()));
        (void)engine->step();
        REQUIRE(engine->state().bpm == Approx(64.0).margin(1.5));
        REQUIRE(engine->post(Command::setLockPinned(true)));
        (void)engine->step();
        CHECK(forward->heldBpm() == Approx(128.0).margin(2.0));
    }

    SECTION("a stop and a start let go of the hold with the pin") {
        // The other half of H3. The tracker's reset drops the pin; the decoder's reset keeps a
        // hold by design. So after a stop and a start the window showed no pin while the
        // decoder went on holding the old tempo, and a new track could not be acquired.
        const auto* forward = dynamic_cast<const takt4::tracking::ForwardFilter*>(&engine->decoder());
        REQUIRE(forward != nullptr);
        REQUIRE(engine->post(Command::setLockPinned(true)));
        (void)engine->step();
        REQUIRE(forward->heldBpm() > 0.0);
        engine->start();
        engine->stop();
        CHECK_FALSE(engine->state().pinned);
        CHECK(forward->heldBpm() == 0.0);
    }
}

TEST_CASE("a pin pressed before the lock holds nothing until the lock arrives",
          "[engine][forward]") {
    // Holding a hunting decoder to whatever it was trying at the moment of the press would be
    // pinning noise (the audit's H3). The pin is kept, and the hold is taken on the frame the
    // lock is earned — at the tempo that earned it.
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeEngine();
    const auto* forward = dynamic_cast<const takt4::tracking::ForwardFilter*>(&engine->decoder());
    REQUIRE(forward != nullptr);
    REQUIRE(engine->post(Command::setLockPinned(true)));
    (void)engine->step();
    CHECK(forward->heldBpm() == 0.0);

    EngineFrame frame;
    EngineBeat beat;
    bool heldAtLock = false;
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
        while (engine->popFrame(frame)) {
        }
        while (engine->popBeat(beat)) {
        }
        if (!heldAtLock && engine->state().locked) {
            heldAtLock = true;
            // The tempo the lock was earned at — not the cloud's raw reading at that moment,
            // which wanders several BPM either side of it. A hop is two frames, so the tempo
            // read here may have been refined by a fraction since.
            CHECK(forward->heldBpm() == Approx(engine->state().bpm).margin(1.0));
        }
        if (!engine->state().locked) {
            CHECK(forward->heldBpm() == 0.0);
        }
    }
    CHECK(heldAtLock);
    // The first lock on this track is a whole-frame interval, 130.9 — the hold keeps one
    // interval either side of it, which still has 128 in it, so the tracker stays on the music
    // under the pin rather than being dragged off it.
    CHECK(forward->heldBpm() == Approx(128.0).margin(4.0));
    CHECK(engine->state().bpm == Approx(128.0).margin(2.0));
    CHECK(engine->state().locked);
}

TEST_CASE("a window moved out from under a pinned lock lets the decoder's hold go",
          "[engine][forward]") {
    // The audit of 2026-09-25, M4. A fold window moved until it excludes the pinned tempo drops
    // the lock, as it should — and left the decoder held to the old tempo, so the tracker
    // re-locked onto it within half a second and the window move did nothing at all. The pin
    // stays the operator's; the hold goes, and is taken again when a lock is earned.
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeEngine();
    const auto* forward = dynamic_cast<const takt4::tracking::ForwardFilter*>(&engine->decoder());
    REQUIRE(forward != nullptr);
    std::size_t h = 0;
    const auto run = [&](std::size_t until, bool stopAtLock) {
        for (; h < until && !(stopAtLock && engine->state().locked); ++h) {
            engine->processHop(signal.data() + h * kHopSize, h);
            (void)engine->step();
        }
    };
    run(hops, true);
    REQUIRE(engine->state().locked);
    REQUIRE(engine->post(Command::setLockPinned(true)));
    (void)engine->step();
    REQUIRE(forward->heldBpm() == Approx(128.0).margin(4.0));

    // A window an octave up: 128 is outside it.
    takt4::tracking::TempoTracker::Options moved = engine->tempoOptions();
    moved.octaveFold = true;
    moved.minBpm = 150.0;
    moved.maxBpm = 300.0;
    REQUIRE(engine->post(Command::setTempoOptions(moved)));
    (void)engine->step();
    CHECK_FALSE(engine->state().locked);
    CHECK(engine->state().pinned); // the operator's pin is not the window's to take
    CHECK(forward->heldBpm() == 0.0);

    // And taken again, in the decoder's own terms, on the lock that comes back.
    run(hops, false);
    if (engine->state().locked) {
        CHECK(forward->heldBpm() > 0.0);
    }
}

TEST_CASE("the options reach the filter and the tempo machine", "[engine]") {
    BeatEngine::Options options;
    options.tempo.minBpm = 90.0;
    options.tempo.maxBpm = 180.0;
    options.tempo.latencyOffsetSeconds = -0.025;
    options.filter.seed = 20260903;
    const std::unique_ptr<BeatEngine> engine = makeParticleEngine(options);

    CHECK(engine->tempo().options().minBpm == Approx(90.0));
    CHECK(engine->tempo().options().maxBpm == Approx(180.0));
    CHECK(engine->tempo().fold(200.0) == Approx(100.0));

    // A different seed is a different run of the same filter, and the engine passed it on.
    const std::unique_ptr<BeatEngine> other = makeParticleEngine();
    const std::vector<float> signal = excerpt("pirates.wav");
    const std::size_t hops = signal.size() / kHopSize;
    std::vector<std::uint32_t> a;
    std::vector<std::uint32_t> b;
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        other->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
        (void)other->step();
        EngineFrame frame;
        while (engine->popFrame(frame)) {
            a.push_back(frame.tracked.gathering);
        }
        while (other->popFrame(frame)) {
            b.push_back(frame.tracked.gathering);
        }
    }
    REQUIRE(a.size() == b.size());
    CHECK(a != b);
}

TEST_CASE("a beat's host time is the beat's own, not the frame it was called on",
          "[engine][forward]") {
    // The audit's H15. The decoder puts a beat a fraction of a frame either side of the frame
    // that calls it (`TrackedFrame::beatOffsetFrames`) — a frame or more back under the default
    // emission — and the tracker adds that to the beat's time. The stamp Link and the output
    // scheduler are given took the frame's instead, so every beat reached them late by an
    // amount that changed from beat to beat.
    constexpr std::int64_t kOrigin = 3'000'000'000;
    struct RulerClock final : takt4::audio::HostTimeSource {
        std::int64_t hostMicrosForSample(double sampleTime) noexcept override {
            return 3'000'000'000 + static_cast<std::int64_t>(sampleTime * 1e6 / 22050.0);
        }
    };
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeEngine();
    RulerClock clock;
    engine->setHostTimeSource(&clock);

    std::vector<EngineBeat> beats;
    EngineBeat beat;
    EngineFrame frame;
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
        while (engine->popFrame(frame)) {
        }
        while (engine->popBeat(beat)) {
            beats.push_back(beat);
        }
    }
    REQUIRE(beats.size() > 15);
    std::size_t offFrame = 0;
    for (const EngineBeat& called : beats) {
        INFO("beat at " << called.event.time << " s");
        // The ruler starts at sample 0, which is decoder frame 0, so a beat's host time is its
        // own time on the ruler — to the microsecond the stamp is rounded to, and the one the
        // interpolated frames between two activations are.
        const double expected = static_cast<double>(kOrigin) + called.event.time * 1e6;
        CHECK(std::abs(static_cast<double>(called.hostMicros) - expected) <= 2.0);
        const double frameTime =
            static_cast<double>(called.event.frameIndex) * engine->secondsPerFrame();
        offFrame += std::abs(called.event.time - frameTime) > 1e-9 ? 1U : 0U;
    }
    // And the case was worth testing: most beats are not on the frame that called them — a
    // frame back or more under the default emission, which is the lateness the stamp had.
    CHECK(offFrame > beats.size() / 2);
}
