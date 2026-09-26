#pragma once

#include <string_view>

namespace takt4::control {

/// What a control surface can ask of HANDOFF §5.8's rules.
///
/// §5.7's table has seven addresses. Five of them are questions for the tracker and reach it
/// through `engine::BeatEngine::post`; two are questions for the *rules*, which live on
/// §4.2's output thread behind `output::OutputRunner`'s own queue. So a surface needs two
/// destinations rather than one, and this is the second.
///
/// **An interface rather than a reference to the runner**, for the reasons `trigger::Sink`
/// is one. `core/control` has no business pulling in a Link session, three sockets and a
/// MIDI port to set two booleans — and a test has to be able to say exactly what a surface
/// asked for, which a recording implementation gives it and a live output thread does not.
///
/// Called from whichever thread the surface runs on: RtMidi's callback, the OSC receiver's,
/// or the UI's. An implementation must be safe there. `OutputRunner::post` is, which is the
/// whole reason it is a queue.
class RuleControl {
public:
    virtual ~RuleControl() = default;

    RuleControl(const RuleControl&) = delete;
    RuleControl& operator=(const RuleControl&) = delete;

    /// §5.7's `/ctl/panic` (`true`), and letting go of it — `/ctl/panic/release`, or the
    /// window's RELEASE (`false`). §5.8 calls panic *"a global halt that stops every rule
    /// instantly ... non-negotiable for live use"*, and it is a latch.
    virtual void panic(bool engaged) = 0;

    /// §5.7's `/ctl/rule/<id>/enable <0|1>`.
    ///
    /// A rule that is not there is **not an error here.** The implementation is the only
    /// thing that knows what rules exist, and a Stream Deck holding a button for a rule the
    /// current preset no longer has is an ordinary state of the world — not something a
    /// surface can report on, since by the time it could the operator has already pressed it.
    ///
    /// `id` may be `output::kAllRules` — "all" — meaning every rule at once.
    virtual void setRuleEnabled(std::string_view id, bool enabled) = 0;

    /// §5.7's `/ctl/rule/<id>/mute <0|1>`, added on the operator's ask of 2026-09-16 for live
    /// per-rule control from a control surface.
    ///
    /// **Not the same as disabling, and both are wanted.** A disabled rule stops running: its
    /// shuffle bag stands still and it restarts when it comes back. A muted rule keeps running
    /// and stops *sending*, so unmuting rejoins the music in phase. Dropping a layer out for
    /// eight bars is the second one; taking a rule out of the show is the first. See
    /// `trigger::Rule::muted`.
    virtual void setRuleMuted(std::string_view id, bool muted) = 0;

    /// §5.7's `/ctl/rule/<id>/double`, `halve`, `rate <f>` and `reset` — how often the rule
    /// fires, as a multiplier on the interval it was written with.
    ///
    /// `relative` is what separates the four spellings from each other. `double` is `2.0`
    /// relative, so pressing it twice gives four; `rate 2` is `2.0` absolute, so a Stream Deck
    /// fader can hold a position; `reset` is `1.0` absolute. A factor at or below zero is
    /// clamped rather than refused — `trigger::Rule::setRate` says to what, and why a rule
    /// that fires every zero bars is not a thing that may be allowed to exist.
    virtual void setRuleRate(std::string_view id, double factor, bool relative) = 0;

    /// `/ctl/manual` — fires every rule whose trigger is "manual hotkey", conditions and all.
    virtual void fireManual() = 0;

protected:
    RuleControl() = default;
    RuleControl(RuleControl&&) = default;
    RuleControl& operator=(RuleControl&&) = default;
};

} // namespace takt4::control
