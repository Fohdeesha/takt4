#include "core/model/activation_engine.hpp"

#include "core/audio/host_time.hpp"
#include "core/audio/rates.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    const takt4::io::WavData audio =
        takt4::io::readWavFile(kTestData / "features" / "synthetic.wav");
    REQUIRE(audio.channels == 1);
    REQUIRE(audio.samples.size() % kHopSize == 0);
    return audio.samples;
}

/// An arbitrary point on the host clock for the ruler below to start from, big enough
/// to catch an off-by-a-lot rather than an off-by-a-little.
constexpr std::int64_t kClockOrigin = 7'000'000'000;

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

TEST_CASE("an activation carries the host time of the audio it was made from", "[model][engine]") {
    // HANDOFF §4.3. LinkSession is the real source; this one is a straight line, so the
    // arithmetic can be checked rather than merely exercised.
    struct RulerClock final : takt4::audio::HostTimeSource {
        std::int64_t hostMicrosForSample(double sampleTime) noexcept override {
            ++calls;
            // 22050 samples a second, and the stream started at this arbitrary offset.
            return kClockOrigin + static_cast<std::int64_t>(sampleTime * 1e6 / 22050.0);
        }
        std::size_t calls = 0;
    };

    const std::vector<float> signal = syntheticExcerpt();
    const std::size_t hops = signal.size() / kHopSize;
    const std::unique_ptr<ActivationEngine> engine = makeEngine();

    RulerClock clock;
    engine->setHostTimeSource(&clock);
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(signal.data() + h * kHopSize, h);
        CHECK(engine->step());
    }
    CHECK(clock.calls == hops); // once per hop, on the audio thread, and nowhere else

    // Frame k is centred on sample kHopSize * k, whichever hop happened to complete it.
    FrameActivation activation;
    std::uint64_t expected = 0;
    while (engine->pop(activation)) {
        CHECK(activation.frameIndex == expected);
        CHECK(activation.hostMicros == kClockOrigin + static_cast<std::int64_t>(expected) * 20000);
        ++expected;
    }
    CHECK(expected == hops - 1);

    SECTION("without a source the stamp is left at zero rather than guessed") {
        const std::unique_ptr<ActivationEngine> plain = makeEngine();
        for (std::size_t h = 0; h < 8; ++h) {
            plain->processHop(signal.data() + h * kHopSize, h);
            (void)plain->step();
        }
        FrameActivation frame;
        std::size_t frames = 0;
        while (plain->pop(frame)) {
            CHECK(frame.hostMicros == 0);
            ++frames;
        }
        CHECK(frames == 7);
    }
}

TEST_CASE("a stamp stays on the audio it was made from after hops were dropped",
          "[model][engine]") {
    // The audit's H15. The stamp was worked back from the hop's own index, which counts every
    // hop the audio thread saw — including the ones dropped because the worker's queue was
    // full. So after one stall every later frame was stamped with audio that many hops older
    // than its own, and every beat after it reached Link that many 20 ms early, to the end of
    // the run.
    struct RulerClock final : takt4::audio::HostTimeSource {
        std::int64_t hostMicrosForSample(double sampleTime) noexcept override {
            return kClockOrigin + static_cast<std::int64_t>(sampleTime * 1e6 / 22050.0);
        }
    };
    const std::vector<float> signal = syntheticExcerpt();
    const std::unique_ptr<ActivationEngine> engine = makeEngine();
    RulerClock clock;
    engine->setHostTimeSource(&clock);
    std::vector<FrameActivation> frames;
    const auto feed = [&](std::size_t h) {
        engine->processHop(signal.data() + (h % 400) * kHopSize, h);
    };
    const auto work = [&] {
        while (engine->step()) {
        }
        FrameActivation activation;
        while (engine->pop(activation)) {
            frames.push_back(activation);
        }
    };

    for (std::size_t h = 0; h < 100; ++h) {
        feed(h);
        work();
    }
    // A stall: a hundred hops arrive and nobody works them, so the queue fills and the rest
    // are dropped.
    for (std::size_t h = 100; h < 200; ++h) {
        feed(h);
    }
    REQUIRE(engine->hopsDropped() > 0);
    work();
    for (std::size_t h = 200; h < 300; ++h) {
        feed(h);
        work();
    }
    REQUIRE(engine->framesDropped() == 0);

    // Past the frame that straddles the gap, every frame is centred on the hop before the one
    // that completed it — as before the stall, and as the ruler says.
    std::size_t checked = 0;
    for (const FrameActivation& activation : frames) {
        if (activation.hopIndex < 202 && activation.hopIndex >= 100) {
            continue; // the queued backlog and the frame across the gap
        }
        INFO("frame completed by hop " << activation.hopIndex);
        CHECK(activation.hostMicros ==
              kClockOrigin + static_cast<std::int64_t>(activation.hopIndex - 1) * 20000);
        ++checked;
    }
    CHECK(checked > 150);
}

TEST_CASE("the worker thread produces the same activations as stepping by hand",
          "[model][engine]") {
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

    // HANDOFF §8 Phase 3's other exit criterion, measured rather than assumed: one hop of
    // audio is 20 ms, and the worker has to be inside that or the queue grows without
    // bound.
    //
    // The claim is about the *mean*, not the worst. A single hop that overruns is
    // absorbed by the 64-hop ring — that is what the ring is for — and on a shared CI
    // runner one is guaranteed: this asserted the worst hop the first time it ever ran
    // outside this machine and saw 156 ms, a preemption rather than a tracker that
    // cannot keep up. The mean over five hundred hops is what says whether the queue
    // drains, and it is 86 us on the development machine against a 20000 us budget, so a
    // real regression has two orders of magnitude to cross before it hides here.
    //
    // Optimised builds only: a Debug build of Eigen is an order of magnitude slower and
    // says nothing about what ships.
    INFO("mean hop " << threaded->meanHopMicros() << " us, worst " << threaded->worstHopMicros()
                     << " us, worst model " << threaded->worstModelMicros()
                     << " us, of 20000 us of audio");
    CHECK(threaded->worstHopMicros() > 0.0);
    CHECK(threaded->meanHopMicros() > 0.0);
#ifdef NDEBUG
    CHECK(threaded->meanHopMicros() < 20000.0);
#endif
}

TEST_CASE("start() clears the queues and the model's memory of the last stream",
          "[model][engine]") {
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
    const std::size_t hops =
        std::min<std::size_t>(signal.size() / kHopSize, ActivationEngine::kHopQueueCapacity);

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
