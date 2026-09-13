#include "core/engine/beat_engine.hpp"

#include "core/audio/rates.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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

/// The chosen decoder, and nothing else built.
std::unique_ptr<tracking::BeatDecoder> makeDecoder(const tracking::StateSpaceModel& model,
                                                   const BeatEngine::Options& options) {
    switch (options.decoder) {
    case tracking::Decoder::Forward:
        return std::make_unique<tracking::ForwardFilter>(options.forward);
    case tracking::Decoder::ParticleFilter:
        break;
    }
    return std::make_unique<tracking::ParticleFilter>(model, options.filter);
}

/// The tracker's options as the decoder wants them: the window is the decoder's to apply
/// when it can, and the tracker's otherwise. See `TempoTracker::Options::foldInDecoder`.
tracking::TempoTracker::Options forDecoder(tracking::TempoTracker::Options options,
                                           const tracking::BeatDecoder& decoder) noexcept {
    options.foldInDecoder = decoder.honoursTempoWindow();
    return options;
}

/// How many decoder frames one network activation is worth. The network's hop is 20 ms;
/// a decoder at 100 Hz takes two steps for it, the first on an activation interpolated
/// between the last two.
std::size_t stepsFor(const tracking::BeatDecoder& decoder) noexcept {
    const double activationPeriod =
        static_cast<double>(audio::kHopSize) / audio::kInternalSampleRate;
    const double steps = activationPeriod / decoder.secondsPerFrame();
    return static_cast<std::size_t>(std::max(1.0, std::round(steps)));
}

} // namespace

BeatEngine::BeatEngine(const model::ModelWeights& weights, const tracking::StateSpaceModel& model,
                       Options options)
    : model_(&model), activations_(std::make_unique<model::ActivationEngine>(weights)),
      decoderKind_(options.decoder), decoder_(makeDecoder(model, options)),
      tempo_(decoder_->secondsPerFrame(), forDecoder(options.tempo, *decoder_)),
      stepsPerActivation_(stepsFor(*decoder_)) {
    syncDecoder();
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
    decoder_->reset();
    tempo_.reset();
    havePrevious_ = false;
    // Anything posted while the engine was stopped applies to the run about to start, not
    // to the one that ended: a latency offset set on a settings screen has to survive the
    // operator then pressing start. The reset above clears tracking state, never options.
    applyCommands();
    state_.publish(tempo_.state());
    onsets_ = 0;
    intensity_.publish(EngineIntensity{});
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

void BeatEngine::syncDecoder() noexcept {
    const tracking::TempoTracker::Options& options = tempo_.options();
    decoder_->setTempoWindow(options.minBpm, options.maxBpm, options.octaveFold);
}

void BeatEngine::applyCommands() noexcept {
    controls_.drain(commands_);
    if (commands_.empty()) {
        return;
    }
    for (const Command& command : commands_) {
        switch (command.kind) {
        case Command::Kind::SetTempoOptions:
            tempo_.setOptions(forDecoder(command.tempo, *decoder_));
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
        case Command::Kind::SetLockPinned:
            tempo_.setLockPinned(command.pinned);
            // And on a decoder that can, the pin is also the tempo hold — the tempo being
            // published, which is what the operator is looking at when they press it. See
            // Command::Kind::SetLockPinned.
            if (decoder_->canHoldTempo()) {
                decoder_->holdTempo(command.pinned ? tempo_.state().bpm : 0.0);
            }
            break;
        case Command::Kind::HoldTempo:
            decoder_->holdTempo(command.bpm);
            break;
        }
    }
    // A tap moves the window, so the decoder is told again after every batch rather than
    // after the one command that is known to move it.
    syncDecoder();
    // A command changes what the tracker is saying, and a reader of state() may not be
    // draining frames at all — so publish rather than wait for the next frame to do it.
    // The settings go with it: this is the only place they ever change, and a caller has
    // to be able to see a window a tap moved.
    state_.publish(tempo_.state());
    options_.publish(tempo_.options());
}

void BeatEngine::track(const model::FrameActivation& activation) noexcept {
    const Clock::time_point started = Clock::now();

    // A decoder running faster than the network is fed the frames in between, made by
    // linear interpolation from the previous activation to this one — TRACKING-PROPOSAL.md
    // §2.5, which measured the finer grid with exactly that interpolation. Nothing is
    // interpolated before there is a previous activation, so the first hop yields one
    // frame and every later hop `stepsPerActivation_`; decoder frame k is at k times the
    // decoder's period, the network's activation i at decoder frame i * steps.
    if (stepsPerActivation_ > 1 && havePrevious_) {
        for (std::size_t k = 1; k < stepsPerActivation_; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(stepsPerActivation_);
            model::FrameActivation between = activation;
            between.beat = previous_.beat + (activation.beat - previous_.beat) * t;
            between.downbeat = previous_.downbeat + (activation.downbeat - previous_.downbeat) * t;
            between.nonBeat = previous_.nonBeat + (activation.nonBeat - previous_.nonBeat) * t;
            between.flux = previous_.flux + (activation.flux - previous_.flux) * t;
            // The host time of the audio the interpolated frame stands for is between the
            // two as well; an onset is a fact about one of the network's frames, not this.
            // `t` is widened explicitly: the multiplication is in double either way, and
            // GCC and Clang refuse the implicit promotion under -Wdouble-promotion -Werror.
            between.hostMicros =
                previous_.hostMicros +
                static_cast<std::int64_t>(
                    std::llround(static_cast<double>(activation.hostMicros - previous_.hostMicros) *
                                 static_cast<double>(t)));
            between.onset = false;
            trackOne(between, true);
        }
    }
    trackOne(activation, false);
    previous_ = activation;
    havePrevious_ = true;

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

void BeatEngine::trackOne(const model::FrameActivation& activation, bool interpolated) noexcept {
    EngineFrame frame;
    frame.activation = activation;
    frame.interpolated = interpolated;
    frame.tracked = decoder_->process(activation.beat, activation.downbeat);
    const std::optional<tracking::BeatEvent> event = tempo_.process(frame.tracked);
    frame.state = tempo_.state();
    frame.beat = event.has_value();

    // The state before the rings: a consumer that reads state() rather than draining
    // should never see a frame on the ring that is newer than the state.
    state_.publish(frame.state);
    if (activation.onset) {
        ++onsets_;
    }
    intensity_.publish(EngineIntensity{activation.intensity, onsets_, activation.flux});
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
}

double BeatEngine::meanFrameMicros() const noexcept {
    const std::uint64_t timed = framesTimed_.load(std::memory_order_relaxed);
    return timed == 0
               ? 0.0
               : totalFrameMicros_.load(std::memory_order_relaxed) / static_cast<double>(timed);
}

} // namespace takt4::engine
