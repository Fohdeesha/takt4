#pragma once

#include "core/audio/devices.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/output/output_runner.hpp"
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
    void refreshDevices();
    void publishStopped();
    void publishOpenStream();
    void publishOptions();
    void publishTrace();
    void publishState();
    void publishLevels();
    void publishTaps();
    void publishOutputs();
    /// Sends a whole `Options` and remembers it until the engine is seen to have it.
    void postOptions(const tracking::TempoTracker::Options& options);
    void setStatus(const std::string& text, bool error);
    /// Seconds since this controller was built, on a steady clock. Only differences are
    /// used, which is all `tracking::TapTempo` asks of it.
    double nowSeconds() const;

    engine::LiveTracker& tracker_;
    std::vector<std::string> midiPorts_;
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

    /// The trace as a plain buffer, oldest first, mirrored into the model each tick.
    std::vector<TracePoint> trace_;
    std::shared_ptr<slint::VectorModel<TracePoint>> traceModel_;
    float peak_ = 0.0f;
    bool statusIsError_ = false;
    std::uint64_t ticks_ = 0;

    /// §4.2's output thread, and the single consumer of the engine's beat ring — which is
    /// why `tick()` no longer drains it.
    ///
    /// **Last, so that it is destroyed first.** Its destructor joins the thread, and that
    /// has to happen before anything the thread could still be touching goes away. Nothing
    /// it holds today reaches back into this class, but a beat observer is the obvious next
    /// thing to give it, and by then the ordering would be a bug rather than a choice.
    output::OutputRunner runner_;
};

} // namespace takt4::ui
