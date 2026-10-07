#pragma once

#include "core/audio/hop_processor.hpp"
#include "core/audio/host_time.hpp"
#include "core/audio/rates.hpp"
#include "core/features/feature_extractor.hpp"
#include "core/features/intensity.hpp"
#include "core/model/beat_model.hpp"
#include "core/model/weights.hpp"
#include "core/rt/spsc_ring.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace takt4::model {

/// What the network made of one feature frame, on its way to the tracker and the UI.
struct FrameActivation {
    std::uint64_t frameIndex = 0; ///< madmom's frame number: centred on sample 441 · k
    std::uint64_t hopIndex = 0;   ///< the hop whose arrival completed the frame
    /// When the audio this frame is centred on was at the input, on the host clock the output
    /// transports use. Zero when no HostTimeSource is installed, which is the
    /// case offline and in every test that does not need it.
    std::int64_t hostMicros = 0;
    float beat = 0.0f;
    float downbeat = 0.0f;
    float nonBeat = 0.0f;

    /// HANDOFF §5.8's intensity classifier, riding along with the activation because it is
    /// made from the same feature frame and on the same thread — see
    /// `features::IntensityClassifier`, which reuses the difference half of the frame and so
    /// costs 144 additions rather than a second FFT.
    ///
    /// Here rather than further down the chain because this is the last place the frame
    /// exists. `tracking::TrackedFrame` is the particle filter's output and has no business
    /// carrying it, and the output thread never sees a frame at all.
    features::Intensity intensity = features::Intensity::Normal;
    /// True on a frame the classifier called a flux peak — §5.8's *"on onset"*.
    bool onset = false;
    /// The raw spectral flux, unsmoothed. For a diagnostic trace; nothing should threshold
    /// it directly, because its scale is a fact about the master rather than about the music.
    float flux = 0.0f;
    /// The RMS of the hop that completed this frame, as a fraction of full scale. Unlike the
    /// flux this *is* an absolute level, and it is thresholded on purpose: it is how the engine
    /// knows the input has no signal at all (`BeatEngine::Options::noSignalBelowDb`), which the
    /// network cannot say — it reads P(beat) 0.46 to digital zeros.
    float rms = 0.0f;
};

/// The live half of Phase 3 (HANDOFF §8): hops in from the audio callback, class
/// probabilities out at 50 Hz.
///
/// Per §6's ruling the audio thread does nothing but copy each hop into a lock-free
/// ring; a worker thread drains it and runs the feature front end and the model. The
/// audio path therefore cannot be blamed for a dropout, at the cost of a few
/// milliseconds — on top of the 40 ms that madmom's centred framing costs inherently,
/// which the latency-offset slider (§5.5) is there to compensate.
///
/// The worker polls rather than waiting on a condition variable: signalling one from
/// the audio thread would mean a syscall on the thread that must not make any.
///
/// One producer (the audio thread, through processHop) and one consumer (whoever calls
/// pop) — SpscRing's contract. Neither side ever blocks the other; a reader that falls
/// behind loses activations and is told how many.
class ActivationEngine final : public audio::HopProcessor {
public:
    /// Hops the audio thread may run ahead by: 1.28 s, far more than the worker can
    /// fall behind without something being badly wrong, and 113 KB of ring.
    static constexpr std::size_t kHopQueueCapacity = 64;
    /// Activations a reader may fall behind by: 10 s at 50 Hz.
    static constexpr std::size_t kActivationQueueCapacity = 512;
    /// How long the worker sleeps when the hop queue is empty.
    static constexpr std::chrono::microseconds kIdleSleep{2000};

    /// Copies the weights in; the ModelWeights need not outlive this. Does not start
    /// the worker.
    explicit ActivationEngine(const ModelWeights& weights);
    ~ActivationEngine() override;

    ActivationEngine(const ActivationEngine&) = delete;
    ActivationEngine& operator=(const ActivationEngine&) = delete;

    /// Installs the clock that stamps each activation with the host time of the audio it
    /// was made from. It is called from the audio thread — told of every
    /// buffer (`beginBuffer`) and asked about every hop — and must outlive the stream. Null —
    /// the default — leaves `hostMicros` at zero, which is what offline runs and most tests
    /// want.
    ///
    /// Atomic because the audio thread reads it, but that only makes the swap itself
    /// safe: the source has to be installed before the stream is started and cleared
    /// after it is stopped, or a hop could still be in the callback with the old one.
    void setHostTimeSource(audio::HostTimeSource* source) noexcept {
        hostTime_.store(source, std::memory_order_relaxed);
    }

    /// Clears both queues, resets the front end and the model's LSTM state, and starts
    /// the worker. Call before the stream is started, never while it is running.
    void start();

    /// Stops the worker and waits for it. Safe to call twice.
    void stop() noexcept;

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Audio thread. Copies the hop into the queue and returns; counts a drop if the
    /// worker has fallen an entire queue behind. A sample that is not a finite number is
    /// copied as silence and counted — see `samplesRepaired`.
    void processHop(const float* hop, std::uint64_t hopIndex) noexcept override;
    /// Audio thread. Tells the host time source where this buffer's first sample is and when it
    /// was heard, and keeps the audio lost so far for the hops it completes. See `HopProcessor`.
    void beginBuffer(double firstSample, std::int64_t steadyMicros,
                     double lostSamples) noexcept override;

    /// Reader thread. False when nothing is queued.
    bool pop(FrameActivation& out) noexcept { return activations_.tryPop(out); }

    /// Hops queued but not yet worked. Exact when read by the audio thread or the
    /// worker; a snapshot from anywhere else.
    std::size_t hopsPending() const noexcept { return hops_.size(); }

    std::uint64_t hopsQueued() const noexcept {
        return hopsQueued_.load(std::memory_order_relaxed);
    }
    std::uint64_t hopsDropped() const noexcept {
        return hopsDropped_.load(std::memory_order_relaxed);
    }
    std::uint64_t framesEmitted() const noexcept {
        return framesEmitted_.load(std::memory_order_relaxed);
    }
    std::uint64_t framesDropped() const noexcept {
        return framesDropped_.load(std::memory_order_relaxed);
    }
    /// Samples that arrived as NaN or an infinity and went on as silence. **One would have
    /// ended the run:** the network is recurrent, so a NaN in its state is a NaN in every
    /// frame after it, and the intensity state freezes the same way — beats stopped until
    /// Stop and Start (the audit's M2). A loopback or a virtual cable can hand one over.
    std::uint64_t samplesRepaired() const noexcept {
        return samplesRepaired_.load(std::memory_order_relaxed);
    }

    /// The Phase 3 real-time check (§8): the worst the worker has taken over one hop,
    /// and over the model alone, in microseconds. One hop is 20000 µs of audio.
    double worstHopMicros() const noexcept {
        return worstHopMicros_.load(std::memory_order_relaxed);
    }
    double worstModelMicros() const noexcept {
        return worstModelMicros_.load(std::memory_order_relaxed);
    }

    /// The same, averaged over every hop the worker has taken. Zero before the first.
    double meanHopMicros() const noexcept;

    /// Runs one queued hop on the calling thread, for tests and offline use; returns
    /// false when the queue is empty. Only valid while the worker is not running.
    bool step() noexcept;

    /// What the worker got from the scheduler and the FPU when it started (the audit's M13;
    /// see rt/thread_priority.hpp), as the worker itself saw it. False before it has started.
    bool workerRaised() const noexcept { return workerRaised_.load(std::memory_order_acquire); }
    bool workerFlushesDenormals() const noexcept {
        return workerFlushes_.load(std::memory_order_acquire);
    }

private:
    struct QueuedHop {
        std::uint64_t index = 0;
        std::int64_t hostMicros = 0; ///< when its first sample was heard; 0 with no source
        std::array<float, audio::kHopSize> samples{};
    };

    void run() noexcept;
    void process(const QueuedHop& hop) noexcept;
    static void recordWorst(std::atomic<double>& worst, double micros) noexcept;

    features::FeatureExtractor extractor_;
    features::IntensityClassifier intensity_;
    BeatModel model_;
    std::atomic<audio::HostTimeSource*> hostTime_{nullptr};
    /// The audio lost so far, as internal-rate samples, which every hop's sample time is moved
    /// past — and, from the last time it grew, where that was and what it had been. A hop whose
    /// first sample came before the gap was heard before it, and is stamped with what was lost
    /// before, though the buffer that finishes it comes after: the frame it completes is centred
    /// two hops before the gap and was stamped the whole gap late without this. The audio
    /// thread's alone; reset with the stream by `start`.
    double lostSamples_ = 0.0;
    double lostBefore_ = 0.0;
    double gapSample_ = 0.0;
    rt::SpscRing<QueuedHop, kHopQueueCapacity> hops_;
    rt::SpscRing<FrameActivation, kActivationQueueCapacity> activations_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> hopsQueued_{0};
    std::atomic<std::uint64_t> hopsDropped_{0};
    std::atomic<std::uint64_t> framesEmitted_{0};
    std::atomic<std::uint64_t> framesDropped_{0};
    std::atomic<std::uint64_t> samplesRepaired_{0};
    std::atomic<double> worstHopMicros_{0.0};
    std::atomic<double> worstModelMicros_{0.0};
    std::atomic<double> totalHopMicros_{0.0};
    std::atomic<std::uint64_t> hopsWorked_{0};
    std::atomic<bool> workerRaised_{false};
    std::atomic<bool> workerFlushes_{false};
};

} // namespace takt4::model
