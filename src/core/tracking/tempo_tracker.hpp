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
    /// The rate the filter is *calling* beats at: `rawBpm` moved by the octave the called
    /// beats sit from the cloud's tempo, which is the same number for a filter that agrees
    /// with itself and half of it for one that does not (`Options::beatOctaveBeats`). This
    /// and not `rawBpm` is what the fold and the lock work on, because it is the grid the
    /// outputs are fed from; `rawBpm` is kept for the readout that says what the cloud is
    /// doing.
    double calledBpm = 0.0;
    /// True once `bpm` is coming from the spacing of the beats themselves rather than
    /// from the particle cloud's tempo interval. See TempoTracker's header.
    bool refined = false;
    bool locked = false; ///< the tempo has agreed with itself long enough
    /// The operator has pinned the lock, so the hysteresis is not allowed to give it up.
    /// Published because a tempo that has stopped responding to the audio is alarming
    /// when nothing on screen says why.
    bool pinned = false;
    bool holding = false;          ///< confidence is below the gate; bpm is the last good one
    /// A lock has been earned since the run started or the input last had no signal. **Until
    /// then nothing fires the rig**: the output thread sends no beat to OSC, the rules or the
    /// clocks before it (the operator, 2026-10-03). A lock lost later does not clear it — an
    /// unlock in the middle of a track is the tracker being unsure, not the rig going dark.
    bool acquired = false;
    /// The input has had no signal for a while (`BeatEngine::Options::noSignalSeconds`). The
    /// lock is dropped with it and `acquired` cleared; the tempo shown is the last one, held.
    bool noSignal = false;
    double confidence = 0.0;       ///< 0 to 1; see TempoTracker's header for what it measures
    std::uint32_t beatsPerBar = 0; ///< from the filter's downbeat stage, never assumed
    std::uint32_t beatInBar = 0;   ///< 1 on the downbeat, counting up; 0 before the first beat
    std::uint64_t bars = 0;        ///< downbeats seen since the last reset
    std::uint64_t beats = 0;       ///< beats seen since the last reset, downbeats included
    /// How many times a DOWNBEAT press has declared that the bar began on the beat already
    /// called — a bar whose first beat went out as some other beat, so nothing that waits for
    /// a downbeat heard it (the audit's M4). The output thread fires that bar's rules when
    /// this moves, late by the press. `declaredBar` is the bar number the press made it.
    std::uint64_t barsDeclared = 0;
    std::uint64_t declaredBar = 0;
    /// How many of the filter's beats go to one published beat — `foldDivisor`. One unless
    /// the fold has put the published tempo below the filter's grid *and* is dividing it.
    ///
    /// Published because the case where it is **one while `bpm` is below `calledBpm`** is
    /// the one an operator cannot otherwise see: the readout says 92, the outputs fire at
    /// 184, and nothing on screen says the two disagree. That happens whenever the window
    /// folds a track the filter is double-timing and `Options::foldSupportFrames` has not
    /// been satisfied — which is every track whose cloud never visits the slower octave.
    /// Three were reported on 2026-09-08 and all three are that state; see
    /// `foldSupportFrames` for why no threshold separates them from a record that is
    /// genuinely fast. (`calledBpm` rather than `rawBpm`: a filter calling beats an octave
    /// below its own cloud is publishing exactly the grid it calls, and that is not this
    /// state — see `Options::beatOctaveBeats`.)
    std::uint32_t beatDivisor = 1;
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
    /// `TempoState::acquired` at this beat: whether it may fire the rig.
    bool acquired = false;
    double confidence = 0.0;
    /// True on the first beat published under a snap's new bar phase. §5.6 reserves Link's
    /// `forceBeatAtTime` for exactly this beat — a `requestBeatAtTime` would be moved to
    /// where the session's phase already matches, which is the phase being corrected.
    ///
    /// That is the beat the operator pointed at only when they pointed at one still to
    /// come. A snap naming the beat just gone (see `snapDownbeat`) cannot mark it, because
    /// it has already been sent, so the flag rides the next beat instead — which carries
    /// the identical correction, since `LinkSession::forceBeat` is given the *bar
    /// position*, and forcing beat 2 of the bar moves a peer's phase exactly as far as
    /// forcing beat 1 would.
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
///     The window is a *preference*, not a fence: see `Options::foldHysteresis`. Folding
///     down takes the **beats** with it and not just the number — a filter calling 186
///     beats a minute under a tempo published as 93 would otherwise fire every output at
///     twice the rate on screen; see `Options::foldBeats`.
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
/// `tools/trace_stability.py` is the harness.
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
        /// **The decoder is applying the window itself**, as evidence inside its posterior
        /// (`BeatDecoder::setTempoWindow`), so nothing is folded here: the octave stays
        /// where the decoder put it, and the latch and the sub-grid vote below never run.
        /// The window and `octaveFold` are still the operator's settings and still travel
        /// through these options — the engine reads them out and hands them on. Set by
        /// `BeatEngine` for a decoder that `honoursTempoWindow()`, never by an operator;
        /// The window belongs in the decoder when the decoder can take it: there it is
        /// evidence, not a relabelling afterwards.
        bool foldInDecoder = false;
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

        /// Whether the fold reaches the **beats**, or only the number on the screen.
        ///
        /// This is the half of the octave fold that was missing, and it is the whole of the
        /// user's report of 2026-09-06: *"the tool kept detecting the bpm as double, when it
        /// has a very obvious kick drum at 93ish bpm ... it kept trying to fold from 186."*
        ///
        /// Measured on `references/audio`'s "03 - Fake Sweat", whose kick is at 92.0 BPM
        /// (independently: the peak of the 30-110 Hz onset envelope's autocorrelation, where
        /// the on-beat energy is **15.7 times** the energy halfway between): the particle
        /// filter tracks the *double-time* grid for 56 % of the track — a beat period of 16
        /// frames, 187.5 BPM, is its single commonest reading, on 5626 of 11987 frames. The
        /// fold then did its job on the number and published 92.0, correctly. But
        /// `TrackedFrame::emitted` went past untouched, so **the beats, the downbeats, and
        /// with them OSC, MIDI clock, Link's phase and every trigger rule ran at 186 while
        /// the readout said 92**: 501 beats in 239.7 s, a mean of 125 BPM, swinging between
        /// 90 and 165 from one twenty-second window to the next as the cloud changed octave.
        ///
        /// A fold is a statement about *which grid the music is on*, not about how a number
        /// is printed. So when it puts the published tempo an octave or two below the
        /// filter's, only every second (or fourth) of the filter's beats is published, and
        /// the bar is divided with them — see `foldPhaseMemory` for which of them.
        ///
        /// This only ever divides. A fold that puts the published tempo *above* the filter's
        /// grid — a ×2, or a window that folds a half-time estimate up — would have to
        /// invent beats between the filter's, which is a different thing needing its own
        /// answer for where they fall and what confidence they carry; §5.5's ×2 still moves
        /// the number alone, as it always has. Set this false to have every fold do that.
        bool foldBeats = true;
        /// How the fold decides *which* of the filter's beats are the music's, and how hard
        /// it is to talk it out of that once decided.
        ///
        /// Each of the candidate sub-grids carries an average of the network's P(beat of any
        /// kind) over the filter beats falling on it — one exponential average per sub-grid,
        /// each stepped only on its own beats, so that two grids are compared at the same
        /// point in their cycle. (Decaying every grid on every beat and adding to one is the
        /// obvious way to write it and is wrong: the grid updated last then leads by 1 /
        /// memory — 1.176 at 0.85 — whatever the music is doing, which is a reading of the
        /// smoother rather than of the audio.) 0.85 averages over about seven beats.
        ///
        /// The leader has to beat the incumbent by `foldPhaseMargin` before the grid moves,
        /// the same incumbent advantage `ParticleFilter::meterOf` gives the meter and for the
        /// same reason: two sub-grids trading places is two beats in the wrong place every
        /// time they do it.
        double foldPhaseMemory = 0.85;
        double foldPhaseMargin = 1.25;

        /// **Whether the music supports the octave the window asks for**, and the gate that
        /// decides it. This is what keeps the fold from halving a track that really is fast.
        ///
        /// A window is a promise about the material, and an operator's promise can simply be
        /// wrong for the record that is playing. Both of these reach the fold as "the filter
        /// is calling twice as many beats as the published tempo", and they are opposite:
        ///
        ///   * "03 - Fake Sweat" — a 92 BPM track whose filter grid is 186. Halving is right.
        ///   * A 204 BPM Quickstep under a 70-140 window. The filter is right, the window
        ///     does not fit the record, and halving throws away every second real beat.
        ///
        /// **Nothing in the activations tells them apart**, and it is worth saying why so it
        /// is not tried again: measured over both, the two sub-grids differ by a factor of
        /// 1.7 to 2.3 in `beatActivation` *in both cases*, because a 4/4 bar puts its
        /// probability alternately on the beat and downbeat classes; the sum of the two —
        /// which is P(beat of any kind), and the only form of the question worth asking — is
        /// 1.02 to 1.13 for every genre and for Fake Sweat alike. The network believes all
        /// 186 of those beats. It is not wrong: there are eighth notes there.
        ///
        /// What does tell them apart is **the filter's own cloud**. If the slower octave is a
        /// real reading of the music, the cloud keeps returning to it; if the record is
        /// simply fast, it never goes there at all. Measured as the fraction of frames whose
        /// continuous tempo is already at the published tempo rather than a multiple of it:
        ///
        /// | | frames at the folded tempo |
        /// |---|---|
        /// | "03 - Fake Sweat" (92, filter at 186) | **30.9 %** |
        /// | Jive (171) | 1.5 % (0.8-3.9) |
        /// | Viennese Waltz (178) | 1.2 % (0.1-1.4) |
        /// | Quickstep (204) | 0.9 % (0.0-2.6) |
        ///
        /// Twenty times over — but **an average of that will not do**, and the reason is the
        /// whole shape of this setting. A cloud visits an octave in runs, not in independent
        /// frames, so a smoothed fraction is a reading of which run it happens to be in: over
        /// ten seconds it reached 0.26 on a Jive and 0.34 on a Viennese Waltz while falling
        /// to 0.002 on Fake Sweat's densest passage. The populations overlap completely and a
        /// gate on the average flickers on both.
        ///
        /// The *longest sustained* run does separate them, because what is being asked is
        /// whether the slower octave is a place the cloud can settle rather than somewhere it
        /// passes through:
        ///
        /// | | longest run at the folded tempo |
        /// |---|---|
        /// | "03 - Fake Sweat" | **748 frames, 15.0 s** |
        /// | Quickstep | 104 frames, 2.1 s (median 32) |
        /// | Jive | 96 frames, 1.9 s (median 58) |
        /// | Viennese Waltz | 72 frames, 1.4 s (median 38) |
        ///
        /// So it is a **latch on a sustained run**, not a proportion: `foldSupportFrames` of
        /// unbroken agreement and the octave is believed thereafter. 250 frames is five
        /// seconds — two and a half times the longest run any genuinely fast record managed,
        /// and a third of what the double-timed one sustained, which is as close to the
        /// middle of a gap that wide as makes no difference.
        ///
        /// Latched rather than continuous because the question is about the record, not the
        /// moment: once the music has shown that the operator's octave is one it lives in,
        /// a passage where the filter runs double is exactly when the fold is wanted most.
        /// The latch is dropped when the lock moves to a *different tempo*, alongside
        /// `refinedBpm_` and for the same reason — the evidence was about the tempo that has
        /// just been replaced.
        ///
        /// Until it latches the fold moves the number alone, as it always did. That costs the
        /// first few seconds of a track, which is the acquisition the tracker is already
        /// openly hunting through.
        ///
        /// A manual ÷2 is not gated. The operator pressing it has said which grid they want,
        /// and evidence is wanted in place of an instruction, not in spite of one.
        std::size_t foldSupportFrames = 250;

        /// **When the filter's own beats disagree with the filter's own tempo**, how much of
        /// a run it takes to believe the beats, and how clean the ratio has to be.
        ///
        /// These describe a state the particle filter should not be in and sometimes is: it
        /// reports a beat period of 15 frames — 200 BPM — and then *calls* its beats 30 frames
        /// apart. Measured on `references/audio`'s "Pogo - Quantum Field - 08 Moonlake": 216
        /// of its 402 beat-to-beat gaps are exactly 30 frames and only 59 are 15, while the
        /// cloud's interval sits at 15 for the whole track. Reported from a rig on 2026-09-08,
        /// and reported exactly right: *"the app said locked and displayed 200bpm, but the beat
        /// counter dot lights were moving very clearly at 100bpm"*.
        ///
        /// Both numbers were true of what they described. The readout is the cloud's tempo;
        /// the dots move on the beats. **The published tempo has to be the one the beats are
        /// on** — Link's tempo, the MIDI clock's rate, `<prefix>/bpm` and every "BPM in range"
        /// condition are all statements about the beats going out — so when the beats are
        /// consistently a clean octave away from the cloud, the beats win.
        ///
        /// This cannot fire on a filter that agrees with itself, which is what makes it safe:
        /// each called beat's own gap has to be within `beatOctaveTolerance` of a factor of
        /// two or four off the cloud's period at that beat, for `beatOctaveBeats` beats in a
        /// row. Anything else leaves it at zero and nothing about the published tempo
        /// changes. (Each beat's own gap, and not a median over the last two dozen: see the
        /// implementation for what the median did on a track that genuinely changed tempo.)
        ///
        /// It is a **symptom fix and says so**. The disagreement is the filter's, and belongs
        /// to the particle filter rather than here; until that is looked at, an operator
        /// should at least not be shown a tempo that nothing else in the app agrees with.
        ///
        /// **The octave the beats are on is where everything below the filter works.** The
        /// fold, the lock, the `foldSupportFrames` latch and the operator's tap all take the
        /// tempo the beats are *arriving* at (`TempoState::calledBpm`) and never the cloud's
        /// own — because the first version of this applied the beats' octave to the
        /// published number *after* the fold had already folded the cloud's, and the two
        /// corrected the same octave twice. Measured on 2026-09-08 with the 70–140 window
        /// on: "02 - Jamie Lidell - Your Sweet Boom", a 107 BPM track whose cloud sits at
        /// 214 and whose beats arrive at 107, was published at **53.5** for the whole track
        /// (the fold halved 214 to 107, then this halved it again), and "Pogo - Quantum
        /// Field - 08 Moonlake" (100, cloud at 200) at **50.0**; Link, MIDI clock and every
        /// `<prefix>/bpm` carried it. With the fold off both read correctly, which is how
        /// it went unseen. And having folded, the latch above then had every reason to
        /// fire — the beats *were* on the folded tempo — and divided a grid that was
        /// already the music's, dropping every other beat of Jamie Lidell in three windows
        /// of the track. Folding the rate the beats are on, there is nothing to divide.
        std::size_t beatOctaveBeats = 8;
        double beatOctaveTolerance = 0.12;

        /// Frames of agreement before the tempo is called locked, and of disagreement
        /// before that is given up. At 50 Hz these are 0.5 s and 1.5 s.
        ///
        /// **Every frame count in these options is stated at the network's 50 Hz**, which
        /// is the rate the particle filter runs at and the rate every number in this header
        /// was measured at. A decoder may run faster — the forward filter takes the
        /// activations at 100 Hz — and the constructor scales these five (`lockAfter`,
        /// `unlockAfter`, `relockAfter`, `foldSupportFrames`, `confidenceSmoothing`) to the
        /// frame period it is given, so half a second is half a second whichever decoder
        /// is underneath. The values here are never rewritten: `options()` hands back what
        /// was set, and a round trip through `setOptions` scales nothing twice.
        std::size_t lockAfter = 25;
        std::size_t unlockAfter = 75;
        /// **A lock needs beats at the tempo being locked**: this many beats called by the
        /// decoder, the gaps between them each one period of that tempo, or an octave either
        /// side of one (a beat let pass, or the beats on the octave above the cloud's), and the
        /// last no more than two periods ago.
        ///
        /// Agreement alone is a statement about the decoder's posterior, and a posterior can
        /// agree with itself about nothing: on digital silence the forward filter put 0.95 of
        /// its mass on 181.8 BPM, and the tracker locked there in 3.5 s with not one beat called
        /// (2026-10-03). A tempo the decoder is not calling beats at is not one the rig can be
        /// locked to.
        ///
        /// Three, measured against four over the 23 tracks with the 70–140 window on, against
        /// the build before either: median first lock 2.40 s and 3.24 s (2.26 before), the
        /// first lock at the right tempo 4.43 s and 4.57 s (4.79), seconds locked at a wrong
        /// tempo 389 and 369 (429). Nothing reaches the rig before a lock (`TempoState::
        /// acquired`), so three: most of four's cut in wrong locks, at nearly the old wait.
        std::size_t lockBeats = 3;
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

        /// Whether a ÷2 or ×2 the operator pressed **carries on into the next track** — the
        /// next time a different tempo earns the lock — or is dropped there. Off by default,
        /// which is the operator's call of 2026-09-23: a set is one record after another, and
        /// a ÷2 that suited a 174 drum-and-bass track turned the 128 house record after it into
        /// 64, with the beats divided, until somebody noticed (the audit's H2). On, the shift
        /// is a set-wide preference that lasts until pressed again or a restart.
        ///
        /// A tapped tempo's octave is never carried either way: a tap names *this* record's
        /// tempo, and `seedTempo` has always promised its shift lasts only as long as the
        /// tracker stays on it.
        bool keepOctaveShift = false;

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

    /// Feeds one frame from the decoder — the forward filter by default, or the particle
    /// filter. Returns a beat if one was called.
    std::optional<BeatEvent> process(const TrackedFrame& frame) noexcept;

    void reset() noexcept;

    const TempoState& state() const noexcept { return state_; }
    const Options& options() const noexcept { return options_; }

    /// Changing the window or the gate takes effect on the next frame. Only a change of
    /// window can cost the lock, and only one that puts the tempo in another octave: a
    /// latency, gate or timing change never does, and neither does a window that a tempo
    /// ÷2, ×2 or the hysteresis has put outside of (the audit's M3).
    void setOptions(const Options& options) noexcept;

    /// Halves or doubles the published tempo — exactly what was showing, refinement and all —
    /// keeping the lock. A manual octave shift, applied after the fold, so it takes effect
    /// whatever the window says — the window itself does not move. Bounded at
    /// `kMaxOctaveShift` either way. §5.5's manual octave shift.
    void halve() noexcept;
    void redouble() noexcept;

    /// §5.5's tap tempo, as a *seed*: the operator has said which tempo they mean, so the
    /// octave-fold window moves to an octave centred on it and any manual ×2 / ÷2 is
    /// cleared. `tracking::TapTempo` turns the taps themselves into this number.
    ///
    /// **Only when the fold is already on.** A tap does not switch it on — it used to, and
    /// the window it left behind then halved or doubled the *next* record of a set (see the
    /// implementation for the report). With the fold off a tap moves the published tempo onto
    /// the octave it named through the manual shift instead, which is what ÷2 and ×2 do and
    /// is the only thing an octave instruction can mean when there is no window.
    ///
    /// This is the half of §5.5's "seeds or overrides the tracker" that costs nothing and
    /// is unambiguously right. It is what fixes the failure §7 deviation 4 measured: a
    /// Quickstep at 204 BPM under a 70-140 window is folded to 102 and there is no way
    /// for the operator to say otherwise. Tapping it moves the window to 144-288. Under the
    /// particle filter the fold then publishes 204 from the very next frame; under the forward
    /// filter, the default, the window is evidence inside the decoder (`foldInDecoder`), so
    /// the published tempo moves when the posterior does, which takes music that supports
    /// the new octave.
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
    /// **What moves is decided by the octave, not by the number** (the audit of 2026-09-25,
    /// H5): a tap that agrees with the octave being published moves nothing — a tap confirming
    /// what is already tracked must not cost sync — and one that names another octave moves the
    /// published tempo by exactly that, keeping the lock, as ÷2 and ×2 do.
    void seedTempo(double bpm) noexcept;

    /// §5.5's manual downbeat: the beat the operator is pointing at starts the bar, and
    /// every bar after it is counted from there.
    ///
    /// **Which beat that is, is the whole difficulty.** This used to be "the next beat
    /// called", and that is a beat late for the only way the control is ever used: a
    /// person presses the button *on* the downbeat they can hear, by which time the
    /// tracker has already called it, so the bar came out anchored to beat 2. The user's
    /// report, 2026-09-05: "if I press the downbeat, it should start at the very beginning
    /// of the four dots, but it's always one too slow." §5.5 wants a control that "snaps
    /// bar phase immediately", and a beat late is not immediately.
    ///
    /// So it takes the **nearest** beat: the one just called if it is less than half a beat
    /// period behind, and the one still to come otherwise. Nothing is assumed about which
    /// side of the beat a press lands on, and nothing needs to be — the split is
    /// symmetric, so an operator anticipating the beat and an operator reacting to it both
    /// land on the beat they meant, and the pipeline's own delay only moves where inside
    /// that window the press falls. There is deliberately no constant to tune: the
    /// alternative is a fixed offset for a reaction time nobody has measured.
    ///
    /// The half is measured against the *filter's* beat period, never the published tempo,
    /// because ÷2 and ×2 move the second and not the first — after a ÷2 the beats keep
    /// arriving at the rate they always did.
    ///
    /// Naming the beat just gone takes effect **at once**: `beatInBar` is 1 before this
    /// returns, so the count moves under the operator's finger instead of a beat later.
    /// What cannot move is that beat's own outgoing event, which has been sent already —
    /// the correction reaches the transports on the next beat, marked `BeatEvent::snapped`.
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
    /// at when they press it — once a lock has been earned. Before that there is nothing to
    /// pin but the hunt's guess of the moment (the audit of 2026-09-25, M4): the pin is
    /// remembered and takes hold on the frame acquisition first locks — a pin cannot hold
    /// up a flag that was never raised.
    ///
    /// Releasing drops the lock with it and starts the hunt again from what is playing
    /// now. An operator letting go is saying "that tempo was wrong, look again", and a
    /// lock left standing afterwards would be the very thing they were trying to shed. A lock
    /// found afterwards at another tempo is the next record, and a ÷2 or ×2 goes with the one
    /// it was pressed for unless the operator asked to keep it (the audit of 2026-09-25, M3).
    ///
    /// It holds against the *hysteresis*, not against the operator's other controls. A
    /// fold window dragged until it no longer contains the pinned tempo still drops the
    /// lock, and should: the alternative is publishing a tempo the operator's own settings
    /// exclude. `reset()` clears the pin, because after a reseed it would be pinning
    /// nothing.
    void setLockPinned(bool pinned) noexcept;
    bool lockPinned() const noexcept { return lockPinned_; }

    /// The input has had no signal for a while, or has it again (`BeatEngine` decides, from
    /// the level; see `TempoState::noSignal`). Going quiet drops the lock — pinned or not — and
    /// clears `acquired`, so nothing fires until a lock is earned again; the tempo shown is the
    /// last one, held. What comes back is a new acquisition at the ordinary price, whatever the
    /// old lock had earned: a deck that stopped is not a challenger to out-argue. The pin is the
    /// operator's and stays.
    void setNoSignal(bool noSignal) noexcept;

    /// The published tempo **in the decoder's own terms**: with the operator's ÷2 or ×2, the
    /// fold's octave and the octave the beats sit at against the cloud all taken back out.
    /// What a tempo hold on the decoder is given when the lock is pinned (`BeatEngine`).
    ///
    /// Not the published tempo, which carries the shift — pinning under ÷2 held the filter at
    /// half its real tempo — and not the cloud's `rawBpm`, which is the right octave but
    /// wanders: caught at the moment a lock is earned it read 132.9 over a 128 BPM track, and a
    /// hold there would have pushed the filter off the music it had just locked to. The audit's
    /// H3.
    double decoderBpm() const noexcept;

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
    /// The octave the window alone would put `bpm` in, with no memory of the last one: 0
    /// anywhere inside the window widened by the hysteresis, else the fold's octave.
    /// `chooseOctave`'s second and third rules, and what `setOptions` asks of a moved window.
    std::int64_t windowOctave(double bpm) const noexcept;
    /// A tempo arrived at some other way, put in the octave the fold has settled on. The
    /// beat-spacing refinement measures a period in frames, so it has to be moved into the
    /// same octave as the value it refines or it would be rejected as a disagreement.
    double inChosenOctave(double bpm) const noexcept;
    /// Forgets the octave the fold settled on, so the next frame decides afresh. Anything
    /// that moves the window or the operator's octave has to call this.
    void forgetFold() noexcept;

    /// How many of the filter's beats go to one published beat: 1 unless the fold has put
    /// the published tempo below the filter's grid, then 2 or 4. See `Options::foldBeats`.
    std::uint32_t foldDivisor() const noexcept;
    /// Starts the sub-grid choice again, with nothing believed about which of the filter's
    /// beats are the music's. Called whenever the divisor changes under it, because a score
    /// collected over two sub-grids says nothing about four.
    void resetFoldPhase(std::uint32_t divisor) noexcept;
    /// Moves everything published — the tempo, the lock, the candidate and the refinement — by
    /// `moved` octaves. What ÷2, ×2 and a tap that names another octave do (the audit of
    /// 2026-09-25, H5).
    void movePublishedOctave(std::int64_t moved) noexcept;
    /// Whether this filter beat falls on the sub-grid being published, having first counted
    /// the network's opinion of it towards that sub-grid's score. Advances the slot, so it
    /// is called exactly once per filter beat and only when the divisor is above one.
    bool onPublishedGrid(const TrackedFrame& frame) noexcept;

    void updateLock(double folded) noexcept;
    /// Whether the decoder is calling beats at the tempo it reports: `Options::lockBeats`.
    bool beatsAtTempo() const noexcept;
    void rememberBeat(std::uint64_t frameIndex) noexcept;
    /// Advances the filter's own bar position and turns it into the published one,
    /// applying whatever a snap has rotated the bar by. Called once per emitted beat.
    void advanceBar(bool filterCalledDownbeat) noexcept;
    /// Rotates the bar so that the beat `filterBeatInBar_` is currently on comes out as 1.
    /// A snap that names the beat just gone calls this before the position advances; one
    /// that names the beat still to come calls it after. Same arithmetic, one beat apart —
    /// which is the entire difference between the two.
    void startBarHere() noexcept;
    /// Whether the beat the filter last called is nearer than the one it will call next.
    /// False when neither is known yet, so a snap before the first beat waits for it.
    bool lastBeatIsNearer() const noexcept;
    /// The mean beat-to-beat gap in frames, over the gaps within `refineGapTolerance` of
    /// `cloudIntervalFrames`. Zero when there are not enough of them.
    double refinedIntervalFrames(double cloudIntervalFrames) const noexcept;
    /// Watches for the filter contradicting itself and settles `beatOctave_`: this beat's
    /// own gap against the cloud's period, unbanded, because this is the one measurement
    /// that has to be able to disagree with the cloud — see `Options::beatOctaveBeats`.
    /// Called once per beat the filter calls, after the gap has been remembered. When the
    /// octave moves, the fold is asked again about the beats' new tempo and the lock is
    /// carried across, so the published number does not move unless the beats did.
    void weighBeatOctave(double cloudIntervalFrames) noexcept;
    /// A tempo of the cloud's, moved onto the octave the filter's beats are on. What the
    /// fold and the lock are given, continuous or discrete: see `Options::beatOctaveBeats`.
    double calledBpm(double cloudBpm) const noexcept;
    /// The period the *published* beats arrive at, in frames — the filter's own, times what
    /// the fold divides the grid by, times the octave the filter is emitting on. What a snap
    /// is judged against, because the operator is pointing at a beat they can hear.
    double publishedPeriodFrames() const noexcept;

    /// The frame counts of `options_` at this tracker's own frame rate. See
    /// `Options::lockAfter` for why they are kept apart from what was set.
    void scaleFrameCounts() noexcept;

    double secondsPerFrame_;
    Options options_;
    TempoState state_;
    std::size_t lockAfter_ = 0;
    std::size_t unlockAfter_ = 0;
    std::size_t relockAfter_ = 0;
    std::size_t foldSupportFrames_ = 0;
    double confidenceSmoothing_ = 1.0;

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
    /// Whether the shift above came from a tap rather than from ÷2 or ×2 — the one that is
    /// dropped at the next track whatever `Options::keepOctaveShift` says.
    bool shiftFromTap_ = false;
    /// The published tempo when the operator dropped the lock — a pin released, a window moved
    /// out from under it — or zero. The next lock taken at another tempo is the next record,
    /// whose octave shift goes as a replacing lock's does (the audit of 2026-09-25, M3).
    double tempoLetGo_ = 0.0;
    /// The furthest the manual shift goes either way: two octaves, a quarter or four times the
    /// tracked tempo. Past that it is not an octave preference but a tempo nothing can be
    /// playing, and unbounded ÷2 presses — or a tap of a wildly wrong tempo with the fold off —
    /// sent Link and the MIDI clock a tempo of a few BPM or a few hundred (the audit's H1).
    static constexpr std::int64_t kMaxOctaveShift = 2;
    /// How many octaves the beats the filter *calls* sit away from the tempo it *reports* —
    /// negative when the beats are slower, which is the case that has been seen. Applied to
    /// the cloud's tempo before the fold and the lock ever see it (`calledBpm`), so that
    /// both argue about the grid the outputs are actually on. See `Options::beatOctaveBeats`
    /// for what applying it *after* the fold instead did.
    std::int64_t beatOctave_ = 0;
    std::int64_t beatOctaveCandidate_ = 0;
    std::size_t beatOctaveRun_ = 0;

    /// The most the fold will divide the beat grid by. Two octaves: a filter reading four
    /// times the published tempo is already a tracking failure rather than an octave
    /// preference, and bounding it keeps the scores below a fixed-size array.
    static constexpr std::size_t kMaxFoldDivisor = 4;
    /// Which of the filter's beats are published, when the fold divides the grid. See
    /// `Options::foldBeats` for the whole of it: `foldSlot_` counts filter beats within one
    /// published beat, `foldPhase_` is the slot being published, and `foldScore_` is the
    /// decayed evidence for each.
    std::uint32_t foldDivisor_ = 1;
    std::uint32_t foldSlot_ = 0;
    std::uint32_t foldPhase_ = 0;
    double foldScore_[kMaxFoldDivisor] = {};
    /// Whether each sub-grid has been scored since the divisor last changed. Its first beat
    /// *sets* its score rather than decaying towards it from zero — from zero, whichever sub-grid
    /// was scored first led the others by the margin for several beats on nothing but its head
    /// start, and took the grid from the one the bar's downbeat had chosen (the audit of
    /// 2026-09-25, M2).
    bool foldScored_[kMaxFoldDivisor] = {};
    /// The filter's own beats — how long since the last one and how many there have been.
    /// The slot above is advanced by how many beat *periods* have gone by rather than by
    /// one per call, so that a beat the filter drops through a quiet bar does not rotate the
    /// published grid onto the off-beat and leave it there.
    std::uint64_t framesSinceCalled_ = 0;
    std::uint64_t beatsCalled_ = 0;
    /// The evidence `Options::foldSupportFrames` gates the fold's division of the beat grid
    /// on: how many frames in a row the cloud's own tempo has been the published one rather
    /// than a multiple of it, and whether that has ever run long enough to believe. Starts at
    /// nothing, so a fold divides only once the music has argued for it.
    std::size_t foldRun_ = 0;
    bool foldSupported_ = false;
    /// Whether the published grid has a beat to be a phase *of*. The first beat after the
    /// divisor changes is published whatever the scores say, because until one has been
    /// there is no grid — only a slot number nothing has voted on.
    bool foldAnchored_ = false;
    /// A ÷2, ×2 or tap since the last frame: whether the next change of divisor is the
    /// operator's. See `resetFoldPhase` (the audit of 2026-09-25, M2).
    bool operatorDivided_ = false;
    /// After the operator divided the grid: the next downbeat the filter calls chooses the
    /// published sub-grid. See `onPublishedGrid`.
    bool foldAnchorOnDownbeat_ = false;
    /// The slot of the first beat after the operator divided an undivided grid with the bar
    /// known, aimed so that beat 1's is the published one; -1 otherwise. See `resetFoldPhase`.
    int foldFirstSlot_ = -1;
    /// Published beats since the last published downbeat, which is what divides the **bar**
    /// along with the beats. A filter bar is as many of its own beats as the meter says, so
    /// under a divisor of two the filter calls a downbeat twice as often as one is due; the
    /// rule is simply that a bar cannot be shorter than the meter, counted in the beats
    /// actually being published. That is inert at a divisor of one — the filter's downbeats
    /// already arrive exactly `beatsPerBar` published beats apart — so it changes nothing
    /// about an unfolded track, and it closes the arithmetic for any meter: `divisor` filter
    /// bars are `beatsPerBar` published beats, whether the meter is odd or even.
    std::uint32_t sinceDownbeat_ = 0;
    bool anyDownbeat_ = false; ///< a bar has been started, so the rule above has a floor

    /// The bar, in two parts: where the filter thinks we are, and how far the operator
    /// has rotated that. `state_.beatInBar` is the second applied to the first, so with
    /// no snap the published bar is the filter's, beat for beat.
    std::uint32_t filterBeatInBar_ = 0; ///< 1 on the filter's downbeat; 0 until it calls one
    std::uint32_t barOffset_ = 0;       ///< beats added to the filter's position
    bool snapPending_ = false;          ///< a snap waiting for the next beat to land on
    /// A snap whose new bar phase has not reached the outputs yet. Set by every snap and
    /// spent on the next beat, which is the earliest one that can carry it either way: a
    /// snap naming a beat still to come is carried by that beat, and one naming the beat
    /// just gone is carried by the beat after it. See `BeatEvent::snapped`.
    bool snapUnsent_ = false;
    /// A snap that arrived before the filter had ever called a downbeat has nothing to
    /// rotate yet, so the bar runs from the snap itself until the filter does — and
    /// `sinceSnap_` is what turns that into an offset when it finally happens.
    bool snapAwaitingFilter_ = false;
    std::uint32_t sinceSnap_ = 0;
    /// How long ago the last **published** beat was and how far apart they are, both in
    /// frames, so that a snap arriving between two frames can tell which beat it means.
    /// Zero beats seen means the first is still to come.
    ///
    /// `filterIntervalFrames_` is the filter's own period, and the one a snap is judged
    /// against is that times `foldDivisor()` — because the operator is pointing at a beat
    /// they can hear, and under a fold that divides the grid the beats they can hear are
    /// the published ones. A ÷2 or ×2 that only moved the number left the filter's beats
    /// arriving at the rate they always did, and this used to say so; that is still true of
    /// ×2, which does not divide anything.
    std::uint64_t framesSinceBeat_ = 0;
    std::uint64_t beatsSeen_ = 0;
    std::uint32_t filterIntervalFrames_ = 0;

    /// The frames the last few beats were called on, oldest first. Bounded by
    /// `refineOverBeats + 1`, so this allocates once and never grows.
    std::vector<std::uint64_t> beatFrames_;
};

} // namespace takt4::tracking
