#pragma once

#include "core/trigger/context.hpp"
#include "core/trigger/rule.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takt4::trigger {

/// HANDOFF §5.8's *"Rule model, evaluator and scheduler on the output thread"* — the last
/// two of the three.
///
/// The rules themselves are `Rule`; this holds them, decides which of them a moment
/// satisfies, and owns the one thing a rule cannot do for itself: §5.8's *"optional
/// follow-up value after a delay"*, which needs a clock and a queue.
///
/// **Not thread-safe, and on the output thread by construction.** §5.8 says so directly:
/// *"Rules are evaluated on the output thread, never the audio thread."* `output::
/// OutputRunner` already owns that thread and the transports on it, and this joins them:
/// one thread touches all three. Changing the rules from a UI goes through the runner's
/// command queue for the same reason changing an OSC target does.
///
/// The two calls the runner makes, and the difference between them:
///
///   * `onBeat` for each beat drained off the ring — §5.8's *every beat · every N beats ·
///     every bar · every N bars · on downbeat*.
///   * `advance` once a round, beat or no beat — *on tempo change · on lock or unlock · on
///     intensity change*, none of which waits for a beat, and the follow-ups that have come
///     due since the last round.
///
/// Nothing is evaluated twice: a trigger belongs to exactly one of the two.
class TriggerEngine {
public:
    /// Told about every message that goes out, and which rule sent it.
    ///
    /// §5.9 wants two things this is the only source of: *"the last-fired line on each rule
    /// card, showing the actually-sent message with a timestamp"*, and Phase 6's event log.
    /// Both need to know what was sent *and* by whom, and `Sink` deliberately does not — it
    /// is the seam to the transports, which have no business knowing what a rule is.
    ///
    /// Called on the output thread, between a beat and the next MIDI tick, so an
    /// implementation must be short and must not block. `output::OutputRunner` is the one
    /// that sets it, and copies into a ring a UI can drain from its own thread — this class
    /// stays single-threaded, which is what its class note promises.
    ///
    /// `followUp` marks §5.6's release half — the second half of a press-and-release, sent
    /// on a timer after the message it follows. It goes to the observer because the event
    /// log should show it (a release that never left is a clip left held, and invisible
    /// otherwise) and because **a rule that fires once must count once**: a Resolume connect
    /// sends a 1 and then a 0, and a counter that could not tell them apart would say every
    /// such rule had fired twice as often as it had.
    ///
    /// `slots` is what each generator produced, in the order §5.9's editor draws the chips
    /// — `Rule::lastSlots`. Empty for a follow-up, which produces no new values: it is the
    /// message just sent with one number changed.
    ///
    /// `muted` marks a fire that was **evaluated and not sent** — §5.7's per-rule mute. It
    /// still reaches the observer, and that is the point: a muted rule is running, and an
    /// operator watching the log has to be able to tell "this rule is muted" from "this rule
    /// has stopped triggering", which look identical from outside.
    using FireObserver = std::function<void(std::string_view ruleId, const Message&, bool followUp,
                                            std::span<const Value> slots, bool muted)>;

    /// The sink must outlive this. Nothing is sent until there are rules.
    explicit TriggerEngine(Sink& sink) noexcept;

    /// Set before anything fires; the output thread reads it without synchronisation.
    void setFireObserver(FireObserver observer) { observer_ = std::move(observer); }

    /// Replaces every rule. The `Rule::Config::id`s are what §5.7's
    /// `/ctl/rule/<id>/enable` and §5.9's cards address them by; a duplicate id is kept
    /// rather than dropped — a preset that holds one is an editing mistake to show, not a
    /// rule to silently discard — and `find` returns the first.
    ///
    /// **A rule that keeps its id keeps everything it was doing** — its mute and its rate, a
    /// switch a control surface flipped, where its shuffle bags and cycles have got to, its
    /// cooldown. See `Rule::carryFrom`. Every edit in §5.9's editor comes through here, a
    /// keystroke of a rename included, and rebuilding every rule from its configuration on each
    /// one replayed every bag from its seed and undid a remote enable (the audit's H7).
    ///
    /// **Unless the set is `fresh`**: a set loaded rather than edited — an IMPORT, a launch — is
    /// the show as it is written, and nothing of the set it replaces carries over, whatever ids
    /// the two share. They share them often: `add` numbers ids `rule1`, `rule2`, and the rig
    /// presets use fixed ones, so an imported show's rule used to come in muted, or at half
    /// rate, because the old show's rule of the same id had been (the audit of 2026-09-25, M11).
    ///
    /// **What is owed stays owed, at its own time.** A follow-up carries everything it needs,
    /// so the ones already queued go out when they are due whatever the rules become — a
    /// release fired early by an edit cut a laser clip or a fade short. **A fire held for its
    /// rule's delay is not owed**: it goes only while its rule is still there, switched on and
    /// not muted, and every one goes with a `fresh` set.
    void setRules(const std::vector<Rule::Config>& rules, bool fresh = false);

    /// Moves every queued follow-up's routing after the outputs (`outputs`) or the patch
    /// (`fixtures`) changed — see `remapBits`. Empty leaves that half alone. A release owed to
    /// the second output goes to that output wherever it now sits, and nowhere if it is gone,
    /// rather than to whatever took its place (the audit's H12).
    void remapPending(const std::vector<int>& outputs, const std::vector<int>& fixtures) noexcept;

    std::size_t ruleCount() const noexcept { return rules_.size(); }
    Rule& rule(std::size_t index) noexcept { return rules_[index]; }
    const Rule& rule(std::size_t index) const noexcept { return rules_[index]; }
    /// The first rule with this id, or null. §5.7's `/ctl/rule/<id>/enable` route.
    Rule* find(std::string_view id) noexcept;

    /// One beat the tracker called, with the state at it.
    void onBeat(const Context& context);

    /// **A release goes before the press it shares a moment with.** Every follow-up owed about a
    /// moment at or before `moment` is handed to the sink now, in the order it was owed, whatever
    /// the round it would otherwise have come due in — and before a beat about `moment` fires.
    ///
    /// "Release after one beat", the editor's default follow-up, is due about the moment of the
    /// next press, and it used to come due a round after the press had already gone, about half
    /// the time: its round was the one its own press fired in plus a beat, and the next press
    /// fires as it is heard or ahead of it on a prediction, so jitter decided which went first. A
    /// fixed MIDI note was cut the instant it started, and a DMX clip went to nothing. The sink
    /// still holds each for its own moment; this only puts the release in the queue first.
    void sendFollowUpsAbout(double moment);

    /// One round of the output thread: the triggers that do not wait for a beat, then any
    /// follow-up that has come due. Call it every round even when nothing has happened —
    /// that is what makes a follow-up delay mean milliseconds rather than beats.
    void advance(const Context& context);

    /// §5.8's *"on onset"*. `output::OutputRunner` calls it once per round in which the
    /// intensity classifier's onset count moved, however far it moved.
    void onOnset(const Context& context);

    /// A bar the operator declared with a late DOWNBEAT press: its first beat had already gone
    /// out as another beat, so the bar and downbeat rules that should have fired on it did
    /// not. They fire now, on `context` — which the caller gives `beatInBar` 1 and the
    /// declared bar's number — late by however long the press took, rather than skipping the
    /// bar (the audit's M4). Beat-counting rules are not touched: that beat did fire them.
    void onBarDeclared(const Context& context);

    /// §5.8's *"on manual hotkey"* — fires every rule whose trigger is `Manual`, conditions
    /// and all.
    void manual(const Context& context);

    /// §5.9's per-rule `[test]` button: fires one rule **whatever its trigger says and
    /// whatever its conditions say**, so that an operator building a rule can see where the
    /// message lands without waiting for a downbeat at the right confidence. Its cooldown
    /// and its generators advance as they would on a real fire, because the point is to see
    /// what it will really send. False when there is no such rule, or it could not build a
    /// message. A disabled rule still tests: that is the state you are most likely to be in
    /// while building one. **Not held for the rule's delay** (`Rule::Config::delayOn`): a test
    /// is pressed to see where the message lands, now.
    ///
    /// **And what it holds, let go of a moment later, when nothing else would** (the operator,
    /// 2026-10-06: a laser left playing is "dangerous"). A note on or a laser clip tested from a
    /// rule that is muted or switched off, or while the tracker is stopped, has nothing to fire
    /// it again or take it off: its note off, or no clip, follows `kTestHoldSeconds` later — or
    /// a beat, if that is longer — on top of any follow-up the rule owes of its own.
    bool test(std::string_view id, const Context& context);

    /// How long a test that nothing else would let go of holds what it lit. See `test`.
    static constexpr double kTestHoldSeconds = 0.5;

    /// **What `id` holds, let go of now** — for a rule muted, switched off, deleted or re-aimed
    /// (`output::OutputRunner`, which knows what each rule holds). Its fires still held for its
    /// delay are dropped, and every MIDI and OSC follow-up it owes is sent now — no earlier than
    /// the press it follows, so a release never overtakes a note on still held for a target's
    /// delay. Its lighting follow-ups are left owed at their time: a lamp's fade out is not a
    /// hazard, and a laser's clip is taken off by the runner.
    void releaseRule(std::string_view id, double now);

    /// Sends `message` as a follow-up of `ruleId` — what the runner releases a held note or a
    /// laser zone with, so the log shows the release beside the press it lets go of.
    void sendRelease(std::string_view ruleId, const Message& message) {
        deliver(message, ruleId, true, {});
    }

    /// Every fire held for a rule's delay, dropped unsent — for the input going quiet, which
    /// fires nothing new. PANIC and Stop drop them too (`panic`, `flushFollowUps`).
    void dropHeldFires() noexcept;

    /// Fires held for their rule's delay — see `Rule::Config::delayOn`.
    std::size_t heldFires() const noexcept;

    /// §5.8's PANIC: *"A global halt that stops every rule instantly, reachable from the UI,
    /// a keyboard shortcut, OSC and MIDI. Non-negotiable for live use."*
    ///
    /// It is a latch, not a flush: nothing fires again until `release()`. But every
    /// follow-up still owed **is sent immediately** rather than dropped, and that is the
    /// deliberate part. §5.6's shape is press-then-release — `int 1 (press) then 0
    /// (release)` — so the pending half is what turns a clip *off*. Dropping it would leave
    /// the rig latched into exactly the state panic was hit to escape.
    void panic(const Context& context);
    void release() noexcept { panicked_ = false; }
    bool panicked() const noexcept { return panicked_; }

    /// Whether the tracker is listening — `output::OutputRunner`'s Start and Stop. While it is
    /// not, **nothing the music would fire fires**: no beat, onset or declared bar, and no change
    /// of tempo, lock or intensity. What an operator fires by hand — `manual`, `test` — still
    /// does, with its follow-ups: building a rig with the tracker stopped is ordinary.
    ///
    /// STOP's half of the audit of 2026-09-25, H3. The engine's last state stays published after
    /// it stops, so a rule watching it could still fire on the way down. Listening until told
    /// otherwise, which is every engine the console and the tests drive.
    void setListening(bool on) noexcept { listening_ = on; }
    bool listening() const noexcept { return listening_; }

    /// Sends every follow-up still owed, now rather than when it comes due, and latches
    /// nothing. `panic` without the halt. A fire still held for its rule's delay is dropped: it
    /// is a press, and the rig is stopping.
    ///
    /// **For a stop.** A note on whose note off has not yet come due is a laser still lit, and
    /// an operator pressing Stop has said the opposite — so the release goes out on the way
    /// down rather than being dropped with the thread that owed it. It is the same argument
    /// `panic` and `setRules` already make, and the same one the operator met as a clip that
    /// would not let go.
    void flushFollowUps() { flushPending(); }

    /// Sends now the part of every owed follow-up that goes to `outputs` (routing bits), and
    /// leaves the rest owed at its own time. Lighting is not touched: it goes to fixtures, not
    /// to an output.
    ///
    /// **For an output that is going** — switched off, deleted, or pointed at another device.
    /// Its port closes as it goes, so a note off owed to it later went nowhere and the synth
    /// on the end of it kept playing the note (the operator, 2026-09-30).
    void flushFollowUpsTo(std::uint64_t outputs);

    /// Messages handed to the sink, follow-ups included.
    std::uint64_t sent() const noexcept { return sent_; }
    /// Fires whose message could not be built — an address that came out illegal once its
    /// generators had filled it in. Worth a number rather than silence: a rule dropping
    /// every fire looks exactly like a rule that never triggers.
    std::uint64_t dropped() const noexcept { return dropped_; }
    /// Fires that happened and were not sent, because their rule was muted.
    std::uint64_t muted() const noexcept { return muted_; }
    /// Follow-ups waiting for their delay to pass, and fires waiting for their rule's.
    std::size_t pending() const noexcept { return pending_.size(); }

private:
    /// A follow-up and when it comes due, on `Context::now` — or a fire held for its rule's delay.
    struct Pending {
        double due = 0.0;
        Message message;
        /// Which rule owes it, so the observer can name the sender of a release the same
        /// way it names the press. Copied rather than pointed at: the rule set can be
        /// replaced while a follow-up is still owed, and §5.8 says those are still sent.
        std::string ruleId;
        /// **A fire, not a follow-up**: held for `Rule::Config::delayOn`. Dropped rather than
        /// sent early by everything that stops the rig, and by its rule going quiet before it
        /// is due; its follow-ups wait inside it until it goes.
        bool press = false;
        /// A held fire's: what its generators drew, for the observer, and what it owes, each
        /// with its delay after it.
        std::vector<Value> slots;
        std::vector<std::pair<double, Message>> owed;
        /// A follow-up's: the moment of the press it follows. A release sent early is held to
        /// no earlier than this, so it cannot overtake its own press in a target's queue.
        double pressMoment = 0.0;
    };

    /// Fires one rule that has already passed its trigger and its conditions: builds the
    /// message, sends it — or holds it for the rule's delay — and queues the follow-up.
    ///
    /// `force` sends even from a muted rule, and at once whatever the rule's delay — the
    /// [test] button, and nothing else.
    bool dispatch(Rule& rule, const Context& context, bool force = false);
    /// Queues what `message` owes, each `delay` seconds after `due`.
    void queueFollowUps(std::vector<std::pair<double, Message>>& owed, const Message& message,
                        double due, const std::string& ruleId);
    /// Everything whose delay has passed, in the order it was queued.
    void drainDue(double now);
    void flushPending();
    void deliver(const Message& message, std::string_view ruleId, bool followUp,
                 std::span<const Value> slots);
public:
    /// Whether a beat satisfies `rule`'s trigger — §5.8's WHEN stage, for the four that
    /// count beats and bars. Public for the tests, which state the bar grid with it.
    static bool beatSatisfies(const Rule& rule, const Context& context) noexcept;

private:

    Sink& sink_;
    FireObserver observer_;
    std::vector<Rule> rules_;
    std::vector<Pending> pending_;
    /// What `Rule::followUpsFor` filled in for the fire being dispatched, reused so that a
    /// rule owing several messages allocates nothing after its first fire — and the same with
    /// each one's delay settled.
    std::vector<std::pair<std::size_t, Message>> owed_;
    std::vector<std::pair<double, Message>> delayed_;
    bool panicked_ = false;
    bool listening_ = true;
    std::uint64_t sent_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t muted_ = 0;
};

} // namespace takt4::trigger
