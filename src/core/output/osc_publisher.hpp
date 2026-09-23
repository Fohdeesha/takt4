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
/// Whether `prefix` is one `OscPublisher` will take: it starts with '/', does not end with
/// one, and every address built from it is a legal OSC address.
///
/// Its own function so that a settings file can be checked **before** a publisher is built
/// from it. The constructor throws on a bad prefix, and it runs inside `Transports`'s — so a
/// hand-edited `"oscPrefix": "vj"` used to stop takt4 opening at all, with no window and no
/// message, on every launch until somebody found the file.
bool isValidOscPrefix(std::string_view prefix);

class OscPublisher {
public:
    /// The namespace prefix, so a second instance on the same network can be told apart.
    /// "/takt4" by default; a trailing '/' is not wanted and not accepted — see
    /// `isValidOscPrefix`, which is the test this throws on.
    explicit OscPublisher(std::string prefix = "/takt4");

    /// Adds a target, at the next routing bit. Throws std::runtime_error if the host
    /// cannot be resolved.
    ///
    /// `bit` is which bit of a rule's `Message::outputs` mask selects this one — the index
    /// of the target in `Transports`' own list, so that a rig with an OSC target, a MIDI
    /// target and another OSC target routes to bits 0 and 2 here rather than 0 and 1.
    /// Anything past `kMaxRoutableTargets` is given `kAllOutputs`, so it still receives
    /// everything a rule sends everywhere.
    /// `delaySeconds` offsets everything bound for this target — later when positive, earlier
    /// (that is, ahead of the *next* beat) when negative. See `OutputTarget::delaySeconds`.
    void addTarget(std::string_view host, std::uint16_t port, std::size_t bit,
                   double delaySeconds = 0.0);

    /// The output thread's clock, which is what a delay is measured against. Set before
    /// anything is published so a message queued this round is due relative to *this* round.
    void setNow(double now) noexcept { now_ = now; }
    double now() const noexcept { return now_; }

    /// How long one beat currently lasts, in seconds — what a negative offset is subtracted
    /// from. Zero when no tempo is known, which makes a negative offset hold nothing rather
    /// than invent a beat length.
    void setBeatSeconds(double seconds) noexcept { beatSeconds_ = seconds > 0.0 ? seconds : 0.0; }

    /// §5.5's latency offset, added to every target's own before either is applied.
    ///
    /// The whole-rig control and the per-target one are the same quantity measured from two
    /// places — one says "everything downstream of me is this late", the other "this device
    /// is". They compose by adding, and the UI shows each row's total for that reason.
    void setOffsetSeconds(double seconds) noexcept { offsetSeconds_ = seconds; }

    /// Sends everything whose delay has run out. Called every round by `Transports::advance`
    /// — a delayed message is not waiting for the next beat, it is waiting for a clock.
    void flushDue();

    /// Datagrams waiting on a delay, and those dropped because too many were. The queue is
    /// bounded: a target delayed a second while a rule fires on every 32nd note holds tens
    /// of messages, not thousands, and something has gone wrong upstream if it holds more.
    std::size_t pending() const noexcept { return pending_.size(); }
    std::uint64_t dropped() const noexcept { return dropped_; }

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
    /// How long a message offset by `delaySeconds` waits, in seconds — the whole-rig offset
    /// included. Never negative: see `OutputTarget::delaySeconds` for why "earlier" is a
    /// shorter wait and not an earlier clock, and `kMinOutputDelaySeconds` for why it stops
    /// at zero rather than going back another beat.
    double holdFor(double delaySeconds) const noexcept;

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
        double delaySeconds = 0.0;
    };
    std::vector<Target> targets_;

    /// One datagram waiting for its target's delay to run out. The bytes are copied rather
    /// than the message kept, because `OscMessage`'s buffer is reused by the next send and a
    /// held span would be rewritten under the queue.
    struct Pending {
        double due = 0.0;
        std::size_t target = 0;
        std::vector<std::byte> packet;
    };
    /// FIFO, and in due order because a target's delay is constant while its messages queue:
    /// two messages to one target keep the order they were sent in, which is the whole point
    /// of delaying rather than dropping. Changing a delay does not reorder what is already
    /// queued — see `flushDue`.
    std::vector<Pending> pending_;
    double now_ = 0.0;
    double beatSeconds_ = 0.0;
    double offsetSeconds_ = 0.0;
    std::uint64_t dropped_ = 0;

    // What was last published, so an unchanged value is not resent between beats.
    double lastBpm_ = -1.0;
    double lastConfidence_ = -1.0;
    int lastLocked_ = -1;
    std::uint32_t lastMeter_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t failed_ = 0;
};

} // namespace takt4::output
