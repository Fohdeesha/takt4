#pragma once

// The scheduler's half of the real-time threads (the 2026-09-22 audit's M13).
//
// takt4's own threads ran at the priority every thread starts with, the same as the window's:
// the model, the tracker and the output thread took their turn beside Slint and Skia drawing
// the trace, and on a busy machine the round that sends a MIDI tick or a cue waited behind a
// redraw. Only Link's own threads asked Windows for better (MMCSS, "Distribution"). This asks
// for the model, the tracker and the output thread, and undoes it when the thread is done.

#include <cstdint>

namespace takt4::rt {

/// What a thread does, which decides what it asks the scheduler for.
enum class ThreadWork : std::uint8_t {
    /// A millisecond's work every millisecond, where a late round is a late MIDI tick and a
    /// late cue: the output thread. MMCSS "Pro Audio" — the class ASIO and WASAPI give their
    /// own audio threads — at normal priority within it, below the audio callback.
    Output,
    /// A few milliseconds of arithmetic per 20 ms hop, where falling behind loses hops: the
    /// model and the tracker. MMCSS "Audio", which is scheduled ahead of ordinary threads and
    /// behind "Pro Audio".
    Compute,
};

/// For as long as it lives, the calling thread is scheduled as `work` asks. Construct it at
/// the top of the thread's own function; it must be destroyed on the same thread.
///
/// Where the platform will not — the MMCSS service stopped, or not Windows — it falls back to
/// a raised ordinary priority, and failing that does nothing: a thread at the default priority
/// is what takt4 always was, so none of this is worth refusing to run over.
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
    int previousPriority_ = 0;
    bool fallback_ = false;
    bool raised_ = false;
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
    std::uint32_t previous_ = 0;
};

} // namespace takt4::rt
