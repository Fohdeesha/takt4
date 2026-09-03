#include "core/model/activation_engine.hpp"

#include <algorithm>
#include <chrono>
#include <span>
#include <thread>

namespace takt4::model {

namespace {

using Clock = std::chrono::steady_clock;

double microsSince(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

} // namespace

ActivationEngine::ActivationEngine(const ModelWeights& weights) : model_(weights) {}

ActivationEngine::~ActivationEngine() {
    stop();
}

void ActivationEngine::start() {
    stop();
    QueuedHop hop;
    while (hops_.tryPop(hop)) {
    }
    FrameActivation activation;
    while (activations_.tryPop(activation)) {
    }
    extractor_.reset();
    model_.reset();
    hopsQueued_.store(0, std::memory_order_relaxed);
    hopsDropped_.store(0, std::memory_order_relaxed);
    framesEmitted_.store(0, std::memory_order_relaxed);
    framesDropped_.store(0, std::memory_order_relaxed);
    worstHopMicros_.store(0.0, std::memory_order_relaxed);
    worstModelMicros_.store(0.0, std::memory_order_relaxed);
    totalHopMicros_.store(0.0, std::memory_order_relaxed);
    hopsWorked_.store(0, std::memory_order_relaxed);

    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run(); });
}

void ActivationEngine::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ActivationEngine::processHop(const float* hop, std::uint64_t hopIndex) noexcept {
    QueuedHop queued;
    queued.index = hopIndex;
    // HANDOFF §4.3: the sample counter goes in here, on the audio thread, and Link's
    // regression turns it into a host time. Nothing else on this thread reads a clock.
    if (hostTime_ != nullptr) {
        queued.hostMicros = hostTime_->hostMicrosForSample(static_cast<double>(hopIndex) *
                                                           static_cast<double>(audio::kHopSize));
    }
    std::copy_n(hop, audio::kHopSize, queued.samples.begin());
    if (hops_.tryPush(queued)) {
        hopsQueued_.fetch_add(1, std::memory_order_relaxed);
    } else {
        hopsDropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ActivationEngine::step() noexcept {
    QueuedHop hop;
    if (!hops_.tryPop(hop)) {
        return false;
    }
    process(hop);
    return true;
}

void ActivationEngine::run() noexcept {
    QueuedHop hop;
    while (running_.load(std::memory_order_acquire)) {
        if (hops_.tryPop(hop)) {
            process(hop);
        } else {
            std::this_thread::sleep_for(kIdleSleep);
        }
    }
    // Whatever the audio thread queued before the stop is still worth having.
    while (hops_.tryPop(hop)) {
        process(hop);
    }
}

void ActivationEngine::process(const QueuedHop& hop) noexcept {
    const Clock::time_point hopStart = Clock::now();
    const std::span<const float, audio::kHopSize> samples(hop.samples);
    if (extractor_.pushHop(samples)) {
        const Clock::time_point modelStart = Clock::now();
        const BeatModel::Activation activation = model_.process(extractor_.frame());
        recordWorst(worstModelMicros_, microsSince(modelStart));

        // The hop's stamp is the host time of its first sample; the frame is centred on
        // sample kHopSize · frameIndex, which is that many hops earlier. The regression
        // behind the stamp is linear, so this is the same answer it would have given.
        const std::uint64_t frameIndex = extractor_.frameIndex();
        const std::int64_t hostMicros =
            hop.hostMicros == 0
                ? 0
                : hop.hostMicros - static_cast<std::int64_t>(hop.index - frameIndex) *
                                       static_cast<std::int64_t>(audio::kHopMicros);
        const FrameActivation out{
            frameIndex,          hop.index, hostMicros, activation.beat(), activation.downbeat(),
            activation.nonBeat()};
        if (activations_.tryPush(out)) {
            framesEmitted_.fetch_add(1, std::memory_order_relaxed);
        } else {
            framesDropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const double micros = microsSince(hopStart);
    recordWorst(worstHopMicros_, micros);
    // Only the worker writes these, so a plain load-add-store is enough.
    totalHopMicros_.store(totalHopMicros_.load(std::memory_order_relaxed) + micros,
                          std::memory_order_relaxed);
    hopsWorked_.fetch_add(1, std::memory_order_relaxed);
}

double ActivationEngine::meanHopMicros() const noexcept {
    const std::uint64_t worked = hopsWorked_.load(std::memory_order_relaxed);
    return worked == 0
               ? 0.0
               : totalHopMicros_.load(std::memory_order_relaxed) / static_cast<double>(worked);
}

void ActivationEngine::recordWorst(std::atomic<double>& worst, double micros) noexcept {
    double seen = worst.load(std::memory_order_relaxed);
    while (micros > seen && !worst.compare_exchange_weak(seen, micros, std::memory_order_relaxed)) {
    }
}

} // namespace takt4::model
