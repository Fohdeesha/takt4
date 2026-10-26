#pragma once

#include "core/control/rule_control.hpp"
#include "core/engine/beat_source.hpp"
#include "core/tracking/tap_tempo.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace takt4::control {

/// One thing a control surface can ask for — every control action but the one still owed
/// (`preset` needs presets to be files).
///
/// An *action* rather than an address string, because every one of these is also bindable
/// to a MIDI note or CC through a learn mode. A MIDI binding names what it does; it has no
/// address. Having the MIDI side synthesise `"/takt4/ctl/tap"` so it could reach the OSC
/// handler would be manufacturing a string for one surface out of a naming convention that
/// belongs to another, and the prefix is configurable, so the string it had to manufacture
/// would not even be constant.
///
/// **Five of these ask the tracker and the rest ask the rules.** That is the whole of why a
/// surface has two destinations: `engine::BeatSource::post` for the first five, and
/// `RuleControl` for the rest — PANIC and its release, the manual rules and the six
/// `rule/<id>/…` verbs — whose subject lives on the output thread.
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
    /// `/ctl/panic` — a global halt that stops every rule instantly. It only ever engages,
    /// whatever it is sent: letting go is `PanicRelease`, or the window's RELEASE.
    Panic,
    /// `/ctl/rule/<id>/enable <0|1>`. The first action that **names** something as well as
    /// doing it; see `ControlTarget`.
    RuleEnable,
    /// `/ctl/rule/<id>/mute <0|1>` — the rule keeps running and stops sending. Not a second
    /// spelling of `RuleEnable`: see `RuleControl::setRuleMuted` for why both exist.
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
    /// `/ctl/manual` — fires every rule whose trigger is "manual hotkey". That trigger was
    /// offered by the editor and the README with nothing anywhere that could fire it, so a rule
    /// set to it was a rule that never did anything. A button, bare.
    Manual,
    /// `/ctl/panic/release` — lets go of a PANIC. Its own address, because a push button sends
    /// 1 on the press and 0 on the release, so a `panic 0` that released lasted only while the
    /// finger was down. A button, bare, like the window's RELEASE.
    PanicRelease,
};

/// Every action, in the order a UI should offer them for binding.
inline constexpr std::array<ControlAction, 14> kControlActions{
    ControlAction::Tap,         ControlAction::Downbeat, ControlAction::TempoHalve,
    ControlAction::TempoDouble, ControlAction::Lock,     ControlAction::Panic,
    ControlAction::RuleEnable,  ControlAction::RuleMute, ControlAction::RuleDouble,
    ControlAction::RuleHalve,   ControlAction::RuleRate, ControlAction::RuleReset,
    ControlAction::Manual,      ControlAction::PanicRelease};

/// True where the action needs a rule named as well — the six `rule/<id>/…` verbs.
///
/// Worth a predicate rather than a comparison at each site, because what it really marks is
/// "a learn mode cannot bind this from a gesture alone": pressing a pad says *which button*,
/// never *which rule*. The rule editor is where that second half gets asked for.
inline constexpr bool takesRuleId(ControlAction action) noexcept {
    return action == ControlAction::RuleEnable || action == ControlAction::RuleMute ||
           action == ControlAction::RuleDouble || action == ControlAction::RuleHalve ||
           action == ControlAction::RuleRate || action == ControlAction::RuleReset;
}

/// One action, and what it acts on where the action needs saying.
///
/// Most addresses are a verb and nothing else. The six `rule/<id>/…` verbs name a rule in the
/// middle of the address, and a MIDI binding to one has to carry that id too — so the two
/// travel together rather than the id being threaded past every call that has no use for it.
struct ControlTarget {
    ControlTarget() = default;
    /// Implicit, deliberately: every action but the six `rule/<id>/…` verbs *is* just an action,
    /// and a call site that names one should not have to say so twice.
    ControlTarget(ControlAction which) noexcept // NOLINT(google-explicit-constructor)
        : action(which) {}
    ControlTarget(ControlAction which, std::string id) : action(which), rule(std::move(id)) {}

    ControlAction action = ControlAction::Tap;
    /// Which rule, for `RuleEnable`. Empty and meaningless for the rest. A rule id is
    /// validated to be one legal OSC address segment (`trigger::Rule::validate`), so it can
    /// never contain the `/` that separates it from `enable`.
    std::string rule;

    friend bool operator==(const ControlTarget&, const ControlTarget&) = default;
};

/// The verb an address spells the target with, after `<prefix>/ctl/` — `"tap"`, `"tempo/halve"`,
/// `"rule/intro/enable"`. A saved binding is written with the same word, so a settings file
/// reads the way the OSC address does and an operator who knows one knows the other.
std::string verbFor(const ControlTarget& target);

/// The target a verb names, or nothing when it names none of them. Round-trips `verbFor`.
std::optional<ControlTarget> targetOf(std::string_view verb);

/// True where the action takes an argument it cannot do without — `lock <0|1>` and
/// `rule/<id>/enable <0|1>`. Those refuse to act without one rather than treating the
/// message as a toggle: a toggle depends on a state the sender cannot see, so a control
/// surface that missed a single message would be inverted from then on.
///
/// **`Panic` is deliberately not one of them.** The other two are written with `<0|1>` and
/// panic is written bare, and a panic button panics: it engages whatever it is sent, a 0
/// included. It used to read a 0 as "let go", so a push button — 1 on the press, 0 on the
/// release — held PANIC only while the finger was down. Panic is a latch, and an operator who
/// hit it from a Stream Deck lets go of it from the same Stream Deck with `panic/release`
/// (`PanicRelease`), a button of its own.
bool takesArgument(ControlAction action) noexcept;

/// A short label for a UI list or a console listing.
std::string_view labelOf(ControlAction action) noexcept;

/// One control surface's route in: the actions it can ask for, and nothing else.
///
/// The five tracker actions reach the beat source's `post`, so they are applied by its own
/// thread exactly as the window's buttons and the console's keys are. Its
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
    /// The beat source must outlive this. `rules` may be null — an app with no output runner
    /// still taps and snaps, and the two rule addresses are then refused rather than
    /// silently accepted, which is what tells an operator their panic button is not wired.
    /// It must outlive this too where it is not.
    explicit ControlSurface(engine::BeatSource& beats, RuleControl* rules = nullptr) noexcept;

    /// Where `panic` and `rule/<id>/enable` go, or null for nowhere.
    ///
    /// A setter as well as a constructor argument because an owner often builds the surface
    /// before the output runner exists — `ui::WindowController` did until the two swapped
    /// places, and a headless mode will have the same ordering problem.
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

    /// `/ctl/bar <n>`: the bar playing is bar `n` (`RuleControl::setBar`). A whole number, 1 or
    /// more, as an int or a float with nothing after the point; anything else is refused rather
    /// than rounded, and so is the address where nothing is wired to the rules. Not a
    /// `ControlAction`: each of those is offered to a MIDI pad, whose value would be a bar number
    /// nobody meant.
    bool setBar(std::optional<double> argument);

    /// Seconds since this surface was created, on its own steady clock.
    double nowSeconds() const noexcept;

    const tracking::TapTempo& taps() const noexcept { return taps_; }
    void resetTaps() noexcept { taps_.reset(); }

private:
    engine::BeatSource& beats_;
    RuleControl* rules_ = nullptr;
    tracking::TapTempo taps_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

} // namespace takt4::control
