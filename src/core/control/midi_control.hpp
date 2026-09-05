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
    MidiControl(engine::BeatEngine& engine, Config config);
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
    /// The port actually opened, which is RtMidi's full name for it rather than the
    /// substring that was asked for. Empty when nothing is open.
    std::string portName() const;

    /// Arms learn mode for `action`. The next note or CC that arrives is bound to it.
    void learn(ControlAction action);
    /// Disarms without binding anything.
    void cancelLearn() noexcept;
    /// What learn mode is waiting to bind, or nothing when it is not armed.
    std::optional<ControlAction> learning() const;

    /// The bindings, in the order they were made.
    std::vector<MidiBinding> bindings() const;
    /// Replaces the table wholesale — how a settings file is restored. Later entries win
    /// where two name the same control, so a hand-edited file behaves like a learn.
    void setBindings(std::vector<MidiBinding> bindings);
    /// Adds or rebinds one control. Returns false only when the binding is unusable.
    bool bind(const MidiBinding& binding);
    /// Forgets every binding for `action`. Returns how many went.
    std::size_t forget(ControlAction action);

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
    std::optional<ControlAction> learning_;
    std::optional<MidiEvent> lastEvent_;
    std::string portName_;
};

} // namespace takt4::control
