#include "core/trigger/trigger_engine.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace takt4::trigger {

TriggerEngine::TriggerEngine(Sink& sink) noexcept : sink_(sink) {}

void TriggerEngine::setRules(const std::vector<Rule::Config>& rules) {
    // **A rule that keeps its id carries on**, by id.
    //
    // Every edit in §5.9's editor replaces the whole set — that is the command's stated
    // purpose — so without this, renaming a rule mid-set would unmute every muted rule, put
    // every ÷2 back, replay every shuffle bag from its seed and switch back on a rule a Stream
    // Deck had switched off. The operator would have made one edit and silently undone half a
    // dozen decisions made ten minutes earlier.
    //
    // By id rather than by position, for the reason `RulesController::firesSeen_` gives: a
    // rule deleted from the middle would otherwise shift every state below it onto a
    // different rule. And a *preset load* brings new ids, so nothing carries over — which is
    // exactly right, because a preset is the show and not somebody's half-played set.
    std::vector<Rule> next;
    next.reserve(rules.size());
    for (const Rule::Config& config : rules) {
        next.emplace_back(config);
        for (const Rule& previous : rules_) {
            if (previous.id() == config.id) {
                next.back().carryFrom(previous);
                break;
            }
        }
    }
    rules_ = std::move(next);
    // And nothing owed is paid early. A queued follow-up is a whole message with its own
    // routing, so it is still right when it comes due — including one owed by a rule this
    // replaced. See the header.
}

void TriggerEngine::remapPending(const std::vector<int>& outputs,
                                 const std::vector<int>& fixtures) noexcept {
    for (Pending& waiting : pending_) {
        if (!outputs.empty()) {
            waiting.message.outputs = remapBits(waiting.message.outputs, outputs);
        }
        if (!fixtures.empty()) {
            waiting.message.fixtures = remapBits(waiting.message.fixtures, fixtures);
        }
    }
}

Rule* TriggerEngine::find(std::string_view id) noexcept {
    for (Rule& rule : rules_) {
        if (rule.id() == id) {
            return &rule;
        }
    }
    return nullptr;
}

void TriggerEngine::reset() noexcept {
    for (Rule& rule : rules_) {
        rule.reset();
    }
    pending_.clear();
    panicked_ = false;
}

bool TriggerEngine::beatSatisfies(const Rule& rule, const Context& context) noexcept {
    const Rule::Config& config = rule.config();
    // `effectiveEvery` and not `config.every`: §5.7's `double` and `halve` are a live gesture
    // on top of what the operator wrote, like the tempo ÷2 button, and they act here so that
    // the counting stays in phase — a rule on every 4 bars taken to every 8 still lands on
    // bar 1, because both are counted from the first bar rather than from when the gesture
    // was made.
    const std::uint64_t every = std::max<std::uint64_t>(1, rule.effectiveEvery());
    switch (config.trigger) {
    case Trigger::Beat:
        // Counted from the first beat, so "every 4 beats" is beats 1, 5, 9 — an operator
        // counting a phrase in starts at one, not at whichever beat the modulo lands on.
        return context.beats >= 1 && (context.beats - 1) % every == 0;
    case Trigger::Bar:
        // A bar begins on the beat the tracker calls the bar's first, and `bars` has
        // already counted it by then — see tracking::TempoTracker::advanceBar.
        return context.beatInBar == 1 && context.bars >= 1 && (context.bars - 1) % every == 0;
    case Trigger::Downbeat:
        return context.beatInBar == 1;
    case Trigger::Euclid:
        // Counted from the first beat, like `Beat` above and for the same reason — an
        // operator counting a phrase in starts at one — so the pattern's step 0 is beat 1
        // and a 3-in-8 lands on beats 1, 4 and 7 of every eight.
        return context.beats >= 1 &&
               euclidHit(static_cast<std::uint32_t>((context.beats - 1) % every), config.pulses,
                         static_cast<std::uint32_t>(every));
    default:
        return false;
    }
}

void TriggerEngine::deliver(const Message& message, std::string_view ruleId, bool followUp,
                            std::span<const Value> slots) {
    sink_.send(message);
    ++sent_;
    if (observer_) {
        // After the sink, so what an observer is told about has already gone out. A UI
        // showing a message that then failed to send would be worse than one showing
        // nothing — `RuleSink`'s own counters are where "it went nowhere" is reported.
        observer_(ruleId, message, followUp, slots, false);
    }
}

bool TriggerEngine::dispatch(Rule& rule, const Context& context, bool force) {
    std::optional<Message> message = rule.fire(context);
    if (!message) {
        ++dropped_;
        return false;
    }
    // What the message is about, and so what every target's offset is measured from: the
    // beat's own moment when one was given, and now for everything that happens as it is
    // judged. See `Message::moment`.
    message->moment = context.moment.value_or(context.now);
    if (rule.muted() && !force) {
        // **The rule has run.** Its generators advanced, its cooldown started and its fire
        // count moved, which is the whole difference between muted and disabled: unmuting
        // rejoins the music where it is rather than restarting a shuffle bag mid-set.
        //
        // The follow-ups are suppressed with the press, not delivered on their own. A press
        // that never left must not be followed by a release that does — on OSC that is a clip
        // switched off that was never switched on, and on DMX it is a fade to black out of
        // nowhere.
        ++muted_;
        if (observer_) {
            observer_(rule.id(), *message, false, rule.lastSlots(), true);
        }
        return true;
    }
    deliver(*message, rule.id(), false, rule.lastSlots());
    // Reused across fires, so a rule that owes two messages allocates nothing after the
    // first. Cleared here rather than by the callee, which appends.
    owed_.clear();
    rule.followUpsFor(context, *message, owed_);
    for (auto& [index, follow] : owed_) {
        // Never in the past, however the delay was configured: a follow-up due before the
        // message it follows would be sent in the same round and read as a rule that sends
        // its release first. `Rule::followUpDelay` is what turns "two beats" into seconds,
        // and it does it here — against the tempo the press went out at, not the one playing
        // when the release comes due.
        const double delay = std::max(0.0, rule.followUpDelay(context, index));
        // Handed over a delay after the press was, and *about* a moment a delay after the
        // press's — so every target, however it is offset, hears the release exactly `delay`
        // after it heard the press. Measuring the release from the round that sends it instead
        // would put it ahead of its own press on a target offset later than the earliest one.
        follow.moment = message->moment + delay;
        pending_.push_back(Pending{context.now + delay, follow, rule.id()});
    }
    return true;
}

void TriggerEngine::drainDue(double now) {
    if (pending_.empty()) {
        return;
    }
    // In the order queued, which for follow-ups of the same rule is the order they were
    // fired in. Two rules whose delays interleave come out interleaved, which is what the
    // clock says and what a receiver expects.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        if (pending_[i].due <= now) {
            deliver(pending_[i].message, pending_[i].ruleId, true, {});
        } else {
            if (kept != i) {
                pending_[kept] = std::move(pending_[i]);
            }
            ++kept;
        }
    }
    pending_.resize(kept);
}

void TriggerEngine::flushPending() {
    for (const Pending& waiting : pending_) {
        deliver(waiting.message, waiting.ruleId, true, {});
    }
    pending_.clear();
}

void TriggerEngine::onBeat(const Context& context) {
    if (panicked_) {
        return;
    }
    for (Rule& rule : rules_) {
        if (!rule.enabled() || !rule.valid()) {
            continue;
        }
        if (!beatSatisfies(rule, context)) {
            continue;
        }
        if (!rule.conditionsHold(context)) {
            continue;
        }
        dispatch(rule, context);
    }
}

void TriggerEngine::advance(const Context& context) {
    // Before the panic check: a follow-up already owed is sent whatever else has happened,
    // which is the same argument panic() itself makes about not leaving a rig latched on.
    drainDue(context.now);
    for (Rule& rule : rules_) {
        switch (rule.config().trigger) {
        case Trigger::TempoChange:
        case Trigger::LockChange:
        case Trigger::IntensityChange:
            break;
        default:
            continue;
        }
        // Every round, enabled or not, panicked or not: what a rule remembers is how it
        // tells a change from a value it has already seen, and one that stopped watching
        // would fire the moment it was switched back on. See Rule::seesChange.
        const bool changed = rule.seesChange(context);
        if (!changed || panicked_ || !rule.enabled() || !rule.valid()) {
            continue;
        }
        if (!rule.conditionsHold(context)) {
            continue;
        }
        dispatch(rule, context);
    }
}

void TriggerEngine::onOnset(const Context& context) {
    if (panicked_) {
        return;
    }
    for (Rule& rule : rules_) {
        if (rule.config().trigger != Trigger::Onset || !rule.enabled() || !rule.valid()) {
            continue;
        }
        if (!rule.conditionsHold(context)) {
            continue;
        }
        dispatch(rule, context);
    }
}

void TriggerEngine::manual(const Context& context) {
    if (panicked_) {
        return;
    }
    for (Rule& rule : rules_) {
        if (rule.config().trigger != Trigger::Manual || !rule.enabled() || !rule.valid()) {
            continue;
        }
        if (!rule.conditionsHold(context)) {
            continue;
        }
        dispatch(rule, context);
    }
}

bool TriggerEngine::test(std::string_view id, const Context& context) {
    Rule* rule = find(id);
    if (rule == nullptr || !rule->valid()) {
        return false;
    }
    // Deliberately past the trigger, the conditions and even `enabled` — see the header.
    // Not past panic, which means what it says.
    if (panicked_) {
        return false;
    }
    // Past `muted` too, and for the same reason it is past `enabled`: an operator pressing
    // [test] has asked to see where the message lands, and a test button that silently did
    // nothing because of a state set from a Stream Deck ten minutes ago would be the least
    // debuggable control in the app.
    return dispatch(*rule, context, true);
}

void TriggerEngine::panic(const Context& context) {
    (void)context; // the clock is not consulted: everything owed goes now, not when due
    panicked_ = true;
    flushPending();
}

} // namespace takt4::trigger
