#pragma once

#include "core/output/osc_sender.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::output {

/// HANDOFF §5.6's generic OSC namespace, published to any number of targets.
///
/// "Always publish a generic namespace regardless of which host preset is active, so
/// anything can consume it with zero configuration." These nine addresses are that
/// namespace, verbatim from §5.6:
///
///     /takt4/bpm         float   the published tempo
///     /takt4/beat        int     1, on every beat
///     /takt4/beat/bar    int     which beat of the bar it was, 1..N
///     /takt4/downbeat    int     1, on downbeats only
///     /takt4/confidence  float   0 to 1
///     /takt4/locked      int     0 or 1
///     /takt4/meter       int     N, the detected beats per bar
///     /takt4/resync      int     1, when the tracker has just re-found itself
///     /takt4/intensity   int     0, 1 or 2 — Phase 6's classifier; not sent yet
///
/// The four state addresses are sent when the value changes and on every beat, so a
/// consumer that started late is right again within a beat rather than waiting for the
/// operator to touch something. Nothing is sent faster than the beats themselves except
/// a tempo or lock change, which is a handful of datagrams a minute.
///
/// Host presets (§5.6: Resolume, TouchDesigner, MadMapper, QLC+) fill in address
/// templates and belong to Phase 6's trigger engine; this is the layer underneath them,
/// and is never a hardcoded code path for any one host.
class OscPublisher {
public:
    /// The namespace prefix, so a second instance on the same network can be told apart.
    /// "/takt4" by default; a trailing '/' is not wanted and not accepted.
    explicit OscPublisher(std::string prefix = "/takt4");

    /// Adds a target. Throws std::runtime_error if the host cannot be resolved.
    void addTarget(std::string_view host, std::uint16_t port);

    /// Removes every target, and forgets what was last published with them.
    ///
    /// The forgetting is the point: only changed values are sent between beats, so a
    /// target added after this has never been told the tempo and would otherwise wait for
    /// it to move before learning it. An operator who types an address mid-set expects
    /// the next message, not the next tempo change.
    void clearTargets() noexcept;

    std::size_t targetCount() const noexcept { return targets_.size(); }
    const OscSender& target(std::size_t index) const noexcept { return *targets_[index]; }

    /// Publishes one beat, and any state that changed with it.
    void publishBeat(const tracking::BeatEvent& event);

    /// Publishes state that changed since the last call, without a beat. Cheap to call
    /// every frame: when nothing has changed it sends nothing.
    void publishState(const tracking::TempoState& state);

    /// §5.6's resync: tell downstream to snap. Sent when the tracker regains its lock
    /// after losing it, and available for the manual downbeat in Phase 5.
    void publishResync();

    std::uint64_t messagesSent() const noexcept { return sent_; }
    std::uint64_t messagesFailed() const noexcept { return failed_; }

private:
    void sendInt(std::string_view address, std::int32_t value);
    void sendFloat(std::string_view address, float value);
    /// Sends the four state addresses whose value has moved, and remembers them.
    void sendChangedState(double bpm, double confidence, bool locked, std::uint32_t meter,
                          bool force);

    std::string prefix_;
    // The addresses, built once so publishing never touches a string.
    std::string bpmAddress_;
    std::string beatAddress_;
    std::string barAddress_;
    std::string downbeatAddress_;
    std::string confidenceAddress_;
    std::string lockedAddress_;
    std::string meterAddress_;
    std::string resyncAddress_;

    std::vector<std::unique_ptr<OscSender>> targets_;

    // What was last published, so an unchanged value is not resent between beats.
    double lastBpm_ = -1.0;
    double lastConfidence_ = -1.0;
    int lastLocked_ = -1;
    std::uint32_t lastMeter_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t failed_ = 0;
};

} // namespace takt4::output
