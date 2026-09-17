#pragma once

#include "core/control/rule_control.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/tracking/tap_tempo.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace takt4::control {

/// One thing a control surface can ask for — HANDOFF §5.7's table, minus the one entry Q7
/// still owes (`preset` needs presets to be files).
///
/// An *action* rather than an address string, because §5.7 says "every one of these is
/// also bindable to a MIDI note or CC through a learn mode". A MIDI binding names what it
/// does; it has no address. Having the MIDI side synthesise `"/takt4/ctl/tap"` so it
/// could reach the OSC handler would be manufacturing a string for one surface out of a
/// naming convention that belongs to another, and the prefix is configurable, so the
/// string it had to manufacture would not even be constant.
///
/// **Five of these ask the tracker and two ask the rules.** That is the whole of why a
/// surface has two destinations: `engine::BeatEngine::post` for the first five, and
/// `RuleControl` for `Panic` and `RuleEnable`, whose subject lives on §4.2's output thread.
///
/// New actions go on the **end**: a binding is stored as a verb, so the order is not part of
/// the file format — but a UI indexes this list, and inserting would silently move what a
/// picker's third row means.
enum class ControlAction : std::uint8_t {
    Tap,
    Downbeat,
    TempoHalve,
    TempoDouble,
    Lock,
    /// §5.7's `/ctl/panic` — §5.8's *"global halt that stops every rule instantly"*.
    Panic,
    /// §5.7's `/ctl/rule/<id>/enable <0|1>`. The first action that **names** something as
    /// well as doing it; see `ControlTarget`.
    RuleEnable,
    /// `/ctl/rule/<id>/mute <0|1>` — the rule keeps running and stops sending. Added on the
    /// operator's ask of 2026-09-16. Not a second spelling of `RuleEnable`: see
    /// `RuleControl::setRuleMuted` for why both exist.
    RuleMute,
    /// `/ctl/rule/<id>/double` — half as often, by doubling the interval. Bare, and
    /// *relative*, so pressing it twice is four times the interval.
    RuleDouble,
    /// `/ctl/rule/<id>/halve` — twice as often. Bare and relative, like `RuleDouble`.
    RuleHalve,
    /// `/ctl/rule/<id>/rate <f>` — the interval multiplier outright, for a fader that holds a
    /// position rather than a button that nudges.
    RuleRate,
    /// `/ctl/rule/<id>/reset` — back to the interval the rule was written with.
    RuleReset,
};

/// Every action, in the order a UI should offer them for binding.
inline constexpr std::array<ControlAction, 12> kControlActions{
    ControlAction::Tap,         ControlAction::Downbeat, ControlAction::TempoHalve,
    ControlAction::TempoDouble, ControlAction::Lock,     ControlAction::Panic,
    ControlAction::RuleEnable,  ControlAction::RuleMute, ControlAction::RuleDouble,
    ControlAction::RuleHalve,   ControlAction::RuleRate, ControlAction::RuleReset};

/// True where the action needs a rule named as well — the six `rule/<id>/…` verbs.
///
/// Worth a predicate rather than a comparison at each site, because what it really marks is
/// "a learn mode cannot bind this from a gesture alone": pressing a pad says *which button*,
/// never *which rule*. §5.9's editor is where that second half gets asked for.
inline constexpr bool takesRuleId(ControlAction action) noexcept {
    return action == ControlAction::RuleEnable || action == ControlAction::RuleMute ||
           action == ControlAction::RuleDouble || action == ControlAction::RuleHalve ||
           action == ControlAction::RuleRate || action == ControlAction::RuleReset;
}

/// One action, and what it acts on where the action needs saying.
///
/// Six of §5.7's seven addresses are a verb and nothing else. `rule/<id>/enable` names a
/// rule in the middle of the address, and a MIDI binding to it has to carry that id too —
/// so the two travel together rather than the id being threaded past every call that has no
/// use for it.
struct ControlTarget {
    ControlTarget() = default;
    /// Implicit, deliberately: six of the seven *are* just an action, and a call site that
    /// names one should not have to say so twice.
    ControlTarget(ControlAction action) noexcept // NOLINT(google-explicit-constructor)
        : action(action) {}
    ControlTarget(ControlAction action, std::string rule) : action(action), rule(std::move(rule)) {}

    ControlAction action = ControlAction::Tap;
    /// Which rule, for `RuleEnable`. Empty and meaningless for the rest. A rule id is
    /// validated to be one legal OSC address segment (`trigger::Rule::validate`), so it can
    /// never contain the `/` that separates it from `enable`.
    std::string rule;

    friend bool operator==(const ControlTarget&, const ControlTarget&) = default;
};

/// The verb §5.7 spells the target with, after `<prefix>/ctl/` — `"tap"`, `"tempo/halve"`,
/// `"rule/intro/enable"`. A saved binding is written with the same word, so a settings file
/// reads the way the OSC address does and an operator who knows one knows the other.
std::string verbFor(const ControlTarget& target);

/// The target a verb names, or nothing when it names none of them. Round-trips `verbFor`.
std::optional<ControlTarget> targetOf(std::string_view verb);

/// True where §5.7 gives the action an argument it cannot do without — `lock <0|1>` and
/// `rule/<id>/enable <0|1>`. Those refuse to act without one rather than treating the
/// message as a toggle: a toggle depends on a state the sender cannot see, so a control
/// surface that missed a single message would be inverted from then on.
///
/// **`Panic` is deliberately not one of them**, though it accepts an argument. §5.7 writes
/// the other two with `<0|1>` and writes panic bare, and the two spellings mean what they
/// say: a panic button panics, so a bare message engages. An argument is still read where
/// one is sent, because §5.8 makes panic a latch and an operator who hit it from a Stream
/// Deck has to be able to let go of it from the same Stream Deck — which is the entire
/// point of §5.7 existing.
bool takesArgument(ControlAction action) noexcept;

/// A short label for a UI list or a console listing.
std::string_view labelOf(ControlAction action) noexcept;

/// One control surface's route in: the actions it can ask for, and nothing else.
///
/// §5.7's five tracker actions reach `BeatEngine::post`, so they are applied between frames
/// on the inference thread exactly as the window's buttons and the console's keys are. Its
/// two rule actions reach `RuleControl`, which is the output thread's queue for the same
/// reason. Neither destination is touched directly; that is what makes a surface safe to run
/// on RtMidi's callback thread or a socket's.
///
/// **One of these per surface, deliberately.** Each owns its own tap set, because a Stream
/// Deck's tap button, a MIDI pad and the window's TAP are three surfaces: interleaving
/// their taps into one set would give an operator using two of them a tempo neither of
/// them meant. Nothing else here holds state, so a single shared instance would work in
/// every other respect — that one reason is enough.
class ControlSurface {
public:
    /// The engine must outlive this. `rules` may be null — an app with no output runner
    /// still taps and snaps, and §5.7's two rule addresses are then refused rather than
    /// silently accepted, which is what tells an operator their panic button is not wired.
    /// It must outlive this too where it is not.
    explicit ControlSurface(engine::BeatEngine& engine, RuleControl* rules = nullptr) noexcept;

    /// Where §5.7's `panic` and `rule/<id>/enable` go, or null for nowhere.
    ///
    /// A setter as well as a constructor argument because an owner often builds the surface
    /// before the output runner exists — `ui::WindowController` did until the two swapped
    /// places, and Q8's headless mode will have the same ordering problem.
    void setRuleControl(RuleControl* rules) noexcept { rules_ = rules; }
    RuleControl* ruleControl() const noexcept { return rules_; }

    /// Acts on one target, reading this object's own steady clock for anything that needs
    /// a time. False when the action needed an argument and was not given one, or needed a
    /// rule control and has none; true otherwise — including a tap that only counts towards
    /// a tempo rather than making one, because the surface understood it either way.
    bool apply(const ControlTarget& target, std::optional<double> argument);

    /// The same with the time supplied, which is how `tracking::TapTempo` is built to be
    /// driven and what lets a test tap out a tempo without spending it in real time.
    bool apply(const ControlTarget& target, std::optional<double> argument, double nowSeconds);

    /// Seconds since this surface was created, on its own steady clock.
    double nowSeconds() const noexcept;

    const tracking::TapTempo& taps() const noexcept { return taps_; }
    void resetTaps() noexcept { taps_.reset(); }

private:
    engine::BeatEngine& engine_;
    RuleControl* rules_ = nullptr;
    tracking::TapTempo taps_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

} // namespace takt4::control
