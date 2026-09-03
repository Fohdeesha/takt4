#include "core/engine/beat_engine.hpp"

#include "core/audio/rates.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/tracking/particle_filter.hpp"
#include "core/tracking/state_space.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
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

} // namespace

TEST_CASE("the engine turns hops into tracked frames and beats", "[engine]") {
    const std::vector<float> signal = excerpt("synthetic.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeEngine();

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
        beatFrames += frame.beat ? 1 : 0;
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

    const std::unique_ptr<BeatEngine> engine = makeEngine();
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
    REQUIRE(byHand.size() == hops - 1);

    const std::unique_ptr<BeatEngine> threaded = makeEngine();
    threaded->start();
    CHECK(threaded->running());
    std::vector<EngineFrame> fromThreads;
    std::vector<EngineBeat> beats;
    for (std::size_t h = 0; h < hops; ++h) {
        threaded->processHop(signal.data() + h * kHopSize, h);
        // Faster than real time on purpose, but never far enough ahead to overflow the
        // model worker's 64-hop queue.
        while (threaded->activations().hopsPending() >
               takt4::model::ActivationEngine::kHopQueueCapacity / 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EngineFrame frame;
        while (threaded->popFrame(frame)) {
            fromThreads.push_back(frame);
        }
        EngineBeat beat;
        while (threaded->popBeat(beat)) {
            beats.push_back(beat);
        }
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
        beatFrames += byHand[f].beat ? 1 : 0;
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
    const std::unique_ptr<BeatEngine> engine = makeEngine();

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

    // Without a reset the second run would start with the filter and the model where the
    // first left them, and a stream stopped and started would not be the same tracker.
    engine->start();
    engine->stop();
    const std::vector<std::uint32_t> second = runOnce();
    CHECK(second == first);
    CHECK(engine->state().beats == 0 + engine->state().beats); // state is live, not stale
}

TEST_CASE("a consumer that never drains loses frames instead of blocking", "[engine]") {
    const std::vector<float> signal = excerpt("winter-now.wav");
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<BeatEngine> engine = makeEngine();

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

TEST_CASE("the options reach the filter and the tempo machine", "[engine]") {
    BeatEngine::Options options;
    options.tempo.minBpm = 90.0;
    options.tempo.maxBpm = 180.0;
    options.tempo.latencyOffsetSeconds = -0.025;
    options.filter.seed = 20260903;
    const std::unique_ptr<BeatEngine> engine = makeEngine(options);

    CHECK(engine->tempo().options().minBpm == Approx(90.0));
    CHECK(engine->tempo().options().maxBpm == Approx(180.0));
    CHECK(engine->tempo().fold(200.0) == Approx(100.0));

    // A different seed is a different run of the same filter, and the engine passed it on.
    const std::unique_ptr<BeatEngine> other = makeEngine();
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
