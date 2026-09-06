#include "core/trigger/trigger_engine.hpp"

#include <algorithm>
#include <utility>

namespace takt4::trigger {

TriggerEngine::TriggerEngine(Sink& sink) noexcept : sink_(sink) {}

void TriggerEngine::setRules(const std::vector<Rule::Config>& rules) {
    rules_.clear();
    rules_.reserve(rules.size());
    for (const Rule::Config& config : rules) {
        rules_.emplace_back(config);
    }
    // The old rules' follow-ups belong to rules that no longer exist. They are still owed:
    // a clip pressed by the preset being replaced would stay latched on if its release went
    // with it, which is the same argument panic() makes.
    flushPending();
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
    const std::uint64_t every = std::max<std::uint64_t>(1, config.every);
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

void TriggerEngine::deliver(const Message& message, std::string_view ruleId) {
    sink_.send(message);
    ++sent_;
    if (observer_) {
        // After the sink, so what an observer is told about has already gone out. A UI
        // showing a message that then failed to send would be worse than one showing
        // nothing — `RuleSink`'s own counters are where "it went nowhere" is reported.
        observer_(ruleId, message);
    }
}

bool TriggerEngine::dispatch(Rule& rule, const Context& context) {
    const std::optional<Message> message = rule.fire(context);
    if (!message) {
        ++dropped_;
        return false;
    }
    deliver(*message, rule.id());
    if (const std::optional<Message> follow = rule.followUpFor(*message)) {
        // Never in the past, however the delay was configured: a follow-up due before the
        // message it follows would be sent in the same round and read as a rule that sends
        // its release first.
        const double delay = std::max(0.0, rule.config().followUpDelaySeconds);
        pending_.push_back(Pending{context.now + delay, *follow, rule.id()});
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
            deliver(pending_[i].message, pending_[i].ruleId);
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
        deliver(waiting.message, waiting.ruleId);
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
    return dispatch(*rule, context);
}

void TriggerEngine::panic(const Context& context) {
    (void)context; // the clock is not consulted: everything owed goes now, not when due
    panicked_ = true;
    flushPending();
}

} // namespace takt4::trigger
