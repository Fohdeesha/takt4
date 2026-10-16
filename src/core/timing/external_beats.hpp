#pragma once

#include "core/engine/beat_source.hpp"
#include "core/engine/control.hpp"
#include "core/rt/published.hpp"
#include "core/rt/spsc_ring.hpp"
#include "core/timing/beat_grid.hpp"
#include "core/timing/grid_source.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace takt4::timing {

/// **The beats of a grid followed from elsewhere** — the house DJ's Ableton Link session, a
/// mixer's MIDI clock — as a `engine::BeatSource`, so the output thread, the window and the
/// control surfaces take them exactly as they take the tracker's.
///
/// A thread of its own reads the source (`GridSource`) once a millisecond, on the same
/// high-resolution grid of rounds as the output thread, and turns what it says into beats
/// (`BeatGrid`): each one pushed onto the ring as its moment passes, stamped with that moment on
/// Link's clock. The published state is everything the tracker's is, filled in for a grid that is
/// given rather than heard — see `BeatGrid`, and:
///
///   * **confidence** is 1 while locked and 0 otherwise, so a rule's confidence condition is
///     always met while a source is followed;
///   * **intensity** stays Normal and no onset is ever counted: there is no audio to hear them in;
///   * **locked** is the source being there (and stays through a coast), and **acquired** the
///     first time it was since START or since it last went quiet.
///
/// **What a followed grid cannot carry out is refused, and counted** (`commandsRefused`): a
/// tapped tempo, a pin of the lock, a tempo hold — the tempo is the source's. ÷2, ×2 and DOWNBEAT
/// are taken; the tracker's settings are taken and change nothing here, since they are the
/// tracker's (the latency reaches the outputs through `output::OutputRunner::setLatencySeconds`).
class ExternalBeats final : public engine::BeatSource {
public:
    /// Beats a consumer may fall behind by — as many as the tracker's ring holds.
    static constexpr std::size_t kBeatQueueCapacity = 256;
    /// Beats the window may fall behind by: half a minute of them at 128 BPM, where it takes
    /// them fifty times a second.
    static constexpr std::size_t kDisplayCapacity = 64;

    /// **A beat as the window draws it** — on a ring of its own (`popDisplayBeat`), so the output
    /// thread's ring keeps its one consumer: when it was, and where in the bar.
    struct DisplayBeat {
        /// Its moment on Link's clock, as `engine::EngineBeat::hostMicros`.
        std::int64_t hostMicros = 0;
        /// 1 to beats per bar, or 0 while nothing has said where the bar is.
        std::uint32_t beatInBar = 0;
        bool downbeat = false;
        /// Run on at the last tempo, the source having gone (`BeatGrid::coasting`).
        bool coasted = false;
        /// The run it belongs to (`runNumber`): one from before the last START is not this run's.
        std::uint64_t run = 0;
    };
    /// How often the source is read — the output thread's own round.
    static constexpr std::chrono::milliseconds kPeriod{1};

    ExternalBeats();
    ~ExternalBeats() override;

    ExternalBeats(const ExternalBeats&) = delete;
    ExternalBeats& operator=(const ExternalBeats&) = delete;

    /// A new run, following `source` until `stop`: what START does. The presses made while
    /// stopped are dropped first (as `BeatEngine::start` drops them), the beat ring is left to its
    /// consumer — what the last run left on it is stamped with that run — and the source is
    /// begun on the worker thread, which is the only thread to touch it from here to its `end`.
    /// `source` has to outlive the run.
    void start(GridSource& source, const BeatGrid::Options& options);
    /// Ends the run: the source is ended on the worker thread, and the thread joined. Safe to
    /// call twice.
    void stop() noexcept;
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    // --- engine::BeatSource -----------------------------------------------------------------
    bool popBeat(engine::EngineBeat& out) noexcept override { return beats_.tryPop(out); }
    /// The next beat sent, for the window: every one `popBeat` hands the output thread, in
    /// order, on a ring the window alone takes from. A window that falls behind by more than
    /// `kDisplayCapacity` loses the newest (`displayDropped`).
    bool popDisplayBeat(DisplayBeat& out) noexcept { return display_.tryPop(out); }
    std::uint64_t displayDropped() const noexcept {
        return displayDropped_.load(std::memory_order_relaxed);
    }
    tracking::TempoState state() const noexcept override { return state_.load(); }
    engine::EngineIntensity intensity() const noexcept override { return {}; }
    std::uint64_t runNumber() const noexcept override {
        return run_.load(std::memory_order_acquire);
    }
    /// See the class for what is taken and what is refused.
    bool post(const engine::Command& command) override;
    /// Each beat is pushed as its moment passes — or, from the CDJs, as its packet comes, a
    /// moment after the tempo puts it — so the output thread has it on time, with its own place
    /// in the bar.
    bool beatsOnTime() const noexcept override { return true; }

    /// Commands refused because a followed grid cannot carry them out — a tap, a pin, a hold.
    std::uint64_t commandsRefused() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }
    /// Commands dropped because the queue was full.
    std::uint64_t commandsDropped() const { return controls_.dropped(); }
    /// Rounds the worker has made this run, beats it has sent, and beats it could not because
    /// the ring was full.
    std::uint64_t rounds() const noexcept { return rounds_.load(std::memory_order_relaxed); }
    std::uint64_t beatsSent() const noexcept { return sent_.load(std::memory_order_relaxed); }
    std::uint64_t beatsDropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    /// Whether the worker got a raised priority, as it saw it. False before it has started.
    bool workerRaised() const noexcept { return workerRaised_.load(std::memory_order_acquire); }
    /// Whether the source has gone and is being run on at its last tempo (`BeatGrid::coasting`).
    bool coasting() const noexcept { return coasting_.load(std::memory_order_relaxed); }

private:
    void run(GridSource& source, std::uint32_t beatsPerBar) noexcept;
    /// One reading of the source and what it makes. On the worker thread.
    void round(GridSource& source) noexcept;
    void applyCommands() noexcept;

    /// The worker's alone while it runs; reset by `start` before the worker exists.
    BeatGrid grid_;
    rt::SpscRing<engine::EngineBeat, kBeatQueueCapacity> beats_;
    rt::SpscRing<DisplayBeat, kDisplayCapacity> display_;
    rt::Published<tracking::TempoState> state_;
    engine::ControlQueue controls_;
    /// Drained into and reused, so a round allocates nothing after the first.
    std::vector<engine::Command> commands_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> run_{0};
    std::atomic<std::uint64_t> refused_{0};
    std::atomic<std::uint64_t> rounds_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> displayDropped_{0};
    std::atomic<bool> workerRaised_{false};
    std::atomic<bool> coasting_{false};
};

} // namespace takt4::timing
