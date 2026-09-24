#include "core/assets/embedded.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/rates.hpp"
#include "core/build_info.hpp"
#include "core/control/control_action.hpp"
#include "core/control/midi_binding.hpp"
#include "core/dmx/fixture.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/io/wav_file.hpp"
#include "core/output/output_target.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/rule.hpp"
#include "core/trigger/trigger_engine.hpp"
#include "ui/model_watch.hpp"
#include "ui/native_window.hpp"
#include "ui/shot.hpp"
#include "ui/window_controller.hpp"
#include "ui/window_state.hpp"

#include "support/loopback_receiver.hpp"
#include "support/scoped_env.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <slint-platform.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

using Catch::Matchers::WithinAbs;
using takt4::audio::InputDevice;
using takt4::engine::LiveTracker;
using takt4::tracking::TempoState;
using Options = takt4::tracking::TempoTracker::Options;
using takt4::tests::ModelWatch;
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
/// Runs the window as its redraw timer and event loop would — `tick`, then whatever Slint timer
/// has come due — until `done` says so, or ten seconds have passed.
template <typename Done>
void waitUntil(WindowController& controller, Done done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < until) {
        controller.tick();
        slint::platform::update_timers_and_animations();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

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

/// What the window's output thread has, read the only safe way: once it has taken every change
/// posted so far, and between two of its rounds. It runs for as long as the window exists
/// (the audit's H5), so reading its rules or transports directly would race it.
struct OutputsSeen {
    std::vector<std::string> ruleIds;
    std::vector<bool> ruleValid;
    bool link = false;
    std::size_t oscTargets = 0;
    bool midiClock = false;
    std::vector<takt4::output::OutputTarget> targets;
    std::string oscPrefix;
};

OutputsSeen seen(WindowController& controller) {
    REQUIRE(controller.settleOutputs());
    return controller.outputs().inspect([](const takt4::trigger::TriggerEngine& rules,
                                           const takt4::output::Transports& transports,
                                           const takt4::output::RuleSink&) {
        OutputsSeen out;
        for (std::size_t i = 0; i < rules.ruleCount(); ++i) {
            out.ruleIds.push_back(rules.rule(i).id());
            out.ruleValid.push_back(rules.rule(i).valid());
        }
        out.link = transports.linkEnabled();
        out.oscTargets = transports.osc().targetCount();
        out.midiClock = transports.midiClock() != nullptr;
        out.targets = transports.outputs();
        out.oscPrefix = transports.oscPrefix();
        return out;
    });
}

/// Whether PANIC is engaged once the output thread has taken what was posted — a gesture posts
/// it, and the thread applies it a round later.
bool panickedNow(WindowController& controller) {
    REQUIRE(controller.settleOutputs());
    return controller.outputs().panicked();
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
    state.calledBpm = 128.25; // a filter calling beats an octave below its cloud
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
    CHECK_THAT(window->get_called_bpm(), WithinAbs(128.25, 1e-4));
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

TEST_CASE("the build's version is on screen and stays there", "[ui]") {
    // It used to be said once, in the opening status line, and the first status after it
    // took it away — so "which build am I looking at?" was unanswerable from the window
    // exactly when somebody had reason to ask, which is while it is running.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    const std::string version(controller.window().get_version());
    CHECK(version == takt4::versionLabel(takt4::buildInfo()));
    CHECK_FALSE(version.empty());
    // A version, not a name: the title composes the two and the status bar shows this on
    // its own, so anything else here would read as "takt4 takt4" in the title bar.
    CHECK(version.find("takt4") == std::string::npos);

    // Whatever the status becomes, the version is not what is spent to say it — they are
    // two properties now, and that is the whole of the fix.
    controller.window().set_status(slint::SharedString("outputs: OSC 127.0.0.1:7000"));
    CHECK(std::string(controller.window().get_version()) == version);
    CHECK(std::string(controller.window().get_status()).find(version) == std::string::npos);
}

TEST_CASE("a short window scrolls rather than losing its bottom row", "[ui]") {
    // Measured, not reasoned about: the window is rendered at each height and the pixels
    // at the bottom of it are read.
    //
    // 0.9.4 shipped with this broken. At the size the app opens at — 1000x900, see
    // `kMainWindowHeight` — with four outputs configured, the status bar was not on screen
    // at all and PANIC was cut in half, because the layout had nowhere to put them and
    // nothing to scroll. Every height below 1105 or so lost something, and a 1366x768
    // laptop could not show the window at all.
    //
    // What is asserted is what an operator would look for: the status bar is the bottom of
    // the window, and PANIC is a whole button. Both are pinned below the scroll view now,
    // so both hold at every height down to the window's own floor.
    auto window = MainWindow::create();
    window->set_version(slint::SharedString("0.0.0-test"));
    window->set_status(slint::SharedString("In 7 of MOTU Pro Audio"));

    // Four of them, which is what made the content taller than the window. A rig with one
    // output fits at 900 and would not have found this.
    auto targets = std::make_shared<slint::VectorModel<OutputRow>>();
    for (const char* name : {"deck", "wall", "robot", "lights"}) {
        OutputRow row{};
        row.name = slint::SharedString(name);
        row.host = slint::SharedString("192.168.1.40");
        row.port = slint::SharedString("7000");
        row.enabled = true;
        targets->push_back(row);
    }
    window->set_outputs_list(targets);

    // Theme.line, Theme.panel and Theme.control, from theme.slint.
    constexpr std::uint8_t kLine[3] = {0x2b, 0x2f, 0x36};
    constexpr std::uint8_t kPanel[3] = {0x19, 0x1b, 0x1f};
    constexpr std::uint8_t kControl[3] = {0x2b, 0x31, 0x3a};

    // 420 is the window's own `min-height`; 1200 is taller than the content needs, which is
    // the case that must keep working exactly as it did before there was a scroll view.
    for (const int height : {1200, 900, 800, 700, 600, 500, 420}) {
        CAPTURE(height);
        const takt4::tests::Shot shot = takt4::tests::render(*window, 1000, height);

        // The status bar is 34px of Theme.panel with the 1px separator above it. Read at
        // x=8, which is inside the bar's left padding and so clear of the status text.
        CHECK(shot.is(8, height - 35, kLine[0], kLine[1], kLine[2]));
        for (int y = height - 34; y < height; ++y) {
            CAPTURE(y);
            REQUIRE(shot.is(8, y, kPanel[0], kPanel[1], kPanel[2]));
        }

        // And PANIC is a whole button: 40px tall less its 1px border, in a column inside
        // its right-hand end and clear of the label. Cut in half, this read 30.
        int face = 0;
        int y = height - 36;
        while (y > 0 && !shot.is(966, y, kControl[0], kControl[1], kControl[2])) {
            --y;
        }
        while (y > 0 && shot.is(966, y, kControl[0], kControl[1], kControl[2])) {
            ++face;
            --y;
        }
        CHECK(face == 38);
    }
}

TEST_CASE("a mixed rig's output rows fit the window at its narrowest", "[ui]") {
    // Rendered, not reasoned about. At the window's minimum width, with an Art-Net node in
    // the rig, a row used to need 870 px: the text boxes' Fluent minimum of 160 px held the
    // destination slot wider than its floor, the port box slid under the delay slider, and
    // each row's "−" was clipped off the right-hand edge. Every row's "−" is looked for, whole.
    auto window = MainWindow::create();
    auto targets = std::make_shared<slint::VectorModel<OutputRow>>();
    const auto row = [](const char* name, int kind, const char* host, const char* port) {
        OutputRow out{};
        out.name = slint::SharedString(name);
        out.kind_index = kind;
        out.host = slint::SharedString(host);
        out.port = slint::SharedString(port);
        out.enabled = true;
        return out;
    };
    targets->push_back(row("deck", 0, "192.168.1.40", "7000"));
    targets->push_back(row("wall", 0, "192.168.1.41", "7000"));
    targets->push_back(row("lights", 1, "", ""));
    targets->push_back(row("truss", 2, "10.0.0.20", "6454"));
    window->set_outputs_list(targets);
    window->set_outputs_any_artnet(true);

    constexpr int kWidth = 820; // MainWindow's min-width
    constexpr int kHeight = 1100; // tall enough that nothing scrolls
    const takt4::tests::Shot shot = takt4::tests::render(*window, kWidth, kHeight);
    constexpr std::uint8_t kControl[3] = {0x2b, 0x31, 0x3a};
    // Down a column just inside the "−" buttons' right-hand end, below the trace and above the
    // footer — the same way the wheel test finds rows.
    const int x = kWidth - 34;
    std::vector<std::pair<int, int>> rows; // top, bottom of each "−"
    int start = -1;
    for (int y = 450; y < kHeight - 110; ++y) {
        const bool face = shot.is(x, y, kControl[0], kControl[1], kControl[2]);
        if (face && start < 0) {
            start = y;
        } else if (!face && start >= 0) {
            if (y - start > 20) {
                rows.emplace_back(start, y - 1);
            }
            start = -1;
        }
    }
    // Four rows, four "−" buttons, all whole; ADD OUTPUT and the rest sit further left.
    REQUIRE(rows.size() == 4);

    // **And every heading stands over the column it names** — the markup's own promise, and
    // the part a row that merely fits can still break: with a text box's Fluent minimum
    // holding the destination slot open, the row fitted and the port box slid right, under
    // "universes". Everything on this panel that is not the panel's own colour is ink: a
    // text box or a dropdown is one solid run of it along a row's middle, and a heading is a
    // run of glyphs. So the columns' left edges can be read off both and compared.
    constexpr std::uint8_t kPanel[3] = {0x19, 0x1b, 0x1f};
    const auto runsAlong = [&](int y, int mergeGap) {
        std::vector<int> starts;
        int lastInk = -1000;
        for (int px = 140; px < kWidth - 60; ++px) {
            if (shot.is(px, y, kPanel[0], kPanel[1], kPanel[2])) {
                continue;
            }
            if (px - lastInk > mergeGap) {
                starts.push_back(px);
            }
            lastInk = px;
        }
        return starts;
    };
    // The headings' text line: the densest line of ink in the strip above the first row.
    int headingY = rows.front().first - 18;
    std::size_t densest = 0;
    for (int y = rows.front().first - 24; y < rows.front().first - 2; ++y) {
        std::size_t ink = 0;
        for (int px = 140; px < kWidth - 60; ++px) {
            ink += shot.is(px, y, kPanel[0], kPanel[1], kPanel[2]) ? 0U : 1U;
        }
        if (ink > densest) {
            densest = ink;
            headingY = y;
        }
    }
    // Letters and the spaces inside "host / device" are a few pixels apart; columns are ten.
    const std::vector<int> headings = runsAlong(headingY, 7);
    // name, protocol, host / device, port, universes, this target's own delay, total
    REQUIRE(headings.size() >= 5);
    const auto checkRow = [&](std::size_t index, std::size_t columns) {
        const int middle = (rows[index].first + rows[index].second) / 2;
        const std::vector<int> boxes = runsAlong(middle, 3);
        INFO("row " << index << " at y=" << middle);
        REQUIRE(boxes.size() >= columns);
        for (std::size_t c = 0; c < columns; ++c) {
            INFO("column " << c << ": heading at x=" << headings[c] << ", box at x=" << boxes[c]);
            CHECK(std::abs(boxes[c] - headings[c]) <= 3);
        }
    };
    checkRow(0, 4); // an OSC row: name, protocol, host, port
    checkRow(3, 5); // the Art-Net row, with its universe list
}

TEST_CASE("the wheel moves the body and leaves PANIC where it is", "[ui]") {
    // The other half of the scroll view, and the half that a picture of one height cannot
    // show: that the wheel actually scrolls, that what is pinned stays pinned, and — the
    // one most likely to break quietly — that a click after scrolling lands on the row that
    // is now under the pointer rather than the one that used to be there.
    auto window = MainWindow::create();
    window->set_status(slint::SharedString("scrolling"));

    auto targets = std::make_shared<slint::VectorModel<OutputRow>>();
    for (const char* name : {"deck", "wall", "robot", "lights"}) {
        OutputRow row{};
        row.name = slint::SharedString(name);
        row.host = slint::SharedString("192.168.1.40");
        row.port = slint::SharedString("7000");
        row.enabled = true;
        targets->push_back(row);
    }
    window->set_outputs_list(targets);

    std::vector<int> removed;
    window->on_output_removed([&removed](int index) { removed.push_back(index); });
    int panics = 0;
    window->on_panic_clicked([&panics] { ++panics; });

    constexpr int kWidth = 1000;
    constexpr int kHeight = 760;
    // Inside the "−" button at the end of each target row, ten pixels short of its right
    // edge: the middle of that button is where the "−" is drawn, which splits the run.
    constexpr int kRemoveX = 966;
    constexpr std::uint8_t kControl[3] = {0x2b, 0x31, 0x3a};
    // The pinned footer: a line, the 60px triggers row, a line, and the 34px status bar.
    constexpr int kFooter = 96;

    // The runs of button face down a column, which is how a row is found without hardcoding
    // a y that a font change would move. Bounded below the trace and above the footer, so
    // neither START at the top nor PANIC at the bottom is mistaken for a target row.
    const auto rowsDown = [&](const takt4::tests::Shot& shot) {
        std::vector<std::pair<int, int>> spans; // first row, last row
        int start = -1;
        for (int y = 400; y < shot.height - kFooter; ++y) {
            const bool face = shot.is(kRemoveX, y, kControl[0], kControl[1], kControl[2]);
            if (face && start < 0) {
                start = y;
            } else if (!face && start >= 0) {
                if (y - start > 20) {
                    spans.emplace_back(start, y - 1);
                }
                start = -1;
            }
        }
        return spans;
    };

    const takt4::tests::Shot before = takt4::tests::render(*window, kWidth, kHeight);
    const auto rowsBefore = rowsDown(before);
    REQUIRE(!rowsBefore.empty());

    // Clicking the first one takes target 0 away — the baseline, before anything scrolls.
    const int firstY = (rowsBefore.front().first + rowsBefore.front().second) / 2;
    const slint::LogicalPosition first({static_cast<float>(kRemoveX), static_cast<float>(firstY)});
    window->window().dispatch_pointer_move_event(first);
    window->window().dispatch_pointer_press_event(first, slint::PointerEventButton::Left);
    window->window().dispatch_pointer_release_event(first, slint::PointerEventButton::Left);
    REQUIRE(removed == std::vector<int>{0});
    removed.clear();

    // Now the wheel, over the middle of the body. Slint animates a wheel scroll, so the
    // timers have to run for it to land.
    window->window().dispatch_pointer_scroll_event(slint::LogicalPosition({500.0f, 300.0f}), 0.0f,
                                                   -120.0f);
    pumpTimers(std::chrono::milliseconds(250));
    const takt4::tests::Shot after = takt4::tests::render(*window, kWidth, kHeight);

    // The body moved.
    const auto rowsAfter = rowsDown(after);
    REQUIRE(!rowsAfter.empty());
    const int moved = rowsBefore.front().first - rowsAfter.front().first;
    CAPTURE(rowsBefore.front().first, rowsAfter.front().first);
    CHECK(moved > 0);

    // PANIC did not. It is below the scroll view, so every pixel of the footer — the
    // triggers row, the status bar and the two separators — is exactly where it was.
    // Reported as one result naming the first pixel that moved, rather than as forty
    // thousand assertions.
    std::string firstMoved;
    for (int y = kHeight - kFooter; y < kHeight && firstMoved.empty(); ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const slint::Rgb8Pixel a = before.at(x, y);
            const slint::Rgb8Pixel b = after.at(x, y);
            if (a.r != b.r || a.g != b.g || a.b != b.b) {
                firstMoved = "the footer moved at " + std::to_string(x) + "," + std::to_string(y);
                break;
            }
        }
    }
    CHECK(firstMoved.empty());
    const slint::LogicalPosition panic(
        {static_cast<float>(kRemoveX), static_cast<float>(kHeight - 65)});
    window->window().dispatch_pointer_move_event(panic);
    window->window().dispatch_pointer_press_event(panic, slint::PointerEventButton::Left);
    window->window().dispatch_pointer_release_event(panic, slint::PointerEventButton::Left);
    CHECK(panics == 1);

    // And the click follows the scroll: the row now at the top of the column is the one
    // that answers, at its new position and not its old one.
    const int nowY = (rowsAfter.front().first + rowsAfter.front().second) / 2;
    const slint::LogicalPosition now({static_cast<float>(kRemoveX), static_cast<float>(nowY)});
    window->window().dispatch_pointer_move_event(now);
    window->window().dispatch_pointer_press_event(now, slint::PointerEventButton::Left);
    window->window().dispatch_pointer_release_event(now, slint::PointerEventButton::Left);
    REQUIRE(removed.size() == 1);
    CHECK(removed.front() >= 0);
    CHECK(removed.front() <= 3);
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
    // What this cannot reach is the one line of markup that binds the Button's `clicked` to
    // this callback. Finding an element *by name* needs Slint's testing API, which is behind
    // SLINT_FEATURE_EXPERIMENTAL and off in this build; the button is named `run-button` in
    // main_window.slint so the test can be written the day that changes.
    //
    // Real gestures are a different matter and are not behind that flag: `dispatch_pointer_*`
    // and `dispatch_key_*` work against the headless platform, and two tests here use them —
    // the splitter drags, and rules_test.cpp's "what was typed is kept when the operator
    // clicks away" clicks into a box, types, and tabs out. What they cost is coordinates, so
    // they suit a control worth the trouble rather than every one.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    REQUIRE_FALSE(tracker.running());

    // The button says it has been pressed before the driver takes the thread, and the press is
    // carried out a frame later (the audit's M23).
    controller.window().invoke_toggle_run();
    CHECK(controller.window().get_run_busy());
    CHECK(std::string(controller.window().get_run_busy_text()) == "OPENING\xE2\x80\xA6");
    CHECK_FALSE(tracker.running());
    waitUntil(controller, [&tracker] { return tracker.running(); });
    REQUIRE(tracker.running());
    CHECK(controller.window().get_running());
    CHECK(std::string(controller.window().get_run_busy_text()).empty());
    CHECK_FALSE(controller.statusIsError());
    // The status line stops offering and starts reporting: it names the channel now.
    CHECK(std::string(controller.window().get_status()).find(" of ") != std::string::npos);

    // A press straight after — the way one made while the driver held the thread arrives — is
    // not taken for STOP, even after the redraw timer has run. A moment later the button is the
    // operator's again.
    controller.tick();
    controller.window().invoke_toggle_run();
    waitUntil(controller, [&controller] { return !controller.window().get_run_busy(); });
    CHECK(tracker.running());
    CHECK_FALSE(controller.window().get_run_busy());

    controller.window().invoke_toggle_run();
    CHECK(std::string(controller.window().get_run_busy_text()) == "STOPPING\xE2\x80\xA6");
    waitUntil(controller, [&tracker] { return !tracker.running(); });
    CHECK_FALSE(tracker.running());
    CHECK_FALSE(controller.window().get_running());
    // Stopping clears the readouts rather than freezing the last tempo on screen.
    CHECK(controller.window().get_bpm() == 0.0f);
    CHECK(controller.window().get_beats_per_bar() == 0);
}

TEST_CASE("a window closed with the tracker running stops it first", "[ui][hardware]") {
    // The window hands the tracker Link's clock, and that clock belongs to the window's output
    // thread. A tracker still running once the window has gone stamps every hop through it —
    // a read of freed memory on the audio thread, fifty times a second, until the tracker goes
    // too. `ui::run` stops the tracker before the window; a test that failed between its START
    // and its STOP did not, and neither does anything else that forgets to.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    {
        WindowController controller(tracker);
        controller.toggleRun();
        REQUIRE(tracker.running());
        REQUIRE(tracker.hostTimeSource() != nullptr);
    }
    CHECK_FALSE(tracker.running());
    CHECK(tracker.hostTimeSource() == nullptr);
    // And long enough that the audio thread would have run a few hops through the pointer if
    // it were still going — which, under AddressSanitizer, is a report rather than a pass.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    CHECK_FALSE(panickedNow(controller));

    // And now it is the panic button — with the tracker stopped, which is exactly what an
    // operator arming a rig before a set does, and which since the audit's H5 reaches a runner
    // that is running.
    CHECK(controller.control().dispatch(pad));
    CHECK(panickedNow(controller));

    SECTION("and the OSC socket moves the same latch the pad just moved") {
        // §5.7's other surface, wired into the window on 2026-09-06. The pad above left
        // panic engaged, and this releases it *from the socket* — which is the whole claim:
        // one runner, two surfaces, one latch. Two runners would show up here as a release
        // that does nothing.
        //
        // Driven through `dispatch` rather than a real datagram: the socket has its own
        // tests in tests/control, and nothing here needs one to exist.
        REQUIRE(panickedNow(controller));
        CHECK(controller.oscControl().dispatch("/takt4/ctl/panic", 0.0));
        CHECK_FALSE(panickedNow(controller));

        // And a bare `/ctl/panic` engages, because a panic button panics.
        CHECK(controller.oscControl().dispatch("/takt4/ctl/panic", std::nullopt));
        CHECK(panickedNow(controller));
    }

    SECTION("the picker offers panic but not the ones that would need a rule named") {
        // §5.7's `rule/<id>/…` verbs cannot be armed from a gesture: pressing a pad says
        // which button, never which rule. The list is the actions minus those.
        //
        // Counted from `takesRuleId` rather than written down as "one fewer", which is what
        // this was: there was one such verb, and when mute, double, halve, rate and reset
        // arrived the number silently became six. A test that knows *why* an action is left
        // out does not need editing when another one is.
        std::size_t armable = 0;
        for (const takt4::control::ControlAction action : takt4::control::kControlActions) {
            if (!takt4::control::takesRuleId(action)) {
                ++armable;
            }
        }
        CHECK(controller.window().get_learn_actions()->row_count() == armable);
        CHECK(armable > 0);
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

    // And the output thread's, which is the live one — read between its rounds, since it is
    // running from the moment the window is built.
    const OutputsSeen live = seen(controller);
    REQUIRE(live.ruleIds.size() == 1);
    CHECK(live.ruleValid[0]);
    // A valid rule says nothing — but only the *rule's* silence is being claimed here. On a
    // machine with no audio input the controller has already set an error of its own at
    // construction ("No input device. Connect an interface and start takt4 again."), which is
    // about the hardware and not about the preset this test loaded. Asserting into that was a
    // false failure on any headless machine, and it went unseen until 2026-09-14 because CI
    // had not reached the test step since the test was written: every run before then stopped
    // at the billing gate. The rest of the test is device-independent and still runs, so this
    // is narrowed rather than skipped.
    if (!tracker.devices().empty()) {
        CHECK_FALSE(controller.statusIsError());
    }

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
        const OutputsSeen after = seen(controller);
        REQUIRE(after.ruleIds.size() == 1);
        CHECK(after.ruleIds[0] == "stab");
    }

    SECTION("a rule that will not fire is kept, and said so") {
        // §5.8's policy: held, shown, and refused at fire time. The alternative — dropping
        // it on load — would delete the rule an operator is half-way through fixing.
        Rule::Config broken;
        broken.id = "broken";
        broken.address = "/a/{x}/b"; // one placeholder, no segments
        controller.setRules({broken});
        CHECK(controller.rules().size() == 1);
        const OutputsSeen after = seen(controller);
        REQUIRE(after.ruleValid.size() == 1);
        CHECK_FALSE(after.ruleValid[0]);
        CHECK(controller.statusIsError());
    }
}

TEST_CASE("the OSC control socket opens only when asked, and is remembered", "[ui][network]") {
    // §5.7's listening socket, which nothing opened until 2026-09-06 — the library existed
    // and no application built one. Three separate decisions live here and each is the
    // operator's: listen at all, on which port, and whether past this machine.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    // Off on a fresh window. A socket is not something to open on somebody's behalf.
    CHECK_FALSE(controller.oscControl().running());
    CHECK_FALSE(controller.window().get_osc_control_on());
    CHECK(controller.oscControlPort() == 0); // nothing bound, so no port to report

    SECTION("a port being typed is not overwritten by the redraw timer") {
        // The same fault as the trigger editor's boxes, in the one text field on the main
        // window: `text <=> root.osc-control-port` makes the property *be* the box, and
        // `publishControl` runs on the redraw timer. Writing the current port back
        // unconditionally erased every digit before the next could be typed, and the field
        // commits on Enter — so a different port could not be reached at all.
        //
        // Typing is simulated by writing the property, which is exactly what the widget
        // does: a two-way binding is one property with two writers.
        const std::string before(controller.window().get_osc_control_port());
        controller.window().set_osc_control_port(slint::SharedString("76"));
        pumpTimers(std::chrono::milliseconds(250)); // several ticks
        CHECK(std::string(controller.window().get_osc_control_port()) == "76");

        // ...and pressing Enter still commits it, error path included: a port that will not
        // do is put back rather than left sitting there looking accepted.
        controller.window().invoke_osc_control_port_edited(slint::SharedString("99999"));
        CHECK(controller.statusIsError());
        CHECK(std::string(controller.window().get_osc_control_port()) == before);

        // And one that will do is taken, which is the whole point of being able to type it.
        controller.window().invoke_osc_control_port_edited(slint::SharedString("7005"));
        CHECK(std::string(controller.window().get_osc_control_port()) == "7005");
        pumpTimers(std::chrono::milliseconds(120));
        CHECK(std::string(controller.window().get_osc_control_port()) == "7005");
    }

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

    SECTION("a new port that will not bind leaves it listening on the old one") {
        // The audit's M24: the working socket was closed first and the new number tried after,
        // so a typo — or a port another program has — left the Stream Deck with nobody
        // listening, mid-show. A fixed port rather than 0 here, since 0 rebound would be a
        // different "any free one" and the test could not tell where it came back.
        std::uint16_t free = 0;
        {
            const takt4::testing::LoopbackReceiver probe;
            free = probe.port();
        }
        controller.window().invoke_osc_control_port_edited(slint::SharedString(std::to_string(free)));
        controller.window().invoke_osc_control_toggled(true);
        REQUIRE(controller.oscControl().running());
        REQUIRE(controller.oscControlPort() == free);

        const takt4::testing::LoopbackReceiver taken; // something else is on this one
        controller.window().invoke_osc_control_port_edited(
            slint::SharedString(std::to_string(taken.port())));
        CHECK(controller.oscControl().running());
        CHECK(controller.oscControlPort() == free);
        CHECK(controller.statusIsError());
        const std::string status(controller.window().get_status());
        INFO(status);
        CHECK(status.find("Still listening on " + std::to_string(free)) != std::string::npos);
        CHECK(std::string(controller.window().get_osc_control_port()) == std::to_string(free));
    }

    SECTION("a port that was busy at launch is still asked for next time") {
        // The other half of M24. Saving `running()` turned a port that was only briefly busy —
        // another copy of takt4 still closing — into OSC control switched off for good, with
        // nothing at the next launch to say so.
        const takt4::testing::LoopbackReceiver taken;
        takt4::settings::Settings settings;
        settings.machine.oscControlEnabled = true;
        settings.machine.oscControlPort = taken.port();
        // And a control surface left at home, so the launch meets two problems: both are said
        // (the audit's M25) — each used to write the one before it away.
        settings.machine.midiControlPort = "takt4 test - a control surface left at home";
        LiveTracker second(kWeights, kStateSpace);
        WindowController busy(second, settings);
        CHECK_FALSE(busy.oscControl().running());
        CHECK(busy.statusIsError());
        CHECK(busy.currentSettings().machine.oscControlEnabled);
        const std::string status(busy.window().get_status());
        INFO(status);
        CHECK(status.find("OSC control") != std::string::npos);
        CHECK(status.find("a control surface left at home") != std::string::npos);

        // And a new number is tried at once, since it is still wanted.
        std::uint16_t free = 0;
        {
            const takt4::testing::LoopbackReceiver probe;
            free = probe.port();
        }
        busy.window().invoke_osc_control_port_edited(slint::SharedString(std::to_string(free)));
        CHECK(busy.oscControl().running());
        CHECK(busy.oscControlPort() == free);
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

    controller.window().invoke_snap_downbeat();
    run.applyPosted();

    // The press names the beat just called, so the bar moves on the press itself and no
    // audio is needed for it. What it then does to the bar afterwards is
    // tests/engine/beat_engine_test.cpp's business.
    CHECK(tracker.engine().state().beatInBar == 1);

    // The next beat is the bar's second. Read from the engine's published state, not by
    // draining its beat ring: that ring has one consumer, the window's output thread, which
    // runs for as long as the window does (the audit's H5) — a test taking beats off it too
    // would be a second consumer, and each would see half of them.
    const std::uint64_t pressedAt = tracker.engine().state().beats;
    REQUIRE(run.until([&] { return tracker.engine().state().beats > pressedAt; }));
    CHECK(tracker.engine().state().beats == pressedAt + 1);
    CHECK(tracker.engine().state().beatInBar == 2);
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

TEST_CASE("the latency slider moves the outputs and not only the tracker", "[ui]") {
    // The audit's C6. The slider posted to the tracker, which applies the offset only to a
    // timestamp nothing in the app reads; the transports took theirs once, at construction. So
    // dragging it moved nothing on Link, the MIDI clock or OSC that night, and the value saved
    // took effect at the *next* launch.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    REQUIRE(controller.outputs().transports().latencySeconds() == 0.0);

    controller.window().invoke_latency_changed(-40.0f);
    CHECK_THAT(controller.outputs().transports().latencySeconds(), WithinAbs(-0.040, 1e-9));
    controller.window().invoke_latency_changed(25.0f);
    CHECK_THAT(controller.outputs().transports().latencySeconds(), WithinAbs(0.025, 1e-9));
}

TEST_CASE("keep for the next track reaches the tracker and the settings file", "[ui]") {
    // The audit's H2 setting, through the controller. The click on the box itself is tested
    // below with the other gestures.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);
    REQUIRE_FALSE(tracker.engine().tempoOptions().keepOctaveShift);
    REQUIRE_FALSE(controller.window().get_keep_shift());

    controller.window().invoke_keep_shift_changed(true);
    run.applyPosted();
    CHECK(tracker.engine().tempoOptions().keepOctaveShift);
    CHECK(controller.window().get_keep_shift());
    controller.window().invoke_keep_shift_changed(false);
    run.applyPosted();
    CHECK_FALSE(tracker.engine().tempoOptions().keepOctaveShift);

    SECTION("and it is saved with the rest of the tempo settings") {
        controller.window().invoke_keep_shift_changed(true);
        run.applyPosted();
        CHECK(controller.currentSettings().preset.tempo.keepOctaveShift);
    }
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

    // Running — the outputs live as long as the window does (the audit's H5) — and with
    // nothing to send, and the tracker not listening.
    CHECK(controller.outputs().running());
    CHECK_FALSE(controller.outputs().tracking());
    const OutputsSeen live = seen(controller);
    CHECK_FALSE(live.link);
    CHECK(live.oscTargets == 0);
    CHECK_FALSE(live.midiClock);
    CHECK_FALSE(controller.window().get_link_on());
    CHECK_FALSE(controller.window().get_osc_on());
    CHECK_FALSE(controller.window().get_midi_on());

    // The picker always offers "none" first, so switching MIDI off is a choice in the
    // same list rather than a second control — and it says so in words rather than being an
    // empty entry, which reads as a box the application failed to fill in.
    const auto ports = controller.window().get_midi_ports();
    REQUIRE(ports);
    REQUIRE(ports->row_count() == controller.midiPorts().size() + 1);
    CHECK(std::string(*ports->row_data(0)) == "no MIDI clock");
    CHECK(controller.window().get_midi_port_index() == 0);
}

TEST_CASE("the Link tick reaches the transports and shows its peers", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    controller.window().invoke_link_toggled(true);
    CHECK(seen(controller).link);
    CHECK(controller.window().get_link_on());
    // **Joined on its own switch, whether or not the tracker is listening** — the operator's
    // call of 2026-09-23 (Q2). Joining publishes nothing: takt4 sends Link a tempo and a phase
    // only from a locked beat, so a peer is not handed anything until there is one.
    CHECK(controller.outputs().transports().link().enabled());
    CHECK_FALSE(controller.outputs().tracking());

    controller.window().invoke_link_toggled(false);
    CHECK_FALSE(seen(controller).link);
    CHECK_FALSE(controller.outputs().transports().link().enabled());
}

TEST_CASE("outputs are parsed, and a bad one does not lose the good ones", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    // What the output thread has, read between its rounds once it has taken the change.
    const auto live = [&controller] { return seen(controller); };

    // The format this row always took, before targets had names — still a target, named
    // after its own address.
    controller.setOscTargets("127.0.0.1:57000");
    CHECK(live().oscTargets == 1);
    REQUIRE(live().targets.size() == 1);
    CHECK(live().targets[0].name == "127.0.0.1:57000");
    CHECK(controller.window().get_osc_on());

    // §5.6's "multiple simultaneous targets", each with a name a rule can use. One piece of
    // text still holds several — a settings line, or a rig pasted in.
    controller.setOscTargets("deck = 127.0.0.1:57000, wall = 127.0.0.1:57001");
    CHECK(live().oscTargets == 2);
    REQUIRE(live().targets.size() == 2);
    CHECK(live().targets[0].name == "deck");
    CHECK(live().targets[1].name == "wall");

    SECTION("an Art-Net row's universes are read whole, and what is not one is said") {
        // The audit's M21. The box read a leading number and ignored the rest: `1:0:0` — the
        // universe a node's panel calls 1:0:0, which is 256 — went to universe 1, `2-4` to 2
        // alone, and a word to nothing, without a word.
        controller.setTargetKind(0, static_cast<int>(takt4::output::OutputTarget::Kind::ArtNet));
        controller.setTargetHost(0, "192.0.2.10", false);
        controller.setTargetUniverses(0, "1:0:0, 2-4, lights, 0-9999", true);
        REQUIRE(live().targets.size() == 2);
        CHECK(live().targets[0].universes == std::vector<std::uint16_t>{256, 2, 3, 4});
        CHECK(controller.statusIsError());
        const std::string status(controller.window().get_status());
        INFO(status);
        CHECK(status.find("\"lights\"") != std::string::npos);
        CHECK(status.find("\"0-9999\"") != std::string::npos);
        // And a destination arriving whole — a pasted line, a settings file — fills the box the
        // way a node spells it, which the box reads back.
        controller.setOscTargets("node = artnet 192.0.2.10:6454 u256,3");
        CHECK(std::string(controller.window().get_outputs_list()->row_data(0)->universes) ==
              "1:0:0, 3");
    }

    SECTION("a node fed several universes is one target, not one per comma") {
        // Found while fixing M21: a row's address is split at commas into targets, and the
        // commas of `u0,1,4` split one Art-Net target into pieces that would not parse, so the
        // whole row was refused and a node fed more than one universe was never sent anything.
        controller.setOscTargets("node = artnet 192.0.2.10:6454 u0,1,4 +50ms, wall = 127.0.0.1:57001");
        REQUIRE(live().targets.size() == 2);
        CHECK(live().targets[0].name == "node");
        CHECK(live().targets[0].universes == std::vector<std::uint16_t>{0, 1, 4});
        CHECK(live().targets[0].delaySeconds == Catch::Approx(0.05));
        CHECK(live().targets[1].name == "wall");
        CHECK_FALSE(controller.statusIsError());
    }

    SECTION("and a line holding several becomes a row each, name and address apart") {
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 2);
        CHECK(std::string(rows->row_data(0)->name) == "deck");
        CHECK(std::string(rows->row_data(0)->address) == "127.0.0.1:57000");
        CHECK(std::string(rows->row_data(1)->name) == "wall");
        CHECK(std::string(rows->row_data(1)->address) == "127.0.0.1:57001");
    }

    SECTION("a target named after its own address leaves the name box empty") {
        // It is still called "127.0.0.1:57000" and a rule can still route to it by that; the
        // box the operator types a *name* into is not where to say so.
        controller.setOscTargets("127.0.0.1:57000");
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 1);
        CHECK(std::string(rows->row_data(0)->name).empty());
        CHECK(live().targets[0].name == "127.0.0.1:57000");
    }

    SECTION("each target carries its own delay, and only its own") {
        // §5.6's per-output latency: the user's ask of 2026-09-07 — *"robot has latency so I
        // need to offset it half a beat or somethin"*. §5.5's single slider moves the whole
        // rig's timeline together, which is the one adjustment a rig with two different lags
        // in it cannot use.
        controller.setTargetDelay(1, 352.0f);
        REQUIRE(live().targets.size() == 2);
        CHECK(live().targets[0].delaySeconds == 0.0);
        CHECK(live().targets[1].delaySeconds == Catch::Approx(0.352));

        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 2);
        CHECK(rows->row_data(0)->delay_ms == Catch::Approx(0.0f));
        CHECK(rows->row_data(1)->delay_ms == Catch::Approx(352.0f));

        // The address box never shows it — the slider is where it lives, and a delay
        // appearing in the text an operator is typing into would be edited by accident.
        CHECK(std::string(rows->row_data(1)->address) == "127.0.0.1:57001");

        // Past either limit is clamped rather than refused: a slider cannot get there, but
        // §5.7's inbound OSC and a hand-edited settings file both can.
        controller.setTargetDelay(1, 5000.0f);
        CHECK(live().targets[1].delaySeconds ==
              Catch::Approx(takt4::output::kMaxOutputDelaySeconds));
        controller.setTargetDelay(1, -5000.0f);
        CHECK(live().targets[1].delaySeconds ==
              Catch::Approx(takt4::output::kMinOutputDelaySeconds));

        // Negative is a real setting now, not a clamp to zero: "this device is 300 ms slow"
        // is the sentence an operator says, and the publisher turns it into a wait.
        controller.setTargetDelay(1, -300.0f);
        CHECK(live().targets[1].delaySeconds == Catch::Approx(-0.300));
        CHECK(rows->row_data(1)->delay_ms == Catch::Approx(-300.0f));

        // And it survives the rest of the row being edited, which is what would break if the
        // delay were carried in the address text rather than beside it.
        controller.setTargetDelay(1, 120.0f);
        controller.setTargetEnabled(1, false);
        controller.setTargetEnabled(1, true);
        CHECK(live().targets[1].delaySeconds == Catch::Approx(0.12));
    }

    SECTION("dragging a delay slider does not rebuild the slider being dragged") {
        // The same fault as the trigger editor's text boxes, in the control shipped to fix
        // the robot's latency. `setTargetDelay` applies as it moves so the offset can be
        // found by ear, and applying republished the rows — which reset the model, which made
        // the repeater destroy and rebuild the row, which took the slider out from under the
        // pointer. The drag would have ended on its first pixel of movement.
        const auto rows = controller.window().get_outputs_list();
        const auto watch = std::make_shared<ModelWatch>();
        rows->attach_peer(watch);

        // One drag, sixty steps of it.
        for (int step = 0; step <= 60; ++step) {
            controller.setTargetDelay(1, static_cast<float>(step) * 5.0f);
        }
        CHECK(watch->resets == 0);
        CHECK(watch->added == 0);
        CHECK(watch->removed == 0);
        // One row written per step, and never the row that was not being dragged.
        CHECK(watch->changes == 60);
        CHECK(rows->row_data(1)->delay_ms == Catch::Approx(300.0f));
        CHECK(rows->row_data(0)->delay_ms == Catch::Approx(0.0f));

        // A step that lands where the slider already is writes nothing at all, so a slider
        // resending its own position cannot churn the row it lives in.
        const int settled = watch->changes;
        controller.setTargetDelay(1, 300.0f);
        CHECK(watch->changes == settled);
    }

    SECTION("a switched-off target is kept and sends nothing") {
        controller.setTargetEnabled(1, false);
        REQUIRE(live().targets.size() == 2);
        CHECK_FALSE(live().targets[1].enabled);
        // Held in the list, so it can be switched back on — but no socket behind it.
        CHECK(live().oscTargets == 1);
        CHECK_FALSE(controller.window().get_outputs_list()->row_data(1)->enabled);

        controller.setTargetEnabled(1, true);
        CHECK(live().oscTargets == 2);
    }

    SECTION("two targets with one name is said rather than silently resolved") {
        // `resolveOutputs` would take the first, and a rule routed to the second would go
        // somewhere its operator did not choose.
        controller.acceptTarget(1, "deck", "127.0.0.1:57001");
        CHECK(controller.statusIsError());
        // And said as what it is: both addresses are fine, and calling one of them "not a
        // target" would send somebody looking at the wrong thing.
        const std::string status(controller.window().get_status());
        INFO(status);
        CHECK(status.find("two are called") != std::string::npos);
    }

    SECTION("halfway through typing, the ones that already worked survive") {
        controller.acceptTarget(1, "wall", "127.0.0.1:");
        CHECK(live().oscTargets == 1);
        CHECK(controller.statusIsError());
        CHECK(std::string(controller.window().get_status()).find("not a target") !=
              std::string::npos);
        // And the half-typed row is still there, exactly as it was typed. Losing it would
        // be the field clearing itself under somebody mid-address.
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 2);
        CHECK(std::string(rows->row_data(1)->address) == "127.0.0.1:");
    }

    SECTION("a keystroke is remembered and not applied") {
        // Applying opens and closes a socket. Doing that per character would rebuild it
        // halfway through an address.
        controller.editTarget(1, "wall", "127.0.0.1:57009");
        CHECK(live().targets[1].port == 57001);

        // And the draft still has to survive the list being republished, which is what [+]
        // does — the row is drawn from the model, so a draft kept only in the widget would go.
        controller.addTarget();
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 3);
        CHECK(std::string(rows->row_data(1)->address) == "127.0.0.1:57009");
        CHECK(live().targets[1].port == 57009);
    }

    SECTION("an added row is a target already") {
        // It used to be blank, and a blank row applies nothing — so a target did not exist
        // until its whole address had been typed and entered, which is the wrong way round
        // for somebody building a rig: a rule cannot be routed to a target that is not there.
        controller.addTarget();
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 3);
        CHECK(rows->row_data(2)->kind_index == 0);
        CHECK(std::string(rows->row_data(2)->host) == "127.0.0.1");
        CHECK(std::string(rows->row_data(2)->port) == "9000");
        REQUIRE(live().targets.size() == 3);
        CHECK(live().targets[2].host == "127.0.0.1");
        CHECK(live().targets[2].port == 9000);
    }

    SECTION("a row asks for a host and a port, not for one string holding both") {
        // The two boxes are edited one at a time and merged here — see `setTargetHost`.
        controller.setTargetHost(0, "192.0.2.40", false);
        CHECK(live().targets[0].host == "127.0.0.1"); // a keystroke, not applied
        controller.setTargetPort(0, "7010", true);
        REQUIRE(live().targets.size() == 2);
        CHECK(live().targets[0].host == "192.0.2.40");
        CHECK(live().targets[0].port == 7010);
        CHECK(live().targets[0].name == "deck");
    }

    SECTION("switching a row to MIDI leaves it unfinished rather than broken") {
        // Nothing is sent until a device is picked, and that is not an error to report: the
        // row is being filled in. Naming a device it cannot open is a different thing, and
        // is reported — see "a MIDI target is a row like any other".
        controller.setTargetKind(0, 1);
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 2);
        CHECK(rows->row_data(0)->kind_index == 1);
        CHECK(std::string(rows->row_data(0)->address).empty());
        CHECK(live().targets.size() == 1);
        CHECK_FALSE(controller.statusIsError());
    }

    SECTION("a row removed is a target removed") {
        controller.removeTarget(0);
        REQUIRE(live().targets.size() == 1);
        CHECK(live().targets[0].name == "wall");
        CHECK(controller.window().get_outputs_list()->row_count() == 1);
    }

    SECTION("deleting a row rebuilds the ones that moved up into its place") {
        // The other half of the drag test above, and the case it could not cover. Updating a
        // row in place keeps its elements — which is what saves the drag — and keeps any
        // *dead* `text:` binding with them: Slint drops a binding the moment somebody types
        // into the box. Delete the first of two outputs and the second's name moves up a row,
        // so a box that had been typed into would go on showing the name of the target that
        // used to be above it. Reported in the trigger editor as "editing one changes them
        // all"; this is the same mechanism in the main window.
        const auto rows = controller.window().get_outputs_list();
        const auto watch = std::make_shared<ModelWatch>();
        rows->attach_peer(watch);

        controller.removeTarget(0);
        controller.tick(); // the rebuild is deferred out of the × that was pressed
        CHECK(watch->resets == 1);
        REQUIRE(rows->row_count() == 1);
        CHECK(std::string(rows->row_data(0)->name) == "wall");

        // And a tick that changed nothing does not keep rebuilding them: that would tear a
        // box down thirty times a second, which is the failure this started as.
        const int settled = watch->resets;
        controller.tick();
        controller.tick();
        CHECK(watch->resets == settled);
    }

    SECTION("a port outside the range is not a port") {
        controller.setOscTargets("127.0.0.1:99999");
        CHECK(live().oscTargets == 0);
    }

    SECTION("and clearing the row clears the outputs") {
        controller.acceptTarget(0, "", "");
        controller.acceptTarget(1, "", "");
        CHECK(live().targets.empty());
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

        controller.setOscTargets("hall = 127.0.0.1:57002");
        CHECK(std::string(controller.editor().window().get_outputs_available()) ==
              "1 output it was routed to is gone");
    }

    SECTION("renaming an output in its row leaves the rules routed to it reaching it") {
        // The operator's report of 2026-09-23: *"if I change the name of a target like an osc
        // target, all the triggers pointed to it break, and I have to reassign them."* A rule
        // is routed to an output's id, which the row keeps whatever is typed into its name.
        controller.editor().add();
        controller.editor().setOutputs("deck");
        const std::vector<std::string> routed = controller.editor().rules().back().outputs;
        REQUIRE(routed.size() == 1);
        const std::string before = live().targets[0].id;
        CHECK(routed.front() == before);

        controller.setTargetName(0, "media server", true);
        controller.tick();
        REQUIRE(live().targets[0].name == "media server");
        CHECK(live().targets[0].id == before);
        CHECK(controller.editor().rules().back().outputs == routed);
        CHECK(std::string(controller.editor().window().get_outputs_available()) ==
              "reaches 1 output");
        CHECK(std::string(controller.editor().window().get_outputs_summary()) == "media server");
    }

    SECTION("every row is an output with an id of its own") {
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 2);
        CHECK_FALSE(std::string(rows->row_data(0)->id).empty());
        CHECK(std::string(rows->row_data(0)->id) == live().targets[0].id);
        CHECK(std::string(rows->row_data(1)->id) == live().targets[1].id);
        CHECK(live().targets[0].id != live().targets[1].id);
        // A row added is one from the start.
        controller.addTarget();
        CHECK_FALSE(std::string(rows->row_data(2)->id).empty());
        CHECK(std::string(rows->row_data(2)->id) == live().targets[2].id);
    }
}

TEST_CASE("a MIDI target is a row like any other", "[ui]") {
    // §5.6's targets are OSC *and* MIDI, named so a rule can pick between them. The address
    // box takes both, which is what `output::parseOutputTarget` accepts either way.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    controller.addTarget();
    controller.acceptTarget(0, "lights", "midi takt4 test - no such port");

    const auto rows = controller.window().get_outputs_list();
    REQUIRE(rows->row_count() == 1);
    CHECK(std::string(rows->row_data(0)->address) == "midi takt4 test - no such port");
    // The device is not on this machine, so opening it fails — and that is *said* rather
    // than dropping the row, because the rest of the rig is still sending.
    CHECK(controller.statusIsError());
    CHECK(std::string(controller.window().get_status()).find("outputs:") != std::string::npos);
    REQUIRE(seen(controller).targets.size() == 1);
    CHECK(seen(controller).targets[0].kind ==
          takt4::output::OutputTarget::Kind::Midi);
}

TEST_CASE("a MIDI port that will not open is said out loud", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    // Straight to the controller, not through the picker: the picker sends an *index* into
    // the machine's own port list now (a ComboBox cannot be moved by its value — Slint
    // 11970), and no index on this machine names a port that does not exist.
    controller.setMidiPort("takt4 test - no such port");
    CHECK(!seen(controller).midiClock);
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

    // The outputs go on — they live as long as the window (the audit's H5) — and are told
    // the tracker has stopped listening.
    CHECK(controller.outputs().running());
    REQUIRE(controller.settleOutputs());
    CHECK_FALSE(controller.outputs().tracking());
    CHECK(tracker.engine().beatsDropped() == 0);
    CHECK(controller.outputs().errors() == 0);
    // Whatever the tracker called on silence, the transports were given all of it — by the
    // output thread, a round or two after the stop, since nothing drains the ring for it now.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (controller.outputs().transports().beats() != tracker.engine().beatsCalled() &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
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

TEST_CASE("a remembered interface that is missing at launch is still the one saved", "[ui]") {
    // The audit's H11: one launch before the MOTU was powered on fell back to another device —
    // right — and then saved that device as if it had been chosen, so the next show opened on
    // the wrong interface at channel 1 even with the MOTU plugged in.
    LiveTracker tracker(kWeights, kStateSpace);
    if (tracker.devices().empty()) {
        SKIP("no input device on this machine");
    }
    takt4::settings::Settings saved;
    saved.machine.deviceName = "An interface that is switched off";
    saved.machine.hostApiName = "ASIO";
    saved.machine.channel = 10;

    WindowController controller(tracker, saved);
    // Running on something else for now...
    CHECK(controller.devices()[static_cast<std::size_t>(controller.deviceIndex())].name !=
          saved.machine.deviceName);
    // ...and still wanting the one that is missing.
    const takt4::settings::Settings out = controller.currentSettings();
    CHECK(out.machine.deviceName == "An interface that is switched off");
    CHECK(out.machine.hostApiName == "ASIO");
    CHECK(out.machine.channel == 10);

    // Unless the operator picks one, which is a choice and is kept.
    controller.window().invoke_device_picked(controller.deviceIndex());
    CHECK(controller.currentSettings().machine.deviceName ==
          controller.devices()[static_cast<std::size_t>(controller.deviceIndex())].name);
}

TEST_CASE("a remembered MIDI clock port that is missing at launch is still the one saved",
          "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.machine.midiClockPort = "takt4 test - a drum machine left at home";
    WindowController controller(tracker, saved);
    REQUIRE(!seen(controller).midiClock);
    CHECK(controller.currentSettings().machine.midiClockPort ==
          "takt4 test - a drum machine left at home");

    // Choosing "no MIDI clock" is a choice too.
    controller.pickMidiPort(0);
    CHECK(controller.currentSettings().machine.midiClockPort.empty());
}

TEST_CASE("RESCAN reads the devices again and keeps the one chosen", "[ui]") {
    // The audit's H11: PortAudio builds its device list once, so an interface switched on after
    // takt4 started was never offered. RESCAN restarts PortAudio — the only thing that makes it
    // look again — and must land back on what was selected, found by name.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    if (controller.devices().empty()) {
        SKIP("no input device on this machine");
    }
    const std::size_t pick = controller.devices().size() > 1 ? 1 : 0;
    controller.window().invoke_device_picked(static_cast<int>(pick));
    const std::string chosen = controller.devices()[pick].name;
    const std::string api = controller.devices()[pick].hostApiName;

    controller.window().invoke_rescan_clicked();
    REQUIRE(controller.deviceIndex() >= 0);
    const InputDevice& after = controller.devices()[static_cast<std::size_t>(controller.deviceIndex())];
    CHECK(after.name == chosen);
    CHECK(after.hostApiName == api);
    CHECK_FALSE(controller.statusIsError());
    CHECK(std::string(controller.window().get_status()).find("Found ") != std::string::npos);
}

TEST_CASE("an ASIO driver that fell over while being asked is said and RESCAN clears it", "[ui]") {
    // The ASIO drivers are asked from a process of their own (core/audio/asio_scan.hpp). When
    // that process falls over twice the interface is missing from the list, and the operator
    // has to be told why — or it reads as unplugged. Made to fall over before it loads any
    // driver, so this touches nothing on the rig.
    std::optional<LiveTracker> tracker;
    {
        const takt4::test::ScopedVariable asio("TAKT4_NO_ASIO", nullptr);
        const takt4::test::ScopedVariable fall("TAKT4_TEST_ASIO_SCAN", "fall-over");
        tracker.emplace(kWeights, kStateSpace); // its PortAudio session scans here
    }
    WindowController controller(*tracker);
    for (const InputDevice& device : controller.devices()) {
        CHECK(device.hostApi != takt4::audio::HostApiKind::Asio);
    }
    CHECK(controller.statusIsError());
    const std::string status(controller.window().get_status());
    INFO("status: " << status);
    CHECK(status.find("An ASIO driver fell over twice") != std::string::npos);
    CHECK(status.find("Press RESCAN") != std::string::npos);

    // RESCAN is what it says to press. A driver that falls over again is said again, over
    // everything else RESCAN reports: the MIDI ports and outputs it goes on to re-read used to
    // leave "Found ..." on the line instead.
    {
        const takt4::test::ScopedVariable asio("TAKT4_NO_ASIO", nullptr);
        const takt4::test::ScopedVariable fall("TAKT4_TEST_ASIO_SCAN", "fall-over");
        controller.window().invoke_rescan_clicked();
    }
    const std::string again(controller.window().get_status());
    INFO("after a RESCAN that fell over again: " << again);
    CHECK(controller.statusIsError());
    CHECK(again.find("An ASIO driver fell over twice") != std::string::npos);

    // And a RESCAN that finds nothing wrong — with no ASIO wanted this time, as in every other
    // test — says so, and what was said goes with the problem.
    controller.window().invoke_rescan_clicked();
    const std::string after(controller.window().get_status());
    INFO("after RESCAN: " << after);
    CHECK(after.find("ASIO") == std::string::npos);
    // What a RESCAN with nothing wrong says, which a problem kept from before would stop.
    CHECK_FALSE(controller.statusIsError());
    CHECK(after.find("Found ") != std::string::npos);
}

TEST_CASE("a dead input says NO AUDIO and is brought back", "[ui][hardware]") {
    // The audit's C4, the window's half. The watchdog's arithmetic is input_watchdog_test.cpp's
    // business; this hands the window the readings a pulled cable, a moved clock and a driver
    // reset produce, and checks what an operator would see and that the input really reopens.
    LiveTracker tracker(kWeights, kStateSpace);
    if (!bestInputDevice(tracker)) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    controller.toggleRun();
    REQUIRE(tracker.running());
    REQUIRE(controller.wantsRunning());

    using Reading = takt4::audio::InputWatchdog::Reading;
    using Verdict = takt4::audio::InputWatchdog::Verdict;
    const takt4::audio::AsioDriverEvents nothing;

    // Silence: NO AUDIO, the meter at nothing, and the status line naming the device.
    Reading silent;
    silent.verdict = Verdict::Silent;
    silent.silentForSeconds = 0.6;
    controller.superviseInput(silent, nothing, 100.0);
    CHECK(controller.window().get_input_lost());
    CHECK(controller.window().get_input_level() == 0.0f);
    CHECK(std::string(controller.window().get_status()).find("No audio from") != std::string::npos);
    // Still running as far as the operator is concerned: STOP is what the button offers.
    CHECK(controller.window().get_running());

    // Not before its time, and then reopened: the stream is open again and the readout clear.
    controller.superviseInput(Reading{}, nothing, 100.5);
    CHECK(controller.window().get_input_lost());
    controller.superviseInput(Reading{}, nothing, 101.1);
    CHECK_FALSE(controller.window().get_input_lost());
    CHECK(tracker.running());
    CHECK(std::string(controller.window().get_status()).find("Audio is back") != std::string::npos);
    // Said, and not in red: it is fixed, and a red status that asks for nothing stays red until
    // something else is said (the audit's M25).
    CHECK_FALSE(controller.statusIsError());

    // A clock moved by another program: reopened at once, and said.
    Reading moved;
    moved.verdict = Verdict::RateChanged;
    moved.measuredRate = 48000.0;
    controller.superviseInput(moved, nothing, 102.0);
    CHECK(tracker.running());
    CHECK(std::string(controller.window().get_status()).find("moved from") != std::string::npos);
    CHECK_FALSE(controller.statusIsError());

    // A driver asking to be reset: the same.
    takt4::audio::AsioDriverEvents reset;
    reset.resetRequest = true;
    controller.superviseInput(Reading{}, reset, 103.0);
    CHECK(tracker.running());
    CHECK(std::string(controller.window().get_status()).find("asked to be reset") !=
          std::string::npos);
    CHECK_FALSE(controller.statusIsError());

    // And STOP during an outage stops, rather than reading "not running" and starting.
    controller.superviseInput(silent, nothing, 104.0);
    REQUIRE(controller.window().get_input_lost());
    controller.toggleRun();
    CHECK_FALSE(controller.wantsRunning());
    CHECK_FALSE(tracker.running());
    CHECK_FALSE(controller.window().get_input_lost());
    CHECK_FALSE(controller.window().get_running());
}

TEST_CASE("the window switches the outputs back on", "[ui][network]") {
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.preset.link = true;
    saved.preset.oscPrefix = "/vj";
    saved.preset.outputs = takt4::output::oscOutputs({{"127.0.0.1", 57000}, {"127.0.0.1", 57001}});

    WindowController controller(tracker, saved);
    CHECK(seen(controller).link);
    CHECK(seen(controller).oscTargets == 2);
    CHECK(controller.outputs().transports().oscPrefix() == "/vj");
    CHECK(controller.window().get_link_on());
    CHECK(controller.window().get_osc_on());
    // And into the rows that edit them, seeded once from what the runner was built with.
    const auto rows = controller.window().get_outputs_list();
    REQUIRE(rows->row_count() == 2);
    CHECK(std::string(rows->row_data(1)->address) == "127.0.0.1:57001");
}

TEST_CASE("a MIDI port that has since been unplugged is reported, not fatal", "[ui]") {
    // The one restored setting that can fail. Construction must still finish: an app that
    // will not open because a MIDI cable moved is worse than one with no clock.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.machine.midiClockPort = "takt4 test - a port from another machine";

    WindowController controller(tracker, saved);
    CHECK(!seen(controller).midiClock);
    CHECK(controller.statusIsError());
    CHECK(std::string(controller.window().get_status()).find("MIDI clock") != std::string::npos);
}

TEST_CASE("saved outputs that cannot open do not stop the window opening", "[ui]") {
    // The audit's C1, reproduced with the Release build before it was fixed: a saved output
    // naming a MIDI interface that was not plugged in threw out of the runner's constructor,
    // out of this class's, and out of `ui::run` — takt4 exited 0xC0000409 within seconds of
    // every launch, with no window and no message. A hostname that does not resolve yet did
    // the same. The one port that had been guarded was the MIDI *clock* (the test above).
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    for (const char* line : {"lights = midi takt4 test - a device left at home",
                             "hall = no-such-host.invalid:57000", "main = 127.0.0.1:57000"}) {
        takt4::output::OutputTarget target;
        REQUIRE(takt4::output::parseOutputTarget(line, target));
        saved.preset.outputs.push_back(target);
    }

    std::optional<WindowController> controller;
    REQUIRE_NOTHROW(controller.emplace(tracker, saved));

    // None is dropped. The MIDI device that is not here is said at once; the host name is an
    // OSC target of its own, looked up on a thread of its own (the audit's H12), and is said
    // when that look-up fails — nothing waits for a name server any more.
    CHECK(seen(*controller).oscTargets == 2);
    CHECK(seen(*controller).targets.size() == 3);
    CHECK(controller->window().get_outputs_list()->row_count() == 3);
    CHECK(controller->statusIsError());
    const std::string first(controller->window().get_status());
    INFO("status at launch: " << first);
    CHECK(first.find("outputs:") != std::string::npos);
    CHECK(first.find("lights") != std::string::npos);

    std::string status;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (status.find("hall") == std::string::npos && std::chrono::steady_clock::now() < until) {
        controller->tick();
        status = std::string(controller->window().get_status());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    INFO(status);
    CHECK(status.find("hall") != std::string::npos);
    CHECK(status.find("cannot resolve") != std::string::npos);
    // And the device left at home is still said beside it rather than written over.
    CHECK(status.find("lights") != std::string::npos);
    CHECK(controller->statusIsError());
}

TEST_CASE("a saved OSC prefix that is not an address does not stop the window opening", "[ui]") {
    // The same crash by the third road: `OscPublisher` throws on "vj", and it is built inside
    // the runner. `settings::fromJson` refuses one on the way in; this is the window's own
    // guard for settings that arrive any other way.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.preset.oscPrefix = "vj";

    std::optional<WindowController> controller;
    REQUIRE_NOTHROW(controller.emplace(tracker, saved));
    CHECK(controller->outputs().transports().oscPrefix() == "/takt4");
    CHECK(controller->oscControl().config().prefix == "/takt4");
    CHECK(controller->statusIsError());
    CHECK(std::string(controller->window().get_status()).find("\"vj\"") != std::string::npos);
}

TEST_CASE("the settings are saved on their own a moment after a change", "[ui][settings]") {
    // The audit's C8: the file was written on a clean exit and on SAVE, and a crash, a power
    // cut or Windows shutting down with the window open lost the whole session. No exit and
    // no button here — only the redraw timer, and the file appears.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "settings.json";
    controller.enableAutosave(file, 0.0);

    // Nothing has changed since it was switched on, so nothing is written however long it runs.
    for (std::uint64_t i = 0; i < 4 * WindowController::kAutosaveCheckTicks; ++i) {
        controller.tick();
    }
    CHECK_FALSE(std::filesystem::exists(file));

    takt4::trigger::Rule::Config strobe;
    strobe.id = "strobe";
    strobe.address = "/composition/layers/1/clips/1/connect";
    controller.setRules({strobe});
    for (std::uint64_t i = 0; i < 4 * WindowController::kAutosaveCheckTicks; ++i) {
        controller.tick();
    }
    REQUIRE(std::filesystem::exists(file));
    const takt4::settings::Settings back = takt4::settings::load(file);
    REQUIRE(back.preset.rules.size() == 1);
    CHECK(back.preset.rules[0].id == "strobe");

    // And a slider moved while the tracker is stopped is saved as moved, not as it was before
    // (the audit's M27: the save read the engine, which takes the change only at Start).
    controller.window().invoke_fold_min_changed(91.0f);
    for (std::uint64_t i = 0; i < 4 * WindowController::kAutosaveCheckTicks; ++i) {
        controller.tick();
    }
    CHECK_THAT(takt4::settings::load(file).preset.tempo.minBpm, WithinAbs(91.0, 1e-6));
}

TEST_CASE("what the window hands back is what it was given", "[ui][network]") {
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.preset.link = true;
    saved.preset.outputs = takt4::output::oscOutputs({{"192.0.2.40", 7000}});
    saved.preset.oscPrefix = "/vj";

    WindowController controller(tracker, saved);
    // Changes made through the window are in it too.
    controller.window().invoke_fold_min_changed(90.0f);
    controller.window().invoke_latency_changed(-25.0f);
    tracker.engine().step(); // let the engine take them, as a running one would

    const takt4::settings::Settings out = controller.currentSettings();
    CHECK(out.preset.link);
    REQUIRE(out.preset.outputs.size() == 1);
    CHECK(out.preset.outputs[0].host == "192.0.2.40");
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

TEST_CASE("the program carries its own weights and state space", "[ui]") {
    // The user's ask of 2026-09-07: *"the exe should be entirely self contained. not relying
    // on weights or whatever in some other directory. isolated exe that works anywhere."*
    //
    // The blobs are compiled in, so this checks two separate things. That they *load* — the
    // format checks and the FNV-1a checksum both run over the embedded copy, which is what
    // would catch a chunking bug in tools/embed_asset.py. And that they are the **right**
    // blobs: a checksum only proves a blob is internally consistent, so the wrong file
    // embedded by mistake would sail through it. Only comparing against the committed
    // asset catches that.
    const takt4::model::ModelWeights built =
        takt4::model::ModelWeights::fromBytes(takt4::assets::weights(), "electronic (built in)");
    const takt4::model::ModelWeights onDisk = takt4::model::ModelWeights::fromFile(
        std::filesystem::path(TAKT4_WEIGHTS_DIR) / "electronic.bin");

    REQUIRE(built.convWeight().size() == onDisk.convWeight().size());
    CHECK(std::equal(built.convWeight().begin(), built.convWeight().end(),
                     onDisk.convWeight().begin()));
    REQUIRE(built.outputWeight().size() == onDisk.outputWeight().size());
    CHECK(std::equal(built.outputWeight().begin(), built.outputWeight().end(),
                     onDisk.outputWeight().begin()));
    // The last block in the blob, so a copy that stopped short would differ here first.
    REQUIRE(built.outputBias().size() == onDisk.outputBias().size());
    CHECK(std::equal(built.outputBias().begin(), built.outputBias().end(),
                     onDisk.outputBias().begin()));

    const takt4::tracking::StateSpaceModel space = takt4::tracking::StateSpaceModel::fromBytes(
        takt4::assets::stateSpace(), "default (built in)");
    const takt4::tracking::StateSpaceModel spaceOnDisk = takt4::tracking::StateSpaceModel::fromFile(
        std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin");
    CHECK(space.beat().numStates() == spaceOnDisk.beat().numStates());
    CHECK(space.beat().numIntervals() == spaceOnDisk.beat().numIntervals());
    CHECK(space.downbeat().numStates() == spaceOnDisk.downbeat().numStates());
    CHECK_THAT(space.secondsPerFrame(), WithinAbs(spaceOnDisk.secondsPerFrame(), 1e-12));
    CHECK_THAT(space.bpmOfInterval(0), WithinAbs(spaceOnDisk.bpmOfInterval(0), 1e-12));

    // And a tracker really starts on them, which is what app.cpp does and the only proof
    // that matters: no path was consulted to get here.
    LiveTracker tracker(
        takt4::model::ModelWeights::fromBytes(takt4::assets::weights(), "electronic (built in)"),
        takt4::tracking::StateSpaceModel::fromBytes(takt4::assets::stateSpace(),
                                                    "default (built in)"),
        LiveTracker::Options{});
    CHECK(tracker.weightsPath() == "electronic (built in)");
    CHECK(tracker.stateSpace().beat().numStates() == spaceOnDisk.beat().numStates());
}

TEST_CASE("settings export and import carry the rules and the outputs", "[ui][settings]") {
    // The operator, 2026-09-12: "how do I force takt4 to save out the config? I made a lot
    // of changes, but I am scared of closing it." Until then the only save was on the way
    // out. This is the round trip the buttons do.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    takt4::trigger::Rule::Config lasers;
    lasers.id = "lasers";
    lasers.sendKind = takt4::trigger::Message::Kind::MidiNote;
    lasers.channel = 3;
    lasers.followUps.push_back(takt4::trigger::FollowUp{});
    lasers.outputs = {"Lasers"};
    takt4::trigger::Rule::Config clips;
    clips.id = "clips";
    clips.address = "/composition/layers/1/clips/{}/connect";
    controller.setRules({lasers, clips});

    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "show.json";
    REQUIRE(controller.exportTo(file));
    REQUIRE(std::filesystem::exists(file));

    // A second window, with nothing in it, reads the file back.
    LiveTracker other(kWeights, kStateSpace);
    WindowController fresh(other);
    REQUIRE(fresh.rules().empty());
    REQUIRE(fresh.importFrom(file));

    REQUIRE(fresh.rules().size() == 2);
    CHECK(fresh.rules()[0].id == "lasers");
    CHECK(fresh.rules()[0].sendKind == takt4::trigger::Message::Kind::MidiNote);
    CHECK(fresh.rules()[0].channel == 3);
    CHECK(fresh.rules()[0].followUps.size() == 1);
    REQUIRE(fresh.rules()[0].outputs.size() == 1);
    CHECK(fresh.rules()[0].outputs[0] == "Lasers");
    CHECK(fresh.rules()[1].id == "clips");
    CHECK(fresh.rules()[1].address == "/composition/layers/1/clips/{}/connect");
}

TEST_CASE("an import brings the lighting patch with it", "[ui][settings][dmx]") {
    // The audit's M18. IMPORT applied the rules and the outputs and left the patch alone, so
    // every imported lighting rule aimed at fixtures this rig did not have and reached nothing;
    // and a file holding only a patch was "no preset in it".
    takt4::settings::Settings show;
    show.preset.fixtures = {takt4::dmx::fixtureFromMode("wash L", 1, 0, 1),
                            takt4::dmx::fixtureFromMode("wash R", 1, 0, 4)};
    show.preset.oscPrefix = "/show";
    show.preset.decoder = takt4::tracking::Decoder::ParticleFilter;
    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "patch-only.json";
    REQUIRE(takt4::settings::save(show, file));

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    REQUIRE(controller.importFrom(file));

    // Into the output thread — which is what a rule aims at — and into what is saved.
    REQUIRE(controller.settleOutputs());
    const std::size_t live = controller.outputs().inspect(
        [](const auto&, const takt4::output::Transports& transports, const auto&) {
            return transports.patch().size();
        });
    CHECK(live == 2);
    const takt4::settings::Settings saved = controller.currentSettings();
    REQUIRE(saved.preset.fixtures.size() == 2);
    CHECK(saved.preset.fixtures[1].name == "wash R");
    // And the patch editor shows it.
    CHECK(controller.patchEditor().fixtures().size() == 2);

    // The two the running application cannot switch are kept for the next launch, and said.
    CHECK(saved.preset.oscPrefix == "/show");
    CHECK(saved.preset.decoder == takt4::tracking::Decoder::ParticleFilter);
    CHECK(std::string(controller.window().get_status()).find("Restart") != std::string::npos);
}

TEST_CASE("a path or a device name in any encoding reaches the status line intact", "[ui]") {
    // The audit's H13, and the wider class behind it. Every string on screen goes through
    // `slint::SharedString`, which aborts the process on a byte that is not UTF-8 — and the
    // status line was built from `path.string()`, which on Windows converts through the ANSI
    // code page: "Jon’s set.json" came out as a lone 0x92 byte. A failure here is not a failed
    // assertion but the test binary dying, which is what the operator's takt4 did.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::test::TempDir dir;
    const std::filesystem::path file =
        dir.path() / std::filesystem::path(L"Shows – 2026") / std::filesystem::path(L"Jon’s set.json");
    REQUIRE(controller.exportTo(file));
    REQUIRE(std::filesystem::exists(file));
    const std::string status(controller.window().get_status());
    CHECK(status.find("Shows \xE2\x80\x93 2026") != std::string::npos);
    CHECK(status.find("Jon\xE2\x80\x99s set.json") != std::string::npos);

    // And a name that arrived from a driver in its own encoding: an ASIO or MIDI device called
    // "µ-Port" read through the ANSI API is the byte 0xB5 on its own. The failure to open it
    // names it on the status line, which is where it used to abort.
    controller.setMidiPort("takt4 test \xB5-Port");
    CHECK(controller.statusIsError());
    const std::string refused(controller.window().get_status());
    CHECK(refused.find("takt4 test \xEF\xBF\xBD-Port") != std::string::npos);
}

namespace {

/// The controller's main window, shown and laid out at `width` x `height` with nothing else
/// between it and a dispatched event — the shape `render` gives a bare window, without the
/// render, whose adapter slot may belong to one of the controller's other windows.
void layOut(WindowController& controller, float width, float height) {
    auto& window = controller.window().window();
    controller.window().show();
    window.dispatch_scale_factor_change_event(1.0f);
    window.dispatch_resize_event(slint::LogicalSize({width, height}));
    window.dispatch_window_active_changed_event(true);
}

void clickAt(slint::Window& window, float x, float y) {
    const slint::LogicalPosition at({x, y});
    window.dispatch_pointer_move_event(at);
    window.dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
    window.dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
}

void press(slint::Window& window, const std::string& key) {
    window.dispatch_key_press_event(slint::SharedString(key));
    window.dispatch_key_release_event(slint::SharedString(key));
}

const std::string kEscape(1, '\x1b');

} // namespace

TEST_CASE("a double click on PANIC leaves it engaged, and RELEASE lets it go", "[ui]") {
    // The audit's H18. PANIC was a plain toggle, so the way a button is hit in a hurry — twice
    // — halted the rig and let go of it again before it had visibly lit. Driven with real
    // clicks, because the fix is as much where RELEASE appears as what PANIC does: RELEASE
    // shows up to PANIC's *left*, so the second click of a double-click lands on PANIC again.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    constexpr float kWidth = 1000.0f;
    constexpr float kHeight = 760.0f;
    layOut(controller, kWidth, kHeight);
    auto& window = controller.window().window();

    // Inside PANIC's right-hand end, in the pinned footer — the spot the wheel test clicks.
    const float panicX = kWidth - 34.0f;
    const float footerY = kHeight - 65.0f;
    clickAt(window, panicX, footerY);
    // Lit by the press itself, not by the next redraw: the outputs run on their own thread for
    // the window's whole life, so the window waits for the press to be applied before drawing
    // what it did — and RELEASE, which appears with it.
    CHECK(controller.window().get_panicked());
    clickAt(window, panicX, footerY);
    CHECK(panickedNow(controller));
    CHECK(controller.window().get_panicked());

    // RELEASE, found rather than written down: the first thing left of PANIC that lets go.
    bool released = false;
    for (float x = panicX - 150.0f; x > 500.0f && !released; x -= 8.0f) {
        clickAt(window, x, footerY);
        released = !panickedNow(controller);
    }
    CHECK(released);
    CHECK_FALSE(controller.window().get_panicked());

    // And the keyboard still works after that click. RELEASE disappears the moment it works,
    // and the focus the click gave it used to disappear with it — so the next Escape, which is
    // the one that matters, reached nothing at all.
    press(window, kEscape);
    CHECK(panickedNow(controller));
}

TEST_CASE("the keep for the next track box is ticked by a click", "[ui]") {
    // Found on the laid-out window by clicking down the settings column until the box ticks —
    // not assumed from the markup — and read back from the engine, which is what an operator's
    // click has to reach.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);
    layOut(controller, 1000.0f, 760.0f);
    auto& window = controller.window().window();
    REQUIRE_FALSE(controller.window().get_keep_shift());

    // The column the fold switch is in, which is where the box lines up.
    constexpr float kColumn = 118.0f;
    float boxY = 0.0f;
    for (float y = 150.0f; y < 700.0f && boxY == 0.0f; y += 3.0f) {
        clickAt(window, kColumn, y);
        if (controller.window().get_keep_shift()) {
            boxY = y;
        }
    }
    INFO("no click in the settings column ticked the box");
    REQUIRE(boxY > 0.0f);
    run.applyPosted();
    CHECK(tracker.engine().tempoOptions().keepOctaveShift);

    clickAt(window, kColumn, boxY);
    CHECK_FALSE(controller.window().get_keep_shift());
    run.applyPosted();
    CHECK_FALSE(tracker.engine().tempoOptions().keepOctaveShift);
}

TEST_CASE("removing an output row does not leave the keyboard dead", "[ui]") {
    // The same trap by a quieter door: the row's "−" button takes the focus on the click and is
    // then destroyed with its row, which left Escape reaching nothing until the next click.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.addTarget();
    controller.addTarget();
    constexpr float kWidth = 1000.0f;
    layOut(controller, kWidth, 760.0f);
    auto& window = controller.window().window();

    // The "−" at the end of a row: found by clicking down the column until a row goes.
    const float removeX = kWidth - 34.0f;
    bool removed = false;
    for (float y = 380.0f; y < 664.0f && !removed; y += 4.0f) {
        clickAt(window, removeX, y);
        removed = controller.window().get_outputs_list()->row_count() == 1;
    }
    REQUIRE(removed);
    press(window, kEscape);
    CHECK(panickedNow(controller));
}

TEST_CASE("Escape engages PANIC, except in a text box, where it only leaves the box", "[ui]") {
    // HANDOFF: PANIC by keyboard is "non-negotiable for live use", and no `.slint` file handled
    // a key at all. And the other half, which is what makes Escape safe to bind: an operator
    // who presses Escape to get out of a text box must not halt the show.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.addTarget(); // a row with a name box in it
    layOut(controller, 1000.0f, 760.0f);
    auto& window = controller.window().window();

    // Nothing clicked yet: the key reaches the window's own scope, and PANIC engages.
    press(window, kEscape);
    CHECK(panickedNow(controller));
    controller.releasePanic();
    REQUIRE_FALSE(panickedNow(controller));

    // The name box of that row, found by typing into candidates until a name commits. A miss
    // leaves Escape to engage PANIC, which is let go before the next try; the hit is the only
    // round in which Escape was pressed *inside* a box, and that is the round asserted on.
    bool found = false;
    bool panickedInBox = true;
    for (float y = 380.0f; y < 690.0f && !found; y += 4.0f) {
        for (float x = 150.0f; x < 250.0f && !found; x += 20.0f) {
            clickAt(window, x, y);
            press(window, "q");
            press(window, kEscape);
            const auto rows = controller.window().get_outputs_list();
            if (rows->row_count() > 0 && std::string(rows->row_data(0)->name) == "q") {
                found = true;
                panickedInBox = panickedNow(controller);
            }
            if (panickedNow(controller)) {
                controller.releasePanic();
            }
        }
    }
    INFO("no click landed in the row's name box");
    REQUIRE(found);
    CHECK_FALSE(panickedInBox);

    // And straight after leaving the box, Escape is PANIC again: leaving it put the focus
    // somewhere that still passes keys up, rather than nowhere.
    press(window, kEscape);
    CHECK(panickedNow(controller));
}

TEST_CASE("T taps and D snaps the downbeat from the keyboard, and a held key counts once",
          "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    layOut(controller, 1000.0f, 760.0f);
    auto& window = controller.window().window();

    // Stopped, the keys do what their buttons do — nothing: both are greyed out.
    press(window, "t");
    CHECK(controller.taps() == 0);

    // The window as a running tracker leaves it, without a device to open.
    controller.window().set_running(true);
    press(window, "t");
    CHECK(controller.taps() == 1);
    // Held down, a key repeats about thirty times a second; each repeat as a tap would seed a
    // tempo of 1800 BPM.
    window.dispatch_key_press_repeat_event(slint::SharedString("t"));
    window.dispatch_key_press_repeat_event(slint::SharedString("t"));
    CHECK(controller.taps() == 1);
    window.dispatch_key_release_event(slint::SharedString("t"));
    // A real second tap, later than a switch bounce could be.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    press(window, "T");
    CHECK(controller.taps() == 2);

    CHECK_FALSE(controller.window().get_snap_pending());
    press(window, "d");
    CHECK(controller.window().get_snap_pending());
}

TEST_CASE("M fires the manual rules from the keyboard", "[ui]") {
    // The audit's M7: the editor offered "manual hotkey" as a trigger and nothing anywhere could
    // fire one, so a rule set to it never did anything. The hotkey is M.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    takt4::trigger::Rule::Config cue;
    cue.id = "cue";
    cue.trigger = takt4::trigger::Trigger::Manual;
    cue.address = "/cue";
    saved.preset.rules = {cue};
    WindowController controller(tracker, saved);
    layOut(controller, 1000.0f, 760.0f);
    auto& window = controller.window().window();
    const auto fires = [&controller] {
        REQUIRE(controller.settleOutputs());
        return controller.outputs().inspect(
            [](const takt4::trigger::TriggerEngine& rules, const takt4::output::Transports&,
               const takt4::output::RuleSink&) { return rules.rule(0).fires(); });
    };
    REQUIRE(fires() == 0);

    // Stopped or not: the outputs run, and a manual cue is the operator's own.
    press(window, "m");
    CHECK(fires() == 1);
    // A held key counts once, as T does.
    window.dispatch_key_press_event(slint::SharedString("M"));
    window.dispatch_key_press_repeat_event(slint::SharedString("M"));
    window.dispatch_key_press_repeat_event(slint::SharedString("M"));
    window.dispatch_key_release_event(slint::SharedString("M"));
    CHECK(fires() == 2);
}

TEST_CASE("closing the main window closes the editor with it", "[ui]") {
    // Slint's event loop runs until the **last** window is hidden, and §5.9's editor is a
    // window of its own — so closing the main window with the editor up left takt4 running
    // with nothing but the editor on screen. `ui::run` writes the settings only after the
    // loop returns, so the operator had closed the app, been left with a stray window, and
    // had nothing written. That is the exact failure the SAVE button was added for.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    controller.openEditor();
    REQUIRE(controller.editor().visible());

    // The real gesture, dispatched into the window — not the handler called directly.
    controller.window().window().dispatch_close_requested_event();
    CHECK_FALSE(controller.editor().visible());
}

TEST_CASE("closing the main window closes the patch editor with it", "[ui]") {
    // The same failure through the other door (the audit's H14): the close handler hid the
    // rule editor and nothing else, so with PATCH LIGHTS open the process stayed up with the
    // patch window alone on screen — the interface, Link and the ports all still held.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    controller.window().invoke_fixtures_clicked();
    REQUIRE(controller.patchEditor().visible());
    REQUIRE(controller.patchEditor().window().window().is_visible());
    controller.openEditor();
    REQUIRE(controller.editor().visible());

    controller.window().window().dispatch_close_requested_event();
    CHECK_FALSE(controller.patchEditor().visible());
    CHECK_FALSE(controller.editor().visible());
    // What Slint itself thinks, which is what decides whether its event loop returns: it runs
    // until the last *visible* window is gone, and the flags above are only ours.
    CHECK_FALSE(controller.patchEditor().window().window().is_visible());
    CHECK_FALSE(controller.editor().window().window().is_visible());
}

TEST_CASE("importing something that is not a preset changes nothing", "[ui][settings]") {
    // `settings::load` never fails — anything it cannot read gives the defaults — which is
    // right at startup and would silently wipe an hour's work here.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::trigger::Rule::Config keep;
    keep.id = "keep-me";
    controller.setRules({keep});

    const takt4::test::TempDir dir;
    const std::filesystem::path junk = dir.path() / "not-settings.json";
    std::ofstream(junk) << "{\"something\":\"else\"}";

    CHECK_FALSE(controller.importFrom(junk));
    REQUIRE(controller.rules().size() == 1);
    CHECK(controller.rules()[0].id == "keep-me");

    // A cancelled dialog is an empty path, and is not an error to report.
    CHECK_FALSE(controller.importFrom(std::filesystem::path{}));
    CHECK(controller.rules().size() == 1);
}

TEST_CASE("a tap does not switch the fold on", "[ui]") {
    // **The setting an operator switched off must not come back on because they tapped.**
    //
    // takt4 is handed a *set* — one record after another — and a window that suited the last
    // track silently halves or doubles the next one. A tap used to turn the fold on and set
    // the window around the tapped tempo; that was taken out on 2026-09-08 and nothing tested
    // it, so this is the guard. Asked again on 2026-09-16: "you're also still automatically
    // enabling the 'keep bpm in range' option. why? will this not harm detection for the next
    // song which could have a wildly different bpm range".
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);

    controller.window().invoke_fold_on_changed(false);
    run.applyPosted();
    REQUIRE_FALSE(tracker.engine().tempoOptions().octaveFold);
    const Options before = tracker.engine().tempoOptions();

    // Three taps half a second apart: 120 BPM, and nothing is sent before the third.
    controller.tap(0.0);
    controller.tap(0.5);
    controller.tap(1.0);
    run.applyPosted();

    const Options after = tracker.engine().tempoOptions();
    CHECK_FALSE(after.octaveFold);
    // And the window it would have moved is where the operator left it, so switching the fold
    // back on later does not switch on a window a tap chose in the meantime.
    CHECK_THAT(after.minBpm, WithinAbs(before.minBpm, 1e-6));
    CHECK_THAT(after.maxBpm, WithinAbs(before.maxBpm, 1e-6));
    CHECK_FALSE(controller.window().get_fold_on());
}

TEST_CASE("IDENTIFY lights a fixture before Start has ever been pressed", "[ui][dmx]") {
    // The audit's H5, where the rig met it: the patch editor's IDENTIFY said "Identifying..."
    // and nothing went out, because the outputs only ran while the tracker did. An operator in
    // the truss patching a rig before the doors open is exactly who presses it.
    takt4::testing::LoopbackReceiver node;
    takt4::settings::Settings settings;
    takt4::output::OutputTarget truss;
    truss.name = "truss";
    truss.kind = takt4::output::OutputTarget::Kind::ArtNet;
    truss.host = "127.0.0.1";
    truss.port = node.port();
    settings.preset.outputs = {truss};
    settings.preset.fixtures = {takt4::dmx::fixtureFromMode("par", 1, 0, 1)};

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker, settings);
    REQUIRE_FALSE(tracker.running());
    controller.patchEditor().pick(0);
    controller.patchEditor().identify();

    bool lit = false;
    for (int attempt = 0; attempt < 200 && !lit; ++attempt) {
        const std::string datagram = node.receive();
        if (datagram.empty()) {
            break;
        }
        // An ArtDmx frame's levels start at byte 18; the par is channels 1 to 3.
        lit = datagram.size() >= 21 && datagram.compare(0, 8, std::string("Art-Net\0", 8)) == 0 &&
              static_cast<std::uint8_t>(datagram[18]) > 200 &&
              static_cast<std::uint8_t>(datagram[19]) > 200 &&
              static_cast<std::uint8_t>(datagram[20]) > 200;
    }
    CHECK(lit);
}

TEST_CASE("an Art-Net row offers no delay of its own", "[ui]") {
    // The audit's M9: the per-output delay slider was on every row, and on an Art-Net row it
    // moved a number nothing reads — a universe is a stream of frames with no message to hold
    // back. Driven by real clicks: what is under test is what the row draws.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    for (const char* line : {"deck = 127.0.0.1:57000", "node = artnet 127.0.0.1:6454"}) {
        takt4::output::OutputTarget target;
        REQUIRE(takt4::output::parseOutputTarget(line, target));
        saved.preset.outputs.push_back(target);
    }
    WindowController controller(tracker, saved);
    layOut(controller, 1000.0f, 1400.0f);
    auto& window = controller.window().window();
    const auto rows = controller.window().get_outputs_list();
    REQUIRE(rows->row_count() == 2);
    REQUIRE(rows->row_data(1)->kind_index == 2);

    // The OSC row's slider: the first click that moves the OSC row's delay.
    float sliderX = -1.0f;
    float sliderY = -1.0f;
    for (float y = 200.0f; y < 1400.0f && sliderX < 0.0f; y += 4.0f) {
        for (float x = 450.0f; x < 900.0f && sliderX < 0.0f; x += 25.0f) {
            clickAt(window, x, y);
            if (rows->row_data(0)->delay_ms != 0.0f) {
                sliderX = x;
                sliderY = y;
            }
        }
    }
    {
        INFO("no click moved the OSC row's delay slider");
        REQUIRE(sliderX >= 0.0f);
    }
    REQUIRE(rows->row_data(1)->delay_ms == 0.0f);

    // The same column on the row below: nothing there to move.
    for (float y = sliderY + 8.0f; y < sliderY + 90.0f; y += 3.0f) {
        for (float x = sliderX - 50.0f; x < sliderX + 60.0f; x += 10.0f) {
            clickAt(window, x, y);
        }
    }
    INFO("slider at " << sliderX << "," << sliderY);
    CHECK(rows->row_data(1)->delay_ms == 0.0f);
    CHECK(seen(controller).targets[1].delaySeconds == 0.0);
}


TEST_CASE("an output's kind dropdown survives the redraws while it is open, and its pick lands",
          "[ui]") {
    // The audit's T3: popups inside repeaters were tested only for the two color pickers. The
    // output rows are a repeater the window builds again whenever a row cannot be updated in
    // place, and a row rebuilt with its dropdown open takes the popup away under the pointer.
    //
    // Swept only down one column, x = 302, where the row's kind dropdown sits (padding, the
    // "targets" label, the tick box and the name come first), and only near the rows: SAVE is
    // in that column too, which a test process may press safely (it saves into a folder of its
    // own), and nothing that opens a dialog or the audio device is.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    takt4::output::OutputTarget deck;
    REQUIRE(takt4::output::parseOutputTarget("deck = 127.0.0.1:57000", deck));
    saved.preset.outputs = {deck};
    WindowController controller(tracker, saved);
    layOut(controller, 1000.0f, 1400.0f);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto reset = [&controller, &settle] {
        controller.setOscTargets("deck = 127.0.0.1:57000");
        settle();
    };
    const auto kindOf = [&controller] {
        const auto rows = controller.window().get_outputs_list();
        return rows->row_count() == 1 ? rows->row_data(0)->kind_index : -1;
    };
    settle();
    REQUIRE(kindOf() == 0);

    // The row, found by its name box: the first click down the name column after which a typed
    // letter lands in the row's name. Not by the dropdown and an arrow key — a click on nothing
    // leaves the keyboard where an earlier probe put it, so a key proves nothing about where
    // the click went. The kind dropdown is on the same row.
    constexpr float kNameColumn = 200.0f;
    constexpr float kKindColumn = 302.0f;
    const auto nameOf = [&controller] {
        const auto rows = controller.window().get_outputs_list();
        return rows->row_count() == 1 ? std::string(rows->row_data(0)->name) : std::string();
    };
    //
    // Every probe starts with a click on the window's own background, off to the right where
    // nothing is drawn, so that the box has lost the focus and a letter can only land in it by
    // the probe's own click. Both edges are found, and the row is aimed at in the middle: the
    // dropdown is shorter than the box, and its top edge is not where the box's is.
    constexpr float kBackgroundX = 985.0f;
    constexpr float kBackgroundY = 1080.0f;
    float top = -1.0f;
    float bottom = -1.0f;
    for (float y = 1080.0f; y < 1320.0f; y += 2.0f) {
        clickAt(window, kBackgroundX, kBackgroundY);
        const std::string before = nameOf();
        clickAt(window, kNameColumn, y);
        press(window, "Q");
        // The letter goes to the controller's draft; the row reads it back on a redraw. Run one
        // here rather than hoping the window's own 30 Hz timer falls inside this probe — under
        // load it mostly did not, and the sweep found no box at all (1 run in 32, 8 at once).
        settle();
        const bool landed = nameOf().size() > before.size();
        if (landed && top < 0.0f) {
            top = y;
        }
        if (landed) {
            bottom = y;
        } else if (top >= 0.0f) {
            break;
        }
    }
    {
        INFO("no click in the name column reached the row's name box");
        REQUIRE(top >= 0.0f);
    }
    const float comboY = (top + bottom) / 2.0f;
    clickAt(window, kBackgroundX, kBackgroundY); // commit it, then put the row back
    reset();
    REQUIRE(kindOf() == 0);

    // What one step of the open dropdown picks, with nothing redrawing: opened with the
    // pointer, then an arrow key and Enter. **Keys, not a click on the list**, which is where
    // the other two tests put the pointer: the list of an output's dropdown could not be found
    // by clicking in the headless window, while the keys reach it every time. A key proves
    // where it went here because the click that opened the dropdown is on a row found by its
    // name box, at the column the dropdown is drawn in.
    const std::string down = "\xEF\x9C\x81"; // Key.DownArrow
    clickAt(window, kKindColumn, comboY);
    slint::platform::update_timers_and_animations();
    press(window, down);
    press(window, "\n");
    settle();
    const int picked = kindOf();
    {
        INFO("a click at " << kKindColumn << ", " << comboY << " and a step did not move the kind");
        REQUIRE(picked > 0);
    }

    // Then the gesture: opened, left open through ten redraws, and the same step taken.
    reset();
    REQUIRE(kindOf() == 0);
    clickAt(window, kKindColumn, comboY);
    for (int redraw = 0; redraw < 10; ++redraw) {
        settle();
    }
    press(window, down);
    press(window, "\n");
    settle();
    INFO("dropdown at " << kKindColumn << ", " << comboY);
    CHECK(kindOf() == picked);
}


TEST_CASE("an output the network refuses is named in the status line", "[ui]") {
    // Where the operator meets the audit's T3: a network that is down, or a cable that is
    // out, fails every send, and the window said nothing — the output just went quiet (M12).
    // The socket refuses every send to `kUnsendableHost`, the stand-in here, since no test can
    // pull a cable. Nothing is started: the outputs run from launch (H5), and the status has
    // to say so before anybody presses anything.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    takt4::output::OutputTarget deck;
    REQUIRE(takt4::output::parseOutputTarget(
        std::string("deck = ") + takt4::testing::kUnsendableHost + ":57000", deck));
    saved.preset.outputs = {deck};
    WindowController controller(tracker, saved);

    std::string status;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (status.find("deck") == std::string::npos && std::chrono::steady_clock::now() < until) {
        controller.tick();
        status = std::string(controller.window().get_status());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO(status);
    CHECK(status.find("deck: sends are failing") != std::string::npos);
    CHECK(status.find(takt4::testing::kUnsendableReason) != std::string::npos);
    CHECK(controller.statusIsError());
}

TEST_CASE("a click made while START is still opening the input does not stop it again",
          "[ui][hardware]") {
    // The audit's M23, with the pointer on the button. Opening a driver holds the window's
    // thread; a click made meanwhile is delivered once it is free, and pressed STOP on an input
    // that had only just opened. Here the second click comes before the press has been carried
    // out and a third straight after it has — the two moments such a click can arrive.
    LiveTracker tracker(kWeights, kStateSpace);
    if (!bestInputDevice(tracker)) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    constexpr float kWidth = 900.0f;
    constexpr float kHeight = 836.0f;
    constexpr float kButtonY = 31.0f;
    layOut(controller, kWidth, kHeight);
    auto& window = controller.window().window();
    const auto click = [&window](float x) {
        clickAt(window, x, kButtonY);
        slint::platform::update_timers_and_animations();
    };

    // The run button, found from the right along the top row by what a click on it does.
    float buttonX = -1.0f;
    for (float x = kWidth - 20.0f; x > kWidth - 220.0f && buttonX < 0.0f; x -= 8.0f) {
        click(x);
        if (controller.window().get_run_busy()) {
            buttonX = x;
        }
    }
    REQUIRE(buttonX > 0.0f);
    CHECK(std::string(controller.window().get_run_busy_text()) == "OPENING\xE2\x80\xA6");

    click(buttonX);
    waitUntil(controller, [&tracker] { return tracker.running(); });
    REQUIRE(tracker.running());
    // The redraw timer, overdue after a driver held the thread for seconds, can run before the
    // clicks queued meanwhile are delivered: they are still refused.
    controller.tick();
    click(buttonX);
    waitUntil(controller, [&controller] { return !controller.window().get_run_busy(); });
    CHECK(tracker.running());
    CHECK(controller.window().get_running());

    // Once it is over, the button is the operator's again, and stops it.
    click(buttonX);
    waitUntil(controller, [&tracker] { return !tracker.running(); });
    CHECK_FALSE(tracker.running());
    CHECK_FALSE(controller.window().get_running());
}

TEST_CASE("the TRIGGERS row counts the rules that would fire, as they are edited", "[ui]") {
    // Counted when the rules change rather than on every redraw (the audit's Low items), so the
    // count has to follow every way they change: a whole set arriving, and the editor.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::trigger::Rule::Config clips;
    clips.id = "clips";
    clips.address = "/clips";
    takt4::trigger::Rule::Config off = clips;
    off.id = "off";
    off.enabled = false;
    takt4::trigger::Rule::Config waiting;
    waiting.id = "waiting";
    waiting.sendKind = takt4::trigger::Message::Kind::MidiNote;
    waiting.numberChosen = false; // a note nobody has picked yet: cannot fire
    controller.setRules({clips, off, waiting});
    controller.tick();
    CHECK(controller.window().get_rules_active() == 1);
    CHECK(controller.window().get_rules_total() == 3);

    // Switched on in the editor, it counts; the one still waiting for a number does not.
    controller.editor().pick(1);
    controller.editor().setEnabled(true);
    controller.tick();
    CHECK(controller.window().get_rules_active() == 2);
    // One added there has no address yet, so it counts in the total and not as active — and
    // counts the moment it is given one.
    controller.editor().add();
    controller.tick();
    CHECK(controller.window().get_rules_total() == 4);
    CHECK(controller.window().get_rules_active() == 2);
    controller.editor().setAddress("/composition/layers/1/clear");
    controller.tick();
    CHECK(controller.window().get_rules_active() == 3);
}

TEST_CASE("what the output thread lost is said under the outputs heading", "[ui]") {
    // The audit's M12: every rule message that reached no output was counted, and the count was
    // shown nowhere. A MIDI rule on a rig with no MIDI output, fired with TEST, the way an
    // operator finds out their laser rule goes nowhere.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::trigger::Rule::Config lasers;
    lasers.id = "lasers";
    lasers.sendKind = takt4::trigger::Message::Kind::MidiNote;
    lasers.channel = 3;
    lasers.numberChosen = true;
    controller.setRules({lasers});
    controller.tick();
    CHECK(std::string(controller.window().get_output_trouble()).empty());

    controller.editor().pick(0);
    controller.editor().test();
    std::string trouble;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (trouble.empty() && std::chrono::steady_clock::now() < until) {
        controller.tick();
        trouble = std::string(controller.window().get_output_trouble());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO(trouble);
    CHECK(trouble == "1 rule message \xE2\x80\x94 reached no output");

    controller.editor().test();
    const auto again = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (trouble.rfind("1 ", 0) == 0 && std::chrono::steady_clock::now() < again) {
        controller.tick();
        trouble = std::string(controller.window().get_output_trouble());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(trouble == "2 rule messages \xE2\x80\x94 reached no output");
}


TEST_CASE("a window opens no taller than the screen has room for", "[ui]") {
    // The audit's M26. At 125 % a 1920 x 1080 screen with a 40-pixel taskbar has a work area of
    // 1536 x 832 logical pixels, and the main window asked for 934: its status bar and PANIC
    // opened under the taskbar. What a real small screen does cannot be seen from this desk;
    // this is the arithmetic that decides it.
    using takt4::ui::fitWithin;
    using takt4::ui::LogicalExtent;
    const LogicalExtent main{takt4::ui::kMainWindowWidth, takt4::ui::kMainWindowHeight};

    const LogicalExtent laptop = fitWithin(main, {1536.0f, 832.0f});
    CHECK(laptop.width == main.width);        // wide enough already
    CHECK(laptop.height == Catch::Approx(784.0)); // the work area less the title bar and frame
    CHECK(laptop.height + 48.0f <= 832.0f);

    // A screen with room to spare changes nothing, and one that cannot be asked neither.
    const LogicalExtent big = fitWithin(main, {2560.0f, 1400.0f});
    CHECK(big.width == main.width);
    CHECK(big.height == main.height);
    const LogicalExtent unknown = fitWithin(main, {0.0f, 0.0f});
    CHECK(unknown.width == main.width);
    CHECK(unknown.height == main.height);

    // And only the application asks the screen: a test process's windows keep the sizes their
    // layout assertions were written against, whatever screen the runner has.
    const LogicalExtent asked = takt4::ui::fitToScreen(main);
    CHECK(asked.width == main.width);
    CHECK(asked.height == main.height);
}

TEST_CASE("ABOUT opens the licence and the notices that are built in", "[ui]") {
    // The audit's licensing finding, the user's answer to its Q8: takt4 ships with no notices
    // for what is built into it, and GPLv3 §5(d) asks its interface to show them. takt4 is one
    // file, so they are inside it; the About box opens them. Driven by clicks — ABOUT in the
    // status line, then the About box's own buttons — with the file opener replaced, since a
    // test must not launch a text viewer.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    std::vector<std::filesystem::path> opened;
    controller.setFileOpener([&opened](const std::filesystem::path& path) {
        opened.push_back(path);
        return true;
    });
    constexpr float kWidth = 1000.0f;
    constexpr float kHeight = 900.0f;
    layOut(controller, kWidth, kHeight);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    settle();
    REQUIRE(controller.about() == nullptr);

    // ABOUT, found along the status line by what a click on it does.
    const float barY = kHeight - 17.0f;
    for (float x = kWidth - 480.0f; x < kWidth - 10.0f && controller.about() == nullptr; x += 6.0f) {
        clickAt(window, x, barY);
        settle();
    }
    REQUIRE(controller.about() != nullptr);
    AboutWindow& about = *controller.about();
    CHECK(about.window().is_visible());
    CHECK(std::string(about.get_version()) == takt4::versionLabel(takt4::buildInfo()));

    about.window().dispatch_scale_factor_change_event(1.0f);
    about.window().dispatch_resize_event(slint::LogicalSize({580.0f, 520.0f}));
    about.window().dispatch_window_active_changed_event(true);
    slint::platform::update_timers_and_animations();

    // Its two buttons, found by the file each one opens.
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    std::string licence;
    std::string notices;
    for (float y = 280.0f; y < 440.0f && (licence.empty() || notices.empty()); y += 6.0f) {
        for (float x = 30.0f; x < 560.0f && (licence.empty() || notices.empty()); x += 20.0f) {
            opened.clear();
            clickAt(about.window(), x, y);
            slint::platform::update_timers_and_animations();
            if (opened.size() != 1) {
                continue;
            }
            const std::string name = opened.front().filename().string();
            if (name == "takt4-LICENSE.txt") {
                licence = read(opened.front());
            } else if (name == "takt4-THIRD-PARTY-NOTICES.txt") {
                notices = read(opened.front());
            }
        }
    }
    INFO("licence " << licence.size() << " bytes, notices " << notices.size() << " bytes");
    REQUIRE_FALSE(licence.empty());
    REQUIRE_FALSE(notices.empty());
    // The texts themselves: the GPL, and the notices that name what is built in.
    CHECK(licence.find("GNU GENERAL PUBLIC LICENSE") != std::string::npos);
    CHECK(licence.find("Version 3, 29 June 2007") != std::string::npos);
    CHECK(notices.rfind("takt4 — third-party notices", 0) == 0);
    for (const char* name : {"PortAudio", "Ableton Link", "Slint", "Skia", "Steinberg ASIO SDK"}) {
        INFO(name);
        CHECK(notices.find(name) != std::string::npos);
    }
    CHECK(notices.find("ASIO is a trademark and software of Steinberg Media Technologies GmbH.") !=
          std::string::npos);
    CHECK(std::string(about.get_opened()).rfind("Opened ", 0) == 0);

    // And CLOSE puts it away.
    for (float y = 420.0f; y < 515.0f && about.window().is_visible(); y += 6.0f) {
        for (float x = 440.0f; x < 575.0f && about.window().is_visible(); x += 12.0f) {
            clickAt(about.window(), x, y);
            slint::platform::update_timers_and_animations();
        }
    }
    CHECK_FALSE(about.window().is_visible());
}

#if defined(_WIN32)
TEST_CASE("SAVE in a test process writes to a folder of its own, not over the rig's",
          "[ui][settings]") {
    // Settings live beside the program, and the test binaries are built into the same folder
    // as takt4.exe — where, on the rig, settings.json is a real show's. A window test whose
    // click landed on SAVE would have written over it. So every test process is given a
    // folder of its own (tests/support/crt_dialogs.cpp, `settings::settingsDirectory`), and
    // here SAVE is pressed on purpose.
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    const std::filesystem::path programs = std::filesystem::path(self).parent_path();
    const std::filesystem::path beside = programs / "settings.json";
    std::error_code code;
    const bool existed = std::filesystem::exists(beside, code);
    const auto stamp = existed ? std::filesystem::last_write_time(beside, code)
                               : std::filesystem::file_time_type{};

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    REQUIRE(controller.saveNow());

    const std::filesystem::path written = takt4::settings::settingsFile();
    INFO("saved to " << written.string());
    CHECK(std::filesystem::exists(written, code));
    CHECK(written.parent_path() != programs);
    // And the file beside the program is exactly as it was: still there if it was, not
    // written, and not created if it was not.
    CHECK(std::filesystem::exists(beside, code) == existed);
    if (existed) {
        CHECK(std::filesystem::last_write_time(beside, code) == stamp);
    }
    std::filesystem::remove_all(written.parent_path(), code);
}
#endif
