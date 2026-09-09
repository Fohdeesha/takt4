#pragma once

#include <cstdint>

namespace takt4::tracking {

/// What the decoder made of one activation frame — the particle filter's, or the exact
/// forward filter's; `TempoTracker` reads either without knowing which.
///
/// One frame of the *decoder's* clock, which is the network's 50 Hz for the particle
/// filter and twice that for the forward filter (`BeatEngine` interpolates the
/// activations to whatever rate the decoder asks for). `frameIndex` counts those frames,
/// and a time is `frameIndex` times the decoder's `secondsPerFrame()`.
struct TrackedFrame {
    enum class Emitted : std::uint8_t { None = 0, Downbeat = 1, Beat = 2 };

    std::uint64_t frameIndex = 0; ///< frames since the decoder was reset
    Emitted emitted = Emitted::None;
    /// Where inside this frame the beat fell, in frames, when one was emitted: zero for a
    /// decoder that can only say "this frame", which is the particle filter; a fraction
    /// either side of zero for one that reads the beat off a posterior and can say "a
    /// third of a frame ago" or "half a frame from now". `TempoTracker` adds it to the
    /// beat's time and nothing else reads it. See `ForwardFilter::Options::emission`.
    double beatOffsetFrames = 0.0;

    /// The median of the beat particles, taken before this frame's motion. Every
    /// decision the particle filter makes rests on it; the reference calls it
    /// `gathering`. The forward filter puts its MAP state here, for a trace.
    std::uint32_t gathering = 0;
    /// The commonest downbeat particle, updated only on frames that could carry a beat.
    /// Particle filter only.
    std::uint32_t downMax = 0;

    std::uint32_t intervalFrames = 0; ///< the decoder's beat period, in whole frames
    /// The same period without the state space's integer quantisation: the mean over the
    /// particles sitting on the median's tempo or either neighbour of it — or the
    /// posterior mass there. madmom's intervals are whole frames, so at 50 fps and 130
    /// BPM the nearest two are 130.43 and 125.00 and there is nothing in between; a real
    /// tempo lands between them and the estimate straddles both, which is what this reads.
    double refinedIntervalFrames = 0.0;
    double bpm = 0.0;              ///< from refinedIntervalFrames, so it is continuous
    double phase = 0.0;            ///< how far through the beat the estimate is, 0 to 1
    std::uint32_t beatsPerBar = 0; ///< the meter the downbeat stage settled on
    /// How much of the estimate agrees with its own tempo, 0 to 1: the fraction of the
    /// beat cloud on the median's interval, or the posterior mass on the MAP interval and
    /// its two neighbours. Not upstream's — it publishes no confidence — but the natural
    /// one to gate on (§5.5).
    double tempoAgreement = 0.0;

    /// The network's own opinion of this frame, carried through untouched.
    ///
    /// The decoder makes no further use of these — they are its *input* — but the layer
    /// above needs them, and this is the only structure that crosses between the two. When
    /// `TempoTracker`'s octave fold halves the beat grid it has to decide *which* half of
    /// the filter's beats are the real ones, and the network already answered that: over
    /// the double-time passages of `references/audio`'s "03 - Fake Sweat" the sub-sequence
    /// carrying the kick averages 0.50 against 0.29 for the one between. See
    /// `TempoTracker::Options::foldBeats`.
    ///
    /// Their sum is P(this frame is a beat of any kind), because the model's three classes
    /// are a softmax over beat / downbeat / non-beat: a downbeat frame reads high on
    /// `downbeatActivation` and *low* on `beatActivation`, so either one alone would call
    /// every bar start a weak beat.
    float beatActivation = 0.0f;
    float downbeatActivation = 0.0f;
};

} // namespace takt4::tracking
