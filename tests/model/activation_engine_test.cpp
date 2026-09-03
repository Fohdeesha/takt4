#include "core/model/activation_engine.hpp"

#include "core/audio/rates.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

using takt4::audio::kHopSize;
using takt4::model::ActivationEngine;
using takt4::model::FrameActivation;
using takt4::model::ModelWeights;

namespace {

const std::filesystem::path kTestData{TAKT4_TEST_DATA_DIR};
const std::filesystem::path kWeightsDir{TAKT4_WEIGHTS_DIR};

std::vector<float> syntheticExcerpt() {
    const takt4::io::WavData audio = takt4::io::readWavFile(kTestData / "features" / "synthetic.wav");
    REQUIRE(audio.channels == 1);
    REQUIRE(audio.samples.size() % kHopSize == 0);
    return audio.samples;
}

// The rings are 130 KB together; an engine belongs on the heap, here as anywhere.
std::unique_ptr<ActivationEngine> makeEngine() {
    const ModelWeights weights = ModelWeights::fromFile(kWeightsDir / "generic.bin");
    return std::make_unique<ActivationEngine>(weights);
}

} // namespace

TEST_CASE("the engine turns hops into one activation each, a hop behind", "[model][engine]") {
    const std::vector<float> signal = syntheticExcerpt();
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<ActivationEngine> engine = makeEngine();

    // Driven by hand, without the worker: the same code path, deterministic.
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        CHECK(engine->step());
    }
    CHECK_FALSE(engine->step());
    CHECK(engine->hopsQueued() == hops);
    CHECK(engine->hopsDropped() == 0);

    // Frame k needs hop k + 1, so N hops give N - 1 frames until the stream ends.
    CHECK(engine->framesEmitted() == hops - 1);
    CHECK(engine->framesDropped() == 0);

    FrameActivation activation;
    std::uint64_t expected = 0;
    float loudest = 0.0f;
    while (engine->pop(activation)) {
        CHECK(activation.frameIndex == expected);
        CHECK(activation.hopIndex == expected + 1);
        const float total = activation.beat + activation.downbeat + activation.nonBeat;
        CHECK(total > 0.99f);
        CHECK(total < 1.01f);
        loudest = std::max(loudest, activation.beat);
        ++expected;
    }
    CHECK(expected == hops - 1);
    CHECK(loudest > 0.5f); // the synthetic excerpt is a drum machine at 128 BPM
}

TEST_CASE("the worker thread produces the same activations as stepping by hand", "[model][engine]") {
    const std::vector<float> signal = syntheticExcerpt();
    const std::size_t hops = signal.size() / kHopSize;

    const std::unique_ptr<ActivationEngine> stepped = makeEngine();
    std::vector<FrameActivation> byHand;
    for (std::size_t h = 0; h < hops; ++h) {
        stepped->processHop(signal.data() + h * kHopSize, h);
        (void)stepped->step();
    }
    FrameActivation activation;
    while (stepped->pop(activation)) {
        byHand.push_back(activation);
    }
    REQUIRE(byHand.size() == hops - 1);

    const std::unique_ptr<ActivationEngine> threaded = makeEngine();
    threaded->start();
    CHECK(threaded->running());
    std::vector<FrameActivation> fromWorker;
    for (std::size_t h = 0; h < hops; ++h) {
        threaded->processHop(signal.data() + h * kHopSize, h);
        // Faster than real time on purpose, but never so far ahead that the 64-hop
        // queue overflows — which is what a stream would do to it in 1.28 s.
        while (threaded->hopsPending() > ActivationEngine::kHopQueueCapacity / 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (threaded->pop(activation)) {
            fromWorker.push_back(activation);
        }
    }
    threaded->stop(); // drains what is left before joining
    while (threaded->pop(activation)) {
        fromWorker.push_back(activation);
    }
    CHECK(threaded->hopsDropped() == 0);
    CHECK_FALSE(threaded->running());

    REQUIRE(fromWorker.size() == byHand.size());
    for (std::size_t f = 0; f < byHand.size(); ++f) {
        INFO("frame " << f);
        CHECK(fromWorker[f].frameIndex == byHand[f].frameIndex);
        CHECK(fromWorker[f].hopIndex == byHand[f].hopIndex);
        CHECK(fromWorker[f].beat == byHand[f].beat);
        CHECK(fromWorker[f].downbeat == byHand[f].downbeat);
        CHECK(fromWorker[f].nonBeat == byHand[f].nonBeat);
    }

    // HANDOFF §8 Phase 3's other exit criterion, measured rather than assumed: one hop
    // of audio is 20 ms, and the worker has to be well inside that or the queue grows
    // without bound. Optimised builds only — a Debug build of Eigen is an order of
    // magnitude slower and says nothing about what ships.
    INFO("worst hop " << threaded->worstHopMicros() << " us, worst model " << threaded->worstModelMicros()
                      << " us, of 20000 us of audio");
    CHECK(threaded->worstHopMicros() > 0.0);
#ifdef NDEBUG
    CHECK(threaded->worstHopMicros() < 20000.0);
#endif
}

TEST_CASE("start() clears the queues and the model's memory of the last stream", "[model][engine]") {
    const std::vector<float> signal = syntheticExcerpt();
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<ActivationEngine> engine = makeEngine();

    const auto runTwentyHops = [&] {
        std::vector<FrameActivation> out;
        FrameActivation activation;
        for (std::size_t h = 0; h < 20; ++h) {
            engine->processHop(signal.data() + h * kHopSize, h);
            (void)engine->step();
        }
        while (engine->pop(activation)) {
            out.push_back(activation);
        }
        return out;
    };

    const std::vector<FrameActivation> first = runTwentyHops();
    REQUIRE(first.size() == 19);

    // Run on so the LSTM state and the front end's history are anything but fresh, and
    // leave hops in the queue that start() has to throw away.
    for (std::size_t h = 20; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        (void)engine->step();
    }
    engine->processHop(signal.data(), 999);
    engine->start();
    engine->stop();
    CHECK(engine->hopsQueued() == 0);
    CHECK(engine->framesEmitted() == 0);

    const std::vector<FrameActivation> again = runTwentyHops();
    REQUIRE(again.size() == first.size());
    for (std::size_t f = 0; f < first.size(); ++f) {
        INFO("frame " << f);
        CHECK(again[f].beat == first[f].beat);
        CHECK(again[f].downbeat == first[f].downbeat);
        CHECK(again[f].nonBeat == first[f].nonBeat);
    }
}

TEST_CASE("a reader that never reads loses activations instead of blocking", "[model][engine]") {
    const std::vector<float> signal = syntheticExcerpt();
    const std::unique_ptr<ActivationEngine> engine = makeEngine();
    const std::size_t hops = std::min<std::size_t>(signal.size() / kHopSize, ActivationEngine::kHopQueueCapacity);

    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
    }
    CHECK(engine->hopsQueued() == hops);
    CHECK(engine->hopsDropped() == 0);
    // One more than the queue holds: the audio thread carries on and counts the loss.
    engine->processHop(signal.data(), hops);
    CHECK(engine->hopsDropped() == 1);
}

TEST_CASE("ActivationEngine::processHop() does not touch the heap", "[model][engine][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    const std::vector<float> signal = syntheticExcerpt();
    const std::unique_ptr<ActivationEngine> engine = makeEngine();

    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope realtime;
        for (std::size_t h = 0; h < 32; ++h) {
            engine->processHop(signal.data() + h * kHopSize, h);
        }
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(takt4::rt::violationCount() == before);
    CHECK(engine->hopsQueued() == 32);
}
