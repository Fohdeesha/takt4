#pragma once

#include "core/audio/hop_processor.hpp"
#include "core/audio/host_time.hpp"
#include "core/engine/control.hpp"
#include "core/model/activation_engine.hpp"
#include "core/model/weights.hpp"
#include "core/rt/published.hpp"
#include "core/rt/spsc_ring.hpp"
#include "core/tracking/particle_filter.hpp"
#include "core/tracking/state_space.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace takt4::engine {

/// One 50 Hz frame: what the network said, what the filter made of it, and what the
/// tempo state machine was publishing when it did.
///
/// This is what §5.9's activation trace draws — "the CRNN hands you a 50 Hz probability
/// signal, and scrolling it live makes the app feel diagnostic rather than magical" — so
/// a consumer gets every frame rather than a snapshot.
struct EngineFrame {
    model::FrameActivation activation;
    tracking::TrackedFrame tracked;
    tracking::TempoState state;
    /// True on the frames where a beat was called; the beat itself is on the beat ring.
    bool beat = false;
};

/// One beat, with everything an output transport needs and nothing it has to look up.
///
/// The state as it stood at *this* beat, so a consumer draining only beats never has to
/// pair them against the frame ring — and the host time of the audio the beat was found
/// in (§4.3), which `tracking::BeatEvent` cannot carry because the tempo state machine
/// knows nothing about a host clock. Zero when no HostTimeSource is installed.
struct EngineBeat {
    tracking::BeatEvent event;
    tracking::TempoState state;
    std::int64_t hostMicros = 0;
};

/// HANDOFF §4.2's inference thread, and the thing both the UI and the console drive.
///
/// The chain from a hop of audio to a beat, in one object:
///
///     audio thread   processHop -> ActivationEngine's ring        (copies, nothing else)
///     model worker   features, BeatNet+                          (ActivationEngine owns it)
///     inference      particle filter, tempo state machine        (this class owns it)
///     consumers      popFrame for the trace, popBeat for output  (lock-free rings)
///
/// §4.2 wants the particle filter off the audio thread because "the particle filter's
/// resampling step is data-dependent in cost", and off the caller's loop because a UI
/// timer or a console print must not decide when a beat is noticed. It allocates on
/// construction and on reset, and nothing on either thread after that.
///
/// **What this deliberately does not own: the transports.** Link, OSC and MIDI are the
/// output thread's business (§4.2 again), and keeping them out means `core/engine` does
/// not depend on `core/output` and can be tested without a socket. Whoever wants them
/// drains popBeat.
///
/// One producer and one consumer per ring, which is SpscRing's contract: the inference
/// thread fills both, and exactly one thread may drain each.
///
/// About a quarter of a megabyte of rings and weights, so it belongs on the heap.
class BeatEngine final : public audio::HopProcessor {
public:
    /// Frames a consumer may fall behind by: 10 s at 50 Hz, as ActivationEngine's own.
    static constexpr std::size_t kFrameQueueCapacity = 512;
    /// Beats a consumer may fall behind by. 256 is a couple of minutes of them.
    static constexpr std::size_t kBeatQueueCapacity = 256;
    /// How long the inference thread sleeps when no activation is waiting. The model
    /// worker's own poll is 2 ms; there is no point being keener than its producer.
    static constexpr std::chrono::microseconds kIdleSleep{2000};

    struct Options {
        tracking::ParticleFilter::Options filter;
        tracking::TempoTracker::Options tempo;
    };

    /// The weights are copied in; the state space is not, and must outlive the engine.
    BeatEngine(const model::ModelWeights& weights, const tracking::StateSpaceModel& model,
               Options options);
    BeatEngine(const model::ModelWeights& weights, const tracking::StateSpaceModel& model);
    ~BeatEngine() override;

    BeatEngine(const BeatEngine&) = delete;
    BeatEngine& operator=(const BeatEngine&) = delete;

    /// Installs the clock that stamps each frame with the host time of its audio
    /// (§4.3). Set before start(), cleared after stop(); see ActivationEngine.
    void setHostTimeSource(audio::HostTimeSource* source) noexcept;

    /// Clears everything, reseeds the filter, and starts both worker threads. Call
    /// before the stream is started, never while it is running.
    void start();
    /// Stops both and waits for them. Safe to call twice.
    void stop() noexcept;
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Audio thread. Hands the hop to the model worker and returns.
    void processHop(const float* hop, std::uint64_t hopIndex) noexcept override;

    /// Consumer thread. Every frame, for the trace and the readouts.
    bool popFrame(EngineFrame& out) noexcept { return frames_.tryPop(out); }
    /// Output thread. Only the beats.
    bool popBeat(EngineBeat& out) noexcept { return beats_.tryPop(out); }

    /// The latest tempo, lock and meter, consistently, without draining anything — for
    /// a status line or a UI that redraws on its own clock.
    tracking::TempoState state() const noexcept { return state_.load(); }

    /// The tracker's settings as they now stand, safely, from any thread.
    ///
    /// A caller that only ever posted them could keep its own copy — but a tap moves the
    /// octave-fold window (see `TempoTracker::seedTempo`), so a copy that is never
    /// refreshed goes stale and the next settings change posted from it would quietly
    /// undo the tap. Edit from this, not from what you last sent. Note that a command
    /// posted moments ago may not have been applied yet; this is what the tracker has,
    /// not what it is about to have.
    tracking::TempoTracker::Options tempoOptions() const noexcept { return options_.load(); }

    /// Runs whatever the model worker has ready, on the calling thread, for tests and
    /// offline use. Returns how many frames were tracked. Only valid while stopped.
    std::size_t step() noexcept;

    /// **The way in.** Any thread but the audio one, running or stopped: a UI button, an
    /// inbound OSC message (§5.7), a keyboard shortcut. The inference thread applies it
    /// before the next frame it tracks, so nothing here disturbs the lock or the filter.
    ///
    /// False when the queue is full; see ControlQueue for when that can happen.
    bool post(const Command& command) { return controls_.post(command); }
    std::uint64_t commandsDropped() const { return controls_.dropped(); }

    /// The tempo state machine, for reading its settings. Changing them on a running
    /// engine is `post(Command::setTempoOptions(...))`'s job — this reference is only
    /// safe to write through while stopped, or from the inference thread itself.
    tracking::TempoTracker& tempo() noexcept { return tempo_; }
    const tracking::TempoTracker& tempo() const noexcept { return tempo_; }
    const model::ActivationEngine& activations() const noexcept { return *activations_; }
    const tracking::StateSpaceModel& stateSpace() const noexcept { return *model_; }

    std::uint64_t framesTracked() const noexcept {
        return framesTracked_.load(std::memory_order_relaxed);
    }
    std::uint64_t framesDropped() const noexcept {
        return framesDropped_.load(std::memory_order_relaxed);
    }
    std::uint64_t beatsCalled() const noexcept {
        return beatsCalled_.load(std::memory_order_relaxed);
    }
    std::uint64_t beatsDropped() const noexcept {
        return beatsDropped_.load(std::memory_order_relaxed);
    }
    /// The worst and the mean time the inference thread has taken over one frame, in
    /// microseconds. One frame is 20000 µs of audio.
    double worstFrameMicros() const noexcept {
        return worstFrameMicros_.load(std::memory_order_relaxed);
    }
    double meanFrameMicros() const noexcept;

private:
    void run() noexcept;
    /// One activation through the filter and the tempo machine. On the inference thread.
    void track(const model::FrameActivation& activation) noexcept;
    /// Everything posted since the last frame, in order. On the inference thread, or on
    /// the caller's while the engine is stopped — never on both at once.
    void applyCommands() noexcept;

    const tracking::StateSpaceModel* model_;
    // On the heap: ActivationEngine carries 130 KB of rings and the network's weights.
    std::unique_ptr<model::ActivationEngine> activations_;
    tracking::ParticleFilter filter_;
    tracking::TempoTracker tempo_;

    rt::SpscRing<EngineFrame, kFrameQueueCapacity> frames_;
    rt::SpscRing<EngineBeat, kBeatQueueCapacity> beats_;
    rt::Published<tracking::TempoState> state_;
    rt::Published<tracking::TempoTracker::Options> options_;

    ControlQueue controls_;
    /// Drained into, and reused, so applying commands allocates nothing after the first.
    std::vector<Command> commands_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> framesTracked_{0};
    std::atomic<std::uint64_t> framesDropped_{0};
    std::atomic<std::uint64_t> beatsCalled_{0};
    std::atomic<std::uint64_t> beatsDropped_{0};
    std::atomic<double> worstFrameMicros_{0.0};
    std::atomic<double> totalFrameMicros_{0.0};
    std::atomic<std::uint64_t> framesTimed_{0};
};

} // namespace takt4::engine
