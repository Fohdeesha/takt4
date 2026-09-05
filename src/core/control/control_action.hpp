#pragma once

#include "core/engine/beat_engine.hpp"
#include "core/tracking/tap_tempo.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takt4::control {

/// One thing a control surface can ask the tracker for — HANDOFF §5.7's table, minus the
/// two entries Phase 6 and Q7 still owe (`panic` needs rules to halt; `preset` needs
/// presets to be files).
///
/// An *action* rather than an address string, because §5.7 says "every one of these is
/// also bindable to a MIDI note or CC through a learn mode". A MIDI binding names what it
/// does; it has no address. Having the MIDI side synthesise `"/takt4/ctl/tap"` so it
/// could reach the OSC handler would be manufacturing a string for one surface out of a
/// naming convention that belongs to another, and the prefix is configurable, so the
/// string it had to manufacture would not even be constant.
enum class ControlAction : std::uint8_t {
    Tap,
    Downbeat,
    TempoHalve,
    TempoDouble,
    Lock,
};

/// Every action, in the order a UI should offer them for binding.
inline constexpr std::array<ControlAction, 5> kControlActions{
    ControlAction::Tap, ControlAction::Downbeat, ControlAction::TempoHalve,
    ControlAction::TempoDouble, ControlAction::Lock};

/// The verb §5.7 spells the action with, after `<prefix>/ctl/`. A saved binding is written
/// with the same word, so a settings file reads the way the OSC address does and an
/// operator who knows one knows the other.
std::string_view verbOf(ControlAction action) noexcept;

/// The action a verb names, or nothing when it names none of them.
std::optional<ControlAction> actionOf(std::string_view verb) noexcept;

/// True where §5.7 gives the action an argument — `lock <0|1>`, and nothing else today.
/// Those refuse to act without one rather than treating the message as a toggle: a toggle
/// depends on a state the sender cannot see, so a control surface that missed a single
/// message would be inverted from then on.
bool takesArgument(ControlAction action) noexcept;

/// A short label for a UI list or a console listing.
std::string_view labelOf(ControlAction action) noexcept;

/// One control surface's route into the tracker: the actions it can ask for, and nothing
/// else. Everything reaches `BeatEngine::post`, so it is applied between frames on the
/// inference thread exactly as the window's buttons and the console's keys are.
///
/// **One of these per surface, deliberately.** Each owns its own tap set, because a Stream
/// Deck's tap button, a MIDI pad and the window's TAP are three surfaces: interleaving
/// their taps into one set would give an operator using two of them a tempo neither of
/// them meant. Nothing else here holds state, so a single shared instance would work in
/// every other respect — that one reason is enough.
class ControlSurface {
public:
    /// The engine must outlive this.
    explicit ControlSurface(engine::BeatEngine& engine) noexcept;

    /// Acts on one action, reading this object's own steady clock for anything that needs
    /// a time. False when the action needed an argument and was not given one; true
    /// otherwise — including a tap that only counts towards a tempo rather than making
    /// one, because the surface understood it either way.
    bool apply(ControlAction action, std::optional<double> argument);

    /// The same with the time supplied, which is how `tracking::TapTempo` is built to be
    /// driven and what lets a test tap out a tempo without spending it in real time.
    bool apply(ControlAction action, std::optional<double> argument, double nowSeconds);

    /// Seconds since this surface was created, on its own steady clock.
    double nowSeconds() const noexcept;

    const tracking::TapTempo& taps() const noexcept { return taps_; }
    void resetTaps() noexcept { taps_.reset(); }

private:
    engine::BeatEngine& engine_;
    tracking::TapTempo taps_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

} // namespace takt4::control
