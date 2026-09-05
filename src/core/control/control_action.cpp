#include "core/control/control_action.hpp"

#include "core/engine/control.hpp"

namespace takt4::control {

std::string_view verbOf(ControlAction action) noexcept {
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
    }
    return {};
}

std::optional<ControlAction> actionOf(std::string_view verb) noexcept {
    for (const ControlAction action : kControlActions) {
        if (verb == verbOf(action)) {
            return action;
        }
    }
    return std::nullopt;
}

bool takesArgument(ControlAction action) noexcept {
    return action == ControlAction::Lock;
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
    }
    return {};
}

ControlSurface::ControlSurface(engine::BeatEngine& engine) noexcept : engine_(engine) {}

double ControlSurface::nowSeconds() const noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

bool ControlSurface::apply(ControlAction action, std::optional<double> argument) {
    return apply(action, argument, nowSeconds());
}

bool ControlSurface::apply(ControlAction action, std::optional<double> argument,
                           double nowSeconds) {
    using engine::Command;

    if (takesArgument(action) && !argument) {
        return false;
    }

    switch (action) {
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
    }
    return false;
}

} // namespace takt4::control
