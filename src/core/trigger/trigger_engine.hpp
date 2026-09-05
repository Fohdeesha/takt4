#pragma once

#include "core/trigger/context.hpp"
#include "core/trigger/rule.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
    /// The sink must outlive this. Nothing is sent until there are rules.
    explicit TriggerEngine(Sink& sink) noexcept;

    /// Replaces every rule. The `Rule::Config::id`s are what §5.7's
    /// `/ctl/rule/<id>/enable` and §5.9's cards address them by; a duplicate id is kept
    /// rather than dropped — a preset that holds one is an editing mistake to show, not a
    /// rule to silently discard — and `find` returns the first.
    void setRules(const std::vector<Rule::Config>& rules);

    std::size_t ruleCount() const noexcept { return rules_.size(); }
    Rule& rule(std::size_t index) noexcept { return rules_[index]; }
    const Rule& rule(std::size_t index) const noexcept { return rules_[index]; }
    /// The first rule with this id, or null. §5.7's `/ctl/rule/<id>/enable` route.
    Rule* find(std::string_view id) noexcept;

    /// One beat the tracker called, with the state at it.
    void onBeat(const Context& context);

    /// One round of the output thread: the triggers that do not wait for a beat, then any
    /// follow-up that has come due. Call it every round even when nothing has happened —
    /// that is what makes a follow-up delay mean milliseconds rather than beats.
    void advance(const Context& context);

    /// §5.8's *"on onset"*. Nothing calls this yet; the spectral-flux classifier that will
    /// is the next piece of Phase 6.
    void onOnset(const Context& context);

    /// §5.8's *"on manual hotkey"* — fires every rule whose trigger is `Manual`, conditions
    /// and all.
    void manual(const Context& context);

    /// §5.9's per-rule `[test]` button: fires one rule **whatever its trigger says and
    /// whatever its conditions say**, so that an operator building a rule can see where the
    /// message lands without waiting for a downbeat at the right confidence. Its cooldown
    /// and its generators advance as they would on a real fire, because the point is to see
    /// what it will really send. False when there is no such rule, or it could not build a
    /// message. A disabled rule still tests: that is the state you are most likely to be in
    /// while building one.
    bool test(std::string_view id, const Context& context);

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

    /// Every generator back to the start, every cooldown cleared, every follow-up dropped.
    /// For a preset load; not for panic, which owes its follow-ups.
    void reset() noexcept;

    /// Messages handed to the sink, follow-ups included.
    std::uint64_t sent() const noexcept { return sent_; }
    /// Fires whose message could not be built — an address that came out illegal once its
    /// generators had filled it in. Worth a number rather than silence: a rule dropping
    /// every fire looks exactly like a rule that never triggers.
    std::uint64_t dropped() const noexcept { return dropped_; }
    /// Follow-ups waiting for their delay to pass.
    std::size_t pending() const noexcept { return pending_.size(); }

private:
    /// A follow-up and when it comes due, on `Context::now`.
    struct Pending {
        double due = 0.0;
        Message message;
    };

    /// Fires one rule that has already passed its trigger and its conditions: builds the
    /// message, sends it, and queues the follow-up.
    bool dispatch(Rule& rule, const Context& context);
    /// Everything whose delay has passed, in the order it was queued.
    void drainDue(double now);
    void flushPending();
    void deliver(const Message& message);
    /// Whether a beat satisfies `rule`'s trigger — §5.8's WHEN stage, for the four that
    /// count beats and bars.
    static bool beatSatisfies(const Rule& rule, const Context& context) noexcept;

    Sink& sink_;
    std::vector<Rule> rules_;
    std::vector<Pending> pending_;
    bool panicked_ = false;
    std::uint64_t sent_ = 0;
    std::uint64_t dropped_ = 0;
};

} // namespace takt4::trigger
