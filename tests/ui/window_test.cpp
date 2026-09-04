#include "ui/window_controller.hpp"

#include "core/audio/devices.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "ui/window_state.hpp"

#include <slint-platform.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
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
using takt4::ui::WindowController;

namespace {

const std::filesystem::path kWeights = std::filesystem::path(TAKT4_WEIGHTS_DIR) / "generic.bin";
const std::filesystem::path kStateSpace =
    std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin";

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
