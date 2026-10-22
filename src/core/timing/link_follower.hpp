#pragma once

#include "core/rt/published.hpp"
#include "core/timing/grid_source.hpp"
#include "core/timing/link_reader.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace takt4::timing {

/// **Following the house DJ's Ableton Link session**, as a `GridSource`.
///
/// Link has no master: a session runs at whatever tempo a peer last set, and takt4 follows the
/// session, not any one app. Its phase is exact and shared by every peer at the same quantum, so
/// the beat read at the bar length is anchored: a whole multiple of it is everyone's bar line.
///
/// **Never the older session.** When two Link sessions meet, the older one wins — the one whose
/// clock has run longest (`Sessions::sawSessionTimeline`) — and switching Link on founds a new
/// session every time. So a follower that switched Link on and waited, alone, would be the older
/// session when the DJ's app joined, and the DJ's app would take on the follower's timeline: a
/// tempo nobody playing chose, and a phase off the music. That is measured, on Link itself, by
/// the test "Link's older session wins when two meet". So this keeps Link switched off and
/// **listens** — Link's discovery traffic, which costs the session nothing — until a peer has
/// been heard for `Options::hearFor` and is still announcing; only then does it switch on, as the
/// youngest session there is, and join theirs. When the last peer leaves it switches off at
/// once, and listens again from nothing; `ExternalBeats` coasts meanwhile.
///
/// One thread — `ExternalBeats`' — calls everything here but `status` and `problem`.
class LinkFollower final : public GridSource {
public:
    struct Options {
        /// How long a peer has to have been heard before Link is switched on to join it: longer
        /// than Link's half-second tie band, so the session joined is older than the one
        /// switching on makes by more than the band, whichever way the session ids fall.
        double hearFor = 1.0;
        /// And how recently, so a peer that has stopped announcing — an app quit, a cable pulled
        /// — is not joined in the seconds before it would time out. Link announces four times a
        /// second, so a peer still there has always been heard more recently than this, and a
        /// single stray announcement cannot satisfy both this and `hearFor`.
        double stillHeardWithin = 0.75;
        /// How long Link may be switched on without a peer joining it before it is switched off
        /// again and the listening starts afresh: a peer that can be heard and not reached.
        ///
        /// **Every moment of it past half a second is a hazard**: a session alone for longer than
        /// Link's tie band (0.5 s) is the older one to anybody who arrives, and a DJ app joining
        /// then takes this session's leftover tempo and phase. A
        /// join is one discovery exchange and about fifty measurement round trips — tens of
        /// milliseconds on a wired network, about a second on Wi-Fi at 20 ms a round trip — so
        /// this is long enough for a slow network, and a join that fails is not tried again at
        /// once (`retryAfter`).
        double joinFor = 1.5;
        /// After a join that found nobody, how long before the next is tried — doubled after each
        /// that fails in a row, up to `retryAtMost` — so a peer that can be heard and not reached
        /// leaves takt4 alone in a session of its own for a second and a half a minute, not three
        /// seconds of every four. A join that works starts it from nothing.
        double retryAfter = 2.0;
        double retryAtMost = 30.0;
        /// How often a listener that would not open is tried again.
        double reopenEvery = 5.0;
        /// How often the discovery traffic is read while listening.
        double listenEvery = 0.05;
    };

    enum class Phase : std::uint8_t {
        /// Link switched off, listening for a session to join.
        Listening,
        /// Link switched on to join a session that has been heard.
        Joining,
        /// In a session with at least one peer: followed.
        Following,
    };

    /// What the follower is doing, for the window. Published by the worker; any thread.
    struct Status {
        Phase phase = Phase::Listening;
        /// Peers in the session while joined (Link's own count); peers heard while listening.
        std::uint32_t peers = 0;
        /// The session's tempo while joined; the tempo the first peer heard announces, while
        /// listening.
        double bpm = 0.0;
        /// Whether the discovery traffic can be heard at all — false when it would not open, and
        /// then `problem` says why.
        bool canListen = true;
        /// Joins tried in a row that found nobody, and how long until the next is tried — zero
        /// when one may be tried as soon as a session is heard.
        std::uint32_t failedJoins = 0;
        double retryIn = 0.0;
    };

    LinkFollower(LinkReader& link, LinkListener& listener);
    LinkFollower(LinkReader& link, LinkListener& listener, Options options);
    ~LinkFollower() override = default;

    void begin(std::uint32_t beatsPerBar) noexcept override;
    GridReading read() noexcept override;
    void end() noexcept override;

    Status status() const noexcept { return status_.load(); }
    /// Why the discovery traffic cannot be heard, or empty. Any thread.
    std::string problem() const;

private:
    void listen(double now);
    /// Starts listening from nothing: every peer heard before is forgotten, so one that has gone
    /// cannot be joined on the strength of what it said before. `now` on Link's clock, in seconds.
    void listenAfresh(double now) noexcept;
    /// Link's clock, in seconds.
    double clockSeconds() const;
    void publish(std::uint32_t peers, double bpm) noexcept;

    LinkReader& link_;
    LinkListener& listener_;
    Options options_;
    double quantum_ = 4.0;
    Phase phase_ = Phase::Listening;
    bool canListen_ = false;
    /// When the listener was last read, and when Link was switched on to join, in seconds on
    /// Link's clock.
    double listenedAt_ = -1.0;
    double joiningSince_ = 0.0;
    /// See `Options::retryAfter`: joins failed in a row, and the earliest the next may be tried.
    std::uint32_t failedJoins_ = 0;
    double nextJoinAt_ = 0.0;
    /// When the listener was last opened, or tried; negative before.
    double openedAt_ = -1.0;
    /// The clock `listen` last ran at, for the status's `retryIn`.
    double now_ = 0.0;
    std::vector<LinkListener::Heard> heard_;
    rt::Published<Status> status_;
    mutable std::mutex problemMutex_;
    std::string problem_;
};

} // namespace takt4::timing
