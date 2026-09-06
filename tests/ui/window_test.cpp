#include "core/audio/devices.hpp"
#include "core/audio/rates.hpp"
#include "core/control/control_action.hpp"
#include "core/control/midi_binding.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/io/wav_file.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/rule.hpp"
#include "core/trigger/trigger_engine.hpp"
#include "ui/window_controller.hpp"
#include "ui/window_state.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <slint-platform.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::audio::InputDevice;
using takt4::engine::LiveTracker;
using takt4::tracking::TempoState;
using Options = takt4::tracking::TempoTracker::Options;
using takt4::ui::WindowController;

namespace {

const std::filesystem::path kWeights = std::filesystem::path(TAKT4_WEIGHTS_DIR) / "generic.bin";
const std::filesystem::path kStateSpace =
    std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin";

/// Feeds the committed synthetic excerpt into a tracker's engine on this thread, keeping
/// its place between calls so a test can track for a while, press something, and carry on
/// from where it left off.
///
/// No audio device and no worker threads: `BeatEngine::step()` is the offline half of the
/// same code path the inference thread takes, so what the window sees here is what it
/// would see live — deterministically, and on a machine with nothing plugged in.
/// `takt4-cli track tests/data/features/synthetic.wav` prints the same run.
class SyntheticRun {
public:
    explicit SyntheticRun(LiveTracker& tracker) : tracker_(tracker) {}

    /// Feeds hops until `enough` says to stop. False if the audio ran out first.
    template <typename Predicate>
    bool until(Predicate enough) {
        while (!enough()) {
            if (hop_ >= hops()) {
                return false;
            }
            tracker_.engine().processHop(samples().data() + hop_ * takt4::audio::kHopSize, hop_);
            ++hop_;
            (void)tracker_.engine().step();
        }
        return true;
    }

    /// Tracks until the tempo locks, which this excerpt does in about three seconds.
    bool untilLocked() {
        return until([this] { return tracker_.engine().state().locked; });
    }

    /// Applies whatever the window has just posted, without feeding any more audio — so a
    /// command's effect is seen on its own, with nothing tracked over the top of it. Only
    /// valid while the engine is stopped, which it is: nothing here opens a device.
    void applyPosted() { (void)tracker_.engine().step(); }

private:
    static const std::vector<float>& samples() {
        static const std::vector<float> loaded = [] {
            const takt4::io::WavData audio = takt4::io::readWavFile(
                std::filesystem::path(TAKT4_TEST_DATA_DIR) / "features" / "synthetic.wav");
            return audio.samples;
        }();
        return loaded;
    }
    static std::size_t hops() { return samples().size() / takt4::audio::kHopSize; }

    LiveTracker& tracker_;
    std::size_t hop_ = 0;
};

/// The same ranking the window itself uses, so a test picks what the window picked.
std::optional<InputDevice> bestInputDevice(const LiveTracker& tracker) {
    std::optional<InputDevice> best;
    const auto rank = [](const InputDevice& d) {
        return std::make_tuple(!d.isLoopback, takt4::audio::hasNativeChannelSelection(d.hostApi),
                               d.isDefaultInput, d.maxInputChannels);
    };
    for (const InputDevice& device : tracker.devices()) {
        if (!best || rank(device) > rank(*best)) {
            best = device;
        }
    }
    return best;
}

/// Runs Slint's timers for roughly `span`, the way an event loop would.
void pumpTimers(std::chrono::milliseconds span) {
    const auto until = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < until) {
        slint::platform::update_timers_and_animations();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    slint::platform::update_timers_and_animations();
}

} // namespace

TEST_CASE("the readouts say what the tempo state says", "[ui]") {
    // The one place an engine value becomes a window property (ui/window_state.cpp), held
    // to what the property actually ends up as. No device and no tracker: this is the
    // mapping alone.
    auto window = MainWindow::create();

    TempoState state;
    state.bpm = 128.25;
    state.rawBpm = 256.5;
    state.locked = true;
    state.pinned = true;
    state.holding = false;
    state.refined = true;
    state.confidence = 0.82;
    state.beatsPerBar = 3; // never 4 by assumption (HANDOFF §5.5)
    state.beatInBar = 2;
    state.bars = 17;
    takt4::ui::publishTempoState(*window, state);

    CHECK_THAT(window->get_bpm(), WithinAbs(128.25, 1e-4));
    CHECK_THAT(window->get_raw_bpm(), WithinAbs(256.5, 1e-4));
    CHECK(window->get_locked());
    CHECK(window->get_pinned());
    CHECK_FALSE(window->get_holding());
    CHECK(window->get_refined());
    CHECK_THAT(window->get_confidence(), WithinAbs(0.82, 1e-4));
    CHECK(window->get_beats_per_bar() == 3);
    CHECK(window->get_beat_in_bar() == 2);
    CHECK(window->get_bars() == 17);

    // Stopping must not leave the last set's tempo sitting there looking live.
    takt4::ui::publishIdleReadouts(*window);
    CHECK(window->get_bpm() == 0.0f);
    CHECK_FALSE(window->get_locked());
    CHECK_FALSE(window->get_pinned()); // and the LOCK button is not still lit
    CHECK(window->get_beats_per_bar() == 0);
    CHECK(window->get_input_level() == 0.0f);
    CHECK(std::string(window->get_input_reading()).empty());
}

TEST_CASE("the fold window reaches the window unchanged", "[ui]") {
    // HANDOFF §7 deviation 4: the operator has to be able to see the octave-fold window,
    // because outside it the fold turns a right answer into a wrong one.
    auto window = MainWindow::create();
    takt4::tracking::TempoTracker::Options options;
    options.octaveFold = true;
    options.minBpm = 88.0;
    options.maxBpm = 176.0;
    takt4::ui::publishTempoOptions(*window, options);

    CHECK(window->get_fold_on());
    CHECK_THAT(window->get_fold_min(), WithinAbs(88.0, 1e-4));
    CHECK_THAT(window->get_fold_max(), WithinAbs(176.0, 1e-4));

    options.octaveFold = false;
    takt4::ui::publishTempoOptions(*window, options);
    CHECK_FALSE(window->get_fold_on());
}

TEST_CASE("the input meter maps a level onto the bar and reads it out", "[ui]") {
    auto window = MainWindow::create();

    // Full scale is the top of the bar.
    takt4::ui::publishInput(*window, 1.0f, 1.0f);
    CHECK_THAT(window->get_input_level(), WithinAbs(1.0, 1e-3));
    CHECK(std::string(window->get_input_reading()) == "0.0 dB");

    // Half scale is about -6 dB, which is 90 % of the way up a bar that bottoms at -60.
    takt4::ui::publishInput(*window, 0.5f, 0.5f);
    CHECK_THAT(window->get_input_level(), WithinAbs(0.8997, 2e-3));
    CHECK(std::string(window->get_input_reading()) == "-6.0 dB");

    // Silence is the bottom, and says so rather than showing a number nobody can read.
    takt4::ui::publishInput(*window, 0.0f, 0.0f);
    CHECK(window->get_input_level() == 0.0f);
    CHECK(std::string(window->get_input_reading()) == "-inf dB");
}

TEST_CASE("a channel is described by the driver's name for it where there is one", "[ui]") {
    InputDevice device;
    device.name = "MOTU Pro Audio";
    device.hostApiName = "ASIO";
    device.maxInputChannels = 2;
    CHECK(takt4::ui::describeChannel(device, 0) == "In 1");

    device.channelNames = {"Mic 1", ""};
    CHECK(takt4::ui::describeChannel(device, 0) == "In 1 \xE2\x80\x94 Mic 1");
    CHECK(takt4::ui::describeChannel(device, 1) == "In 2"); // an empty name is no name

    CHECK(takt4::ui::describeDevice(device) == "ASIO / MOTU Pro Audio  (2 in)");
    device.isLoopback = true;
    CHECK(takt4::ui::describeDevice(device) == "ASIO / MOTU Pro Audio  (2 in, loopback)");
}

TEST_CASE("the window comes up stopped, offering the machine's inputs", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    CHECK_FALSE(controller.window().get_running());
    CHECK_FALSE(tracker.running());
    CHECK(controller.window().get_bpm() == 0.0f);

    // Every device the tracker can see is in the picker, in the same order.
    const auto devices = controller.window().get_devices();
    REQUIRE(devices->row_count() == tracker.devices().size());
    for (std::size_t i = 0; i < tracker.devices().size(); ++i) {
        CHECK(std::string(*devices->row_data(i)) ==
              takt4::ui::describeDevice(tracker.devices()[i]));
    }

    // The trace is there and the right length before a single frame has arrived, so the
    // markup's `parent.width / root.trace.length` never divides by zero.
    const auto trace = controller.window().get_trace();
    REQUIRE(trace);
    CHECK(trace->row_count() == takt4::ui::kTraceLength);
}

TEST_CASE("picking a device fills the channel list from that device", "[ui][hardware]") {
    LiveTracker tracker(kWeights, kStateSpace);
    if (tracker.devices().empty()) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);

    // Through the window's own callback, so this covers the binding the constructor makes
    // and not just the handler behind it.
    for (std::size_t i = 0; i < tracker.devices().size(); ++i) {
        controller.window().invoke_device_picked(static_cast<int>(i));
        CHECK(controller.deviceIndex() == static_cast<int>(i));
        CHECK(controller.channelIndex() == 0); // a new device starts at its first input
        const auto channels = controller.window().get_channels();
        REQUIRE(channels);
        CHECK(channels->row_count() ==
              static_cast<std::size_t>(tracker.devices()[i].maxInputChannels));
    }

    // An index the list does not have is ignored rather than crashing: a stale click from
    // a picker that was rebuilt underneath it must not take the app down.
    const int before = controller.deviceIndex();
    controller.window().invoke_device_picked(9999);
    controller.window().invoke_device_picked(-1);
    CHECK(controller.deviceIndex() == before);
}

TEST_CASE("the redraw timer runs on its own", "[ui]") {
    // §7.5 wants the engine drained on a UI timer. That the timer exists is one claim and
    // that the draining is right is another; this is the first, and it is the one a still
    // picture of the window can never show.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    const std::uint64_t before = controller.ticks();
    pumpTimers(std::chrono::milliseconds(250));
    const std::uint64_t after = controller.ticks();

    INFO(after - before << " ticks in 250 ms at " << WindowController::kRedrawInterval.count()
                        << " ms");
    // 250 ms is seven intervals; assert a floor well under that so a busy machine cannot
    // fail it, and a ceiling that would catch a timer firing far too often.
    CHECK(after - before >= 3);
    CHECK(after - before <= 40);
}

TEST_CASE("the run callback opens a device and closes it again", "[ui][hardware]") {
    // The path a click takes once it has left the button: the window's own toggle-run
    // callback, through the controller, into a real device. Nothing below it is mocked.
    //
    // What this cannot reach is the one line of markup that binds the Button's `clicked`
    // to this callback. Clicking an element needs Slint's testing API, which is behind
    // SLINT_FEATURE_EXPERIMENTAL and off in this build; the button is named
    // `run-button` in main_window.slint so the test can be written the day that changes.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    REQUIRE_FALSE(tracker.running());

    controller.window().invoke_toggle_run();
    REQUIRE(tracker.running());
    CHECK(controller.window().get_running());
    CHECK_FALSE(controller.statusIsError());
    // The status line stops offering and starts reporting: it names the channel now.
    CHECK(std::string(controller.window().get_status()).find(" of ") != std::string::npos);

    controller.window().invoke_toggle_run();
    CHECK_FALSE(tracker.running());
    CHECK_FALSE(controller.window().get_running());
    // Stopping clears the readouts rather than freezing the last tempo on screen.
    CHECK(controller.window().get_bpm() == 0.0f);
    CHECK(controller.window().get_beats_per_bar() == 0);
}

TEST_CASE("the trace scrolls while the tracker is running", "[ui][hardware]") {
    // The other claim a still picture cannot make: that the window's redraw actually
    // moves the activation trace along as frames arrive.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    controller.toggleRun();
    REQUIRE(tracker.running());

    const auto trace = controller.window().get_trace();
    REQUIRE(trace);
    // The newest frame is the last row; before any arrive it is the zero a fresh model
    // holds, and the whole trace is that.
    CHECK(trace->row_data(takt4::ui::kTraceLength - 1)->beat == 0.0f);

    // Two seconds is a hundred frames at 50 Hz, comfortably more than the trace's length
    // of live data even on a slow start.
    pumpTimers(std::chrono::milliseconds(2000));
    controller.toggleRun();

    std::size_t carrying = 0;
    for (std::size_t i = 0; i < takt4::ui::kTraceLength; ++i) {
        if (trace->row_data(i)->beat != 0.0f) {
            ++carrying;
        }
    }
    const std::uint64_t frames = tracker.engine().framesTracked();
    INFO(controller.ticks() << " redraws, " << frames << " frames tracked, " << carrying << " of "
                            << takt4::ui::kTraceLength << " trace points carrying");

    // Two seconds is a hundred frames at 50 Hz. Assert a floor well under that rather
    // than the number itself, so a busy machine cannot fail this.
    CHECK(frames > 50);
    CHECK(carrying >= 50);

    // That it *scrolled* is the point, and it is what the direction shows: the newest
    // frame is the last row, so the right-hand end is live...
    CHECK(trace->row_data(takt4::ui::kTraceLength - 1)->beat != 0.0f);
    // ...and with fewer frames than the trace is long, the oldest end is still the zero
    // it started as, which a fill rather than a rolling window would have overwritten.
    if (frames < takt4::ui::kTraceLength) {
        CHECK(trace->row_data(0)->beat == 0.0f);
    }
}

// ---------------------------------------------------------------------------------------
// §5.5's manual controls. Every one of these drives the window's own callback and then
// checks the *tracker*, so what is under test is the whole path from a press to the engine
// — not that a handler was called. What each command then means is covered by
// tests/engine and tests/tracking; these are about the wiring, and about the settings
// round trip the window adds on top of it.
// ---------------------------------------------------------------------------------------

TEST_CASE("halving and doubling move the published tempo by an octave", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);
    REQUIRE(run.untilLocked());

    // The excerpt tracks at about 128, inside the default 70-140 window, so folding leaves
    // it alone and an octave shift is exactly a halving of what the cloud reports.
    const double raw = tracker.engine().state().rawBpm;
    const Options fold = tracker.engine().tempoOptions();
    REQUIRE(raw >= fold.minBpm);
    REQUIRE(raw < fold.maxBpm);

    controller.window().invoke_halve();
    run.applyPosted();
    CHECK_THAT(tracker.engine().state().bpm, WithinAbs(raw / 2.0, 1e-6));

    controller.window().invoke_redouble();
    run.applyPosted();
    CHECK_THAT(tracker.engine().state().bpm, WithinAbs(raw, 1e-6));
}

TEST_CASE("the LOCK button pins the tracker's lock and lets go of it", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);
    REQUIRE(run.untilLocked());
    REQUIRE_FALSE(controller.window().get_pinned());

    controller.window().invoke_pin_changed(true);
    // Lit before the engine has had a frame to agree, so a second press reads the
    // intention rather than the state it is about to replace.
    CHECK(controller.window().get_pinned());
    run.applyPosted();
    CHECK(tracker.engine().state().pinned);
    CHECK(tracker.engine().state().locked);

    controller.window().invoke_pin_changed(false);
    CHECK_FALSE(controller.window().get_pinned());
    run.applyPosted();
    CHECK_FALSE(tracker.engine().state().pinned);
    // Released, so the tracker has the tempo back and is hunting for it again.
    CHECK_FALSE(tracker.engine().state().locked);
}

TEST_CASE("the window learns a control and remembers what it learned", "[ui]") {
    // §5.7's learn mode, end to end through the window's own callbacks. No hardware:
    // `MidiControl::dispatch` delivers the event a controller would have sent, which is the
    // only way this path is checkable without somebody standing at the machine pressing a
    // pad — and it is the same seam RtMidi's callback uses.
    using takt4::control::ControlAction;
    using takt4::control::MidiEvent;

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    // Learn is armed from the window, for whichever action the picker is on.
    const int downbeat = 1; // kControlActions[1]
    REQUIRE(takt4::control::kControlActions[downbeat] == ControlAction::Downbeat);
    controller.window().invoke_learn_action_picked(downbeat);
    controller.window().invoke_learn_clicked();
    CHECK(controller.window().get_learning());
    CHECK(controller.control().learning() == ControlAction::Downbeat);

    MidiEvent pad;
    pad.kind = MidiEvent::Kind::Note;
    pad.channel = 10;
    pad.number = 36;
    pad.value = 100;
    CHECK(controller.control().dispatch(pad));

    REQUIRE(controller.control().bindings().size() == 1);
    CHECK(controller.control().bindings().front().target.action == ControlAction::Downbeat);
    CHECK_FALSE(controller.control().learning().has_value());

    SECTION("and it shows on the row, without repeating the action beside it") {
        // Off, because no port is open — the reading has to say that rather than claim a
        // binding is live when nothing is listening.
        controller.tick();
        CHECK_FALSE(controller.window().get_learning());
        CHECK_FALSE(controller.window().get_control_on());
        CHECK(std::string(controller.window().get_control_reading()).find("note 36") ==
              std::string::npos);
    }

    SECTION("what was learned is what gets saved") {
        const takt4::settings::Settings saved = controller.currentSettings();
        REQUIRE(saved.machine.midiBindings.size() == 1);
        CHECK(saved.machine.midiBindings.front() == "note 36 ch 10 -> downbeat");

        // And a window built from that file has it again, which is the whole round trip.
        LiveTracker restored(kWeights, kStateSpace);
        WindowController second(restored, saved);
        REQUIRE(second.control().bindings().size() == 1);
        CHECK(second.control().bindings().front().number == 36);
        CHECK(second.control().bindings().front().channel == 10);
        CHECK(second.control().bindings().front().target.action == ControlAction::Downbeat);
    }

    SECTION("pressing LEARN again gives up rather than stranding the operator") {
        controller.window().invoke_learn_clicked();
        CHECK(controller.control().learning().has_value());
        controller.window().invoke_learn_clicked();
        CHECK_FALSE(controller.control().learning().has_value());
        CHECK(controller.control().bindings().size() == 1); // and nothing was bound
    }

    SECTION("FORGET unbinds the action the picker is on, and only that one") {
        controller.window().invoke_learn_action_picked(0); // tap
        controller.window().invoke_forget_clicked();
        CHECK(controller.control().bindings().size() == 1); // downbeat's is untouched

        controller.window().invoke_learn_action_picked(downbeat);
        controller.window().invoke_forget_clicked();
        CHECK(controller.control().bindings().empty());
    }
}

TEST_CASE("a learned control reaches the tracker through the window", "[ui]") {
    using takt4::control::ControlAction;
    using takt4::control::MidiEvent;

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);
    REQUIRE(run.untilLocked());

    controller.window().invoke_learn_action_picked(2); // halve
    REQUIRE(takt4::control::kControlActions[2] == ControlAction::TempoHalve);
    controller.window().invoke_learn_clicked();

    MidiEvent pad;
    pad.number = 40;
    pad.value = 127;
    REQUIRE(controller.control().dispatch(pad));

    // The binding gesture must not also fire it: an operator binding `tempo/halve` would
    // otherwise halve the tempo in the act of binding it. Held as "did not halve" rather
    // than to an exact number, because what is published here is the *refined* tempo from
    // the beat spacing (129.03) and not the cloud's own rawBpm (130.40) — the two differ
    // by more than any equality worth writing.
    const double raw = tracker.engine().state().rawBpm;
    const double before = tracker.engine().state().bpm;
    run.applyPosted();
    CHECK(tracker.engine().state().bpm > before * 0.9);

    // The same pad again is the control doing its job. A halve republishes from rawBpm and
    // drops the refinement, so this one *is* exact — as tests/ui's own ÷2 test relies on.
    CHECK(controller.control().dispatch(pad));
    run.applyPosted();
    CHECK_THAT(tracker.engine().state().bpm, WithinAbs(raw / 2.0, 1e-6));
}

TEST_CASE("a learned control reaches the rules through the window", "[ui][trigger]") {
    // The other destination. §5.7's `panic` is a question for §5.8's rules rather than for
    // the tracker, so a binding to it has to arrive at the window's *output runner* — which
    // is a different object, on a different thread, behind a different queue.
    //
    // This is also what holds the member ordering in `WindowController` in place: the MIDI
    // surface now posts to the runner from RtMidi's callback thread, so it has to be
    // declared after it and destroyed before it. Nothing here can catch that ordering going
    // wrong, but a test that exercises the route makes the reason visible next to it.
    using takt4::control::ControlAction;
    using takt4::control::MidiEvent;

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    const int panic = 5;
    REQUIRE(takt4::control::kControlActions[panic] == ControlAction::Panic);
    controller.window().invoke_learn_action_picked(panic);
    controller.window().invoke_learn_clicked();

    MidiEvent pad;
    pad.number = 44;
    pad.value = 127;
    REQUIRE(controller.control().dispatch(pad)); // learned, not fired
    CHECK_FALSE(controller.outputs().panicked());

    // And now it is the panic button. The runner is stopped, so the command applies on this
    // thread at once — which is exactly what an operator arming a rig before a set does.
    CHECK(controller.control().dispatch(pad));
    CHECK(controller.outputs().panicked());

    SECTION("and the OSC socket moves the same latch the pad just moved") {
        // §5.7's other surface, wired into the window on 2026-09-06. The pad above left
        // panic engaged, and this releases it *from the socket* — which is the whole claim:
        // one runner, two surfaces, one latch. Two runners would show up here as a release
        // that does nothing.
        //
        // Driven through `dispatch` rather than a real datagram: the socket has its own
        // tests in tests/control, and nothing here needs one to exist.
        REQUIRE(controller.outputs().panicked());
        CHECK(controller.oscControl().dispatch("/takt4/ctl/panic", 0.0));
        CHECK_FALSE(controller.outputs().panicked());

        // And a bare `/ctl/panic` engages, because a panic button panics.
        CHECK(controller.oscControl().dispatch("/takt4/ctl/panic", std::nullopt));
        CHECK(controller.outputs().panicked());
    }

    SECTION("the picker offers panic but not the one that would need a rule named") {
        // §5.7's `rule/<id>/enable` cannot be armed from a gesture: pressing a pad says
        // which button, never which rule. The list is the actions minus that one, so it is
        // one shorter than the table.
        CHECK(controller.window().get_learn_actions()->row_count() ==
              takt4::control::kControlActions.size() - 1);
    }
}

TEST_CASE("rules load from a preset, run, and are saved back", "[ui][trigger]") {
    // Q7 puts §5.8's rules in the portable half, and this is the whole path an operator
    // takes without knowing it: a file, into the output thread, and back to a file.
    using takt4::trigger::Rule;

    Rule::Config clip;
    clip.id = "drop";
    clip.name = "Random clip on downbeat";
    clip.trigger = takt4::trigger::Trigger::Downbeat;
    clip.address = "/composition/layers/3/clips/{c}/connect";
    takt4::trigger::Generator::Config which;
    which.pool = takt4::trigger::Pool::List;
    which.values = {takt4::trigger::Value::ofInt(3), takt4::trigger::Value::ofInt(7),
                    takt4::trigger::Value::ofInt(12)};
    clip.segments = {which};

    takt4::settings::Settings loaded;
    loaded.preset.rules = {clip};

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker, loaded);

    // The window's own copy, which is the editable one.
    REQUIRE(controller.rules().size() == 1);
    CHECK(controller.rules()[0].id == "drop");

    // And the output thread's, which is the live one. Safe to read here because the runner
    // has not been started — which is what `rules()` documents as the only time it is.
    REQUIRE(controller.outputs().triggers().ruleCount() == 1);
    CHECK(controller.outputs().triggers().rule(0).valid());
    CHECK_FALSE(controller.statusIsError()); // a valid rule says nothing

    SECTION("and they go back into the file they came from") {
        const takt4::settings::Settings saved = controller.currentSettings();
        REQUIRE(saved.preset.rules.size() == 1);
        CHECK(saved.preset.rules[0].id == "drop");
        REQUIRE(saved.preset.rules[0].segments.size() == 1);
        CHECK(saved.preset.rules[0].segments[0].values.size() == 3);
    }

    SECTION("replacing the set replaces it on the output thread too") {
        Rule::Config other;
        other.id = "stab";
        other.address = "/fire";
        controller.setRules({other});
        CHECK(controller.rules().size() == 1);
        CHECK(controller.outputs().triggers().ruleCount() == 1);
        CHECK(controller.outputs().triggers().rule(0).id() == "stab");
    }

    SECTION("a rule that will not fire is kept, and said so") {
        // §5.8's policy: held, shown, and refused at fire time. The alternative — dropping
        // it on load — would delete the rule an operator is half-way through fixing.
        Rule::Config broken;
        broken.id = "broken";
        broken.address = "/a/{x}/b"; // one placeholder, no segments
        controller.setRules({broken});
        CHECK(controller.rules().size() == 1);
        REQUIRE(controller.outputs().triggers().ruleCount() == 1);
        CHECK_FALSE(controller.outputs().triggers().rule(0).valid());
        CHECK(controller.statusIsError());
    }
}

TEST_CASE("the OSC control socket opens only when asked, and is remembered", "[ui]") {
    // §5.7's listening socket, which nothing opened until 2026-09-06 — the library existed
    // and no application built one. Three separate decisions live here and each is the
    // operator's: listen at all, on which port, and whether past this machine.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    // Off on a fresh window. A socket is not something to open on somebody's behalf.
    CHECK_FALSE(controller.oscControl().running());
    CHECK_FALSE(controller.window().get_osc_control_on());
    CHECK(controller.oscControlPort() == 0); // nothing bound, so no port to report

    SECTION("switching it on binds a socket and says which port") {
        // Port 0 asks the platform for a free one, which is the only way a test on a shared
        // runner cannot lose a race with whatever else is listening.
        controller.window().invoke_osc_control_port_edited(slint::SharedString("0"));
        controller.window().invoke_osc_control_toggled(true);

        REQUIRE(controller.oscControl().running());
        // The port *bound*, not the 0 that was asked for — an operator cannot aim a Stream
        // Deck at "any free one".
        const std::uint16_t bound = controller.oscControlPort();
        CHECK(bound != 0);
        CHECK(std::string(controller.window().get_osc_control_port()) == std::to_string(bound));
        CHECK_FALSE(controller.statusIsError());

        SECTION("and what is saved is the port asked for, not the one handed out") {
            // Saving the bound port would silently pin every future launch to whatever the
            // platform happened to give this run.
            const takt4::settings::Settings saved = controller.currentSettings();
            CHECK(saved.machine.oscControlEnabled);
            CHECK(saved.machine.oscControlPort == 0);
            CHECK(saved.machine.oscControlLocalOnly);
        }

        SECTION("switching it off closes it") {
            controller.window().invoke_osc_control_toggled(false);
            CHECK_FALSE(controller.oscControl().running());
            CHECK(controller.oscControlPort() == 0);
            CHECK_FALSE(controller.currentSettings().machine.oscControlEnabled);
        }

        SECTION("opening it to the network rebinds rather than leaving it loopback") {
            controller.window().invoke_osc_control_network_toggled(true);
            CHECK(controller.oscControl().running()); // it was listening, so it still is
            CHECK_FALSE(controller.oscControl().config().localOnly);
            CHECK_FALSE(controller.currentSettings().machine.oscControlLocalOnly);
        }
    }

    SECTION("a port that is not a number is refused rather than silently ignored") {
        controller.window().invoke_osc_control_port_edited(slint::SharedString("70o1"));
        CHECK(controller.statusIsError());
        CHECK_FALSE(controller.oscControl().running());
        // And the config is untouched, so the field goes back to what it was.
        CHECK(controller.oscControl().config().port == 7001);
    }

    SECTION("editing the port while it is off does not open a socket") {
        // A number typed into a field is a number typed into a field. Binding on it would
        // be opening a port nobody asked for, which is the one thing this must not do.
        controller.window().invoke_osc_control_port_edited(slint::SharedString("7005"));
        CHECK_FALSE(controller.oscControl().running());
        CHECK(controller.oscControl().config().port == 7005);
        CHECK_FALSE(controller.currentSettings().machine.oscControlEnabled);
    }

    SECTION("what the last run had, the next run opens") {
        takt4::settings::Settings remembered;
        remembered.machine.oscControlEnabled = true;
        remembered.machine.oscControlPort = 0; // any free one, as above
        remembered.machine.oscControlLocalOnly = false;

        LiveTracker second(kWeights, kStateSpace);
        WindowController restored(second, remembered);
        CHECK(restored.oscControl().running());
        CHECK(restored.oscControlPort() != 0);
        CHECK_FALSE(restored.oscControl().config().localOnly);
    }
}

TEST_CASE("a tap seeds the fold window onto the tapped tempo", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);

    // The times are passed rather than read from a clock, which is how tracking::TapTempo
    // is built to be driven — so three taps half a second apart cost no wall-clock time
    // and mean exactly 120 BPM.
    REQUIRE(controller.taps() == 0);
    controller.tap(0.0);
    controller.tap(0.5);
    CHECK(controller.window().get_tap_count() == 2);       // counting the operator in
    CHECK(tracker.engine().tempoOptions().minBpm == 70.0); // nothing sent before three
    controller.tap(1.0);
    run.applyPosted();

    // §7's locked decision: a tap *seeds*. The window moves onto an octave centred on the
    // tapped tempo — 120/sqrt(2) to 120*sqrt(2) — and the tracker keeps tracking.
    const Options options = tracker.engine().tempoOptions();
    CHECK(options.octaveFold);
    CHECK_THAT(options.minBpm, WithinAbs(84.853, 0.05));
    CHECK_THAT(options.maxBpm, WithinAbs(169.706, 0.05));

    // And the button reaches all of that: this is the callback the markup is bound to.
    controller.window().invoke_tap();
    CHECK(controller.taps() > 0);
}

TEST_CASE("the manual downbeat reaches the tracker", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);

    // Far enough in that the filter has settled on a bar of its own to disagree with, and
    // stopped on a beat that is not already the bar's first — a press there would agree with
    // the filter and move nothing.
    REQUIRE(run.until([&tracker] {
        return tracker.engine().state().bars >= 2 && tracker.engine().state().beatInBar > 1;
    }));
    takt4::engine::EngineBeat beat;
    while (tracker.engine().popBeat(beat)) {
    }

    controller.window().invoke_snap_downbeat();
    run.applyPosted();

    // The press names the beat just called, so the bar moves on the press itself and no
    // audio is needed for it. What it then does to the bar afterwards is
    // tests/engine/beat_engine_test.cpp's business.
    CHECK(tracker.engine().state().beatInBar == 1);

    // The next beat is what carries the new phase out to the transports, and it is the
    // bar's second.
    std::optional<takt4::engine::EngineBeat> next;
    REQUIRE(run.until([&] {
        while (tracker.engine().popBeat(beat)) {
            if (!next) {
                next = beat;
            }
        }
        return next.has_value();
    }));
    REQUIRE(next);
    CHECK(next->event.snapped);
    CHECK_FALSE(next->event.downbeat);
    CHECK(next->event.beatInBar == 2);
}

TEST_CASE("the downbeat button says it has been pressed", "[ui]") {
    // The user's report, 2026-09-05: "the downbeat button has never seemed to do anything
    // ... nothing changes". The snap itself was working — tests/engine proves it through
    // the real filter — but the window said nothing between the press and the beat it
    // lands on, which at 70 BPM is the best part of a second. A control that is invisible
    // for that long, and then shifts a bar phase that is hard to see moving (and in a 2/4
    // bar is barely visible at all), is indistinguishable from one that is broken.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);

    REQUIRE(run.until([&tracker] { return tracker.engine().state().bars >= 2; }));
    CHECK_FALSE(controller.window().get_snap_pending());

    controller.window().invoke_snap_downbeat();
    // Lit immediately, on the press itself rather than on the next redraw: the whole point
    // is that nothing else happens for a while.
    CHECK(controller.window().get_snap_pending());

    // It stays lit while no beat has been called...
    const std::uint64_t before = tracker.engine().state().beats;
    controller.tick();
    CHECK(controller.window().get_snap_pending());
    CHECK(tracker.engine().state().beats == before);

    // ...and goes out on the beat the snap lands on.
    REQUIRE(run.until([&] { return tracker.engine().state().beats != before; }));
    controller.tick();
    CHECK_FALSE(controller.window().get_snap_pending());

    // A stop also clears it — there is no beat coming that it was waiting for, since the
    // next run reseeds the tracker. Not asserted here: `SyntheticRun` feeds the engine
    // without opening a device, so `toggleRun()` would take the *start* path rather than
    // the stop one. `publishStopped` is where it happens, beside the tap count that is
    // dropped for the same reason.
}

TEST_CASE("the settings sliders send whole options, clamped to what they offer", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);

    controller.window().invoke_latency_changed(-20.0f);
    run.applyPosted();
    CHECK_THAT(tracker.engine().tempoOptions().latencyOffsetSeconds, WithinAbs(-0.020, 1e-9));

    // Past the end of its own slider — which the markup cannot produce, but a test can and
    // §5.7's inbound OSC will be able to.
    controller.window().invoke_latency_changed(-9999.0f);
    run.applyPosted();
    CHECK_THAT(tracker.engine().tempoOptions().latencyOffsetSeconds,
               WithinAbs(-takt4::ui::kLatencyLimitMs / 1000.0, 1e-9));

    controller.window().invoke_fold_on_changed(false);
    run.applyPosted();
    CHECK_FALSE(tracker.engine().tempoOptions().octaveFold);
    controller.window().invoke_fold_on_changed(true);
    run.applyPosted();
    CHECK(tracker.engine().tempoOptions().octaveFold);

    // The two ends cannot cross. `foldInto` returns the tempo unfolded when the window is
    // inverted, so an inverted window would look like the fold quietly not working.
    controller.window().invoke_fold_min_changed(300.0f);
    controller.window().invoke_fold_max_changed(10.0f);
    run.applyPosted();
    const Options options = tracker.engine().tempoOptions();
    INFO("fold window " << options.minBpm << " to " << options.maxBpm);
    CHECK(options.minBpm >= takt4::ui::kFoldFloorBpm);
    CHECK(options.maxBpm <= takt4::ui::kFoldCeilingBpm);
    CHECK(options.maxBpm - options.minBpm >= takt4::ui::kFoldLeastSpanBpm);
}

TEST_CASE("two settings changes in a row do not undo each other", "[ui]") {
    // The engine applies a posted command on its own thread, which has not run yet here —
    // exactly as it would not have two milliseconds after a drag. An edit that started
    // from `tempoOptions()` alone would read the fold window from before the first change
    // and send it back, silently losing it.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);
    REQUIRE(tracker.engine().tempoOptions().minBpm == 70.0);
    REQUIRE(tracker.engine().tempoOptions().maxBpm == 140.0);

    controller.window().invoke_fold_min_changed(90.0f);
    controller.window().invoke_fold_max_changed(180.0f);
    run.applyPosted();

    const Options options = tracker.engine().tempoOptions();
    CHECK_THAT(options.minBpm, WithinAbs(90.0, 1e-6));
    CHECK_THAT(options.maxBpm, WithinAbs(180.0, 1e-6));
}

TEST_CASE("a setting shows on the window before the engine has taken it", "[ui]") {
    // The other half of the same problem: the redraw timer reads the engine back 30 times
    // a second, and a change that has been sent but not applied must not be snapped out
    // from under the operator's finger.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    controller.window().invoke_fold_min_changed(90.0f);
    controller.window().invoke_latency_changed(-30.0f);

    // Nothing has drained the queue, so the engine still has the old settings...
    CHECK(tracker.engine().tempoOptions().minBpm == 70.0);
    CHECK(tracker.engine().tempoOptions().latencyOffsetSeconds == 0.0);
    // ...and the window shows what was sent, which is what the operator did.
    CHECK_THAT(controller.window().get_fold_min(), WithinAbs(90.0, 1e-4));
    CHECK_THAT(controller.window().get_latency_ms(), WithinAbs(-30.0, 1e-4));
    CHECK_THAT(controller.settings().minBpm, WithinAbs(90.0, 1e-6));
}

TEST_CASE("the controls drive a tracker that is really running", "[ui][hardware]") {
    // The same wiring against the live path — an open device, the engine's own inference
    // thread applying the commands, and the redraw timer reading them back — so that
    // nothing in the offline tests above is passing only because one thread did it all.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    controller.toggleRun();
    REQUIRE(tracker.running());

    controller.window().invoke_latency_changed(-30.0f);
    controller.window().invoke_fold_min_changed(90.0f);
    controller.window().invoke_snap_downbeat();
    controller.window().invoke_halve();

    // Long enough for the inference thread to have applied all four and for several
    // redraws to have read them back.
    pumpTimers(std::chrono::milliseconds(250));
    controller.toggleRun();

    const Options options = tracker.engine().tempoOptions();
    CHECK_THAT(options.latencyOffsetSeconds, WithinAbs(-0.030, 1e-9));
    CHECK_THAT(options.minBpm, WithinAbs(90.0, 1e-6));
    CHECK(tracker.engine().commandsDropped() == 0);

    // The redraws left the window agreeing with the engine rather than fighting it.
    CHECK_THAT(controller.window().get_fold_min(), WithinAbs(90.0, 1e-4));
    CHECK_THAT(controller.window().get_latency_ms(), WithinAbs(-30.0, 1e-4));
}

// ---------------------------------------------------------------------------------------
// §5.9's outputs row. The window owns §4.2's output thread now, so these check the path
// from a control to the transports — and that the window stopped draining the beat ring,
// which is the runner's to consume.
// ---------------------------------------------------------------------------------------

TEST_CASE("the window comes up sending nothing", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    CHECK_FALSE(controller.outputs().transports().linkEnabled());
    CHECK(controller.outputs().transports().osc().targetCount() == 0);
    CHECK(controller.outputs().transports().midiClock() == nullptr);
    CHECK_FALSE(controller.window().get_link_on());
    CHECK_FALSE(controller.window().get_osc_on());
    CHECK_FALSE(controller.window().get_midi_on());

    // The picker always offers "none" first, so switching MIDI off is a choice in the
    // same list rather than a second control.
    const auto ports = controller.window().get_midi_ports();
    REQUIRE(ports);
    REQUIRE(ports->row_count() == controller.midiPorts().size() + 1);
    CHECK(std::string(*ports->row_data(0)).empty());
}

TEST_CASE("the Link tick reaches the transports and shows its peers", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    controller.window().invoke_link_toggled(true);
    CHECK(controller.outputs().transports().linkEnabled());
    CHECK(controller.window().get_link_on());
    // Switched on while stopped says what to do, not to do it now: nothing is in front of
    // peers until the tracker runs.
    CHECK_FALSE(controller.outputs().transports().link().enabled());
    CHECK(controller.window().get_link_peers() == 0);

    controller.window().invoke_link_toggled(false);
    CHECK_FALSE(controller.outputs().transports().linkEnabled());
}

TEST_CASE("outputs are parsed, and a bad one does not lose the good ones", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const auto& transports = [&controller]() -> const takt4::output::Transports& {
        return controller.outputs().transports();
    };

    // The format this field always took, before targets had names — still a target, named
    // after its own address.
    controller.window().invoke_osc_targets_edited(slint::SharedString("127.0.0.1:7000"));
    CHECK(transports().osc().targetCount() == 1);
    REQUIRE(transports().outputs().size() == 1);
    CHECK(transports().outputs()[0].name == "127.0.0.1:7000");
    CHECK(controller.window().get_osc_on());

    // A single line holds more than one, separated by commas, because the window offers
    // one line. §5.6's "multiple simultaneous targets", each with a name a rule can use.
    controller.window().invoke_osc_targets_edited(
        slint::SharedString("deck = 127.0.0.1:7000, wall = 127.0.0.1:7001"));
    CHECK(transports().osc().targetCount() == 2);
    REQUIRE(transports().outputs().size() == 2);
    CHECK(transports().outputs()[0].name == "deck");
    CHECK(transports().outputs()[1].name == "wall");

    SECTION("and the field shows them back in the form they can be typed in") {
        const std::string shown(controller.window().get_osc_targets());
        INFO(shown);
        CHECK(shown.find("deck = 127.0.0.1:7000") != std::string::npos);
        CHECK(shown.find("wall = 127.0.0.1:7001") != std::string::npos);
    }

    SECTION("a switched-off target is kept and sends nothing") {
        controller.window().invoke_osc_targets_edited(
            slint::SharedString("deck = 127.0.0.1:7000, off wall = 127.0.0.1:7001"));
        REQUIRE(transports().outputs().size() == 2);
        CHECK_FALSE(transports().outputs()[1].enabled);
        // Held in the list, so it can be switched back on — but no socket behind it.
        CHECK(transports().osc().targetCount() == 1);
    }

    SECTION("two targets with one name is said rather than silently resolved") {
        // `resolveOutputs` would take the first, and a rule routed to the second would go
        // somewhere its operator did not choose.
        controller.window().invoke_osc_targets_edited(
            slint::SharedString("deck = 127.0.0.1:7000, deck = 127.0.0.1:7001"));
        CHECK(controller.statusIsError());
    }

    SECTION("halfway through typing, the ones that already worked survive") {
        controller.window().invoke_osc_targets_edited(
            slint::SharedString("deck = 127.0.0.1:7000, wall = 127.0.0.1:"));
        CHECK(transports().osc().targetCount() == 1);
        CHECK(controller.statusIsError());
        CHECK(std::string(controller.window().get_status()).find("not a target") !=
              std::string::npos);
    }

    SECTION("a port outside the range is not a port") {
        controller.window().invoke_osc_targets_edited(slint::SharedString("127.0.0.1:99999"));
        CHECK(transports().osc().targetCount() == 0);
    }

    SECTION("and clearing the field clears the outputs") {
        controller.window().invoke_osc_targets_edited(slint::SharedString(""));
        CHECK(transports().outputs().empty());
        CHECK_FALSE(controller.window().get_osc_on());
    }

    SECTION("the editor is told what there is to route to") {
        // The rule editor names these, so it has to know what the names are — and has to be
        // told again when they change, or it would be checking a rule against a rig that no
        // longer exists.
        controller.editor().add();
        controller.editor().setOutputs("deck");
        CHECK(std::string(controller.editor().window().get_outputs_available()) ==
              "reaches 1 output");

        controller.window().invoke_osc_targets_edited(slint::SharedString("hall = 127.0.0.1:7002"));
        CHECK(std::string(controller.editor().window().get_outputs_available()) ==
              "no output called deck");
    }
}

TEST_CASE("a MIDI port that will not open is said out loud", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    controller.window().invoke_midi_port_picked(slint::SharedString("takt4 test - no such port"));
    CHECK(controller.outputs().transports().midiClock() == nullptr);
    CHECK(controller.statusIsError());
    CHECK(std::string(controller.window().get_status()).find("MIDI clock") != std::string::npos);
    CHECK_FALSE(controller.window().get_midi_on());
}

TEST_CASE("the window leaves the beat ring to the output thread", "[ui][hardware]") {
    // `rt::SpscRing` allows one consumer. The window used to drain beats and throw them
    // away; if it still did, it and the runner would take half each and the transports
    // would send every other beat. Nothing dropped is what says exactly one is draining.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    controller.toggleRun();
    REQUIRE(tracker.running());
    REQUIRE(controller.outputs().running());

    pumpTimers(std::chrono::milliseconds(400));
    controller.toggleRun();

    CHECK_FALSE(controller.outputs().running());
    CHECK(tracker.engine().beatsDropped() == 0);
    CHECK(controller.outputs().errors() == 0);
    // Whatever the tracker called on silence, the transports were given all of it.
    CHECK(controller.outputs().transports().beats() == tracker.engine().beatsCalled());
}

// ---------------------------------------------------------------------------------------
// Q7's two layers, from the window's side: what it restores on the way in and what it
// hands back on the way out. Where the file lives and what it looks like is
// tests/settings/settings_test.cpp's business.
// ---------------------------------------------------------------------------------------

TEST_CASE("a window with no settings comes up on the machine's own best guess", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    if (tracker.devices().empty()) {
        SKIP("no input device on this machine");
    }
    // The ranking, unchanged by settings that say nothing.
    const std::optional<InputDevice> best = bestInputDevice(tracker);
    REQUIRE(best);
    CHECK(controller.devices()[static_cast<std::size_t>(controller.deviceIndex())].name ==
          best->name);
    CHECK(controller.channelIndex() == 0);
}

TEST_CASE("the window restores the device and channel it was left on", "[ui][hardware]") {
    LiveTracker tracker(kWeights, kStateSpace);
    if (tracker.devices().empty()) {
        SKIP("no input device on this machine");
    }
    // A device with more than one input, so there is a channel worth remembering.
    std::optional<InputDevice> multi;
    for (const InputDevice& device : tracker.devices()) {
        if (device.maxInputChannels > 1) {
            multi = device;
            break;
        }
    }
    if (!multi) {
        SKIP("no device on this machine has two inputs");
    }

    takt4::settings::Settings saved;
    saved.machine.deviceName = multi->name;
    saved.machine.hostApiName = multi->hostApiName;
    saved.machine.channel = 1;

    WindowController controller(tracker, saved);
    REQUIRE(controller.deviceIndex() >= 0);
    const InputDevice& chosen =
        controller.devices()[static_cast<std::size_t>(controller.deviceIndex())];
    CHECK(chosen.name == multi->name);
    CHECK(chosen.hostApiName == multi->hostApiName);
    CHECK(controller.channelIndex() == 1);
    CHECK(controller.window().get_channel_index() == 1);
}

TEST_CASE("a remembered device that is gone falls back rather than picking wrongly", "[ui]") {
    // Restoring the wrong interface is worse than restoring none, which is why the device
    // is remembered by name and not by index.
    LiveTracker tracker(kWeights, kStateSpace);
    if (tracker.devices().empty()) {
        SKIP("no input device on this machine");
    }
    takt4::settings::Settings saved;
    saved.machine.deviceName = "An interface that was unplugged";
    saved.machine.hostApiName = "ASIO";
    saved.machine.channel = 5;

    WindowController controller(tracker, saved);
    const std::optional<InputDevice> best = bestInputDevice(tracker);
    REQUIRE(best);
    CHECK(controller.devices()[static_cast<std::size_t>(controller.deviceIndex())].name ==
          best->name);
    // And the channel of a device we did not restore is not restored either.
    CHECK(controller.channelIndex() == 0);
}

TEST_CASE("the window switches the outputs back on", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.preset.link = true;
    saved.preset.oscPrefix = "/vj";
    saved.preset.outputs = takt4::output::oscOutputs({{"127.0.0.1", 7000}, {"127.0.0.1", 7001}});

    WindowController controller(tracker, saved);
    CHECK(controller.outputs().transports().linkEnabled());
    CHECK(controller.outputs().transports().osc().targetCount() == 2);
    CHECK(controller.outputs().transports().oscPrefix() == "/vj");
    CHECK(controller.window().get_link_on());
    CHECK(controller.window().get_osc_on());
    CHECK(std::string(controller.window().get_osc_targets()).find("7001") != std::string::npos);
}

TEST_CASE("a MIDI port that has since been unplugged is reported, not fatal", "[ui]") {
    // The one restored setting that can fail. Construction must still finish: an app that
    // will not open because a MIDI cable moved is worse than one with no clock.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.machine.midiClockPort = "takt4 test - a port from another machine";

    WindowController controller(tracker, saved);
    CHECK(controller.outputs().transports().midiClock() == nullptr);
    CHECK(controller.statusIsError());
    CHECK(std::string(controller.window().get_status()).find("MIDI clock") != std::string::npos);
}

TEST_CASE("what the window hands back is what it was given", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.preset.link = true;
    saved.preset.outputs = takt4::output::oscOutputs({{"192.168.1.40", 7000}});
    saved.preset.oscPrefix = "/vj";

    WindowController controller(tracker, saved);
    // Changes made through the window are in it too.
    controller.window().invoke_fold_min_changed(90.0f);
    controller.window().invoke_latency_changed(-25.0f);
    tracker.engine().step(); // let the engine take them, as a running one would

    const takt4::settings::Settings out = controller.currentSettings();
    CHECK(out.preset.link);
    REQUIRE(out.preset.outputs.size() == 1);
    CHECK(out.preset.outputs[0].host == "192.168.1.40");
    CHECK(out.preset.oscPrefix == "/vj");
    CHECK_THAT(out.preset.tempo.minBpm, WithinAbs(90.0, 1e-6));
    CHECK_THAT(out.preset.tempo.latencyOffsetSeconds, WithinAbs(-0.025, 1e-9));
    if (!tracker.devices().empty()) {
        CHECK_FALSE(out.machine.deviceName.empty());
        CHECK_FALSE(out.machine.hostApiName.empty());
    }

    // And it survives the file, which is what the app actually does with it.
    const takt4::settings::Settings reloaded =
        takt4::settings::fromJson(takt4::settings::toJson(out));
    CHECK(reloaded.preset.link);
    CHECK_THAT(reloaded.preset.tempo.minBpm, WithinAbs(90.0, 1e-6));
    CHECK(reloaded.machine.deviceName == out.machine.deviceName);
}
