#include "core/output/output_runner.hpp"

#if defined(_WIN32)
#include <windows.h>
// timeapi.h must follow windows.h.
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace takt4::output {
namespace {

/// Windows' default timer granularity is 15.6 ms, and this loop wants 1 ms; without
/// raising it a `sleep_for(1ms)` is a `sleep_for(15.6ms)` and every MIDI tick inherits
/// that as jitter. Process-wide, reference-counted by the OS — so nesting inside a caller
/// that has already raised it is safe — and reverted when the runner stops.
bool raiseTimerResolution() noexcept {
#if defined(_WIN32)
    return ::timeBeginPeriod(1) == TIMERR_NOERROR;
#else
    return false;
#endif
}

void restoreTimerResolution(bool raised) noexcept {
#if defined(_WIN32)
    if (raised) {
        ::timeEndPeriod(1);
    }
#else
    (void)raised;
#endif
}

} // namespace

OutputRunner::OutputRunner(engine::BeatEngine& engine, Transports& transports)
    : engine_(engine), transports_(transports) {}

OutputRunner::~OutputRunner() {
    stop();
}

void OutputRunner::setBeatObserver(BeatObserver observer) {
    observer_ = std::move(observer);
}

double OutputRunner::elapsed() const noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

void OutputRunner::start() {
    if (running()) {
        return;
    }
    started_ = std::chrono::steady_clock::now();
    raisedTimer_ = raiseTimerResolution();
    // Before the thread: startOutputs enables Link and starts the MIDI clock, and the
    // thread's first round must not find them half up.
    transports_.startOutputs(0.0);
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run(); });
}

void OutputRunner::stop() noexcept {
    if (!worker_.joinable()) {
        return;
    }
    running_.store(false, std::memory_order_release);
    worker_.join();

    // The last beats of a set are still beats: the engine may have called one between the
    // final round and the join. Safe on this thread now — the only other consumer of that
    // ring has been joined.
    try {
        drainOnce(elapsed());
    } catch (...) {
        // Nothing useful to do while shutting down, and letting it out of a noexcept
        // function would call std::terminate.
        errors_.fetch_add(1, std::memory_order_relaxed);
    }
    transports_.stopOutputs();
    restoreTimerResolution(raisedTimer_);
    raisedTimer_ = false;
}

void OutputRunner::run() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        try {
            drainOnce(elapsed());
        } catch (...) {
            // A transport that throws must not take the process down with it: an OSC
            // target can go away mid-set, and Link's networking has its own opinions
            // about sockets. Counted rather than swallowed, so it is visible.
            errors_.fetch_add(1, std::memory_order_relaxed);
        }
        rounds_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(kPeriod);
    }
}

void OutputRunner::drainOnce(double now) {
    engine::EngineBeat beat;
    while (engine_.popBeat(beat)) {
        transports_.publish(beat.event, beat.hostMicros, now);
        if (observer_) {
            observer_(beat);
        }
    }
    // Every round, beat or no beat: the MIDI clock's 24 PPQN does not wait for one, and
    // OSC's state addresses are how a peer learns the tempo drifted.
    transports_.advance(now, engine_.state());
}

} // namespace takt4::output
