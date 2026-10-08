#pragma once

#include <cstdint>
#include <deque>
#include <string>

namespace takt4::audio {

/// What the two sides of a stereo input have brought so far, added up by the audio callback —
/// each side's energy and the two multiplied together, over every frame since the stream opened.
/// A reader takes two of these and subtracts, which gives the same three sums over the time
/// between them.
struct StereoSums {
    std::uint64_t frames = 0;
    double left = 0.0;  ///< Σ a²
    double right = 0.0; ///< Σ b²
    double both = 0.0;  ///< Σ a·b
};

/// Watches a stereo input for the ways averaging its two sides goes wrong (2026-09-28).
///
/// takt4 hears a stereo feed as the average of its two sides, which is what its model was trained
/// on and every accuracy figure measured on — and what measured best: over the 23 electronic
/// tracks and GiantSteps' 664, the average tracked as well as the better side alone and better on
/// downbeats. But an average has failures a single side has not:
///
/// - **Out of phase.** One leg wired backwards, or a mastering trick that flips one side: the two
///   cancel wherever they agree, which is the kick and the bass. Measured on the same tracks with
///   one side flipped, beat F fell from 0.84 to 0.70 and downbeat F from 0.66 to 0.43. Music's
///   correlation between its sides ran 0.83 at the median and never below −0.12 in 664 tracks;
///   a flipped leg reads about −0.8.
/// - **One side silent.** Half the cable, or a mono feed on one input of the pair: the average is
///   the live side at half its level, which tracks, and is worth saying.
/// - **Unrelated.** Two inputs that share nothing — a pair picked by position whose other half is
///   a microphone or another source. Music is never below 0.1 for long.
///
/// Pure arithmetic over `StereoSums`, so a test can hand it any story; the window decides what to
/// say about the answer.
class StereoCheck {
public:
    enum class Verdict : std::uint8_t {
        /// Not enough of either side to judge yet, or nothing playing.
        Quiet,
        Fine,
        OutOfPhase,
        LeftSilent,
        RightSilent,
        Unrelated,
    };

    struct Reading {
        Verdict verdict = Verdict::Quiet;
        /// Over the short window; 0 while there is not a window's worth.
        double correlation = 0.0;
        /// Each side's RMS level over the short window, in dB below full scale.
        double leftDb = -120.0;
        double rightDb = -120.0;
    };

    struct Options {
        /// What phase and a silent side are judged over.
        double windowSeconds = 3.0;
        /// What "unrelated" is judged over: longer, since a quiet passage of one source can
        /// share little with the other for a moment.
        double unrelatedSeconds = 8.0;
        double outOfPhaseBelow = -0.3;
        double unrelatedWithin = 0.1;
        /// A short window at least this far from zero, either way, shows the two sides belong
        /// together — in phase or flipped — and "unrelated" waits a whole `unrelatedSeconds` after
        /// the last one. Without it, a flipped leg wired back read as two strangers: on the way
        /// from −0.9 to +0.9 the short window passes through zero while the long one straddles
        /// both and sums to nothing (2026-10-08, the operator: the warning would not go).
        double relatedBeyond = 0.3;
        /// Below this, a side is nothing — and if both are, nothing is judged.
        double silentBelowDb = -60.0;
        /// A side has to be at least this loud for its correlation with the other to mean
        /// anything.
        double judgedAboveDb = -50.0;
    };

    StereoCheck();
    explicit StereoCheck(Options options);

    /// Forgets everything: a new stream, whose sums start again at zero.
    void reset();
    /// One look, at `now` seconds on any steady clock.
    Reading observe(const StereoSums& sums, double now);

    /// A sentence for the window, naming the two inputs as `left` and `right` — empty for
    /// `Quiet` and `Fine`.
    static std::string describe(Verdict verdict, const std::string& left, const std::string& right);

private:
    struct Sample {
        double at = 0.0;
        StereoSums sums;
    };
    /// The sums over the span from the oldest sample at least `seconds` back to the newest, or
    /// nothing when the samples do not reach that far.
    bool over(double seconds, StereoSums& out) const;

    Options options_;
    std::deque<Sample> samples_;
    /// When a short window last showed the two sides related (`Options::relatedBeyond`), or
    /// −∞ for not since the stream opened.
    double relatedAt_;
};

} // namespace takt4::audio
