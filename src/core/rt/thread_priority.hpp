#pragma once

// The scheduler's half of the real-time threads.
//
// takt4's own threads ran at the priority every thread starts with, the same as the window's:
// the model, the tracker and the output thread took their turn beside Slint and Skia drawing
// the trace, and on a busy machine the round that sends a MIDI tick or a cue waited behind a
// redraw. Only Link's own threads asked Windows for better (MMCSS, "Distribution"). This asks
// for the model, the tracker and the output thread, and undoes it when the thread is done.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace takt4::rt {

/// What a thread does, which decides what it asks the scheduler for.
enum class ThreadWork : std::uint8_t {
    /// A millisecond's work every millisecond, where a late round is a late MIDI tick and a
    /// late cue: the output thread. MMCSS "Pro Audio", at normal priority within it. That is
    /// the class PortAudio's WASAPI host gives its audio thread only in exclusive mode; in
    /// shared mode, which is how takt4 opens WASAPI, that thread asks for "Audio" — so on a
    /// WASAPI input the output thread is scheduled *ahead* of the audio callback, not behind it
    /// as this said. An ASIO driver's callback
    /// thread is the driver's own business.
    Output,
    /// A few milliseconds of arithmetic per 20 ms hop, where falling behind loses hops: the
    /// model and the tracker. MMCSS "Audio", which is scheduled ahead of ordinary threads and
    /// behind "Pro Audio".
    Compute,
};

/// For as long as it lives, the calling thread is scheduled as `work` asks. Construct it at
/// the top of the thread's own function; it must be destroyed on the same thread.
///
/// On macOS the output thread takes Mach's time-constraint policy — what Core Audio's own
/// threads run under, and no privilege asked — and the model and the tracker the user-interactive
/// quality of service, which keeps them on an Apple Silicon Mac's performance cores.
///
/// Where the platform will not — the MMCSS service stopped, or Linux, where it needs a privilege
/// an application has not got — it falls back to a raised ordinary priority, and failing that
/// does nothing: a thread at the default priority is what takt4 always was, so none of this is
/// worth refusing to run over.
class PriorityScope {
public:
    explicit PriorityScope(ThreadWork work) noexcept;
    ~PriorityScope();

    PriorityScope(const PriorityScope&) = delete;
    PriorityScope& operator=(const PriorityScope&) = delete;

    /// Whether MMCSS took the thread. False with a fallback priority, or none.
    bool multimedia() const noexcept { return task_ != nullptr; }
    /// Whether the thread's priority is above where it started, by either means.
    bool raised() const noexcept { return raised_; }

private:
    void* task_ = nullptr;
    /// Windows: the priority before a fallback, and whether there was one. macOS: the quality of
    /// service before, and whether it was raised. Unused on Linux.
    [[maybe_unused]] int previousPriority_ = 0;
    [[maybe_unused]] bool fallback_ = false;
    /// macOS: the time-constraint policy taken.
    [[maybe_unused]] bool timeConstrained_ = false;
    bool raised_ = false;
};

/// **The whole process at full speed, whatever its window is doing.** Windows' power throttling
/// opted out of, both kinds: the execution-speed kind, which runs a process whose window is
/// minimised or hidden on efficient cores at low clocks; and, from Windows 11, the timer kind,
/// under which a `timeBeginPeriod(1)` from a process whose window is minimised or covered is not
/// honoured — so every millisecond round of the output thread became 15.6 ms, the MIDI clock's
/// ticks went in 15.6 and 31.2 ms gaps, and Art-Net fell to about 32 frames a second. Once, early,
/// by the program (`ui::run`, the console).
///
/// Returns whether Windows took both. One too old to know the timer kind is asked for the other
/// alone, and one that knows neither throttles nothing, so the answer is something to know, not
/// a reason to stop.
bool keepFullSpeed() noexcept;

/// **How late a thread's rounds came**, kept by its `RoundTimer` and read from any thread.
///
/// A round is late in one of two ways, and they are different faults. The thread **woke** late for
/// it: it asked to be woken and the machine let it sleep on — a busy core, a virtual machine
/// paused. Or the round **ran** long once it had woken: the thread's own work, or the machine
/// taking the core from it partway through, which nothing here can tell apart. A cue that went
/// late is put down to one or the other by these: one went out 141 ms late on a CI machine, and
/// the test that caught it could not say whether takt4 or the machine had held it.
class RoundLateness {
public:
    /// A round that woke or ran `kNoted` or more late, on the steady clock: when the thread should
    /// have woken for it — its moment, or when the wait began if that was later, since a thread
    /// already behind does not sleep — when it woke, and when the round was done.
    struct Late {
        std::chrono::steady_clock::time_point due;
        std::chrono::steady_clock::time_point woke;
        std::chrono::steady_clock::time_point done;
    };
    using Observer = std::function<void(const Late&)>;
    /// What a round has to be late by to reach the observer: a round on Windows wakes a tick of
    /// the system timer late every time (see `RoundTimer`), half a millisecond on the rig.
    static constexpr std::chrono::milliseconds kNoted{1};

    /// Both longest back to nothing. Before the thread starts.
    void reset() noexcept;
    /// Called on the thread after each round late by `kNoted` or more, woken or run. Set before
    /// the thread starts. What a timing test puts a late cue down to; nothing in takt4 sets it.
    void setObserver(Observer observer) { observer_ = std::move(observer); }
    /// The longest the thread has slept past when it should have woken, and the longest a round
    /// has run, since `reset`. Any thread.
    double longestWakeSeconds() const noexcept { return wake_.load(std::memory_order_relaxed); }
    double longestRoundSeconds() const noexcept { return round_.load(std::memory_order_relaxed); }

private:
    friend class RoundTimer;
    /// On the thread, for every round.
    void note(const Late& round) noexcept;

    std::atomic<double> wake_{0.0};
    std::atomic<double> round_{0.0};
    Observer observer_;
};

/// The output thread's wait between rounds: a round every millisecond, a thousand a second.
///
/// **On a grid, not a millisecond from whenever the last round finished.** Every wait Windows
/// offers ends on a tick of a timer, and after the time asked, not on it: `sleep_for(1ms)` under
/// `timeBeginPeriod(1)` was measured on the rig at a round every 1.47 ms, 682 a second, and a
/// high-resolution waitable timer asked for 1 ms at 1.50 (the rig's system timer at 0.5 ms; a
/// tick late, every time). Asked for what is left until the next millisecond of the grid, a wait
/// that ends a tick late is a round a tick late, not a round a tick longer — and the rounds come a
/// thousand a second. A round more than one period behind starts the grid again from where it is,
/// so a stall is not made up for with a burst.
///
/// The high-resolution timer (Windows 10 1803 and later) because from Windows 11 a process whose
/// window is minimised or covered may not be given the timer resolution it asked for, and it is
/// not used by that timer. Where one cannot be made, the wait is a sleep until the same moment.
class RoundTimer {
public:
    /// `lateness`, where given, is told of every round from the second on (the first has no moment
    /// it was due at), and has to outlive this.
    explicit RoundTimer(RoundLateness* lateness = nullptr) noexcept;
    ~RoundTimer();

    RoundTimer(const RoundTimer&) = delete;
    RoundTimer& operator=(const RoundTimer&) = delete;

    /// Waits until the next round is due: `period` after the last one was.
    void wait(std::chrono::microseconds period) noexcept;
    /// Whether the waits are the high-resolution timer's.
    bool precise() const noexcept { return timer_ != nullptr; }

private:
    /// The wait ending: the moment the round now beginning woke, kept for `lateness_`.
    void noteWoke() noexcept;

    void* timer_ = nullptr;
    /// When the round being waited for is due; unset before the first wait.
    std::chrono::steady_clock::time_point next_{};
    bool started_ = false;
    RoundLateness* lateness_ = nullptr;
    /// The round running between waits: when the thread should have woken for it, and when it
    /// did. Unset before the first wait.
    std::chrono::steady_clock::time_point due_{};
    std::chrono::steady_clock::time_point woke_{};
};

/// Flush-to-zero and denormals-are-zero on the calling thread, for as long as it lives, and
/// the thread's previous setting back after.
///
/// A denormal — a float too small for its exponent — can take a slow path through the FPU
/// many times the cost of an ordinary one, and a recurrent network whose state decays through
/// that range in a quiet passage would spend its hops there. **Insurance, not a measured
/// gain:** takt4's network was timed over the synthetic excerpt and two minutes of digital
/// silence after it, 2026-09-24, and took about 80 us a hop with this and without it. Treating
/// denormals as zero costs nothing a listener could hear: they are below 1.2e-38. Only where
/// the arithmetic is the network's (`ActivationEngine`). Not on the tracker, whose
/// probabilities can be legitimately tiny before they are normalised. Does nothing where the
/// processor is not x86.
class DenormalsAsZero {
public:
    DenormalsAsZero() noexcept;
    ~DenormalsAsZero();

    DenormalsAsZero(const DenormalsAsZero&) = delete;
    DenormalsAsZero& operator=(const DenormalsAsZero&) = delete;

    /// Whether the calling thread flushes denormals to zero now.
    static bool active() noexcept;

private:
    [[maybe_unused]] std::uint32_t previous_ = 0; // x86's alone: Arm keeps the mode itself
};

} // namespace takt4::rt
