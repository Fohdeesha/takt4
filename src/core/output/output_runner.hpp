#pragma once

#include "core/engine/beat_engine.hpp"
#include "core/output/transports.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace takt4::output {

/// HANDOFF §4.2's output thread.
///
/// §4.2 wants the transports off the audio thread *and* off the caller's loop, so that
/// neither a UI redraw nor a console print can decide when a beat reaches a peer. The
/// tracking already does not wait on anyone — the engine's inference thread runs at the
/// audio's pace — so what this buys is punctuality: a MIDI clock tick is 19 ms apart at
/// 130 BPM, and a window that redraws at 30 Hz would hand it 33 ms of jitter.
///
/// **This is the single consumer of `BeatEngine::popBeat`.** That ring is single-consumer
/// by `rt::SpscRing`'s contract, so nothing else may drain it while this runs. The *frame*
/// ring is a different ring with a different consumer — §5.9's activation trace — and is
/// not touched here.
///
/// `takt4-cli track` obeys that already. §5.9's window still drains beats and throws them
/// away, purely so the ring does not fill, and **that loop has to go the moment the window
/// owns one of these**; `ui::WindowController::tick` says so at the point it happens.
///
/// The caller still owns two things that have to happen around it:
///
///   * `engine.setHostTimeSource(transports.link())` **before the engine is started**,
///     because §4.3's stamp is taken on the audio thread and there has to be a clock in
///     place before there is one.
///   * Draining `popFrame`, if anything wants the frames. Nobody has to, but a ring that
///     nobody drains fills and the engine starts counting frames lost.
class OutputRunner {
public:
    /// Called on the output thread for every beat, after the transports have had it — a
    /// console that prints them, a UI that flashes on one. Keep it short: it runs between
    /// a beat and the next MIDI tick, and a slow one is jitter.
    using BeatObserver = std::function<void(const engine::EngineBeat&)>;

    /// How often the thread wakes. A MIDI clock tick is 19 ms apart at 130 BPM, so the
    /// loop has to come round well inside that or the ticks inherit its period as jitter.
    static constexpr std::chrono::milliseconds kPeriod{1};

    /// Both must outlive this. Nothing is sent until `start()`.
    OutputRunner(engine::BeatEngine& engine, Transports& transports);
    ~OutputRunner();

    OutputRunner(const OutputRunner&) = delete;
    OutputRunner& operator=(const OutputRunner&) = delete;

    /// Set before `start()`; the thread reads it without synchronisation.
    void setBeatObserver(BeatObserver observer);

    /// Enables the transports and starts the thread. `start()` on a running runner does
    /// nothing. The seconds-since-start clock the transports are given begins here.
    void start();

    /// Stops the thread, drains whatever was still on the ring on the calling thread —
    /// the last beats of a set are still beats — and then stops the transports. Safe to
    /// call twice, and called by the destructor.
    void stop() noexcept;

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Rounds the loop has made. What a test watches to tell "the thread is running" from
    /// "the thread does the right thing", which are two separate claims.
    std::uint64_t rounds() const noexcept { return rounds_.load(std::memory_order_relaxed); }

    /// Rounds that threw. A transport failing must not take the process down — an OSC
    /// target can go away mid-set — but it must not be silent either.
    std::uint64_t errors() const noexcept { return errors_.load(std::memory_order_relaxed); }

    /// Seconds since `start()`, on the steady clock the transports are driven from.
    double elapsed() const noexcept;

private:
    void run() noexcept;
    /// One round: every beat waiting, then the clock. On the output thread, or on the
    /// caller's in `stop()` once the thread has been joined — never on both at once.
    void drainOnce(double now);

    engine::BeatEngine& engine_;
    Transports& transports_;
    BeatObserver observer_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> rounds_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::chrono::steady_clock::time_point started_{};
    /// Whether this runner is the one holding the platform's timer resolution up.
    bool raisedTimer_ = false;
};

} // namespace takt4::output
