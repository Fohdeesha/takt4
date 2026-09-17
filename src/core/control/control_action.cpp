#include "core/control/control_action.hpp"

#include "core/engine/control.hpp"

#include <array>

namespace takt4::control {
namespace {

/// What a `rule/<id>/…` verb reads as before the id. Kept here rather than spelled twice, so
/// `verbFor` and `targetOf` cannot drift apart about the shape they agree on.
constexpr std::string_view kRulePrefix = "rule/";

/// The tail of each `rule/<id>/…` verb. One table rather than a switch in each direction, for
/// the same reason: the two functions read the same rows.
struct RuleVerb {
    ControlAction action;
    std::string_view tail;
};

constexpr std::array<RuleVerb, 6> kRuleVerbs{{
    {ControlAction::RuleEnable, "enable"},
    {ControlAction::RuleMute, "mute"},
    {ControlAction::RuleDouble, "double"},
    {ControlAction::RuleHalve, "halve"},
    {ControlAction::RuleRate, "rate"},
    {ControlAction::RuleReset, "reset"},
}};

std::string_view ruleTailOf(ControlAction action) noexcept {
    for (const RuleVerb& verb : kRuleVerbs) {
        if (verb.action == action) {
            return verb.tail;
        }
    }
    return {};
}

/// The verb for everything that does not name a rule.
std::string_view plainVerbOf(ControlAction action) noexcept {
    switch (action) {
    case ControlAction::Tap:
        return "tap";
    case ControlAction::Downbeat:
        return "downbeat";
    case ControlAction::TempoHalve:
        return "tempo/halve";
    case ControlAction::TempoDouble:
        return "tempo/double";
    case ControlAction::Lock:
        return "lock";
    case ControlAction::Panic:
        return "panic";
    case ControlAction::RuleEnable:
    case ControlAction::RuleMute:
    case ControlAction::RuleDouble:
    case ControlAction::RuleHalve:
    case ControlAction::RuleRate:
    case ControlAction::RuleReset:
        break;
    }
    return {};
}

} // namespace

std::string verbFor(const ControlTarget& target) {
    if (takesRuleId(target.action)) {
        return std::string(kRulePrefix) + target.rule + "/" +
               std::string(ruleTailOf(target.action));
    }
    return std::string(plainVerbOf(target.action));
}

std::optional<ControlTarget> targetOf(std::string_view verb) {
    for (const ControlAction action : kControlActions) {
        if (!takesRuleId(action) && verb == plainVerbOf(action)) {
            return ControlTarget(action);
        }
    }

    // Which leaves `rule/<id>/<tail>`. The id is one address segment by
    // `trigger::Rule::validate` — no '/' of its own — so the shape is exactly three parts and
    // splitting on the *last* '/' is what separates the tail from the id.
    if (!verb.starts_with(kRulePrefix)) {
        return std::nullopt;
    }
    const std::string_view rest = verb.substr(kRulePrefix.size());
    const std::size_t split = rest.rfind('/');
    // An empty id is not a rule any more than a missing one is, and `split == 0` is exactly
    // that: `rule//enable`, or `rule/enable` read as having no tail.
    if (split == std::string_view::npos || split == 0 || split + 1 >= rest.size()) {
        return std::nullopt;
    }
    const std::string_view id = rest.substr(0, split);
    const std::string_view tail = rest.substr(split + 1);
    // One segment, and the check is not academic: `rule/x/enable/enable` would otherwise name
    // a rule called `x/enable`. No rule can be called that — `Rule::validate` refuses a '/' —
    // so a message like it is one nothing here understands, and saying so is what keeps it out
    // of the "handled" count.
    if (id.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    for (const RuleVerb& known : kRuleVerbs) {
        if (tail == known.tail) {
            return ControlTarget(known.action, std::string(id));
        }
    }
    return std::nullopt;
}

bool takesArgument(ControlAction action) noexcept {
    // The four that are a *state* somebody can hold: the lock, a rule's arming, its mute, and
    // its rate. The rest are buttons, and §5.7's reasoning applies to all of them — a toggle
    // depends on a state the sender cannot see, so a control surface that missed one message
    // would be inverted from then on.
    return action == ControlAction::Lock || action == ControlAction::RuleEnable ||
           action == ControlAction::RuleMute || action == ControlAction::RuleRate;
}

std::string_view labelOf(ControlAction action) noexcept {
    switch (action) {
    case ControlAction::Tap:
        return "tap tempo";
    case ControlAction::Downbeat:
        return "downbeat now";
    case ControlAction::TempoHalve:
        return "halve the tempo";
    case ControlAction::TempoDouble:
        return "double the tempo";
    case ControlAction::Lock:
        return "pin or release the lock";
    case ControlAction::Panic:
        return "panic — halt every rule";
    case ControlAction::RuleEnable:
        return "enable or disable a rule";
    case ControlAction::RuleMute:
        return "mute or unmute a rule";
    case ControlAction::RuleDouble:
        return "rule fires half as often";
    case ControlAction::RuleHalve:
        return "rule fires twice as often";
    case ControlAction::RuleRate:
        return "set how often a rule fires";
    case ControlAction::RuleReset:
        return "rule back to its written rate";
    }
    return {};
}

ControlSurface::ControlSurface(engine::BeatEngine& engine, RuleControl* rules) noexcept
    : engine_(engine), rules_(rules) {}

double ControlSurface::nowSeconds() const noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

bool ControlSurface::apply(const ControlTarget& target, std::optional<double> argument) {
    return apply(target, argument, nowSeconds());
}

bool ControlSurface::apply(const ControlTarget& target, std::optional<double> argument,
                           double nowSeconds) {
    using engine::Command;

    if (takesArgument(target.action) && !argument) {
        return false;
    }

    switch (target.action) {
    case ControlAction::Tap: {
        const std::optional<double> tapped = taps_.tap(nowSeconds);
        if (tapped) {
            // A seed, not an override — §7's locked decision, and the same thing the
            // window's TAP and the console's space bar do.
            (void)engine_.post(Command::seedTempo(*tapped));
        }
        // The taps before the third say nothing, which is not the same as failing.
        return true;
    }
    case ControlAction::Downbeat:
        (void)engine_.post(Command::snapDownbeat());
        return true;
    case ControlAction::TempoHalve:
        (void)engine_.post(Command::halve());
        return true;
    case ControlAction::TempoDouble:
        (void)engine_.post(Command::redouble());
        return true;
    case ControlAction::Lock:
        // Anything a surface spells "true" with: OSC booleans arrive as 1 and 0, a fader
        // sends 1.0, and a MIDI note-on sends its velocity.
        (void)engine_.post(Command::setLockPinned(*argument != 0.0));
        return true;

    // The two that are not the tracker's. Refused outright where nothing is wired to the
    // rules: an app with no output runner has no rules to halt, and reporting that is what
    // puts "panic is not connected" in front of an operator instead of leaving them to find
    // out during a set.
    case ControlAction::Panic:
        if (rules_ == nullptr) {
            return false;
        }
        // Bare means engage — see `takesArgument`. Anything sent is read the way `lock`
        // reads its argument, so a CC bound to panic is a switch and a pad is a button.
        rules_->panic(!argument || *argument != 0.0);
        return true;
    case ControlAction::RuleEnable:
        if (rules_ == nullptr || target.rule.empty()) {
            return false;
        }
        rules_->setRuleEnabled(target.rule, *argument != 0.0);
        return true;
    case ControlAction::RuleMute:
        if (rules_ == nullptr || target.rule.empty()) {
            return false;
        }
        rules_->setRuleMuted(target.rule, *argument != 0.0);
        return true;
    case ControlAction::RuleDouble:
    case ControlAction::RuleHalve:
        if (rules_ == nullptr || target.rule.empty()) {
            return false;
        }
        // Relative, so a button can be pressed twice and mean four times the interval. Bare —
        // these are buttons, not switches — and an argument of zero is read as a press rather
        // than as a release, because a pad that sends note-off would otherwise undo itself.
        rules_->setRuleRate(target.rule, target.action == ControlAction::RuleDouble ? 2.0 : 0.5,
                            true);
        return true;
    case ControlAction::RuleRate:
        if (rules_ == nullptr || target.rule.empty()) {
            return false;
        }
        // Absolute: a fader holds a position, and sending it twice must not compound.
        rules_->setRuleRate(target.rule, *argument, false);
        return true;
    case ControlAction::RuleReset:
        if (rules_ == nullptr || target.rule.empty()) {
            return false;
        }
        rules_->setRuleRate(target.rule, 1.0, false);
        return true;
    }
    return false;
}

} // namespace takt4::control
