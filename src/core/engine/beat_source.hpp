#pragma once

#include "core/engine/control.hpp"
#include "core/features/intensity.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <atomic>
#include <cstdint>

namespace takt4::engine {

/// One beat, with everything an output transport needs and nothing it has to look up.
///
/// The state as it stood at *this* beat, so a consumer draining only beats never has to
/// pair them against anything else — and the beat's moment on Link's clock, which
/// `tracking::BeatEvent` cannot carry because the tempo state machine knows nothing about a
/// host clock.
struct EngineBeat {
    tracking::BeatEvent event;
    tracking::TempoState state;
    /// When the beat is, on Link's clock (`output::LinkSession::now`), in microseconds. For the
    /// tracker, the host time of the audio the beat was found in; for a source that
    /// follows a beat grid from elsewhere, the beat's own moment on that grid. Zero means no
    /// timeline at all — the tracker offline, with no HostTimeSource installed — and must never
    /// come from a live source: the output thread fires such a beat as it arrives and predicts
    /// nothing from it (`output::BeatScheduler::heard`).
    std::int64_t hostMicros = 0;
    /// The run of the source that called it (`BeatSource::runNumber`). A beat an earlier run left
    /// in the ring is its consumer's to drop: nothing else may drain the ring.
    std::uint64_t run = 0;
};

/// the intensity, as the *output* thread can read it.
///
/// It is computed per frame (`features::IntensityClassifier`, on the model worker) and rides
/// each `model::FrameActivation` — but the frame ring has exactly one consumer and that is
/// the trace, so the output thread cannot have it that way. This is published instead,
/// like the tempo state beside it.
///
/// It is **not** part of `tracking::TempoState`, deliberately. That is what the tempo state
/// machine is saying, and intensity is not a tracking quantity; folding it in would make
/// `core/tracking` depend on the features and would put a number in the Phase 4 gate's
/// neighbourhood that has no business there.
struct EngineIntensity {
    features::Intensity level = features::Intensity::Normal;
    /// Onsets seen since the engine was reset. The output thread has no frames, so a count
    /// that moved is how it learns one happened — the *"on onset"*.
    std::uint64_t onsets = 0;
    /// The raw flux, for a diagnostic readout. See `FrameActivation::flux`.
    float flux = 0.0f;
};

/// **Where the beats come from**: everything the output thread, the window and a control
/// surface ask of whatever is calling them, and nothing more.
///
/// The tracker listening to the audio is one (`BeatEngine`). Following a beat grid from
/// somewhere else — the house DJ's Ableton Link session, a mixer's MIDI clock — is another.
/// Whatever fires the rig holds one of these and cannot tell which it has, so the rules, the
/// clocks, Link and the window's readouts work the same on both.
///
/// The beat ring has **one consumer**, by `rt::SpscRing`'s contract: `popBeat` is the output
/// thread's alone (`output::OutputRunner`). Everything else is any thread's but the audio one.
class BeatSource {
public:
    virtual ~BeatSource() = default;

    BeatSource(const BeatSource&) = delete;
    BeatSource& operator=(const BeatSource&) = delete;

    /// The next beat called, or false with none waiting. One consumer only: see the class.
    virtual bool popBeat(EngineBeat& out) noexcept = 0;
    /// The latest tempo, lock and bar, consistently, without draining anything — for a status
    /// line, a window that redraws on its own clock, or the rules between beats.
    virtual tracking::TempoState state() const noexcept = 0;
    /// The intensity and the onset count, for the rules that watch them and the design's
    /// `/takt4/intensity`.
    virtual EngineIntensity intensity() const noexcept = 0;
    /// Which run this is: one more at every start, and what each beat it calls is stamped with.
    virtual std::uint64_t runNumber() const noexcept = 0;
    /// **The way in** for a button, a key or a control surface: ÷2, ×2, DOWNBEAT, a tap, a pin,
    /// the settings. Applied by the source's own thread, between whatever it is doing. False
    /// when it was not taken — the queue is full, or this source cannot carry the command out.
    virtual bool post(const Command& command) = 0;
    /// Whether a beat reaches the output thread on its own moment — a grid followed from
    /// somewhere else, whose beats are sent as their moment passes — rather than a pipeline after
    /// it, as the tracker's are. The output thread then fires a beat ahead of being heard only
    /// for an output that wants it early (`output::BeatScheduler`).
    virtual bool beatsOnTime() const noexcept { return false; }

protected:
    BeatSource() = default;
};

/// The source the output thread, the window and the control surfaces all hold: whichever one
/// is selected, forwarded to.
///
/// **Selected only while the rig is stopped**, as the audio pickers are ("stop to change") —
/// the caller's rule, which the window keeps by locking its picker while running. Not for the
/// memory's sake: the selection is atomic, so the output thread reading it as it changes is no
/// race, and that thread is the only one that ever drains a ring, whichever source it belongs
/// to. For the rig's: STOP is posted to the output thread before the source changes — and applied
/// a round or so later, perhaps after it — and every beat a source calls carries the run it
/// belongs to, which the output thread checks against the selected source's own run before it
/// fires one. So no run of one source goes on with the beats of another.
///
/// Every source it can select has to outlive it and everything holding it: a source selected
/// away from can still be in use for a moment — the output thread inside its `popBeat`, a
/// control surface inside its `post`.
class BeatSourceSwitch final : public BeatSource {
public:
    explicit BeatSourceSwitch(BeatSource& initial) noexcept : selected_(&initial) {}

    /// See the class for when.
    void select(BeatSource& source) noexcept {
        selected_.store(&source, std::memory_order_release);
    }
    BeatSource& selected() const noexcept { return *selected_.load(std::memory_order_acquire); }
    bool isSelected(const BeatSource& source) const noexcept { return &selected() == &source; }

    bool popBeat(EngineBeat& out) noexcept override { return selected().popBeat(out); }
    tracking::TempoState state() const noexcept override { return selected().state(); }
    EngineIntensity intensity() const noexcept override { return selected().intensity(); }
    std::uint64_t runNumber() const noexcept override { return selected().runNumber(); }
    bool post(const Command& command) override { return selected().post(command); }
    bool beatsOnTime() const noexcept override { return selected().beatsOnTime(); }

private:
    std::atomic<BeatSource*> selected_;
};

} // namespace takt4::engine
