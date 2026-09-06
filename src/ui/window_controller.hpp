#pragma once

#include "core/audio/devices.hpp"
#include "core/control/midi_control.hpp"
#include "core/control/osc_control.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/output/output_runner.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/tap_tempo.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "ui/window_state.hpp"

#include "main_window.h" // generated from main_window.slint

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace takt4::ui {

/// Everything the window needs that is not the window: the tracker behind it, the model
/// behind its trace, and the one timer that drains the engine's rings.
///
/// Nothing here is ever reached from the audio thread, which is the whole of HANDOFF
/// §7.5's rule. The engine writes into lock-free rings from its own threads; this reads
/// them on the UI thread, on a timer, and touches a Slint property nowhere else.
///
/// It is a header of its own rather than a class hidden in `app.cpp` so that
/// `takt4_ui_tests` can drive it under Slint's testing backend: press the real button,
/// step the real redraw, and read the real properties back. Without that, the wiring
/// between a click and the tracker could only be checked by a person at the machine.
class WindowController {
public:
    /// How often the window drains the engine's rings and redraws. §7.5: "Push audio-side
    /// results through a lock-free ring and drain on a UI timer — do not queue one closure
    /// per audio callback." 30 Hz is slower than the 50 Hz the frames arrive at, so every
    /// tick finds one or two waiting and none is ever missed; the ring holds ten seconds
    /// of them, so even a stalled event loop loses nothing.
    static constexpr std::chrono::milliseconds kRedrawInterval{33};

    /// How far the peak indicator falls back each tick — slow enough that a moment of
    /// clipping is still on screen when the operator looks up.
    static constexpr float kPeakDecay = 0.88f;

    /// Redraws a posted settings change is given to appear on the engine before the
    /// window goes back to showing what the engine actually has. See `postOptions`.
    /// Three redraws is about a tenth of a second, where the engine needs about two
    /// milliseconds — so this is a backstop, not a timing assumption.
    static constexpr int kSettleRedraws = 3;

    /// The tracker must outlive this. Builds the window, fills the pickers from the
    /// tracker's device list, and starts the redraw timer.
    ///
    /// It also builds §4.2's output thread and hands the tracker Link's clock, which is
    /// why nothing outside has to know the ordering §4.3 needs: `LiveTracker::start`
    /// installs the clock before it opens the stream.
    ///
    /// `settings` is what the last run left behind (Q7): the interface and channel to
    /// select if they are still there, and the outputs to switch back on. The tracker's
    /// own tuning is not applied here — it belongs to the engine, and whoever built the
    /// `LiveTracker` passes it in `Options::engine::tempo`.
    ///
    /// Two constructors rather than a default argument: §6 records that
    /// `const Settings& = {}` in a declaration compiles on MSVC and on nothing else.
    WindowController(engine::LiveTracker& tracker, const settings::Settings& settings);
    explicit WindowController(engine::LiveTracker& tracker);

    WindowController(const WindowController&) = delete;
    WindowController& operator=(const WindowController&) = delete;

    /// Shows the window and runs Slint's event loop until it is closed.
    void run();

    /// The window itself, for reading properties back.
    MainWindow& window() { return *window_; }
    const slint::ComponentHandle<MainWindow>& handle() const { return window_; }

    /// One round of draining and redrawing — what the timer calls. Public so a test can
    /// step it without waiting on a wall clock, and so that the timer path and the work
    /// it does can be checked separately.
    void tick();

    /// What the window's callbacks do, reachable directly as well as through a click.
    void pickDevice(int index);
    void pickChannel(int index) { channel_ = index; }
    void toggleRun();

    /// §5.5's manual controls. Each one posts on the engine's control queue and returns;
    /// the inference thread applies it before the next frame it tracks, so none of them
    /// blocks a redraw and none of them touches the tracker from this thread.
    void halve();
    void redouble();
    void snapDownbeat();

    /// §5.7's lock, pinned rather than set. Takes the state being asked for rather than
    /// toggling: the button already shows the current one, so sending the intention is
    /// what makes two quick presses land as two changes instead of reading the same
    /// not-yet-updated value twice.
    void setPinned(bool pinned);

    /// One tap. The no-argument form reads this object's steady clock; the other takes
    /// the time, which is how `tracking::TapTempo` is built to be driven and what lets a
    /// test tap out a tempo without spending it in real time.
    void tap();
    void tap(double seconds);

    /// §5.5's settings. Clamped to what the sliders offer, then posted whole — a slider
    /// sends an absolute value, so two arriving in one round cannot lose a step the way
    /// a relative nudge can.
    void setFoldEnabled(bool on);
    void setFoldMin(double bpm);
    void setFoldMax(double bpm);
    void setLatencyMs(double milliseconds);

    /// The settings as the engine has them — or as it is about to, when a change posted
    /// moments ago has not been applied yet. This is what the window is showing, and it
    /// is what an edit starts from, so that two quick drags do not undo each other.
    tracking::TempoTracker::Options settings() const;

    /// Taps counted so far in the set being tapped in, for the button's own label.
    std::size_t taps() const noexcept { return taps_.taps(); }

    /// §5.9's outputs row. Each posts on the output thread's queue and returns; the change
    /// is applied before its next round, or immediately while it is stopped.
    void setLinkEnabled(bool on);
    /// One `host:port` per line, blank lines ignored. A line that is not `host:port` is
    /// reported on the status line and the rest are still applied — an operator halfway
    /// through typing an address must not lose the ones that already worked.
    void setOscTargets(const std::string& text);
    /// The MIDI output port to send 24 PPQN to, or empty for none.
    void setMidiPort(const std::string& name);

    /// §5.7's control input. The port is opened at once rather than at Start: an operator
    /// binding buttons is doing it *before* the set, and a learn mode that needs the
    /// tracker running would be a worse tool than a pen and paper.
    void setMidiControlPort(const std::string& name);

    /// §5.7's *other* control input: the OSC listening socket, so a Stream Deck or Bitfocus
    /// Companion can drive this without touching the laptop.
    ///
    /// Off until asked, like the MIDI port and for a stronger reason — a listening socket is
    /// something to open on somebody's say-so, never on their behalf. `port` 0 asks the
    /// platform for a free one; `oscControlPort()` reports which it got, which is what an
    /// operator has to point their surface at.
    void setOscControlEnabled(bool on);
    void setOscControlPort(int port);
    /// Whether anything but 127.0.0.1 is accepted. False is the default; true is what a
    /// control surface on another machine needs, and is the operator's call because OSC
    /// carries no authentication and takt4 invents none.
    void setOscControlNetwork(bool allowNetwork);

    /// The OSC control surface, for reading and for a test to `dispatch` into.
    control::OscControl& oscControl() noexcept { return oscControl_; }
    const control::OscControl& oscControl() const noexcept { return oscControl_; }
    /// The port actually listening, or 0 when nothing is. Not the port that was *asked*
    /// for: with port 0 those differ, and the operator needs the real one.
    std::uint16_t oscControlPort() const noexcept { return oscControl_.port(); }
    /// Which action LEARN will bind, as an index into `control::kControlActions`.
    void pickLearnAction(int index);
    /// Arms learn mode for that action, or disarms when it is already armed. One button
    /// for both, so an operator who pressed it by mistake is not stranded in a mode.
    void toggleLearn();
    /// Unbinds every control bound to the selected action.
    void forgetLearned();

    /// The MIDI surface itself. Non-const for the same reason `window()` is: a test drives
    /// `dispatch` to deliver an event no hardware here can send, which is the only way the
    /// path from a controller to a binding is checkable without somebody pressing a pad.
    control::MidiControl& control() noexcept { return control_; }
    const control::MidiControl& control() const noexcept { return control_; }
    /// Every MIDI *input* port on the machine, as offered in the picker.
    const std::vector<std::string>& midiInputPorts() const noexcept { return midiInputPorts_; }

    /// Everything worth remembering for next time (Q7), as it stands now. The caller
    /// saves it; this class does not know where settings live and does not want to.
    settings::Settings currentSettings() const;

    /// What is being sent, for the row that draws it.
    const output::OutputRunner& outputs() const noexcept { return runner_; }
    /// Every MIDI output port on the machine, as offered in the picker.
    const std::vector<std::string>& midiPorts() const noexcept { return midiPorts_; }

    const std::vector<audio::InputDevice>& devices() const noexcept { return devices_; }
    /// Index into `devices()`, or -1 when the machine has none.
    int deviceIndex() const noexcept { return device_; }
    /// 0-based channel of the selected device.
    int channelIndex() const noexcept { return channel_; }
    bool statusIsError() const noexcept { return statusIsError_; }

    /// Times `tick()` has been entered, however it was reached. What a test watches to
    /// tell "the redraw timer is running" from "the redraw does the right thing" — two
    /// separate claims that a window either meets or silently does not.
    std::uint64_t ticks() const noexcept { return ticks_; }

private:
    /// Fills the device picker and selects one: the remembered device if it is still
    /// there, otherwise the most useful one on the machine.
    void refreshDevices(const settings::MachineSettings& remembered);
    void publishStopped();
    void publishOpenStream();
    void publishOptions();
    void publishPin();
    void publishTrace();
    void publishState();
    void publishLevels();
    void publishTaps();
    void publishOutputs();
    /// Both of §5.7's surfaces. Each half publishes independently, because the MIDI half
    /// returns early when no port is open and anything written after that return would
    /// never run on a window that has only the OSC socket.
    void publishControl();
    void publishMidiControl();
    void publishOscControl();
    void publishSnap();
    /// Sends a whole `Options` and remembers it until the engine is seen to have it.
    void postOptions(const tracking::TempoTracker::Options& options);
    void setStatus(const std::string& text, bool error);
    /// Seconds since this controller was built, on a steady clock. Only differences are
    /// used, which is all `tracking::TapTempo` asks of it.
    double nowSeconds() const;

    engine::LiveTracker& tracker_;
    std::vector<std::string> midiPorts_;
    std::vector<std::string> midiInputPorts_;
    slint::ComponentHandle<MainWindow> window_;
    slint::Timer timer_;

    std::vector<audio::InputDevice> devices_;
    int device_ = -1;
    int channel_ = 0;

    tracking::TapTempo taps_;
    /// When the last tap landed, so a set that has gone quiet stops claiming to be
    /// counting. `TapTempo` only notices its own timeout on the *next* tap.
    double lastTapSeconds_ = 0.0;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    /// The settings last posted, until the engine is seen to have them or `kSettleRedraws`
    /// have passed. While one is in flight the window shows it rather than what the engine
    /// still has: a redraw landing in the couple of milliseconds before the inference
    /// thread applies a change would otherwise snap the slider back out from under the
    /// operator's finger.
    std::optional<tracking::TempoTracker::Options> posted_;
    int settling_ = 0;

    /// The same thing for the pin, which travels as its own command rather than inside
    /// the settings — a pin followed by a release is not a pin, so it must not be
    /// superseded the way `SetTempoOptions` is.
    std::optional<bool> pinPosted_;
    int pinSettling_ = 0;

    /// The trace as a plain buffer, oldest first, mirrored into the model each tick.
    std::vector<TracePoint> trace_;
    std::shared_ptr<slint::VectorModel<TracePoint>> traceModel_;
    float peak_ = 0.0f;
    bool statusIsError_ = false;
    std::uint64_t ticks_ = 0;

    /// The tracker's beat count when a downbeat snap was posted, while one is still in
    /// flight. §5.5's snap lands on the *next* beat called, so the count moving is what
    /// says it has arrived — and until then the DOWNBEAT button stays lit, because a
    /// control that does nothing visible for the best part of a second reads as broken.
    std::optional<std::uint64_t> snapAwaitingBeat_;

    /// §5.7's control input. Which action LEARN would bind, as an index into
    /// `learnActions_`; the binding table itself lives in `control_`.
    int learnAction_ = 0;
    /// The actions this window offers for binding: `control::kControlActions` minus the
    /// ones that need a rule named too, which a gesture cannot supply. See the constructor.
    std::vector<control::ControlAction> learnActions_;

    /// §4.2's output thread, and the single consumer of the engine's beat ring — which is
    /// why `tick()` no longer drains it.
    ///
    /// Its destructor joins the thread, and that has to happen before anything the thread
    /// could still be touching goes away. Nothing it holds today reaches back into this
    /// class, but a beat observer is the obvious next thing to give it.
    output::OutputRunner runner_;

    /// §5.7's two control surfaces. **Both after `runner_`, so both are destroyed first**,
    /// and that ordering is load-bearing rather than a preference: since §5.7's `panic` and
    /// `rule/<id>/enable` landed, each of these posts to the *runner* as well as to the
    /// engine — MIDI on RtMidi's callback thread, OSC on its own receive loop. Destroying
    /// the runner first would leave either thread with a queue that has gone. The engine
    /// outlives all three either way, which is what made the opposite order safe before and
    /// is no longer the whole question.
    ///
    /// Two surfaces rather than one because each owns its own tap set: a Stream Deck's tap
    /// button and a MIDI pad are two surfaces, and interleaving their taps would give an
    /// operator using both a tempo neither meant (`control::ControlSurface`).
    control::MidiControl control_;
    control::OscControl oscControl_;
};

} // namespace takt4::ui
