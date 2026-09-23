#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace takt4::tracking {

/// §5.5's tap tempo: taps in, a tempo out.
///
/// Deliberately not part of `TempoTracker`. That class is "deterministic, allocation-free
/// and independent of wall-clock time: everything is derived from the frame index the
/// filter reports", and a tap is the one input to the tracker that is not — it happens
/// when a person's finger does. Keeping the clock on this side leaves the tracker
/// reproducible frame for frame, and leaves this testable without a clock at all: the
/// caller passes the time, from whatever steady clock it already has.
///
/// The tempo is the **median** of the recent gaps, not their mean. An operator tapping
/// quarter notes who fumbles one tap leaves a single gap of half the period among
/// correct ones, and a mean over eight gaps would carry a fumble like that into the
/// published tempo; a median throws it away.
class TapTempo {
public:
    struct Options {
        /// A gap longer than this starts a fresh set rather than extending the last one.
        /// Two seconds is 30 BPM, well below anything the tracker follows, so it can only
        /// separate one attempt from the next.
        double timeoutSeconds = 2.0;
        /// Taps before a tempo is offered at all, and taps kept. Three taps is two gaps,
        /// which is the least that can be called a tempo; eight is about two bars of 4/4.
        std::size_t needTaps = 3;
        std::size_t keepTaps = 8;
        /// A tap closer than this to the one before is a **bounce** — a switch that chattered,
        /// a pad that retriggered, a key held down — and is ignored outright rather than
        /// counted. A tenth of a second is 600 BPM; nobody taps that, and a switch bounces in a
        /// few milliseconds. Without it two taps 20 ms apart were a tempo of 3000 BPM, and with
        /// the fold off that went straight to Link and the MIDI clock as an octave shift (the
        /// audit's H1).
        double bounceSeconds = 0.1;
        /// The tempi a set of taps may offer. Outside this nothing is offered and the set goes
        /// on counting: a tap tempo of 12 or 900 BPM is a set that went wrong, not a tempo.
        double minBpm = 30.0;
        double maxBpm = 300.0;
    };

    /// Two constructors rather than `Options options = {}`: a default argument is written
    /// inside the enclosing class but outside any member function, where a nested class's
    /// own default member initialisers are not yet available. MSVC accepts it; Clang
    /// rejects it, correctly.
    TapTempo();
    explicit TapTempo(Options options);

    /// One tap, at `seconds` on any steady clock — only the differences are used, so the
    /// epoch does not matter as long as it does not change. Returns a tempo once enough
    /// taps have landed close enough together, and nothing before that.
    ///
    /// A tap after the timeout, or one that goes backwards in time, starts again from
    /// this tap.
    std::optional<double> tap(double seconds) noexcept;

    void reset() noexcept;

    /// Taps in the current set, for a UI counting the operator in.
    std::size_t taps() const noexcept { return times_.size(); }
    /// The last tempo offered, or 0 before there is one. Cleared by a timeout.
    double bpm() const noexcept { return bpm_; }
    const Options& options() const noexcept { return options_; }

private:
    Options options_;
    /// The taps in the current set, oldest first, bounded by `keepTaps`.
    std::vector<double> times_;
    /// The gaps between them, reused so measuring allocates nothing.
    std::vector<double> gaps_;
    double bpm_ = 0.0;
};

} // namespace takt4::tracking
