#include "core/engine/beat_engine.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace takt4::engine {

namespace {

using Clock = std::chrono::steady_clock;

double microsSince(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

} // namespace

BeatEngine::BeatEngine(const model::ModelWeights& weights, const tracking::StateSpaceModel& model,
                       Options options)
    : model_(&model), activations_(std::make_unique<model::ActivationEngine>(weights)),
      filter_(model, options.filter), tempo_(model.secondsPerFrame(), options.tempo) {
    state_.publish(tempo_.state());
    options_.publish(tempo_.options());
}

BeatEngine::BeatEngine(const model::ModelWeights& weights, const tracking::StateSpaceModel& model)
    : BeatEngine(weights, model, Options{}) {}

BeatEngine::~BeatEngine() {
    stop();
}

void BeatEngine::setHostTimeSource(audio::HostTimeSource* source) noexcept {
    activations_->setHostTimeSource(source);
}

void BeatEngine::start() {
    stop();

    EngineFrame frame;
    while (frames_.tryPop(frame)) {
    }
    EngineBeat beat;
    while (beats_.tryPop(beat)) {
    }
    filter_.reset();
    tempo_.reset();
    // Anything posted while the engine was stopped applies to the run about to start, not
    // to the one that ended: a latency offset set on a settings screen has to survive the
    // operator then pressing start. The reset above clears tracking state, never options.
    applyCommands();
    state_.publish(tempo_.state());
    framesTracked_.store(0, std::memory_order_relaxed);
    framesDropped_.store(0, std::memory_order_relaxed);
    beatsCalled_.store(0, std::memory_order_relaxed);
    beatsDropped_.store(0, std::memory_order_relaxed);
    worstFrameMicros_.store(0.0, std::memory_order_relaxed);
    totalFrameMicros_.store(0.0, std::memory_order_relaxed);
    framesTimed_.store(0, std::memory_order_relaxed);

    // The model worker first: it is this thread's producer, and it clears its own rings.
    activations_->start();
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run(); });
}

void BeatEngine::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    // After the inference thread, so nothing is still draining what the model produces.
    activations_->stop();
    // Whatever the model worker managed before it stopped is still worth tracking.
    (void)step();
}

void BeatEngine::processHop(const float* hop, std::uint64_t hopIndex) noexcept {
    activations_->processHop(hop, hopIndex);
}

std::size_t BeatEngine::step() noexcept {
    applyCommands();
    // Both halves, in order: the model worker's queued hops through the network, then
    // the activations that produced through the filter. Only valid while stopped —
    // ActivationEngine::step() and its worker are the same consumer.
    while (activations_->step()) {
    }
    std::size_t tracked = 0;
    model::FrameActivation activation;
    while (activations_->pop(activation)) {
        track(activation);
        ++tracked;
    }
    return tracked;
}

void BeatEngine::run() noexcept {
    model::FrameActivation activation;
    while (running_.load(std::memory_order_acquire)) {
        // Every time round, not only when a frame is waiting: a ÷2 pressed during a
        // silence must not sit in the queue until the music starts again. The cost of an
        // uncontended mutex 500 times a second is nothing.
        applyCommands();
        if (activations_->pop(activation)) {
            track(activation);
        } else {
            std::this_thread::sleep_for(kIdleSleep);
        }
    }
}

void BeatEngine::applyCommands() noexcept {
    controls_.drain(commands_);
    if (commands_.empty()) {
        return;
    }
    for (const Command& command : commands_) {
        switch (command.kind) {
        case Command::Kind::SetTempoOptions:
            tempo_.setOptions(command.tempo);
            break;
        case Command::Kind::Halve:
            tempo_.halve();
            break;
        case Command::Kind::Redouble:
            tempo_.redouble();
            break;
        case Command::Kind::SnapDownbeat:
            tempo_.snapDownbeat();
            break;
        case Command::Kind::SeedTempo:
            tempo_.seedTempo(command.bpm);
            break;
        }
    }
    // A command changes what the tracker is saying, and a reader of state() may not be
    // draining frames at all — so publish rather than wait for the next frame to do it.
    // The settings go with it: this is the only place they ever change, and a caller has
    // to be able to see a window a tap moved.
    state_.publish(tempo_.state());
    options_.publish(tempo_.options());
}

void BeatEngine::track(const model::FrameActivation& activation) noexcept {
    const Clock::time_point started = Clock::now();

    EngineFrame frame;
    frame.activation = activation;
    frame.tracked = filter_.process(activation.beat, activation.downbeat);
    const std::optional<tracking::BeatEvent> event = tempo_.process(frame.tracked);
    frame.state = tempo_.state();
    frame.beat = event.has_value();

    // The state before the rings: a consumer that reads state() rather than draining
    // should never see a frame on the ring that is newer than the state.
    state_.publish(frame.state);
    if (event) {
        beatsCalled_.fetch_add(1, std::memory_order_relaxed);
        if (!beats_.tryPush(EngineBeat{*event, frame.state, activation.hostMicros})) {
            beatsDropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (frames_.tryPush(frame)) {
        framesTracked_.fetch_add(1, std::memory_order_relaxed);
    } else {
        framesDropped_.fetch_add(1, std::memory_order_relaxed);
    }

    const double micros = microsSince(started);
    double worst = worstFrameMicros_.load(std::memory_order_relaxed);
    while (micros > worst &&
           !worstFrameMicros_.compare_exchange_weak(worst, micros, std::memory_order_relaxed)) {
    }
    // Only this thread writes these, so a plain load-add-store is enough.
    totalFrameMicros_.store(totalFrameMicros_.load(std::memory_order_relaxed) + micros,
                            std::memory_order_relaxed);
    framesTimed_.fetch_add(1, std::memory_order_relaxed);
}

double BeatEngine::meanFrameMicros() const noexcept {
    const std::uint64_t timed = framesTimed_.load(std::memory_order_relaxed);
    return timed == 0
               ? 0.0
               : totalFrameMicros_.load(std::memory_order_relaxed) / static_cast<double>(timed);
}

} // namespace takt4::engine
