#pragma once

#include "core/rt/published.hpp"
#include "core/timing/grid_source.hpp"
#include "core/timing/prolink/virtual_player.hpp"

#include <cstdint>

namespace takt4::timing {

/// What a CDJ follower reads: the beats of the device followed, as the network heard them, and
/// the clock they are stamped on (Link's). `prolink::Network` is the one; a test hands in its
/// own. `join` and `leave` may be called from any thread; `popBeat` has one consumer.
class CdjFeed {
public:
    virtual ~CdjFeed() = default;

    CdjFeed(const CdjFeed&) = delete;
    CdjFeed& operator=(const CdjFeed&) = delete;

    virtual std::int64_t now() const noexcept = 0;
    virtual bool popBeat(prolink::VirtualPlayer::Heard& out) noexcept = 0;
    /// Announce takt4 as a player and start hearing status; stop.
    virtual void join() noexcept = 0;
    virtual void leave() noexcept = 0;

protected:
    CdjFeed() = default;
};

/// **Following the CDJs** — the tempo master on a Pro DJ Link network, or one player — as a
/// `GridSource`.
///
/// **Each beat packet is a beat, at the moment it arrived.** A player sends one as each beat of
/// its track plays, and the tempo between them is the one it says (BPM × pitch). Measured on two
/// CDJ-2000nexus players (Deep Symmetry's hardware captures, firmware 1.44) the packets sit within
/// ±1.7 ms of a straight line, 0.9 ms rms — a sawtooth of the player's own ~3 ms step, not network
/// noise — at 128 and 145 BPM: well inside a lighting frame, so they are taken as they come, with
/// no filter to lag the DJ's pitch fader. Each beat is waited for a few milliseconds past where
/// the tempo puts it (`kWaitMicros`), so it goes out on its own packet; a packet lost on the way
/// is stood in for by the tempo's prediction.
///
/// **The bar is the DJ's**: the player's beat in the bar (Bb, 1 to 4) is where its beat grid puts
/// the beat, so a whole multiple of four on the grid read out is a downbeat. CDJs count four to
/// the bar always.
///
/// **A beat that does not carry on the grid moves it** — a hot cue, a loop, a jump, a hand-off to
/// another player, the first beat after a pause or after START. Such a beat is a new placement
/// (`GridReading::placements`): bar 1 goes where the DJ's grid says, over any DOWNBEAT pressed
/// before, and the beat itself is sent, on its own moment. Quantised hot cues keep the beat's time
/// and restart the bar, which is how two CDJ-2000nexus players were seen to send them (Bb 2, 3, 1,
/// 2, 3, 1 with no gap out of time).
///
/// **Present** while beats keep coming — one within two beats and a tenth of a second of the last.
/// When they stop — the deck paused, the master gone — `BeatGrid` coasts, then lets go. The same
/// beat heard twice (a machine with two network cards on the DJ's network hears every broadcast
/// twice) is taken once.
class ProLinkFollower final : public GridSource {
public:
    /// How far off the grid, in beats, a beat may arrive and still be the next one on it.
    static constexpr double kOnGrid = 0.25;
    /// A beat of the same device and bar position this soon after the last is that beat again.
    static constexpr double kRepeat = 0.4;
    /// Beats missing before the source is gone: one lost packet is ridden over.
    static constexpr double kAbsentBeats = 2.0;
    static constexpr std::int64_t kAbsentMarginMicros = 100'000;
    /// How long past the moment the tempo gives a beat its packet is waited for, before the
    /// prediction is sent in its place — still stamped with its own moment, so the outputs fire
    /// it on time where they fire ahead and a few milliseconds late where they do not. Longer
    /// than the CDJs' own ±1.7 ms, so a beat goes out on its packet and with its own place in the
    /// bar: a hot cue's downbeat is sent as one, not as the beat the grid expected.
    static constexpr std::int64_t kWaitMicros = 6'000;
    /// How far short of a beat a reading is held while it is waited for.
    static constexpr double kHeld = 1e-6;
    /// The tempi a beat is believed at; one outside them keeps the last.
    static constexpr double kMinBpm = 30.0;
    static constexpr double kMaxBpm = 400.0;

    /// What is being followed, for the window and the console. Published by the worker; any
    /// thread.
    struct Status {
        /// The device whose beats are coming, or 0.
        std::uint8_t device = 0;
        /// Its beat in the bar, as it said it, or 0.
        std::uint8_t beatInBar = 0;
        bool present = false;
        double bpm = 0.0;
        /// Beats taken this run, beats that moved the grid, and repeats dropped.
        std::uint64_t beats = 0;
        std::uint64_t placements = 0;
        std::uint64_t repeats = 0;
    };

    explicit ProLinkFollower(CdjFeed& feed);
    ~ProLinkFollower() override = default;

    /// Joins the network; `beatsPerBar` is ignored — the CDJs count four.
    void begin(std::uint32_t beatsPerBar) noexcept override;
    GridReading read() noexcept override;
    void end() noexcept override;

    Status status() const noexcept { return status_.load(); }

private:
    void take(const prolink::VirtualPlayer::Heard& beat) noexcept;
    /// The beat is somewhere the grid did not carry on to: it is `index` on a grid placed afresh.
    void place(std::int64_t index) noexcept;
    /// How long without a beat before the source is gone, at `periodMicros`.
    static double absentAfter(double periodMicros) noexcept;
    void reset() noexcept;
    void publish(bool present) noexcept;

    CdjFeed& feed_;
    rt::Published<Status> status_;

    // The worker's alone, from `begin` to `end`.
    bool have_ = false;
    /// The last beat: where on the grid read out, when it arrived, the beat period it said, its
    /// device and its place in the bar.
    std::int64_t index_ = 0;
    std::int64_t beatAt_ = 0;
    double periodMicros_ = 0.0;
    std::uint8_t device_ = 0;
    std::uint8_t beatInBar_ = 0;
    std::uint32_t placements_ = 0;
    double placedAt_ = 0.0;
    std::uint64_t beats_ = 0;
    std::uint64_t repeats_ = 0;
};

} // namespace takt4::timing
