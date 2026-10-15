#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace takt4::timing {

/// Ableton Link's session, as a follower needs it: switched on and off, asked who is in it, and
/// read. `output::LinkSession` is the one — the same session the outputs lead with, since two
/// Link instances in one process would be two peers. A test supplies one of its own.
///
/// Every member is thread-safe, as Link's own API is.
class LinkReader {
public:
    virtual ~LinkReader() = default;

    LinkReader(const LinkReader&) = delete;
    LinkReader& operator=(const LinkReader&) = delete;

    /// Link's clock, which every beat in takt4 is stamped on.
    virtual std::chrono::microseconds now() const = 0;
    /// Joins or leaves the Link network. Switching on founds a new session, which Link does on
    /// every enable — see `LinkFollower` for why that matters.
    virtual void enable(bool on) = 0;
    virtual bool enabled() const = 0;
    /// Peers in this process's session, itself not counted.
    virtual std::size_t numPeers() const = 0;

    /// The session's tempo, and its beat at `at` for `quantum` beats to the bar — read from one
    /// capture of the session, so the two agree. Link aligns the phase of peers that share a
    /// quantum: a whole multiple of `quantum` is a bar line for every one of them.
    struct Timeline {
        double bpm = 0.0;
        double beat = 0.0;
    };
    virtual Timeline timelineAt(std::chrono::microseconds at, double quantum) const = 0;

protected:
    LinkReader() = default;
};

/// Link's discovery traffic, **listened to and never spoken on**: what tells a follower that a
/// session is out there before it joins one. `output::LinkPeerWatch` is the one; it reads the
/// announcements every Link peer multicasts. Not thread-safe: one thread owns it.
class LinkListener {
public:
    virtual ~LinkListener() = default;

    LinkListener(const LinkListener&) = delete;
    LinkListener& operator=(const LinkListener&) = delete;

    /// Starts listening. False, with why, when it cannot.
    virtual bool open(std::string& problem) = 0;
    virtual void close() noexcept = 0;
    /// Reads every announcement waiting, and forgets each peer that has gone quiet. `now` is any
    /// steady clock, in seconds. Never blocks.
    virtual void poll(double now) = 0;

    /// One peer heard and not yet gone, other than this process: when it was first heard and
    /// when last, on `poll`'s clock, and the tempo it announces. Link makes a peer anew — a new
    /// node — every time it is switched on or founds a session, and only ever moves a peer to an
    /// older session than its own, so "first heard" is how long the session it is in has been out
    /// there at least.
    struct Heard {
        double firstHeard = 0.0;
        double lastHeard = 0.0;
        double bpm = 0.0;
    };
    virtual std::vector<Heard> heard() const = 0;

protected:
    LinkListener() = default;
};

} // namespace takt4::timing
