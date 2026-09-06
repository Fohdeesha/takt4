#pragma once

#include "core/output/osc_message.hpp"
#include "core/output/osc_sender.hpp"
#include "core/output/output_target.hpp"
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

    /// Adds a target, at the next routing bit. Throws std::runtime_error if the host
    /// cannot be resolved.
    ///
    /// `bit` is which bit of a rule's `Message::outputs` mask selects this one — the index
    /// of the target in `Transports`' own list, so that a rig with an OSC target, a MIDI
    /// target and another OSC target routes to bits 0 and 2 here rather than 0 and 1.
    /// Anything past `kMaxRoutableTargets` is given `kAllOutputs`, so it still receives
    /// everything a rule sends everywhere.
    void addTarget(std::string_view host, std::uint16_t port, std::size_t bit);

    /// Removes every target, and forgets what was last published with them.
    ///
    /// The forgetting is the point: only changed values are sent between beats, so a
    /// target added after this has never been told the tempo and would otherwise wait for
    /// it to move before learning it. An operator who types an address mid-set expects
    /// the next message, not the next tempo change.
    void clearTargets() noexcept;

    std::size_t targetCount() const noexcept { return targets_.size(); }
    const OscSender& target(std::size_t index) const noexcept { return *targets_[index].sender; }

    /// Publishes one beat, and any state that changed with it.
    void publishBeat(const tracking::BeatEvent& event);

    /// Publishes state that changed since the last call, without a beat. Cheap to call
    /// every frame: when nothing has changed it sends nothing.
    void publishState(const tracking::TempoState& state);

    /// §5.6's resync: tell downstream to snap. Sent when the tracker regains its lock
    /// after losing it, and available for the manual downbeat in Phase 5.
    void publishResync();

    /// One address of Phase 6's own choosing, to every target. §5.8's rules build their
    /// addresses from templates and aim them at a host's namespace — Resolume's
    /// `/composition/layers/3/clips/7/connect` — so **the prefix is deliberately not
    /// applied**: it names this app inside the generic namespace, and a rule's address
    /// belongs to whatever is listening.
    ///
    /// A malformed address sends nothing and counts a failure, because `OscMessage` refuses
    /// it. Rules validate their templates and check the filled result, so reaching here with
    /// one should not happen; counting it is what makes it visible if it does.
    /// To every target. §5.6's generic namespace goes this way: *"Always publish a generic
    /// namespace regardless of which host preset is active, so anything can consume it with
    /// zero configuration"* — a routing decision belongs to a rule, not to the app's own
    /// description of what the tempo is.
    void sendAddress(std::string_view address);
    void sendAddress(std::string_view address, std::int32_t value);
    void sendAddress(std::string_view address, float value);
    void sendAddress(std::string_view address, std::string_view value);

    /// The same, to the targets `outputs` selects — §5.6's *"rule subset"*. `kAllOutputs`
    /// is every one of them and is what an unrouted rule carries.
    void sendAddressTo(std::uint64_t outputs, std::string_view address);
    void sendAddressTo(std::uint64_t outputs, std::string_view address, std::int32_t value);
    void sendAddressTo(std::uint64_t outputs, std::string_view address, float value);
    void sendAddressTo(std::uint64_t outputs, std::string_view address, std::string_view value);

    /// Whether any target is selected by `outputs`. What tells "the rule sent nothing
    /// because it is routed nowhere" from "the rule never fired", which look identical.
    bool anyTargetIn(std::uint64_t outputs) const noexcept;

    std::uint64_t messagesSent() const noexcept { return sent_; }
    std::uint64_t messagesFailed() const noexcept { return failed_; }

private:
    /// One assembled message to the selected targets, counting what each one did with it.
    /// The only place a datagram leaves this class.
    void sendPacket(OscMessage& message, std::uint64_t outputs);
    void sendInt(std::string_view address, std::int32_t value, std::uint64_t outputs);
    void sendFloat(std::string_view address, float value, std::uint64_t outputs);
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

    /// A sender and the routing bit that selects it; see `addTarget`.
    struct Target {
        std::unique_ptr<OscSender> sender;
        std::uint64_t bit = 0;
    };
    std::vector<Target> targets_;

    // What was last published, so an unchanged value is not resent between beats.
    double lastBpm_ = -1.0;
    double lastConfidence_ = -1.0;
    int lastLocked_ = -1;
    std::uint32_t lastMeter_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t failed_ = 0;
};

} // namespace takt4::output
