#pragma once

#include "core/audio/hop_processor.hpp"
#include "core/audio/host_time.hpp"
#include "core/engine/control.hpp"
#include "core/model/activation_engine.hpp"
#include "core/model/weights.hpp"
#include "core/rt/published.hpp"
#include "core/rt/spsc_ring.hpp"
#include "core/tracking/beat_decoder.hpp"
#include "core/tracking/forward_filter.hpp"
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

/// One frame of the decoder's clock: what the network said, what the decoder made of it,
/// and what the tempo state machine was publishing when it did.
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
    /// True when the activation is one the engine made up between two of the network's,
    /// for a decoder running faster than 50 Hz (see `BeatEngine::stepsPerActivation`). A
    /// trace drawn at the network's rate skips these — and carries `beat` forward if one
    /// landed here, because the beat is real even though the activation is interpolated.
    bool interpolated = false;
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
    /// The run of the engine that called it (`BeatEngine::runNumber`). A beat an earlier run left in
    /// the ring is its consumer's to drop: nothing else may drain the ring (`start`).
    std::uint64_t run = 0;
};

/// HANDOFF §5.8's intensity, as the *output* thread can read it.
///
/// It is computed per frame (`features::IntensityClassifier`, on the model worker) and rides
/// each `model::FrameActivation` — but the frame ring has exactly one consumer and that is
/// §5.9's trace, so the output thread cannot have it that way. This is published instead,
/// like the tempo state beside it.
///
/// It is **not** part of `tracking::TempoState`, deliberately. That is what the tempo state
/// machine is saying, and intensity is not a tracking quantity; folding it in would make
/// `core/tracking` depend on the features and would put a number in the Phase 4 gate's
/// neighbourhood that has no business there.
struct EngineIntensity {
    features::Intensity level = features::Intensity::Normal;
    /// Onsets seen since the engine was reset. The output thread has no frames, so a count
    /// that moved is how it learns one happened — §5.8's *"on onset"*.
    std::uint64_t onsets = 0;
    /// The raw flux, for a diagnostic readout. See `FrameActivation::flux`.
    float flux = 0.0f;
};

/// HANDOFF §4.2's inference thread, and the thing both the UI and the console drive.
///
/// The chain from a hop of audio to a beat, in one object:
///
///     audio thread   processHop -> ActivationEngine's ring        (copies, nothing else)
///     model worker   features, BeatNet+                          (ActivationEngine owns it)
///     inference      the decoder, the tempo state machine        (this class owns it)
///     consumers      popFrame for the trace, popBeat for output  (lock-free rings)
///
/// §4.2 wants the particle filter off the audio thread because "the particle filter's
/// resampling step is data-dependent in cost", and off the caller's loop because a UI
/// timer or a console print must not decide when a beat is noticed. It allocates on
/// construction and on reset, and nothing on either thread after that.
///
/// **The decoder is one of two** (`tracking::Decoder`): the particle filter, which is
/// upstream's algorithm and runs on the network's 50 Hz frames, or the exact forward
/// filter, which runs at 100 Hz on activations this class interpolates between the
/// network's. Whichever it is, `TempoTracker` runs at
/// the decoder's frame rate and every time this class hands out is in seconds, so nothing
/// outside knows the difference except through `secondsPerFrame()` and
/// `EngineFrame::interpolated`.
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
    /// Frames a consumer may fall behind by: 10 s at 50 Hz, as ActivationEngine's own,
    /// and 5 s at the forward filter's 100.
    static constexpr std::size_t kFrameQueueCapacity = 512;
    /// Beats a consumer may fall behind by. 256 is a couple of minutes of them.
    static constexpr std::size_t kBeatQueueCapacity = 256;
    /// How long the inference thread sleeps when no activation is waiting. The model
    /// worker's own poll is 2 ms; there is no point being keener than its producer.
    static constexpr std::chrono::microseconds kIdleSleep{2000};

    struct Options {
        /// Which decoder, and each one's own settings; only the chosen one is built. The
        /// forward filter is the default — see `tracking::Decoder` for the measurements
        /// that made it so — and the particle filter is a choice.
        tracking::Decoder decoder = tracking::Decoder::Forward;
        tracking::ParticleFilter::Options filter;
        tracking::ForwardFilter::Options forward;
        tracking::TempoTracker::Options tempo;

        /// **No signal**: the input's level below `noSignalBelowDb` (the RMS of each hop, dBFS)
        /// for `noSignalSeconds` on end. Then the deck has stopped rather than paused: the
        /// decoder forgets the beat and calls no more (`BeatDecoder::silence`), the tracker drops
        /// the lock and says so (`TempoTracker::setNoSignal`), and the next sound is listened to
        /// afresh. The operator's call of 2026-10-03: beats go on through any breakdown that
        /// has sound, and stop after a few seconds of none.
        ///
        /// -60 dBFS because digital zero is -inf, and white noise at -70 dBFS — about where a
        /// mixer channel with nothing playing sits (not measured on the rig) — locked exactly
        /// as zeros did, while music, a breakdown's pad included, sits tens of dB higher. Four
        /// seconds because a track's own dead stop before a drop is a bar or two of silence —
        /// 1.9 s a bar at 128 BPM — and the flywheel should carry the beat through that as it
        /// does through a breakdown.
        double noSignalBelowDb = -60.0;
        double noSignalSeconds = 4.0;
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
    ///
    /// Everything but the beat ring: that is the output thread's to drain, and only its (see
    /// `EngineBeat::run`).
    void start();
    /// Stops both and waits for them. Safe to call twice.
    void stop() noexcept;
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    /// Which run this is: one more at every `start`, and what each beat it calls is stamped
    /// with. Any thread.
    std::uint64_t runNumber() const noexcept { return run_.load(std::memory_order_acquire); }

    /// Audio thread. Hands the hop to the model worker and returns.
    void processHop(const float* hop, std::uint64_t hopIndex) noexcept override;
    /// Audio thread. What stamps the hops with when they were heard: see `audio::HopProcessor`.
    void beginBuffer(double firstSample, std::int64_t steadyMicros,
                     double lostSamples) noexcept override;

    /// Consumer thread. Every frame, for the trace and the readouts.
    bool popFrame(EngineFrame& out) noexcept { return frames_.tryPop(out); }
    /// Output thread. Only the beats.
    bool popBeat(EngineBeat& out) noexcept { return beats_.tryPop(out); }

    /// The latest tempo, lock and meter, consistently, without draining anything — for
    /// a status line or a UI that redraws on its own clock.
    tracking::TempoState state() const noexcept { return state_.load(); }

    /// §5.8's intensity and onset count, for the output thread's rules and §5.6's
    /// `/takt4/intensity`. See `EngineIntensity` for why it is published separately.
    EngineIntensity intensity() const noexcept { return intensity_.load(); }

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

    /// The decoder, for reading. Only safe to write through while stopped, or from the
    /// inference thread; the commands are the way in otherwise.
    const tracking::BeatDecoder& decoder() const noexcept { return *decoder_; }
    tracking::Decoder decoderKind() const noexcept { return decoderKind_; }
    /// One frame of the decoder's clock, in seconds: what `EngineFrame`s and beats are
    /// timed in. 0.02 for the particle filter, 0.01 for the forward filter.
    double secondsPerFrame() const noexcept { return decoder_->secondsPerFrame(); }
    /// Decoder frames per network activation: one, or two when the decoder runs at twice
    /// the network's rate, the second being interpolated between the last two activations.
    std::size_t stepsPerActivation() const noexcept { return stepsPerActivation_; }

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
    /// The worst and the mean time the inference thread has taken over one activation —
    /// every decoder step it took for it — in microseconds. One activation is 20000 µs
    /// of audio.
    double worstFrameMicros() const noexcept {
        return worstFrameMicros_.load(std::memory_order_relaxed);
    }
    double meanFrameMicros() const noexcept;
    /// Whether the inference thread got a raised priority when it started (the audit's M13;
    /// see rt/thread_priority.hpp), as the thread itself saw it. False before it has started.
    bool workerRaised() const noexcept { return workerRaised_.load(std::memory_order_acquire); }

private:
    void run() noexcept;
    /// One activation through the decoder and the tempo machine — as many decoder steps
    /// as the decoder's rate asks for. On the inference thread.
    void track(const model::FrameActivation& activation) noexcept;
    /// One decoder step.
    void trackOne(const model::FrameActivation& activation, bool interpolated) noexcept;
    /// Everything posted since the last frame, in order. On the inference thread, or on
    /// the caller's while the engine is stopped — never on both at once.
    void applyCommands() noexcept;
    /// Hands the tracker's window to a decoder that takes one. After any command, because
    /// a tap moves the window.
    void syncDecoder() noexcept;

    const tracking::StateSpaceModel* model_;
    // On the heap: ActivationEngine carries 130 KB of rings and the network's weights.
    std::unique_ptr<model::ActivationEngine> activations_;
    tracking::Decoder decoderKind_;
    /// Before `tempo_`, which is built at its frame rate.
    std::unique_ptr<tracking::BeatDecoder> decoder_;
    tracking::TempoTracker tempo_;
    std::size_t stepsPerActivation_ = 1;
    /// `Options::noSignalBelowDb` as an RMS, and `noSignalSeconds` in decoder frames.
    float noSignalRms_ = 0.0f;
    std::uint64_t noSignalFrames_ = 1;
    /// Decoder frames in a row below that level, and whether they have reached the count.
    /// Only the inference thread touches these.
    std::uint64_t quietFrames_ = 0;
    bool noSignal_ = false;
    /// The activation before this one, to interpolate from. Only the inference thread
    /// touches these.
    model::FrameActivation previous_;
    bool havePrevious_ = false;
    /// The operator pinned before there was a lock; the decoder's tempo hold is taken on the
    /// frame one arrives. See `Command::Kind::SetLockPinned` in `applyCommands`.
    bool pinHoldPending_ = false;

    rt::SpscRing<EngineFrame, kFrameQueueCapacity> frames_;
    rt::SpscRing<EngineBeat, kBeatQueueCapacity> beats_;
    rt::Published<tracking::TempoState> state_;
    rt::Published<EngineIntensity> intensity_;
    rt::Published<tracking::TempoTracker::Options> options_;
    /// Running count behind `EngineIntensity::onsets`; only the inference thread touches it.
    std::uint64_t onsets_ = 0;

    ControlQueue controls_;
    /// Drained into, and reused, so applying commands allocates nothing after the first.
    std::vector<Command> commands_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    /// See `runNumber`. Written by `start` before the worker exists; read by it and by consumers.
    std::atomic<std::uint64_t> run_{0};
    std::atomic<std::uint64_t> framesTracked_{0};
    std::atomic<std::uint64_t> framesDropped_{0};
    std::atomic<std::uint64_t> beatsCalled_{0};
    std::atomic<std::uint64_t> beatsDropped_{0};
    std::atomic<double> worstFrameMicros_{0.0};
    std::atomic<double> totalFrameMicros_{0.0};
    std::atomic<std::uint64_t> framesTimed_{0};
    std::atomic<bool> workerRaised_{false};
};

} // namespace takt4::engine
