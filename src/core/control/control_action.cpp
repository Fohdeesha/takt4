#include "core/control/control_action.hpp"

#include "core/engine/control.hpp"

namespace takt4::control {
namespace {

/// What `rule/<id>/enable` reads as either side of the id. Kept here rather than spelled
/// twice, so `verbFor` and `targetOf` cannot drift apart about the shape they agree on.
constexpr std::string_view kRulePrefix = "rule/";
constexpr std::string_view kRuleSuffix = "/enable";

/// The verb for everything except `RuleEnable`, whose verb is not a constant.
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
        break;
    }
    return {};
}

} // namespace

std::string verbFor(const ControlTarget& target) {
    if (target.action == ControlAction::RuleEnable) {
        return std::string(kRulePrefix) + target.rule + std::string(kRuleSuffix);
    }
    return std::string(plainVerbOf(target.action));
}

std::optional<ControlTarget> targetOf(std::string_view verb) {
    for (const ControlAction action : kControlActions) {
        if (!takesRuleId(action) && verb == plainVerbOf(action)) {
            return ControlTarget(action);
        }
    }

    // Which leaves `rule/<id>/enable`. The id is one address segment by
    // `trigger::Rule::validate` — no '/' of its own — so what is between the two fixed parts
    // is the whole of it, and an empty id is not a rule any more than a missing one is.
    //
    // The length test is not redundant with the two `starts_with`/`ends_with`: both of them
    // hold on `"rule/enable"`, where the fixed parts *overlap* and there is no id at all.
    if (!verb.starts_with(kRulePrefix) || !verb.ends_with(kRuleSuffix) ||
        verb.size() <= kRulePrefix.size() + kRuleSuffix.size()) {
        return std::nullopt;
    }
    const std::size_t idSize = verb.size() - kRulePrefix.size() - kRuleSuffix.size();
    const std::string_view id = verb.substr(kRulePrefix.size(), idSize);
    // One segment, and the check is not academic: `rule/x/enable/enable` satisfies both
    // ends and would otherwise name a rule called `x/enable`. No rule can be called that —
    // `Rule::validate` refuses a '/' — so a message like it is one nothing here understands,
    // and saying so is what keeps it out of the "handled" count.
    if (id.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    return ControlTarget(ControlAction::RuleEnable, std::string(id));
}

bool takesArgument(ControlAction action) noexcept {
    return action == ControlAction::Lock || action == ControlAction::RuleEnable;
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
    }
    return false;
}

} // namespace takt4::control
