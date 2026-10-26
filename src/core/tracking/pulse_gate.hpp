#pragma once

#include <array>
#include <cstddef>

namespace takt4::tracking {

/// **Whether what is being heard has a beat** — what a first lock waits for.
///
/// The network answers "beat" to anything with movement in it. Run through `takt4-cli track`, a
/// minute of each of pink, white and brown noise, held chords, a swelling pad, a drone, a noise
/// riser, a crowd, rain, a sine sweep, a melody at irregular times, knocks at random times and a
/// voice talking earned the tracker a lock — most of them within four seconds, as fast as music —
/// and five of the 23 tracks of `references/audio` locked during their intros, a second or more
/// before their first beat. A lock fires the rig: a show controller gates on `locked`, and a laser
/// rule fires on the beats it brings.
///
/// So until it opens, no first lock is taken (`TempoTracker::setWaitingForPulse`). It is judged
/// every half second over the last six seconds of the network's activation, and opens when:
///
///   * **the activation swings** — its 95th percentile less its 5th at least 0.40. Smooth sound
///     (noise, a pad, a drone, a riser, a crowd) moves it no more than about 0.3; music swings it
///     from near nothing to near one;
///   * **and repeats at the decoder's own beat period**, or an octave either side of it: the
///     autocorrelation there, less its median between 40 and 200 BPM, at least 0.35. Knocks and
///     words at random times swing the activation as far as music does, but do not come round
///     again at the period the decoder is counting — the decoder's, and no other: a gate that
///     searched every period for the one that flattered the signal found one in random knocks;
///   * **and either the activation falls back to nothing between beats** — its 10th percentile at
///     most 0.03, as nine judgements in ten of music did where the first two held, and none of
///     sound without a beat (0.04 at the least) — **or two of three other signs are there**: the
///     activation repeating at twice that period too (0.35 over the median), the downbeat
///     activation repeating at a bar of three or four of those beats (0.30 over its own), and the
///     repeat at the period itself far from chance (0.52). Busy music — breakbeats, a dense mix —
///     keeps the activation up between beats, and shows the others instead.
///
/// The first two alone were not enough: chance lined random knocks up at the decoder's period in
/// eleven seconds, and a voice in eighteen. With all three, on that minute of each sound at two
/// levels, and ten minutes of each of the riskiest — knocks one to five a second, clicks, glasses,
/// applause, rain, a crowd, the melody and the voice — nothing opened it but knocks once or
/// twice a second, after four and a half and five minutes, and the melody, after six and a half.
/// Music pays at the start of a set, and after no signal: all 698 Ballroom excerpts still locked,
/// a median of half a second later than without the gate and one in eight more than five seconds
/// later; of the 23 tracks of `references/audio`, the 18 that had not locked in their intros a
/// median of two seconds later — up to seventeen on the busiest and half a minute on the quietest.
/// A tap or a DOWNBEAT press opens it at once (`markHeard`): a person saying there is a beat.
///
/// Once open it stays open until `reset` — it says this source has a beat, which is what a first
/// lock needs, and the tracker's own conditions still decide when. Fed the network's frames only
/// (50 a second), never the ones the engine interpolates for the decoder: their activation is a
/// straight line between two real ones, and would look smoother than the sound is. Allocates
/// nothing.
class PulseGate {
public:
    struct Options {
        /// Seconds of activation judged. Six: shorter windows found periods in random clicks.
        double windowSeconds = 6.0;
        double minSpread = 0.40;
        double minRepeat = 0.35;
        double maxFloor = 0.03;
        double minTwice = 0.35;
        double minBar = 0.30;
        double strongRepeat = 0.52;
        /// How often the window is judged.
        double everySeconds = 0.5;
        /// The rate `push` is called at: the network's.
        double framesPerSecond = 50.0;
    };

    /// Two constructors rather than a defaulted argument: GCC and Clang will not take a nested
    /// struct's default member initializers as a default argument inside the class that holds it.
    PulseGate() noexcept;
    explicit PulseGate(Options options) noexcept;

    /// One network frame's beat and downbeat activations, and the decoder's beat period in
    /// network frames — zero or less when it has none, which judges the window shut.
    void push(float beat, float downbeat, double periodFrames) noexcept;

    /// Whether a beat has been heard since the last `reset`.
    bool open() const noexcept { return open_; }
    /// A person says there is one: a tap, or a DOWNBEAT press. Open until `reset`.
    void markHeard() noexcept { open_ = true; }

    /// The last judgement's numbers, for a trace. `repeat` is -1 when the decoder's period was out
    /// of range or the window short; `twice` and `bar` are not a number when there was no room in
    /// the window for them.
    double spread() const noexcept { return spread_; }
    double repeat() const noexcept { return repeat_; }
    double floor() const noexcept { return floor_; }
    double twice() const noexcept { return twice_; }
    double bar() const noexcept { return bar_; }

    /// Forgets everything heard: a new run, or the input back after no signal.
    void reset() noexcept;

    /// The window's longest, in frames: ten seconds at 50 a second.
    static constexpr std::size_t kMaxFrames = 512;

private:
    void judge(double periodFrames) noexcept;
    /// The autocorrelation of the `count` values in `centred_` over `energy`, into `acf_`, at
    /// every lag up to `upTo`.
    void autocorrelate(std::size_t count, double energy, std::size_t upTo) noexcept;
    /// `acf_`'s peak within a frame of `lag`, or not a number when that runs off its ends.
    double peakNear(std::size_t lag, std::size_t count) const noexcept;
    /// The median of `acf_` from `from` to `to`, both included.
    double medianOf(std::size_t from, std::size_t to) noexcept;

    Options options_;
    std::size_t window_ = 1;
    std::size_t every_ = 1;
    /// The typical lag's bounds, in frames: 200 BPM's beat and 40 BPM's.
    std::size_t lagLow_ = 0;
    std::size_t lagHigh_ = 0;
    std::array<float, kMaxFrames> beats_{};
    std::array<float, kMaxFrames> downbeats_{};
    std::size_t head_ = 0;
    std::size_t filled_ = 0;
    std::size_t sinceJudged_ = 0;
    bool open_ = false;
    double spread_ = 0.0;
    double repeat_ = -1.0;
    double floor_ = 0.0;
    double twice_ = 0.0;
    double bar_ = 0.0;
    // Worked in on each judgement, so it allocates nothing.
    std::array<float, kMaxFrames> sorted_{};
    std::array<double, kMaxFrames> centred_{};
    std::array<double, kMaxFrames> acf_{};
    std::array<double, kMaxFrames> scratch_{};
};

} // namespace takt4::tracking
