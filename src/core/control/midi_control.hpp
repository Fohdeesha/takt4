#pragma once

#include "core/control/control_action.hpp"
#include "core/control/midi_binding.hpp"
#include "core/engine/beat_engine.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::control {

/// HANDOFF §5.7's other half: "Every one of these is also bindable to a MIDI note or CC
/// through a learn mode."
///
/// A MIDI input port, a table of bindings, and `ControlSurface` at the end of it — the
/// same route into the tracker `OscControl` uses, so the two surfaces cannot drift apart
/// about what `downbeat` means or about `lock` needing its argument.
///
/// **Learn mode is the point of it.** Nobody knows what their controller's pad 3 sends,
/// and a text field asking for a note number is a worse question than "press it now".
/// `learn(action)` arms; the next control that moves is bound to that action and the mode
/// disarms itself. A control that is already bound to something else is *rebound* rather
/// than duplicated — an operator pressing the same pad twice while assigning two actions
/// has changed their mind, not asked for both.
///
/// Messages arrive on RtMidi's own callback thread, which is not the audio thread and not
/// the inference thread. Everything it touches is either atomic or under `mutex_`, and the
/// only thing it does to the tracker is `BeatEngine::post`, which is built for exactly
/// this: many producers, one consumer, applied between frames.
class MidiControl {
public:
    struct Config {
        /// Off unless an operator asks for it, and a port has to be named — opening
        /// whatever MIDI input happens to be first would bind a DAW's clock stream to
        /// somebody's tap button.
        bool enabled = false;
        /// A substring of the port name, or its index, as `output::MidiOutput` takes.
        std::string port;
    };

    /// The engine must outlive this. Nothing is opened until `start()`.
    ///
    /// `rules` is where §5.7's `panic` and `rule/<id>/enable` go — `output::OutputRunner`
    /// in an app, null where there is none, in which case a binding to either is refused
    /// rather than silently doing nothing. It must outlive this where it is not null;
    /// `setRuleControl` exists because an owner usually builds the runner second.
    MidiControl(engine::BeatEngine& engine, Config config, RuleControl* rules = nullptr);
    ~MidiControl();

    MidiControl(const MidiControl&) = delete;
    MidiControl& operator=(const MidiControl&) = delete;

    /// Opens the port and begins listening. Throws `std::runtime_error` when there is no
    /// such port or no usable MIDI API — an operator has to be told, because a control
    /// surface that silently does nothing is worse than one that will not start. Does
    /// nothing when the config says disabled, and nothing when already running.
    void start();
    void stop() noexcept;
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    const Config& config() const noexcept { return config_; }

    /// Points it at a different port, or at none when `port` is empty. Stops first; the
    /// caller starts it again.
    ///
    /// A setter rather than building a new one, because **the bindings have to survive
    /// this**: an operator moving from one controller to another is not asking for
    /// everything they learned to be forgotten, and in a picker those two are one
    /// keystroke apart. (A new instance could not be assigned in anyway — this holds a
    /// mutex, so it is neither copyable nor movable.)
    void setPort(std::string port);
    /// The port actually opened, which is RtMidi's full name for it rather than the
    /// substring that was asked for. Empty when nothing is open.
    std::string portName() const;

    /// Where §5.7's two rule actions go. See the constructor.
    void setRuleControl(RuleControl* rules) noexcept { surface_.setRuleControl(rules); }

    /// Arms learn mode for `target`. The next note or CC that arrives is bound to it.
    ///
    /// A whole target, so that a caller who knows which rule — §5.9's editor, when it draws
    /// one — can arm `rule/<id>/enable` from the card that already names it. A gesture says
    /// *which button*, never *which rule*, so nothing else can supply that half.
    void learn(ControlTarget target);
    /// Disarms without binding anything.
    void cancelLearn() noexcept;
    /// What learn mode is waiting to bind, or nothing when it is not armed.
    std::optional<ControlTarget> learning() const;

    /// The bindings, in the order they were made.
    std::vector<MidiBinding> bindings() const;
    /// Replaces the table wholesale — how a settings file is restored. Later entries win
    /// where two name the same control, so a hand-edited file behaves like a learn.
    void setBindings(std::vector<MidiBinding> bindings);
    /// Adds or rebinds one control. Returns false only when the binding is unusable.
    bool bind(const MidiBinding& binding);
    /// Forgets every binding for `action`, whatever rule it names. Returns how many went.
    ///
    /// By action rather than by target because that is what a FORGET button beside an action
    /// picker means. Forgetting the binding for one particular rule is `forget(target)`.
    std::size_t forget(ControlAction action);
    /// Forgets every binding for exactly this target. Returns how many went.
    std::size_t forget(const ControlTarget& target);

    /// Acts on one event as if it had arrived, and learns from it when armed. The way a
    /// test drives this without hardware, and the seam a UI uses to show what arrived.
    /// True when the event was learned from or matched a binding.
    bool dispatch(const MidiEvent& event);

    /// Messages that matched a binding and were acted on.
    std::uint64_t handled() const noexcept { return handled_.load(std::memory_order_relaxed); }
    /// Notes and CCs that are not bound to anything. Worth showing: a controller on the
    /// wrong channel looks exactly like a broken one until somebody can see that its
    /// messages are arriving.
    std::uint64_t ignored() const noexcept { return ignored_.load(std::memory_order_relaxed); }

    /// The last note or CC seen, whether or not it was bound. For a UI, and for an
    /// operator working out which channel their controller is on.
    std::optional<MidiEvent> lastEvent() const;

private:
    /// RtMidi's callback, and the only thing that runs on its thread.
    void onMessage(std::span<const unsigned char> message) noexcept;

    Config config_;
    ControlSurface surface_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> handled_{0};
    std::atomic<std::uint64_t> ignored_{0};

    mutable std::mutex mutex_;
    std::vector<MidiBinding> bindings_;
    std::optional<ControlTarget> learning_;
    std::optional<MidiEvent> lastEvent_;
    /// The CC just learned, whose release is still to come. Learn binds on the press; a CC
    /// button then sends its release, and that used to be dispatched through the brand-new
    /// binding — so learning a CC pad fired its action after all, which is what learn promises
    /// not to do (the audit's H1). The next event from this control, if it is a release, is
    /// swallowed.
    std::optional<MidiEvent> learnedRelease_;
    std::string portName_;
};

} // namespace takt4::control
