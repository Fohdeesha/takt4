#pragma once

#include "core/tracking/particle_filter.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace takt4::tracking {

/// What the tracker is currently saying, whether or not a beat just happened.
struct TempoState {
    double bpm = 0.0;    ///< after octave folding, and held while unconfident
    double rawBpm = 0.0; ///< what the particle filter's cloud says, unfolded
    /// True once `bpm` is coming from the spacing of the beats themselves rather than
    /// from the particle cloud's tempo interval. See TempoTracker's header.
    bool refined = false;
    bool locked = false; ///< the tempo has agreed with itself long enough
    /// The operator has pinned the lock, so the hysteresis is not allowed to give it up.
    /// Published because a tempo that has stopped responding to the audio is alarming
    /// when nothing on screen says why.
    bool pinned = false;
    bool holding = false;          ///< confidence is below the gate; bpm is the last good one
    double confidence = 0.0;       ///< 0 to 1; see TempoTracker's header for what it measures
    std::uint32_t beatsPerBar = 0; ///< from the filter's downbeat stage, never assumed
    std::uint32_t beatInBar = 0;   ///< 1 on the downbeat, counting up; 0 before the first beat
    std::uint64_t bars = 0;        ///< downbeats seen since the last reset
    std::uint64_t beats = 0;       ///< beats seen since the last reset, downbeats included
};

/// One beat, as the output transports want it.
struct BeatEvent {
    std::uint64_t frameIndex = 0; ///< the activation frame it was called on
    double time = 0.0;            ///< seconds since the stream started, latency offset applied
    bool downbeat = false;
    std::uint32_t beatInBar = 0;
    std::uint32_t beatsPerBar = 0;
    double bpm = 0.0;
    bool locked = false;
    double confidence = 0.0;
    /// True on the one beat an operator's downbeat snap landed on. §5.6 reserves Link's
    /// `forceBeatAtTime` for exactly this beat — a `requestBeatAtTime` would be moved to
    /// where the session's phase already matches, which is the phase being corrected.
    bool snapped = false;
};

/// HANDOFF §5.5, the layer between the particle filter and the outputs.
///
/// "The tracker is perhaps 40% of perceived quality. This layer is most of the rest, and
/// it is where the existing tools are weakest." It does five things, all of them
/// user-configurable:
///
///   * **Octave fold.** The filter tracks 55-215 BPM; an operator knows their material
///     sits in, say, 70-140. An estimate outside that is halved or doubled into it
///     before anything else looks at it, which is what stops a house set reading 170.
///     The window is a *preference*, not a fence: see `Options::foldHysteresis`.
///   * **Lock and hysteresis.** A tempo has to agree with itself for `lockAfter` frames
///     before it is called locked, and disagree for `unlockAfter` frames before that is
///     given up — never on one frame.
///   * **Confidence gate.** Below the threshold the last good tempo is held and `holding`
///     is set, rather than a wrong number being published. Silence beats wrong.
///   * **Latency offset.** Every beat's timestamp is moved by a fixed number of
///     milliseconds so downstream fires early enough to be in time.
///   * **Meter.** Taken from the filter's downbeat stage. Nothing here hardcodes 4.
///
/// It also fixes the one number the state space cannot give. madmom's tempo intervals
/// are whole 20 ms frames, so around 130 BPM the only values that exist are 130.43 and
/// 125.00 — a 5.4 BPM step, and 14 BPM up at 214. Nothing inside the filter can do
/// better than the nearer of the two. But the beats it calls are spaced 23, 24, 23, 23
/// frames apart, and the mean of those over a few bars resolves the tempo to a fraction
/// of a BPM. So once the tempo is locked, the published value comes from the beat
/// spacing, and the cloud's interval is used only to decide *which* tempo is being
/// tracked. On the synthetic excerpt that is the difference between reporting 130.4 and
/// reporting 128.0, which is what the drum machine actually plays.
///
/// **Once a lock has been earned, the published tempo only ever moves for a reason.** Three
/// of them, and no others: the beat-spacing refinement sharpening it within
/// `refineTempoTolerance`, a *different* tempo earning a lock of its own, or the operator
/// asking. In particular losing the lock does **not** move it — an unlock says the tracker
/// is no longer sure, not that it has a better answer, so what it publishes meanwhile is
/// the tempo that last earned one, with `locked` false to say so.
///
/// That rule is the answer to the report this layer was rebuilt for: *"the BPM is way too
/// eager to change hugely on mostly steady songs — as soon as the main beat drops it will
/// go from 140 to 90 in seconds, just because the clap stopped."* All three of the places
/// that came from were here rather than in the filter. Publishing the cloud's median while
/// hunting is one: the median is a whole-frame interval, so it steps by 5.4 BPM at 130 and
/// walks freely during a passage with nothing percussive in it, and the moment the lock
/// unwound that walk was on screen. The fold's discontinuity at its own edge is the second
/// (`Options::foldHysteresis`), and dropping from the refined tempo back to the coarse one
/// whenever a frame had too few usable beat gaps is the third (`refinedBpm_`).
///
/// Measured over the seventeen full tracks in `references/audio`, 68 minutes of the
/// material the report came from: **2737 tempo jumps of more than 2 % became 457** — and of
/// those 457, **316 are inside the first 5.5 seconds of a track**, which is the acquisition
/// the tracker is openly hunting through and says so. Past acquisition it is 141 against
/// 2737: one every 29 seconds where it had been one every 1.5. Unlocks, 221 to 122.
/// `tools/trace_stability.py` is the harness and
/// `tests/data/tracking/evaluation/README.md` has the working.
///
/// Confidence is how much of the beat particle cloud agrees with the tempo its own
/// median reports, smoothed over about a second. Upstream publishes no confidence at
/// all; this is the natural one, because it is exactly what falls apart when the tracker
/// is lost — the cloud spreads across tempi before the reported tempo starts jumping.
///
/// Deterministic, allocation-free and independent of wall-clock time: everything is
/// derived from the frame index the filter reports.
class TempoTracker {
public:
    struct Options {
        /// The octave-fold window. Estimates are doubled or halved into it. Set
        /// `octaveFold` false to publish whatever the filter says.
        double minBpm = 70.0;
        double maxBpm = 140.0;
        bool octaveFold = true;
        /// How far outside the window an estimate may stray before the fold picks a
        /// different octave, as a fraction of the window's edges.
        ///
        /// Without this the fold is *discontinuous at its own edges*, and that was the
        /// single largest source of "the BPM jumps by a factor of two". A 140 BPM track
        /// under the default 70-140 window sits exactly on the edge: the cloud's estimate
        /// wanders between 138 and 142 — which is one frame of the state space at that
        /// tempo, the smallest step it can take — and the published tempo flips 136 / 71 /
        /// 136 with it. Measured on `references/audio`'s "01 - Pirates": **fifty octave
        /// flips in 213 seconds**, in bursts of two a second, with confidence at 0.6 and
        /// the cloud's own tempo never moving more than 3 %.
        ///
        /// So the fold remembers the octave it chose and keeps it until the estimate
        /// leaves the window by this margin. Near an edge that publishes a tempo slightly
        /// outside the operator's window, and that is the right answer rather than a
        /// compromise: the window says which octave the material is in, and a track at 141
        /// under a 70-140 window is a 141 BPM track, not a 70.5 BPM one.
        ///
        /// 0.10 covers the state space's own resolution with room to spare — its step is
        /// about 4 % at 130 BPM and 6.5 % at 214 — so a one-interval wobble can never
        /// reach an edge from inside the window.
        double foldHysteresis = 0.10;

        /// Frames of agreement before the tempo is called locked, and of disagreement
        /// before that is given up. At 50 Hz these are 0.5 s and 1.5 s.
        std::size_t lockAfter = 25;
        std::size_t unlockAfter = 75;
        /// The *most* agreement a tempo can ever be asked for before it replaces the one
        /// being published. 150 is three seconds.
        ///
        /// Acquiring a tempo and replacing one are not the same question, and treating them
        /// as one is the last of the reasons a steady track's BPM would not sit still.
        /// Acquisition has nothing to argue with; replacement has however long the old tempo
        /// has already held, against a candidate agreed with for half a second. Measured
        /// over `references/audio` with everything else here in place, the lock still moved
        /// 173 times in 68 minutes and **the median time before it moved again was 7.1
        /// seconds** — it was going somewhere and coming back, which is not a tempo change,
        /// it is a passage the filter found hard.
        ///
        /// What a replacement actually costs is a *share of how long the incumbent has
        /// held*, bounded below by `lockAfter` and above by this. That proportionality is
        /// not decoration: charging the full price from birth made a bad first lock
        /// permanent, and Ballroom said so at once — 28 clips where the baseline published
        /// the right tempo went to a wrong faster one and stayed there, taking tempo
        /// accuracy 1 from 0.769 to 0.734. A lock two seconds old has earned nothing and is
        /// replaced for `lockAfter`; one that has run a whole track has earned all of this.
        ///
        /// The cost, at full price, is that a real change — a DJ blending into the next
        /// record — is followed `unlockAfter + relockAfter` frames after the cloud settles
        /// on it, 4.5 s rather than 2. A blend takes longer than that. A breakdown does not.
        std::size_t relockAfter = 150;
        /// How close two estimates have to be to count as the same *value*. The state space
        /// is discrete, so in practice this means "the same tempo interval". Used where an
        /// exact comparison is wanted — did a tap really move the tempo — rather than for
        /// the lock, which asks the question below instead.
        double lockToleranceBpm = 0.5;
        /// How far apart two estimates have to be before they are different *tempi*, as a
        /// fraction. This is what the lock is decided on.
        ///
        /// Two questions were being asked with one tolerance and they are not the same
        /// question. "Is this the same tempo interval?" is worth half a BPM. "Has the tempo
        /// changed?" is not: the state space's neighbouring intervals are 4.3 % apart at
        /// 130 BPM, so a cloud whose median hops between interval 22 and interval 23 — which
        /// is what a cloud sitting on a 133 BPM track does, continually — read as
        /// disagreement on every other frame, and no lock could survive a passage of it.
        ///
        /// Measured over `references/audio`: the median run of a constant cloud interval is
        /// **six frames**, and only 141 runs in 68 minutes reach three seconds. Anything
        /// that has to be sustained for seconds cannot be counted in consecutive frames of
        /// an exact match; it has to be counted in frames of the same tempo.
        ///
        /// 0.06 spans a whole-frame step anywhere the fold puts a tempo (2.3 % at 70 BPM,
        /// 5.1 % at 154) and nothing musically distinct: 130 against 96 is 26 % apart, and
        /// 130 against 120 is 8 %.
        double sameTempoTolerance = 0.06;

        /// Below this the published tempo is held and `holding` is set.
        double confidenceThreshold = 0.15;
        /// Time constant of the confidence smoother, in frames. 25 is half a second.
        double confidenceSmoothing = 25.0;

        /// Added to every beat's timestamp, to compensate for what happens downstream.
        /// Negative fires early, which is the useful direction.
        double latencyOffsetSeconds = 0.0;

        /// How many beat-to-beat gaps the refined tempo is averaged over, and how many of
        /// them have to survive outlier rejection before it is published at all. Twenty-four
        /// gaps is about six bars of 4/4.
        ///
        /// This was eight — two bars — and eight is not enough to average out the noise
        /// that is *inherent* in the gaps. The filter emits a beat when its median particle
        /// is anywhere in the first `kGatherSeconds` of one, which is four frames wide, so
        /// a called beat can be up to four frames late: at 130 BPM that is 17 % of a period,
        /// on every beat, before any question of the tracking being wrong. The mean of n
        /// consecutive gaps telescopes to the span over n, so its error falls as 1/n rather
        /// than 1/sqrt(n) — but at n = 8 that is still ±4 %, and the refinement was swinging
        /// across most of the ±10 % `refineTempoTolerance` band while *locked*: 610 moves of
        /// more than 2 % over `references/audio`, a median of 3.4 % and a worst of 13.6 %.
        std::size_t refineOverBeats = 24;
        std::size_t refineNeedsBeats = 8;
        /// How far a gap may be from the cloud's period and still be counted, and how far
        /// the refined tempo may be from the locked one before it is disbelieved. The
        /// first throws out a missed or doubled beat; the second stops a refinement ever
        /// silently becoming a different tempo.
        ///
        /// **Do not narrow this to reduce noise** — it was tried, and it made the tempo
        /// *wrong* rather than steadier: Ballroom's tempo accuracy 1 fell 0.769 to 0.735 at
        /// 0.15. The band is there to throw out a gap that is a missed beat (twice the
        /// period) or an invented one (half), which are 50 % and 100 % out. The ±17 % that
        /// a genuine gap carries at 130 BPM is the filter's four-frame gather window and is
        /// *symmetric noise to be averaged*, not error to be rejected — clipping it keeps
        /// only the gaps nearest the cloud's own quantised period and biases the mean back
        /// onto it, which is the one thing the refinement exists to escape. Average over
        /// more gaps instead; that is what `refineOverBeats` is for.
        double refineGapTolerance = 0.25;
        double refineTempoTolerance = 0.10;
        /// How much of the way to a newly measured spacing the published tempo moves, on
        /// each beat. The measurement is noisy by construction (see `refineOverBeats`), so
        /// following it exactly puts that noise on screen; this bounds one beat's worth of
        /// it to a quarter of the disagreement and costs about four beats of response,
        /// which the lock is not waiting on — a *tempo change* moves the lock, and moving
        /// the lock discards the refinement outright.
        double refineSmoothing = 0.25;
    };

    /// Two constructors rather than `Options options = {}`: a default argument is
    /// written inside the enclosing class but outside any member function, where a
    /// nested class's own default member initialisers are not yet available. MSVC
    /// accepts it; Clang rejects it, correctly.
    TempoTracker(double secondsPerFrame, Options options);
    explicit TempoTracker(double secondsPerFrame);

    /// Feeds one frame from the particle filter. Returns a beat if one was called.
    std::optional<BeatEvent> process(const TrackedFrame& frame) noexcept;

    void reset() noexcept;

    const TempoState& state() const noexcept { return state_; }
    const Options& options() const noexcept { return options_; }

    /// Changing the window or the gate takes effect on the next frame; the lock is kept
    /// unless the published tempo no longer folds into the new window.
    void setOptions(const Options& options) noexcept;

    /// Halves or doubles the published tempo, and the fold window with it if that is the
    /// only way the new tempo can survive folding. §5.5's manual octave shift.
    void halve() noexcept;
    void redouble() noexcept;

    /// §5.5's tap tempo, as a *seed*: the operator has said which tempo they mean, so the
    /// octave-fold window moves to an octave centred on it and any manual ×2 / ÷2 is
    /// cleared. `tracking::TapTempo` turns the taps themselves into this number.
    ///
    /// This is the half of §5.5's "seeds or overrides the tracker" that costs nothing and
    /// is unambiguously right. It is what fixes the failure §7 deviation 4 measured: a
    /// Quickstep at 204 BPM under a 70-140 window is folded to 102 and there is no way
    /// for the operator to say otherwise. Tapping it moves the window to 144-288 and the
    /// tracker publishes 204 from the very next frame.
    ///
    /// Moving the window rather than adding an octave shift is deliberate: the window is
    /// a setting the UI shows, so afterwards the readout and the setting agree on why the
    /// tempo is what it is. §8's Phase 5 asks for exactly that visibility.
    ///
    /// **Overriding** — free-running from the tap and ignoring the audio — is the other
    /// half of §5.5's sentence and is not here. It is a mode, not a setting: it needs its
    /// own beat generator, its own answer for downbeats and confidence, and a UI that
    /// says the tracker is not tracking. Nothing else can be built on top of a half of it.
    ///
    /// The lock is kept if the published tempo did not really move, as when changing the
    /// window by hand — a tap confirming what is already tracked must not cost sync.
    void seedTempo(double bpm) noexcept;

    /// §5.5's manual downbeat: the next beat called starts the bar, and every bar after
    /// it is counted from there.
    ///
    /// The rotation this sets up is **kept** until the next snap or a reset, rather than
    /// being given back on the filter's next downbeat call. §5.5 asks for this because
    /// "the best online downbeat tracker in the world scores 56% F1" — an operator taps
    /// precisely because the filter has the bar wrong, and a correction the filter undoes
    /// one bar later would not be a correction. What is handed back is the beat and tempo
    /// tracking, which never stopped; only the bar anchor is now the operator's.
    ///
    /// Beat times are untouched: this moves which beat is called number 1, never when a
    /// beat happens.
    void snapDownbeat() noexcept;

    /// §5.7's `/ctl/lock <0|1>`, which **pins the lock rather than setting it**.
    ///
    /// A one-shot `setLocked(true)` would not be a lock at all: the hysteresis that owns
    /// the flag would unwind it `unlockAfter` frames later — 1.5 s by default — and the
    /// operator who pressed it during a breakdown would watch it come undone. So this
    /// holds the flag up instead. While pinned the disagreement is still measured and
    /// simply never acted on, which is the difference between a pin and an override: the
    /// tracker goes on tracking, and letting go hands it straight back.
    ///
    /// What that buys is not the flag but what hangs off it. Once locked, the published
    /// tempo comes from the spacing of the beats themselves (see this class's header) and
    /// resolves to a fraction of a BPM; unlocked, it falls back to the state space's whole
    /// frame intervals, which near 130 BPM step by 5.4.
    ///
    /// Be precise about how much that is worth, because it is less than it first looks.
    /// An unlock is not ruinous on its own: `beatFrames_` survives it, so a few frames
    /// later the hysteresis re-locks on the new value and the refinement pulls the
    /// published tempo back to what the beats actually say. The coarse reading is a
    /// transient. What the pin really prevents is the lock *moving to a different tempo*
    /// and the refinement then re-anchoring around that — a cloud that sustainedly reads
    /// one frame long takes the published tempo with it, permanently, and a pin is the
    /// operator saying they know better.
    ///
    /// Pinning locks to whatever is showing, because that is what the operator is looking
    /// at when they press it. With nothing tracked yet there is nothing to pin: the pin is
    /// remembered and takes hold on the frame acquisition first locks — a pin cannot hold
    /// up a flag that was never raised.
    ///
    /// Releasing drops the lock with it and starts the hunt again from what is playing
    /// now. An operator letting go is saying "that tempo was wrong, look again", and a
    /// lock left standing afterwards would be the very thing they were trying to shed.
    ///
    /// It holds against the *hysteresis*, not against the operator's other controls. A
    /// fold window dragged until it no longer contains the pinned tempo still drops the
    /// lock, and should: the alternative is publishing a tempo the operator's own settings
    /// exclude. `reset()` clears the pin, because after a reseed it would be pinning
    /// nothing.
    void setLockPinned(bool pinned) noexcept;
    bool lockPinned() const noexcept { return lockPinned_; }

    /// The octave fold on its own, with no memory of what has been tracked: what a given
    /// tempo folds to from a standing start. For tests and for the UI to preview. The
    /// tracker itself folds *with* memory — see `Options::foldHysteresis`.
    double fold(double bpm) const noexcept;

private:
    /// Settles which octave the fold is in, from the cloud's *continuous* tempo. Separate
    /// from applying it because the two want different inputs: the octave is a question
    /// about roughly where the material sits, which the state space's whole-frame steps
    /// answer badly, while the lock is a question about *which interval*, which is exactly
    /// what they answer.
    void chooseOctave(double bpm) noexcept;
    /// A tempo arrived at some other way, put in the octave the fold has settled on. The
    /// beat-spacing refinement measures a period in frames, so it has to be moved into the
    /// same octave as the value it refines or it would be rejected as a disagreement.
    double inChosenOctave(double bpm) const noexcept;
    /// Forgets the octave the fold settled on, so the next frame decides afresh. Anything
    /// that moves the window or the operator's octave has to call this.
    void forgetFold() noexcept;

    void updateLock(double folded) noexcept;
    void rememberBeat(std::uint64_t frameIndex) noexcept;
    /// Advances the filter's own bar position and turns it into the published one,
    /// applying whatever a snap has rotated the bar by. Called once per emitted beat.
    void advanceBar(bool filterCalledDownbeat) noexcept;
    /// The mean beat-to-beat gap in frames, over the gaps within `refineGapTolerance` of
    /// `cloudIntervalFrames`. Zero when there are not enough of them.
    double refinedIntervalFrames(double cloudIntervalFrames) const noexcept;

    double secondsPerFrame_;
    Options options_;
    TempoState state_;

    double lockedBpm_ = 0.0;   ///< the discrete tempo the lock is on; state_.bpm may refine it
    double candidate_ = 0.0;   ///< the tempo currently being agreed with
    std::size_t agreeing_ = 0; ///< frames it has been agreed with for
    std::size_t disagreeing_ = 0;
    double smoothedConfidence_ = 0.0;
    bool everConfident_ = false;
    /// A lock has been earned at least once, so there is a tempo worth holding on to while
    /// the tracker hunts. Before that there is not, and the published tempo follows the
    /// cloud so something sensible is on screen during acquisition.
    bool everLocked_ = false;
    /// The last tempo the beat spacing gave, kept so a frame with too few usable gaps
    /// publishes it again rather than dropping back to the state space's coarse value.
    /// Those two differ by up to 5.4 BPM at 130, so alternating between them *is* a jump.
    double refinedBpm_ = 0.0;
    /// Frames the tempo now being published has held a lock for. What a challenger has to
    /// out-argue: see `Options::relockAfter`.
    std::size_t lockHeld_ = 0;
    /// The octave the fold has settled on, as a power of two, and whether it has settled
    /// on one yet. See `Options::foldHysteresis`.
    std::int64_t foldOctave_ = 0;
    bool foldChosen_ = false;
    bool lockPinned_ = false;      ///< the operator is holding the lock up; see setLockPinned
    std::int64_t octaveShift_ = 0; ///< manual ×2 (+1) and ÷2 (-1) steps, applied after folding

    /// The bar, in two parts: where the filter thinks we are, and how far the operator
    /// has rotated that. `state_.beatInBar` is the second applied to the first, so with
    /// no snap the published bar is the filter's, beat for beat.
    std::uint32_t filterBeatInBar_ = 0; ///< 1 on the filter's downbeat; 0 until it calls one
    std::uint32_t barOffset_ = 0;       ///< beats added to the filter's position
    bool snapPending_ = false;          ///< a snap waiting for the next beat to land on
    /// A snap that arrived before the filter had ever called a downbeat has nothing to
    /// rotate yet, so the bar runs from the snap itself until the filter does — and
    /// `sinceSnap_` is what turns that into an offset when it finally happens.
    bool snapAwaitingFilter_ = false;
    std::uint32_t sinceSnap_ = 0;

    /// The frames the last few beats were called on, oldest first. Bounded by
    /// `refineOverBeats + 1`, so this allocates once and never grows.
    std::vector<std::uint64_t> beatFrames_;
};

} // namespace takt4::tracking
