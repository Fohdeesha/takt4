#pragma once

#include <cstdint>

namespace takt4::timing {

/// What a followed source says about its beat grid, read at one instant.
///
/// Every source is reduced to this — Ableton Link's session, a MIDI clock's ticks — so that one
/// piece of code turns all of them into beats (`BeatGrid`) and none of them has to know how a
/// beat is counted, labelled, halved or coasted through.
struct GridReading {
    /// The instant this reading is for — now — on Link's clock (`output::LinkSession::now`), in
    /// microseconds. Filled in whether or not the source is there: it is what a source that has
    /// gone is coasted to.
    std::int64_t at = 0;
    /// The source is there and saying enough to follow: a Link session with a peer in it, a
    /// MIDI clock ticking with enough ticks to measure.
    bool present = false;
    /// The source has said to stop, and is obeyed — a MIDI Stop with "obey start/stop" ticked.
    /// Not coasted through: the beats stop at once.
    bool stopped = false;
    /// The source's tempo, in BPM. Positive while `present`.
    double bpm = 0.0;
    /// Where the source is on its own beat grid at `at`, continuously: beat n is at the whole
    /// number n, and the fraction is how far towards the next.
    double beat = 0.0;
    /// Whether the source says where its bars are, and so where its beats are: then a whole
    /// multiple of the bar length is a downbeat — Link's beat at a quantum of the bar, a MIDI
    /// clock's beats counted from its Start or a Song Position. False for a MIDI clock that has
    /// had neither, whose ticks carry a tempo and no phase at all: its whole numbers are nobody's
    /// beats until the operator presses DOWNBEAT on one.
    bool anchored = false;
};

/// A beat grid followed from somewhere else — what `ExternalBeats` reads every round on its own
/// thread, from `begin` to `end`. Nothing here is called from any other thread, so an
/// implementation owns what it touches for the length of a run without a lock.
class GridSource {
public:
    virtual ~GridSource() = default;

    GridSource(const GridSource&) = delete;
    GridSource& operator=(const GridSource&) = delete;

    /// A run is starting: open, join or listen for whatever the source needs. `beatsPerBar` is
    /// the bar length `GridReading::beat` is to be anchored to — Link's quantum.
    virtual void begin(std::uint32_t beatsPerBar) noexcept = 0;
    /// The source as it stands now. Called about once a millisecond.
    virtual GridReading read() noexcept = 0;
    /// The run is over: let go of everything `begin` took.
    virtual void end() noexcept = 0;

protected:
    GridSource() = default;
};

} // namespace takt4::timing
