#pragma once

#include "core/audio/host_time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace takt4::output {

/// Owns an Ableton Link instance and drives it from the tracker (HANDOFF §5.6).
///
/// Construction starts Link's service thread but does not touch the network; nothing is
/// visible to peers until the session is enabled.
///
/// **takt4 is always tempo master.** §5.7: "Link follow is explicitly out of scope." So
/// the session only ever writes, and what `Transports` writes is: `snap` — the tempo and a
/// forced beat in one commit — on the first locked beat and on a DOWNBEAT, and after that
/// `setTempo`, nudged by how far `phaseAtTime` says the session has drifted (the audit's C3).
/// `requestBeat`, `forceBeat`, `beatAtTime` and start/stop sync are the tests' — some of them
/// play the part of other peers with these — and nothing in the application calls them (the
/// audit of 2026-09-25's stale-comment list). Start/stop sync is off by default.
///
/// Timing goes through HostTimeSource (§4.3): the audio thread hands a sample count to
/// `hostMicrosForSample` and Link's own 512-point regression turns it into a host time,
/// so nothing depends on the driver reporting one. That method is the only one here that
/// may be called from the audio thread, and only from that one thread. Everything else
/// belongs to the output thread.
class LinkSession final : public audio::HostTimeSource {
public:
    explicit LinkSession(double initialTempoBpm);
    ~LinkSession() override;

    LinkSession(const LinkSession&) = delete;
    LinkSession& operator=(const LinkSession&) = delete;

    /// Joins or leaves the Link network. Off until this is called.
    void enable(bool on);
    bool enabled() const;

    /// §5.6: start/stop sync off by default, because a VJ rig's transport is not the
    /// tracker's business.
    void enableStartStopSync(bool on);
    bool startStopSyncEnabled() const;

    std::size_t numPeers() const;
    double tempoBpm() const;

    /// Link's own clock, for a caller with no sample counter of its own.
    std::chrono::microseconds now() const;

    /// **Audio thread.** HANDOFF §4.3's regression; see HostTimeSource.
    std::int64_t hostMicrosForSample(double sampleTime) noexcept override;
    /// Forgets the regression, for a stream that has been restarted.
    void resetHostTimeFilter() noexcept override;

    /// Publishes the tracker's tempo, effective at `at`.
    void setTempo(double bpm, std::chrono::microseconds at);

    /// Asks Link to put `beat` at `at` with `quantum` beats to the bar, keeping the phase
    /// relationship peers already have — which, with a peer in the session, means the phase
    /// does not move at all. So no beat of takt4's uses it any more (see the class comment);
    /// the tests do.
    void requestBeat(double beat, std::chrono::microseconds at, double quantum);

    /// Moves the timeline so `beat` lands on `at` regardless of what peers think.
    /// §5.6: "use forceBeatAtTime() only for the manual downbeat snap — it is disruptive
    /// to peers by design." The application's snap is `snap`, which forces the beat and sets
    /// the tempo in one commit; this is a peer's move, for the tests.
    void forceBeat(double beat, std::chrono::microseconds at, double quantum);

    /// The tempo and a forced beat **in one commit** — the snap `Transports` makes on the first
    /// locked beat and on a DOWNBEAT.
    ///
    /// One commit, not `setTempo` then `forceBeat`: Link's own thread writes the session's
    /// timeline back into the app's copy as it handles each commit (`Controller::
    /// updateSessionTiming`), so a second commit made before the first has been handled can be
    /// overwritten by the first one's echo. Measured: one run in four lost the snap that way when
    /// beats were published back to back.
    void snap(double bpm, double beat, std::chrono::microseconds at, double quantum);

    /// The beat Link's timeline is on at `at`. The tests' only; the application reads the phase.
    double beatAtTime(std::chrono::microseconds at, double quantum) const;
    /// Where in the bar that beat falls, 0 to quantum.
    double phaseAtTime(std::chrono::microseconds at, double quantum) const;

    std::uint64_t tempoUpdates() const noexcept { return tempoUpdates_; }
    std::uint64_t beatRequests() const noexcept { return beatRequests_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint64_t tempoUpdates_ = 0;
    std::uint64_t beatRequests_ = 0;
};

} // namespace takt4::output
