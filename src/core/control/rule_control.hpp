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

    /// §5.7's `/ctl/panic`, and letting go of it. §5.8 calls panic *"a global halt that
    /// stops every rule instantly ... non-negotiable for live use"*, and it is a latch:
    /// `false` is what releases it.
    virtual void panic(bool engaged) = 0;

    /// §5.7's `/ctl/rule/<id>/enable <0|1>`.
    ///
    /// A rule that is not there is **not an error here.** The implementation is the only
    /// thing that knows what rules exist, and a Stream Deck holding a button for a rule the
    /// current preset no longer has is an ordinary state of the world — not something a
    /// surface can report on, since by the time it could the operator has already pressed it.
    virtual void setRuleEnabled(std::string_view id, bool enabled) = 0;

protected:
    RuleControl() = default;
    RuleControl(RuleControl&&) = default;
    RuleControl& operator=(RuleControl&&) = default;
};

} // namespace takt4::control
