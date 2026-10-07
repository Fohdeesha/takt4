#pragma once

#include "core/output/osc_message.hpp"
#include "core/output/osc_sender.hpp"
#include "core/output/output_target.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
/// To every target that wants them — `OutputTarget::sendsNamespace`, which an output has off
/// until it is asked for. A target with it off is sent the rules aimed at it and nothing of this.
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

    /// Adds a target, at the next routing bit. Throws std::runtime_error only when a literal
    /// address cannot have a socket at all (`OscSender`): a name is looked up on a thread of its
    /// own, and one that does not resolve is the target's `problem()`, not a throw.
    ///
    /// `bit` is which bit of a rule's `Message::outputs` mask selects this one — the index
    /// of the target in `Transports`' own list, so that a rig with an OSC target, a MIDI
    /// target and another OSC target routes to bits 0 and 2 here rather than 0 and 1.
    /// Anything past `kMaxRoutableTargets` has no bit of its own: it receives everything a rule
    /// sends everywhere, and nothing routed (it used to be given every bit, and so every rule
    /// routed anywhere).
    /// `delaySeconds` offsets everything bound for this target — later when positive, earlier
    /// when negative, measured from the moment a message is about. See
    /// `OutputTarget::delaySeconds`.
    void addTarget(std::string_view host, std::uint16_t port, std::size_t bit,
                   double delaySeconds = 0.0);

    /// One target as `setTargets` wants it: which output it is (`OutputTarget::id`), where it
    /// sends, its routing bit, its delay and whether it is sent the namespace
    /// (`OutputTarget::sendsNamespace`). On here, as for `addTarget`: this class sends what it is
    /// told to whoever it is given, and `Transports` says for each output whether it wants it.
    struct TargetSpec {
        std::string id;
        std::string host;
        std::uint16_t port = 0;
        std::size_t bit = 0;
        double delaySeconds = 0.0;
        bool sendsNamespace = true;
    };
    /// Replaces the targets with `specs`, **keeping what did not change** (the audit's H12).
    ///
    /// Every output edit used to clear every sender and build them all again, on the output
    /// thread: what was queued for a delayed target was dropped, releases included, so a clip
    /// was left latched; and a dragged delay slider did it on every pixel. Now a target keeps
    /// its sender while its id and its host and port are the same; one whose address was
    /// edited gets a new sender and keeps what is queued for it; and only what was queued for a
    /// target that is gone — deleted or switched off, which both mean *send it nothing* — is
    /// dropped. (`OutputRunner` sends it first, with `flushTo`: a release among it is what lets
    /// go of a clip that target was sent.)
    ///
    /// A target whose namespace is switched off loses what of the namespace is queued for it, and
    /// one switched on is told the state at the next round, as a new target is.
    ///
    /// Returns, for each spec whose sender could not be made at all, its bit and why.
    std::vector<std::pair<std::size_t, std::string>> setTargets(const std::vector<TargetSpec>& specs);

    /// Moves the delay of the target on routing bit `bit`, and nothing else. What the delay
    /// slider sends while it is dragged. What is already queued keeps the moment it was given.
    void setDelay(std::size_t bit, double delaySeconds) noexcept;

    /// Which output target `index` is — its routing bit, as `addTarget` and `setTargets` were
    /// given it.
    std::size_t outputOf(std::size_t index) const noexcept { return targets_[index].output; }
    /// Lets every target find its address and open its socket if it can — see
    /// `OscSender::ready`. Called now and then by the output thread, so a target nothing is
    /// being sent to still resolves, and still says when it cannot.
    void refresh() noexcept;

    /// The output thread's clock, which is what a delay is measured against. Set before
    /// anything is published so a message queued this round is due relative to *this* round.
    void setNow(double now) noexcept { now_ = now; }
    double now() const noexcept { return now_; }

    /// §5.5's latency offset, added to every target's own before either is applied.
    ///
    /// The whole-rig control and the per-target one are the same quantity measured from two
    /// places — one says "everything downstream of me is this late", the other "this device
    /// is". They compose by adding, and the UI shows each row's total for that reason.
    void setOffsetSeconds(double seconds) noexcept { offsetSeconds_ = seconds; }

    /// Sends everything whose delay has run out. Called every round by `Transports::advance`
    /// — a delayed message is not waiting for the next beat, it is waiting for a clock.
    void flushDue();
    /// Sends everything waiting, now, whatever it was waiting for. For a PANIC and a stop: a
    /// release held back for a delayed target is still a release, and a rig told to halt must
    /// not be left latched on until the next Start.
    void flushAll();
    /// Sends now everything waiting for the targets on routing bits `outputs`, and keeps the
    /// rest waiting. Before those targets go: `setTargets` drops what is queued for a target
    /// that is gone, and a release is among it.
    void flushTo(std::uint64_t outputs);

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
    OscSender& target(std::size_t index) noexcept { return *targets_[index].sender; }

    /// Publishes one beat, and any state that changed with it. `moment` is when the beat is in
    /// the music, on the clock `setNow` is given — ahead of now when the output thread fires it
    /// on a prediction — and each target has it at that moment plus its offset. Unset is now.
    void publishBeat(const tracking::BeatEvent& event, std::optional<double> moment = std::nullopt);

    /// Publishes state that changed since the last call, without a beat. Cheap to call
    /// every frame: when nothing has changed it sends nothing.
    void publishState(const tracking::TempoState& state);

    /// §5.6's resync: tell downstream to snap. Sent by itself whenever the published lock goes
    /// from off to on — see `sendChangedState` — and callable for anything else that means the
    /// same.
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
    /// To every target, whether or not it takes the namespace: this is an address of a rule's.
    void sendAddress(std::string_view address);
    void sendAddress(std::string_view address, std::int32_t value);
    void sendAddress(std::string_view address, float value);
    void sendAddress(std::string_view address, std::string_view value);

    /// The same, to the targets `outputs` selects — §5.6's *"rule subset"*. `kAllOutputs`
    /// is every one of them and is what an unrouted rule carries.
    ///
    /// `moment` is when the thing the message is about happens — `trigger::Message::moment` —
    /// and each target has it at that moment plus the rig's offset plus its own, or now if that
    /// has already gone. Unset is now.
    void sendAddressTo(std::uint64_t outputs, std::string_view address,
                       std::optional<double> moment = std::nullopt);
    void sendAddressTo(std::uint64_t outputs, std::string_view address, std::int32_t value,
                       std::optional<double> moment = std::nullopt);
    void sendAddressTo(std::uint64_t outputs, std::string_view address, float value,
                       std::optional<double> moment = std::nullopt);
    void sendAddressTo(std::uint64_t outputs, std::string_view address, std::string_view value,
                       std::optional<double> moment = std::nullopt);

    /// Whether any target is selected by `outputs`. What tells "the rule sent nothing
    /// because it is routed nowhere" from "the rule never fired", which look identical.
    bool anyTargetIn(std::uint64_t outputs) const noexcept;

    std::uint64_t messagesSent() const noexcept { return sent_; }
    std::uint64_t messagesFailed() const noexcept { return failed_; }

private:
    /// One assembled message to the selected targets, counting what each one did with it.
    /// The only place a datagram leaves this class.
    ///
    /// Each target has it at `moment` plus the rig's offset plus its own delay, or at once
    /// when that has already gone. **Nothing else decides when**, and that is the audit's H4:
    /// this used to turn a negative offset into "hold for what is left of a beat" and apply it
    /// to every message, so a bar-1 clip cue landed just before beat 2 and a manual cue nearly
    /// a beat late. A message about a beat the output thread is firing ahead of time now carries
    /// that beat's own moment, and "earlier" means earlier than it; a message about now goes now.
    ///
    /// `own` is a message of the namespace's, which skips a target that does not take it.
    void sendPacket(OscMessage& message, std::uint64_t outputs, double moment, bool own);
    void sendInt(std::string_view address, std::int32_t value, std::uint64_t outputs, double moment,
                 bool own);
    void sendFloat(std::string_view address, float value, std::uint64_t outputs, double moment,
                   bool own);
    /// Sends the four state addresses whose value has moved, and remembers them.
    void sendChangedState(double bpm, double confidence, bool locked, std::uint32_t meter,
                          bool force, double moment);

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
        /// The routing bit's number, and the index of the output in `Transports`' list.
        std::size_t output = 0;
        /// `OutputTarget::id`, which is what `setTargets` recognises a target by. Empty for one
        /// added with `addTarget`.
        std::string id;
        /// `OutputTarget::sendsNamespace`.
        bool sendsNamespace = true;
    };
    std::vector<Target> targets_;

    /// One datagram waiting for its target's delay to run out. The bytes are copied rather
    /// than the message kept, because `OscMessage`'s buffer is reused by the next send and a
    /// held span would be rewritten under the queue.
    ///
    /// **Its moment, not a time it is due**: when it goes is worked out as it is looked at, from
    /// the rig's offset and the target's delay as they are then — so a delay dragged shorter
    /// cannot send a release held after its press ahead of it. See `RuleSink::HeldMidi`.
    struct Pending {
        double moment = 0.0;
        std::size_t target = 0;
        std::vector<std::byte> packet;
        /// One of the namespace's, dropped if its target stops taking the namespace first.
        bool own = false;
    };
    /// When `item` goes, as things are now; never for a target that is gone.
    double dueOf(const Pending& item) const noexcept;
    /// Sends now everything held for target `index` due by `until`, in the order held — before a
    /// datagram for it that is going at once, so it cannot overtake them.
    void sendHeld(std::size_t index, double until);
    /// Kept in the order queued, and sent in that order among whatever has come due. Two
    /// messages about the same moment to one target — a beat's state and then the beat —
    /// keep their order, which is the whole point of delaying rather than dropping. A message
    /// about *now* can overtake one about a beat still to come, and should: the beat has not
    /// happened yet.
    std::vector<Pending> pending_;
    /// Reused by `flushDue`.
    std::vector<std::size_t> lastDue_;
    double now_ = 0.0;
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
