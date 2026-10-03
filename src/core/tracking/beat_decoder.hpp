#pragma once

#include "core/tracking/tracked_frame.hpp"

#include <cstdint>

namespace takt4::tracking {

/// Which decoder turns the network's activations into beats. See `BeatDecoder`.
enum class Decoder : std::uint8_t {
    /// BeatNet+'s two-stage particle filter cascade, ported (`ParticleFilter`). What
    /// shipped until 2026-09-08, and what the Phase 4 parity gate still holds to upstream.
    ParticleFilter,
    /// An exact forward filter over madmom's joint bar-pointer state space
    /// (`ForwardFilter`). **The default** since 2026-09-08, when it met every criterion set
    /// for replacing the particle filter — Ballroom beat F 0.956 against the particle
    /// filter's 0.894 and downbeat F 0.943 against 0.850; on the electronic material
    /// `tools/refeval/gate.py` measures, beat F 0.895 against
    /// 0.864 and downbeat F 0.708 against 0.602 over the tracks the references agree on,
    /// timing no worse, and half the tempo jumps and a third fewer unlocks over 91
    /// minutes.
    Forward,
};

/// The thing between the network and `TempoTracker`: activations in, one `TrackedFrame`
/// out per frame of its own clock.
///
/// Two of them exist and the engine holds one through this. The particle filter is
/// upstream's algorithm and runs on the network's 50 Hz frames; the forward filter is an
/// exact Bayesian filter over the same kind of state space and runs at 100 Hz on
/// activations the engine interpolates — the finer grid measured
/// as the largest cheap gain there is. `secondsPerFrame()` is how a decoder says which,
/// and everything downstream — `TempoTracker`'s frame counts, the trace, a beat's time —
/// is derived from it rather than assumed.
///
/// The two optional abilities are the ones that belong *inside* the decoder because
/// they cannot be done well outside it: the operator's tempo window as evidence rather
/// than as a relabelling afterwards, and a tempo hold that tracks phase only. A decoder
/// that lacks one says so, and the engine leaves the corresponding
/// `TempoTracker` mechanism in place instead.
///
/// `process` and `reset` run on the inference thread and allocate nothing; construction
/// may allocate whatever it likes.
class BeatDecoder {
public:
    virtual ~BeatDecoder() = default;

    /// Back to the state a freshly built decoder is in.
    virtual void reset() noexcept = 0;

    /// One frame of class probabilities in, one decision out.
    virtual TrackedFrame process(float beatActivation, float downbeatActivation) noexcept = 0;

    /// One frame of this decoder's clock, in seconds.
    virtual double secondsPerFrame() const noexcept = 0;

    /// The name a status line prints.
    virtual const char* name() const noexcept = 0;

    /// Whether `setTempoWindow` does anything. When it does, `TempoTracker` is built with
    /// `Options::foldInDecoder` and folds nothing itself.
    virtual bool honoursTempoWindow() const noexcept { return false; }
    /// The operator's window as a prior inside the decoder: a tempo outside it has to keep
    /// out-arguing a per-frame penalty, and when the octave inside the window wins, the
    /// beats come out of the posterior on that grid with the posterior's own phase — the
    /// whole of what a post-hoc fold could not do. `enabled` false removes it.
    virtual void setTempoWindow(double /*minBpm*/, double /*maxBpm*/, bool /*enabled*/) noexcept {}

    /// Whether `holdTempo` does anything.
    virtual bool canHoldTempo() const noexcept { return false; }
    /// Pin the tempo and track phase only, which is what a DJ's beat grid is and the only
    /// thing that holds a track whose periodicities are in non-octave ratios. Zero releases.
    virtual void holdTempo(double /*bpm*/) noexcept {}

    /// **The input has had no signal for a while** (`BeatEngine::Options::noSignalSeconds`):
    /// the deck has stopped, not paused for a breakdown. Stop calling beats and forget the beat
    /// that was being followed, as if nothing had been heard yet, so that whatever plays next
    /// is listened to afresh. The window and a hold are settings and survive, and the frame
    /// count goes on. A decoder that never calls beats through silence need do nothing: the
    /// particle filter's information gate already takes nothing from it.
    virtual void silence() noexcept {}
};

} // namespace takt4::tracking
