#include "core/trigger/trigger_engine.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace takt4::trigger {

TriggerEngine::TriggerEngine(Sink& sink) noexcept : sink_(sink) {}

void TriggerEngine::setRules(const std::vector<Rule::Config>& rules, bool fresh) {
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
    // different rule. And not at all for a set that was loaded (`fresh`): a preset is the show
    // and not somebody's half-played set, and it cannot be told apart by its ids — this said a
    // preset load brought new ids, and an import of a show whose rules were called what the
    // last show's were took their mutes and rates with it (the audit of 2026-09-25, M11).
    std::vector<Rule> next;
    next.reserve(rules.size());
    for (const Rule::Config& config : rules) {
        next.emplace_back(config);
        if (fresh) {
            continue;
        }
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
    //
    // A fire held for its rule's delay is another matter: it is the rule firing, and goes only
    // while the rule may. Its follow-ups go with it, never having been queued.
    std::erase_if(pending_, [this, fresh](const Pending& waiting) {
        if (!waiting.press) {
            return false;
        }
        const Rule* const rule = fresh ? nullptr : find(waiting.ruleId);
        return rule == nullptr || !rule->enabled() || rule->muted();
    });
}

std::size_t TriggerEngine::heldFires() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(pending_.begin(), pending_.end(), [](const Pending& p) { return p.press; }));
}

void TriggerEngine::dropHeldFires() noexcept {
    std::erase_if(pending_, [](const Pending& waiting) { return waiting.press; });
}

void TriggerEngine::releaseRule(std::string_view id, double now) {
    std::vector<Pending> sending;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        Pending& waiting = pending_[i];
        if (waiting.ruleId == id && (waiting.press || !isDmx(waiting.message.kind))) {
            if (!waiting.press) {
                // Now — but about no earlier moment than its press's, which a target's delay may
                // still be holding back: a note off sent past its own note on is a note stuck on.
                waiting.message.moment =
                    std::min(waiting.message.moment, std::max(now, waiting.pressMoment));
                sending.push_back(std::move(waiting));
            }
            continue; // a held fire is dropped: it has not gone, and now will not
        }
        if (kept != i) {
            pending_[kept] = std::move(waiting);
        }
        ++kept;
    }
    pending_.resize(kept);
    for (const Pending& owed : sending) {
        deliver(owed.message, owed.ruleId, true, {});
    }
}

void TriggerEngine::remapPending(const std::vector<int>& outputs,
                                 const std::vector<int>& fixtures) noexcept {
    const auto remap = [&](Message& message) {
        if (!outputs.empty()) {
            message.outputs = remapBits(message.outputs, outputs);
        }
        if (!fixtures.empty()) {
            message.fixtures = remapBits(message.fixtures, fixtures);
        }
    };
    for (Pending& waiting : pending_) {
        remap(waiting.message);
        for (auto& [delay, follow] : waiting.owed) {
            remap(follow);
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

bool TriggerEngine::beatSatisfies(const Rule& rule, const Context& context) noexcept {
    const Rule::Config& config = rule.config();
    // `effectiveEvery` and not `config.every`: §5.7's `double` and `halve` are a live gesture
    // on top of what the operator wrote, like the tempo ÷2 button, and they act here so that
    // the counting stays in phase — a rule on every 4 bars taken to every 8 still lands on
    // bar 1, because both are counted from the first bar rather than from when the gesture
    // was made.
    const std::uint64_t every = std::max<std::uint64_t>(1, rule.effectiveEvery());
    // Which beat of the bar the rule names, from 0 — see `Rule::Config::onBeat`.
    const std::uint32_t beatOfBar = std::clamp<std::uint32_t>(config.onBeat, 1, kMaxBeatOfBar);
    switch (config.trigger) {
    case Trigger::Beat: {
        // **On the bar's grid**, from the beat the rule names: every 2 beats from beat 2 is 2 and
        // 4 of every bar, whichever beat the tracker happened to call first — which is what
        // "which two beats" asks for, and what the count from the first beat could never say.
        // The beat's place on that grid is its bar's start plus its place in the bar, counted
        // from beat 1 of the first bar; before the first downbeat that is below zero, so the
        // remainder is taken the floor way.
        if (context.meter > 0 && context.beatInBar >= 1 && context.beatInBar <= context.meter) {
            const auto meter = static_cast<std::int64_t>(context.meter);
            const std::int64_t place = (static_cast<std::int64_t>(context.bars) - 1) * meter +
                                       static_cast<std::int64_t>(context.beatInBar - 1) -
                                       static_cast<std::int64_t>(beatOfBar - 1);
            const auto step = static_cast<std::int64_t>(every);
            return ((place % step) + step) % step == 0;
        }
        // No bar to count in yet: from the first beat, so "every 4 beats" is beats 1, 5, 9 — an
        // operator counting a phrase in starts at one, not at whichever beat the modulo lands on.
        return context.beats >= 1 && (context.beats - 1) % every == 0;
    }
    case Trigger::Bar:
        // A bar begins on the beat the tracker calls the bar's first, and `bars` has
        // already counted it by then — see tracking::TempoTracker::advanceBar. The rule fires on
        // the beat of it that it names: a bar too short to have that beat is not fired in.
        return context.beatInBar == beatOfBar && context.bars >= 1 &&
               (context.bars - 1) % every == 0;
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
    // Reused across fires, so a rule that owes two messages allocates nothing after the
    // first. Cleared here rather than by the callee, which appends.
    owed_.clear();
    rule.followUpsFor(context, *message, owed_);
    delayed_.clear();
    for (auto& [index, follow] : owed_) {
        // Never in the past, however the delay was configured: a follow-up due before the
        // message it follows would be sent in the same round and read as a rule that sends
        // its release first. `Rule::followUpDelay` is what turns "two beats" into seconds,
        // and it does it here — against the tempo the press went out at, not the one playing
        // when the release comes due.
        delayed_.emplace_back(std::max(0.0, rule.followUpDelay(context, index)), std::move(follow));
    }
    // The rule's own wait, settled against the same tempo — but not for a test, which is pressed
    // to see where the message lands now.
    const double wait = force ? 0.0 : rule.fireDelay(context);
    if (wait > 0.0) {
        // Held, follow-ups and all: they are owed only once it has gone. About its moment plus
        // the wait, so every target hears it the wait after it would have.
        Pending held;
        held.due = context.now + wait;
        held.press = true;
        held.ruleId = rule.id();
        message->moment += wait;
        held.message = std::move(*message);
        held.slots.assign(rule.lastSlots().begin(), rule.lastSlots().end());
        held.owed = std::move(delayed_);
        delayed_ = {};
        pending_.push_back(std::move(held));
        return true;
    }
    deliver(*message, rule.id(), false, rule.lastSlots());
    queueFollowUps(delayed_, *message, context.now, rule.id());
    return true;
}

void TriggerEngine::queueFollowUps(std::vector<std::pair<double, Message>>& owed,
                                   const Message& message, double due,
                                   const std::string& ruleId) {
    for (auto& [delay, follow] : owed) {
        // Handed over a delay after the press was, and *about* a moment a delay after the
        // press's — so every target, however it is offset, hears the release exactly `delay`
        // after it heard the press. Measuring the release from the round that sends it instead
        // would put it ahead of its own press on a target offset later than the earliest one.
        follow.moment = message.moment + delay;
        Pending waiting;
        waiting.due = due + delay;
        waiting.message = std::move(follow);
        waiting.ruleId = ruleId;
        waiting.pressMoment = message.moment;
        pending_.push_back(std::move(waiting));
    }
}

void TriggerEngine::drainDue(double now) {
    if (pending_.empty()) {
        return;
    }
    // In the order queued, which for follow-ups of the same rule is the order they were
    // fired in. Two rules whose delays interleave come out interleaved, which is what the
    // clock says and what a receiver expects.
    std::vector<Pending> due;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        if (pending_[i].due <= now) {
            due.push_back(std::move(pending_[i]));
        } else {
            if (kept != i) {
                pending_[kept] = std::move(pending_[i]);
            }
            ++kept;
        }
    }
    pending_.resize(kept);
    for (Pending& waiting : due) {
        if (!waiting.press) {
            deliver(waiting.message, waiting.ruleId, true, {});
            continue;
        }
        // A held fire goes only while its rule may still fire: one muted while it waited is
        // a muted fire, and one switched off or deleted is no fire at all.
        const Rule* const rule = find(waiting.ruleId);
        if (rule == nullptr || !rule->enabled() || panicked_) {
            continue;
        }
        if (rule->muted()) {
            ++muted_;
            if (observer_) {
                observer_(waiting.ruleId, waiting.message, false, waiting.slots, true);
            }
            continue;
        }
        deliver(waiting.message, waiting.ruleId, false, waiting.slots);
        // Its follow-ups from when it was due, not from this round, which can be a
        // millisecond later.
        queueFollowUps(waiting.owed, waiting.message, waiting.due, waiting.ruleId);
    }
}

void TriggerEngine::flushPending() {
    // Every follow-up, now; a fire held for its rule's delay is a press, and goes nowhere.
    std::vector<Pending> owed;
    owed.swap(pending_);
    for (const Pending& waiting : owed) {
        if (!waiting.press) {
            deliver(waiting.message, waiting.ruleId, true, {});
        }
    }
}

void TriggerEngine::flushFollowUpsTo(std::uint64_t outputs) {
    if (outputs == 0) {
        return;
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        Pending& waiting = pending_[i];
        if (waiting.press && !isDmx(waiting.message.kind)) {
            // A held fire will not reach an output that has gone, and neither will what it owes.
            waiting.message.outputs &= ~outputs;
            for (auto& [delay, follow] : waiting.owed) {
                follow.outputs &= ~outputs;
            }
            if (waiting.message.outputs == 0) {
                continue;
            }
        } else if (!isDmx(waiting.message.kind) && (waiting.message.outputs & outputs) != 0) {
            // Split: the leaving outputs' half now, the rest when it is due. One release routed
            // to two synths is two releases, and only one of the synths is going.
            Message now = waiting.message;
            now.outputs &= outputs;
            deliver(now, waiting.ruleId, true, {});
            waiting.message.outputs &= ~outputs;
            if (waiting.message.outputs == 0) {
                continue; // all of it has gone
            }
        }
        if (kept != i) {
            pending_[kept] = std::move(waiting);
        }
        ++kept;
    }
    pending_.resize(kept);
}

void TriggerEngine::onBeat(const Context& context) {
    if (panicked_ || !listening_) {
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
        if (!changed || panicked_ || !listening_ || !rule.enabled() || !rule.valid()) {
            continue;
        }
        if (!rule.conditionsHold(context)) {
            continue;
        }
        dispatch(rule, context);
    }
}

void TriggerEngine::onOnset(const Context& context) {
    if (panicked_ || !listening_) {
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

void TriggerEngine::onBarDeclared(const Context& context) {
    if (panicked_ || !listening_) {
        return;
    }
    for (Rule& rule : rules_) {
        const Trigger trigger = rule.config().trigger;
        if ((trigger != Trigger::Bar && trigger != Trigger::Downbeat) || !rule.enabled() ||
            !rule.valid()) {
            continue;
        }
        if (!beatSatisfies(rule, context) || !rule.conditionsHold(context)) {
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
    if (!dispatch(*rule, context, true)) {
        return false;
    }
    // **And let go of a moment later, where nothing else will** — see the header. A rule that
    // cannot fire again on its own has nothing to take a laser's clip off or end a note, and a
    // beam left in the air by a button that says "test" is the hazard the operator met.
    if (rule->enabled() && !rule->muted() && listening_) {
        return true;
    }
    const Rule::Config& config = rule->config();
    const bool note = config.sendKind == Message::Kind::MidiNote;
    const bool clip = config.sendKind == Message::Kind::Dmx &&
                      dmx::takesClip(config.dmx.effect);
    if (!note && !clip) {
        return true;
    }
    // The press just sent is the newest message this rule made: rebuilt from what it drew
    // rather than kept, since `dispatch` does not hand it back.
    Message release;
    release.kind = note ? Message::Kind::MidiNoteOff : Message::Kind::Dmx;
    release.outputs = rule->outputMask();
    release.channel = std::clamp(config.channel, 1, 16);
    if (note) {
        const std::span<const Value> drawn = rule->lastSlots();
        release.number = drawn.empty() ? 0 : std::clamp(drawn.front().asInt(), 0, 127);
        release.value = 0;
    } else {
        release.fixtures = rule->fixtureMask();
        release.payload.kind = dmx::EffectKind::Clip;
        release.payload.clip = 0;
    }
    const double beat = context.bpm > 0.0 ? 60.0 / context.bpm : 0.0;
    const double hold = std::max(kTestHoldSeconds, beat);
    const double moment = context.moment.value_or(context.now);
    release.moment = moment + hold;
    Pending waiting;
    waiting.due = context.now + hold;
    waiting.message = std::move(release);
    waiting.ruleId = rule->id();
    waiting.pressMoment = moment;
    pending_.push_back(std::move(waiting));
    return true;
}

void TriggerEngine::panic(const Context& context) {
    (void)context; // the clock is not consulted: everything owed goes now, not when due
    panicked_ = true;
    flushPending();
}

} // namespace takt4::trigger
