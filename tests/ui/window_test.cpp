#include "core/assets/embedded.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/rates.hpp"
#include "core/build_info.hpp"
#include "core/control/control_action.hpp"
#include "core/control/midi_binding.hpp"
#include "core/dmx/fixture.hpp"
#include "core/dmx/liberation.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/fixtures/fixture_library.hpp"
#include "core/fixtures/import.hpp"
#include "core/io/utf8.hpp"
#include "core/io/wav_file.hpp"
#include "core/output/osc_message.hpp"
#include "core/output/output_target.hpp"
#include "core/remote/remote.hpp"
#include "core/sandbox.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/rule.hpp"
#include "core/trigger/trigger_engine.hpp"
#include "ui/delete_guard.hpp"
#include "ui/file_dialog.hpp"
#include "ui/keys.hpp"
#include "ui/model_watch.hpp"
#include "ui/native_window.hpp"
#include "ui/nothing_real.hpp"
#include "ui/shot.hpp"
#include "ui/window_controller.hpp"
#include "ui/window_state.hpp"

#include "support/beatless.hpp"
#include "support/fake_grid_source.hpp"
#include "support/link_network.hpp"
#include "support/loopback_receiver.hpp"
#include "support/osc_messages.hpp"
#include "support/prolink_peer.hpp"
#include "support/scoped_env.hpp"
#include "support/temp_dir.hpp"
#include "support/this_machine.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <portaudio.h>
#include <slint-platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include "support/virtual_midi.hpp"
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

/// Waits up to five seconds for `done`, looking every couple of milliseconds.
template <typename Done>
bool waitFor(Done done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// RESCAN carried through: begun by `rescanDevices` or the button, finished by the redraws after
/// it once the drivers have answered.
void rescanned(WindowController& controller) {
    waitUntil(controller, [&] { return !controller.rescanning(); });
    REQUIRE_FALSE(controller.rescanning());
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
/// posted so far, and between two of its rounds. It runs for as long as the window exists, so
/// reading its rules or transports directly would race it.
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

/// The Weltformat-dark colours the window tests read off a render (weltformat.slint's `Wf`).
struct Rgb {
    std::uint8_t r, g, b;
};
constexpr Rgb kPage{0x0e, 0x0e, 0x0e};
constexpr Rgb kSheet{0x1b, 0x1b, 0x1a};
constexpr Rgb kEdge{0xef, 0xee, 0xe9};
constexpr Rgb kPanicRed{0xd8, 0x30, 0x1f};
constexpr Rgb kThumb{0xb5, 0x47, 0x3b};

bool is(const takt4::tests::Shot& shot, int x, int y, Rgb c) {
    return shot.is(x, y, c.r, c.g, c.b);
}

/// The window's sheets, top to bottom: runs of anything but the page's colour down x = 15, which
/// is inside every sheet's 10 px margin and clear of everything drawn on them. The status bar is
/// the page's colour and is not one of them: the seven are the top bar, the tempo, the activation,
/// the controls, the inputs, the outputs and the triggers row.
std::vector<std::pair<int, int>> sheetsDown(const takt4::tests::Shot& shot) {
    std::vector<std::pair<int, int>> sheets;
    int start = -1;
    for (int y = 0; y < shot.height; ++y) {
        const bool page = is(shot, 15, y, kPage);
        if (!page && start < 0) {
            start = y;
        } else if (page && start >= 0) {
            sheets.emplace_back(start, y - 1);
            start = -1;
        }
    }
    if (start >= 0) {
        sheets.emplace_back(start, shot.height - 1);
    }
    return sheets;
}

/// Runs of ink — anything visibly unlike `ground`, a box's dark fill included — along row y between
/// x0 and x1, joining runs whose gap is at most `join` pixels (a dashed edge, the letters of a
/// word).
std::vector<std::pair<int, int>> inkAlong(const takt4::tests::Shot& shot, int y, int x0, int x1,
                                          Rgb ground, int join) {
    std::vector<std::pair<int, int>> runs;
    for (int x = x0; x < x1; ++x) {
        const slint::Rgb8Pixel p = shot.at(x, y);
        const int d =
            std::abs(p.r - ground.r) + std::abs(p.g - ground.g) + std::abs(p.b - ground.b);
        if (d <= 12) {
            continue;
        }
        if (!runs.empty() && x - runs.back().second <= join + 1) {
            runs.back().second = x;
        } else {
            runs.emplace_back(x, x);
        }
    }
    return runs;
}

float middleOf(std::pair<int, int> run) {
    return static_cast<float>(run.first + run.second) / 2.0f;
}

/// What the controls in a row occupy: the columns between x0 and x1 where any pixel of rows y0 to
/// y1 is unlike `ground`, in runs, joining gaps of at most `join` pixels (the spaces between the
/// words of a label). A button's face is the sheet's own colour, so along one line it is two edges
/// and a label; projected over its height, its top and bottom edges make it one run.
std::vector<std::pair<int, int>> occupied(const takt4::tests::Shot& shot, int y0, int y1, int x0,
                                          int x1, Rgb ground, int join) {
    std::vector<std::pair<int, int>> runs;
    for (int x = x0; x < x1; ++x) {
        bool any = false;
        for (int y = y0; y <= y1 && !any; ++y) {
            const slint::Rgb8Pixel p = shot.at(x, y);
            any =
                std::abs(p.r - ground.r) + std::abs(p.g - ground.g) + std::abs(p.b - ground.b) > 12;
        }
        if (!any) {
            continue;
        }
        if (!runs.empty() && x - runs.back().second <= join + 1) {
            runs.back().second = x;
        } else {
            runs.emplace_back(x, x);
        }
    }
    return runs;
}

/// Runs as text, for a failure message: Catch cannot print a pair.
std::string spans(const std::vector<std::pair<int, int>>& runs) {
    std::string out;
    for (const auto& [a, b] : runs) {
        out += (out.empty() ? "" : " ") + std::to_string(a) + ".." + std::to_string(b);
    }
    return out;
}

/// The output rows' × buttons at the window's width: each a 24 px box with a 2 px off-white edge,
/// the last thing in its row. Found by its left edge, a run of off-white at least 20 px tall down
/// the column just inside it; nothing else in that column is that tall (the fold arrows' edges
/// cross it for 2 px). Between `from` and `to`, top and bottom of each.
std::vector<std::pair<int, int>> removeButtonsDown(const takt4::tests::Shot& shot, int from,
                                                   int to) {
    const int x = shot.width - 26 - 24; // the sheet's right padding, then the 24 px column
    std::vector<std::pair<int, int>> spans;
    int start = -1;
    for (int y = from; y < to; ++y) {
        const bool edge = is(shot, x, y, kEdge);
        if (edge && start < 0) {
            start = y;
        } else if (!edge && start >= 0) {
            if (y - start >= 20) {
                spans.emplace_back(start, y - 1);
            }
            start = -1;
        }
    }
    return spans;
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
    state.beatsPerBar = 3; // never 4 by assumption
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
    CHECK_FALSE(window->get_no_signal());
    CHECK_FALSE(window->get_acquired());
    state.acquired = true;
    takt4::ui::publishTempoState(*window, state);
    CHECK(window->get_acquired());

    // Nothing with a beat in it heard yet: no lock is taken, and the readout says why rather than
    // "hunting", which an operator reads as about to lock.
    window->set_running(true);
    state.locked = false;
    state.acquired = false;
    state.waitingForPulse = true;
    takt4::ui::publishTempoState(*window, state);
    CHECK(window->get_waiting_for_pulse());
    CHECK(std::string(window->get_lock_word()) == "no beat");
    // And no tempo, bar or beat beside it: those are the hunt's guesses at sound with no beat, and
    // a number, a lit dot and "bar 17" beside "no beat" said there was one.
    CHECK(window->get_bpm() == 0.0f);
    CHECK(window->get_called_bpm() == 0.0f);
    CHECK(window->get_beats_per_bar() == 0);
    CHECK(window->get_beat_in_bar() == 0);
    CHECK(window->get_bars() == 0);
    state.waitingForPulse = false;
    takt4::ui::publishTempoState(*window, state);
    CHECK(std::string(window->get_lock_word()) == "hunting");
    // Heard, the guesses are shown again, as they always were while hunting.
    CHECK_THAT(window->get_bpm(), WithinAbs(128.25, 1e-4));
    CHECK(window->get_beats_per_bar() == 3);
    CHECK(window->get_beat_in_bar() == 2);
    CHECK(window->get_bars() == 17);

    // A deck that stopped: the lock readout says "no signal" over the tempo it holds — and the
    // wait for a beat starts again behind it, which "no signal" is the truer word for.
    state.noSignal = true;
    state.waitingForPulse = true;
    takt4::ui::publishTempoState(*window, state);
    CHECK(window->get_no_signal());
    CHECK_FALSE(window->get_locked());
    CHECK(std::string(window->get_lock_word()) == "no signal");
    window->set_running(false);

    // Stopping must not leave the last set's tempo sitting there looking live.
    takt4::ui::publishIdleReadouts(*window);
    CHECK(window->get_bpm() == 0.0f);
    CHECK_FALSE(window->get_no_signal());
    CHECK_FALSE(window->get_locked());
    CHECK_FALSE(window->get_pinned()); // and the LOCK button is not still lit
    CHECK_FALSE(window->get_acquired());
    CHECK(window->get_beats_per_bar() == 0);
    CHECK(window->get_input_level() == 0.0f);
    CHECK(std::string(window->get_input_reading()).empty());
}

TEST_CASE("the beats' own rate is said whenever the outputs are live, locked or not",
          "[ui][relock]") {
    // The re-lock trap: a lock lost mid-track leaves the
    // number where the lock left it — 188 — while the dots and every output go at the record's
    // 94. The line that says so ("beats going out at ...") was drawn only while locked, so in
    // exactly that state nothing on screen said why. Rendered, and the line's amber looked for
    // where it is drawn: under the number, in the tempo sheet's left half.
    auto window = MainWindow::create();
    window->set_running(true);
    window->set_locked(false);
    window->set_bpm(187.7f);
    window->set_beats_bpm(93.8f);
    const auto amberUnderNumber = [&](bool acquired) {
        window->set_acquired(acquired);
        const takt4::tests::Shot shot = takt4::tests::render(*window, 900, 1300);
        const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
        REQUIRE(sheets.size() == 7);
        const int top = sheets[1].first; // the tempo sheet
        std::size_t amber = 0;
        for (int y = top + 64; y < top + 96 && y < shot.height; ++y) {
            for (int x = 15; x < shot.width / 2; ++x) {
                const slint::Rgb8Pixel p = shot.at(x, y);
                // Wf.holding, #ffb541, as text renders it: the glyphs' cores and their edges.
                if (p.r > 200 && p.g > 120 && p.g < 210 && p.b < 110) {
                    ++amber;
                }
            }
        }
        return amber;
    };
    CHECK(amberUnderNumber(true) > 40);
    // And not before the outputs are live: before the first lock nothing is going out, and the
    // number is the hunt's.
    CHECK(amberUnderNumber(false) == 0);
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

TEST_CASE("the status bar's corner says the version and, under it, release or dev", "[ui]") {
    // The corner is always two lines. The full label of a build between
    // releases — "0.9.9 (v0.9.9-1-g16611d86d-dirty)" — left the status line too narrow for either
    // of its lines; it is in the title bar.
    using takt4::ui::shortVersionLabel;
    CHECK(shortVersionLabel("0.9.9", "v0.9.9") == "0.9.9\nrelease");
    CHECK(shortVersionLabel("0.9.9", "v0.9.9-1-g16611d86d-dirty") == "0.9.9\ndev");
    CHECK(shortVersionLabel("0.9.9", "v0.9.9-dirty") == "0.9.9\ndev");
    CHECK(shortVersionLabel("0.9.10", "v0.9.1") == "0.9.10\ndev");
    CHECK(shortVersionLabel("0.9.9", "") == "0.9.9\ndev");

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const std::string corner(controller.window().get_version_short());
    CHECK(corner == shortVersionLabel(takt4::buildInfo().version, takt4::buildInfo().commit));
    CHECK(corner.find('\n') != std::string::npos);
}

TEST_CASE("a short window scrolls rather than losing its bottom row", "[ui]") {
    // Measured, not reasoned about: the window is rendered at each height and the pixels
    // at the bottom of it are read.
    //
    // 0.9.4 shipped with this broken: at the size the app opened at, with four outputs, the status
    // bar was not on screen at all and PANIC was cut in half, because the layout had nowhere to
    // put them and nothing to scroll. Both are pinned below the scroll view, so both hold at every
    // height down to the window's own floor.
    auto window = MainWindow::create();
    window->set_version(slint::SharedString("0.0.0-test"));
    window->set_status(slint::SharedString("In 7 of MOTU Pro Audio"));

    // Four of them, which is what makes the content taller than the window.
    auto targets = std::make_shared<slint::VectorModel<OutputRow>>();
    for (const char* name : {"deck", "wall", "winch", "lights"}) {
        OutputRow row{};
        row.name = slint::SharedString(name);
        row.host = slint::SharedString("192.168.1.40");
        row.port = slint::SharedString("7000");
        row.enabled = true;
        targets->push_back(row);
    }
    window->set_outputs_list(targets);

    // 420 is the window's own `min-height`; 1400 is taller than the content needs, which is the
    // case that must keep working exactly as it does with a scroll bar.
    constexpr int kWidth = 800;
    for (const int height : {1400, 934, 800, 700, 600, 500, 420}) {
        CAPTURE(height);
        const takt4::tests::Shot shot = takt4::tests::render(*window, kWidth, height);

        // The status bar: 51 px of the page's colour at the bottom, read at x = 4, left of where
        // its text starts.
        for (int y = height - 51; y < height; ++y) {
            CAPTURE(y);
            REQUIRE(is(shot, 4, y, kPage));
        }
        // Over it the triggers row: a 66 px sheet, 10 px in from the edge.
        for (int y = height - 51 - 66; y < height - 51; ++y) {
            CAPTURE(y);
            REQUIRE(is(shot, 15, y, kSheet));
        }
        // And PANIC is a whole button: 46 px of its red, in a column inside its right-hand end and
        // clear of its label. Cut in half, this read 23.
        int face = 0;
        for (int y = height - 51 - 10; y > height - 51 - 66; --y) {
            face += is(shot, kWidth - 36, y, kPanicRed) ? 1 : 0;
        }
        CHECK(face == 46);
    }
}

TEST_CASE("a mixed rig's output rows fit the window at its narrowest", "[ui]") {
    // Rendered, not reasoned about. At the window's width — 800, which is also its minimum —
    // with an Art-Net node and a MIDI device in the rig, every row's × is looked for, whole, at the
    // end of its row; and every column heading is held to being centred over the column it names,
    // since a row that merely fits can still put a box under the wrong heading.
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

    constexpr int kWidth = 800;   // MainWindow's min-width
    constexpr int kHeight = 1300; // tall enough that nothing scrolls
    const takt4::tests::Shot shot = takt4::tests::render(*window, kWidth, kHeight);
    const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
    REQUIRE(sheets.size() == 7);
    const std::vector<std::pair<int, int>> rows =
        removeButtonsDown(shot, sheets[5].first, sheets[5].second);
    // Four rows, four × buttons, whole.
    REQUIRE(rows.size() == 4);
    for (const auto& span : rows) {
        CHECK(span.second - span.first + 1 == 24);
    }

    // The headings' line: 8 px above the first row, 18 px tall — its middle.
    const int headingY = rows.front().first - 5 - 8 - 9;
    const std::vector<std::pair<int, int>> headings =
        inkAlong(shot, headingY, 20, kWidth - 20, kSheet, 7);
    INFO("headings at y=" << headingY << ": " << Catch::Detail::stringify(headings));
    // on, name, protocol, host / device, port, this output's own delay
    REQUIRE(headings.size() == 6);
    const auto checkRow = [&](std::size_t index, std::vector<std::size_t> columns) {
        const int middle = (rows[index].first + rows[index].second) / 2;
        const std::vector<std::pair<int, int>> boxes = inkAlong(shot, middle, 20, 505, kSheet, 2);
        INFO("row " << index << " at y=" << middle << ": " << Catch::Detail::stringify(boxes));
        REQUIRE(boxes.size() >= columns.size());
        for (std::size_t c = 0; c < columns.size(); ++c) {
            const std::size_t h = columns[c];
            INFO("column " << h << ": heading centred at " << middleOf(headings[h]) << ", box at "
                           << middleOf(boxes[c]));
            CHECK(std::abs(middleOf(boxes[c]) - middleOf(headings[h])) <= 3.0f);
        }
    };
    // The tick, the name, the protocol, the host and the port under their headings.
    checkRow(0, {0, 1, 2, 3, 4}); // an OSC row
    checkRow(3, {0, 1, 2, 3, 4}); // the Art-Net row
    checkRow(2, {0, 1, 2, 3});    // the MIDI row: its device where the host is, and no port
    // The delay heading over the slider and its reading, not over the × column.
    CHECK(std::abs(middleOf(headings[5]) - (kWidth - 26 - 24 - 6 - 239 / 2.0f)) <= 3.0f);
}

TEST_CASE("a delay slider follows its row after it has been dragged", "[ui]") {
    // A slider that sets its own value drops its binding the first time it is dragged, and after
    // one drag a delay typed into the reading beside it — or brought in by an import — moved the
    // reading and left the handle where the drag had put it. This one never sets its own value:
    // it says where the hand is, and shows its row. Read off the picture: the handle is the
    // brick-red run along the row's middle.
    auto window = MainWindow::create();
    auto targets = std::make_shared<slint::VectorModel<OutputRow>>();
    OutputRow deck{};
    deck.name = slint::SharedString("deck");
    deck.host = slint::SharedString("127.0.0.1");
    deck.port = slint::SharedString("7000");
    deck.enabled = true;
    targets->push_back(deck);
    window->set_outputs_list(targets);
    // The owner, as the window's controller is: what the slider says goes into the row.
    bool owner = true;
    std::vector<float> moved;
    window->on_output_delay_changed([&](int index, float ms) {
        moved.push_back(ms);
        if (owner) {
            OutputRow row = *targets->row_data(static_cast<std::size_t>(index));
            row.delay_ms = ms;
            targets->set_row_data(static_cast<std::size_t>(index), row);
        }
    });

    constexpr int kWidth = 800;
    constexpr int kHeight = 1300; // tall enough that nothing scrolls
    const auto handleOn = [&](const takt4::tests::Shot& shot, int y) {
        int first = -1;
        int last = -1;
        for (int x = 480; x < 700; ++x) {
            if (is(shot, x, y, kThumb)) {
                first = first < 0 ? x : first;
                last = x;
            }
        }
        return first < 0 ? -1.0f : static_cast<float>(first + last) / 2.0f;
    };
    const takt4::tests::Shot first = takt4::tests::render(*window, kWidth, kHeight);
    const std::vector<std::pair<int, int>> sheets = sheetsDown(first);
    REQUIRE(sheets.size() == 7);
    const std::vector<std::pair<int, int>> rows =
        removeButtonsDown(first, sheets[5].first, sheets[5].second);
    REQUIRE(rows.size() == 1);
    const int middle = (rows[0].first + rows[0].second) / 2;
    const float atZero = handleOn(first, middle);
    INFO("the handle at 0 ms is at x=" << atZero);
    REQUIRE(atZero > 0.0f);

    // Dragged well to the left by its handle: a press, a move, a release.
    const float y = static_cast<float>(middle);
    auto& handle = window->window();
    handle.dispatch_pointer_move_event(slint::LogicalPosition({atZero, y}));
    handle.dispatch_pointer_press_event(slint::LogicalPosition({atZero, y}),
                                        slint::PointerEventButton::Left);
    handle.dispatch_pointer_move_event(slint::LogicalPosition({atZero - 60.0f, y}));
    handle.dispatch_pointer_release_event(slint::LogicalPosition({atZero - 60.0f, y}),
                                          slint::PointerEventButton::Left);
    // Off the handle, which is drawn a lighter brick under the pointer.
    handle.dispatch_pointer_move_event(slint::LogicalPosition({5.0f, 5.0f}));
    REQUIRE_FALSE(moved.empty());
    REQUIRE(moved.back() < -300.0f);
    const float afterDrag = handleOn(takt4::tests::render(*window, kWidth, kHeight), middle);
    CHECK(afterDrag == Catch::Approx(atZero - 60.0f).margin(1.5f));

    // Then the row changes from outside — what a typed number or an import does — to well to the
    // right of zero. The handle follows.
    deck = *targets->row_data(0);
    deck.delay_ms = 500.0f;
    targets->set_row_data(0, deck);
    const float afterTyped = handleOn(takt4::tests::render(*window, kWidth, kHeight), middle);
    INFO("handle at " << afterDrag << " after the drag and " << afterTyped << " at +500 ms");
    // A quarter of the way along the 150 px the handle travels, from the middle.
    CHECK(afterTyped == Catch::Approx(atZero + 37.5f).margin(1.5f));

    // And with nobody writing it back, a drag moves nothing: the slider shows its row.
    owner = false;
    moved.clear();
    handle.dispatch_pointer_move_event(slint::LogicalPosition({afterTyped, y}));
    handle.dispatch_pointer_press_event(slint::LogicalPosition({afterTyped, y}),
                                        slint::PointerEventButton::Left);
    handle.dispatch_pointer_move_event(slint::LogicalPosition({afterTyped - 50.0f, y}));
    handle.dispatch_pointer_release_event(slint::LogicalPosition({afterTyped - 50.0f, y}),
                                          slint::PointerEventButton::Left);
    handle.dispatch_pointer_move_event(slint::LogicalPosition({5.0f, 5.0f}));
    CHECK_FALSE(moved.empty());
    CHECK(handleOn(takt4::tests::render(*window, kWidth, kHeight), middle) == afterTyped);
}

TEST_CASE("the wheel moves the body and leaves PANIC where it is", "[ui]") {
    // The other half of the scroll view, and the half that a picture of one height cannot
    // show: that the wheel actually scrolls, that what is pinned stays pinned, and — the
    // one most likely to break quietly — that a click after scrolling lands on the row that
    // is now under the pointer rather than the one that used to be there.
    auto window = MainWindow::create();
    window->set_status(slint::SharedString("scrolling"));

    auto targets = std::make_shared<slint::VectorModel<OutputRow>>();
    for (const char* name : {"deck", "wall", "winch", "lights"}) {
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

    constexpr int kWidth = 800;
    // Tall enough that the first rows are on screen before anything scrolls, and short enough
    // that the body still has to: the inputs sit above the outputs — the timing row, then MIDI
    // and OSC.
    constexpr int kHeight = 944;
    // The middle of the "×" at the end of each target row.
    constexpr int kRemoveX = kWidth - 26 - 12;
    // The pinned footer: 10 px of page, the 66 px triggers row, the 51 px status bar.
    constexpr int kFooter = 127;

    // The × buttons down their column, which is how a row is found without hardcoding a y that a
    // font change would move. Bounded above the footer, so PANIC is never taken for a row.
    const auto rowsDown = [&](const takt4::tests::Shot& shot) {
        return removeButtonsDown(shot, 400, shot.height - kFooter);
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

    // Now the wheel, over the middle of the body, and the pointer out of the window, as it was for
    // the first picture, so nothing is drawn hovered in one and not the other.
    window->window().dispatch_pointer_scroll_event(slint::LogicalPosition({500.0f, 300.0f}), 0.0f,
                                                   -120.0f);
    window->window().dispatch_pointer_exit_event();

    // The body moved — by how much, measured on the picture itself: the body scrolls as one
    // piece, so the distance is the shift that lays the new picture over the old one. Not the
    // distance between the first row found each time, which is the distance between two
    // different rows as soon as one has gone off the top. The scroll bar is left out: it is not
    // the body, and it moves its own way, a little down as the body goes a lot up (119 of these
    // pixels here).
    constexpr int kBar = 15;
    const auto mismatches = [&](const takt4::tests::Shot& after, int shift) {
        int count = 0;
        for (int y = 400; y + shift < kHeight - kFooter; y += 2) {
            for (int x = 0; x < kWidth - kBar; x += 4) {
                const slint::Rgb8Pixel a = before.at(x, y + shift);
                const slint::Rgb8Pixel b = after.at(x, y);
                count += (a.r != b.r || a.g != b.g || a.b != b.b) ? 1 : 0;
            }
        }
        return count;
    };
    // Slint animates a wheel scroll, so the timers run until two pictures in a row are the same —
    // the scroll has stopped, as the click below needs — for as long as a few seconds.
    const auto same = [](const takt4::tests::Shot& a, const takt4::tests::Shot& b) {
        for (std::size_t i = 0; i < a.pixels.size(); ++i) {
            const slint::Rgb8Pixel p = a.pixels[i];
            const slint::Rgb8Pixel q = b.pixels[i];
            if (p.r != q.r || p.g != q.g || p.b != q.b) {
                return false;
            }
        }
        return true;
    };
    takt4::tests::Shot after;
    takt4::tests::Shot last;
    int pictures = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true) {
        pumpTimers(std::chrono::milliseconds(50));
        after = takt4::tests::render(*window, kWidth, kHeight);
        ++pictures;
        if ((pictures > 1 && same(after, last)) || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        last = after;
    }
    int moved = 0;
    int fewest = std::numeric_limits<int>::max();
    for (int shift = 0; shift < 300; ++shift) {
        const int count = mismatches(after, shift);
        if (count < fewest) {
            fewest = count;
            moved = shift;
        }
    }
    // Not an exact match: where the scroll stops is Slint's, and on a busy runner it can stop a
    // fraction of a pixel off a whole one, when the edges of the glyphs come out a little
    // differently — measured on the Linux, macOS and Windows runners as 3 or 756 pixels, 0 or 95
    // of these samples, whether or not the pointer had been in the window. A body a whole pixel
    // out of place misses by 2,266 or more. Here the scroll stops on the pixel.
    CAPTURE(fewest, moved, pictures);
    CHECK(fewest < 200);
    CHECK(moved > 0);
    const auto rowsAfter = rowsDown(after);
    REQUIRE(!rowsAfter.empty());
    REQUIRE(rowsBefore.size() >= 2);
    // Which row is first in the column now. Row 0 is `rowsBefore.front()` — the click above
    // took 0 away — and the rows are a fixed pitch apart.
    const int pitch = rowsBefore[1].first - rowsBefore[0].first;
    REQUIRE(pitch > 0);
    const int shown = static_cast<int>(std::lround(
        static_cast<double>(rowsAfter.front().first + moved - rowsBefore.front().first) / pitch));
    CAPTURE(rowsBefore.front().first, rowsAfter.front().first, moved, pitch, shown);

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
        {static_cast<float>(kWidth - 36), static_cast<float>(kHeight - 51 - 10 - 23)});
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
    // **Which** row: the one drawn there now. This took any of the four, and a click landing on
    // whatever used to be there is one of them too.
    CHECK(removed.front() == shown);
}

TEST_CASE("a status longer than the bar keeps its first line and loses its end", "[ui]") {
    // The status text was centred in a bar two lines tall, so a message of more lines was laid
    // out from the middle and clipped at both ends: what failed — the first line — was never
    // drawn. The message here says which line is which in ink: a first line that starts with W's,
    // dense — four words of them, less than a line at this width — and many lines of full stops,
    // nearly no ink at all. The top line of the bar has to be the dense one.
    auto window = MainWindow::create();
    std::string message;
    for (int i = 0; i < 4; ++i) {
        message += "WWWWW ";
    }
    for (int i = 0; i < 1500; ++i) {
        message += ".    "; // spaced out: the line they fill is shorter since PRESETS came
    }
    constexpr int kWidth = 800;
    constexpr int kHeight = 760;
    constexpr int kBar = 51;
    // Ink: pixels unlike the bar's own colour, in the text's stretch of it, by row — x 16 to about
    // 376 at this width since PRESETS joined the buttons to its right (measured on a render).
    const auto inkByRow = [&](const takt4::tests::Shot& shot) {
        const slint::Rgb8Pixel bar = shot.at(4, kHeight - 2);
        std::vector<int> rows(kBar, 0);
        for (int y = kHeight - kBar; y < kHeight; ++y) {
            for (int x = 16; x < 340; ++x) {
                const slint::Rgb8Pixel p = shot.at(x, y);
                const int d = std::abs(p.r - bar.r) + std::abs(p.g - bar.g) + std::abs(p.b - bar.b);
                rows[static_cast<std::size_t>(y - (kHeight - kBar))] += d > 60 ? 1 : 0;
            }
        }
        return rows;
    };

    window->set_status(slint::SharedString(message));
    const std::vector<int> rows = inkByRow(takt4::tests::render(*window, kWidth, kHeight));
    int top = 0;
    int bottom = 0;
    for (int y = 0; y < kBar; ++y) {
        (y < kBar / 2 ? top : bottom) += rows[static_cast<std::size_t>(y)];
    }
    INFO("ink in the bar's top half " << top << ", bottom half " << bottom);
    CHECK(top > 3 * bottom); // the W's, above the full stops
    CHECK(bottom > 0);       // and a second line under them

    SECTION("and a message of one line sits in the middle of the bar, beside the version") {
        window->set_status(slint::SharedString("Ready."));
        const std::vector<int> one = inkByRow(takt4::tests::render(*window, kWidth, kHeight));
        double weighted = 0.0;
        int total = 0;
        for (int y = 0; y < kBar; ++y) {
            weighted += y * one[static_cast<std::size_t>(y)];
            total += one[static_cast<std::size_t>(y)];
        }
        REQUIRE(total > 0);
        const double centre = weighted / total;
        INFO("the line's ink centres " << centre << " px into a " << kBar << " px bar");
        CHECK(std::abs(centre - kBar / 2.0) < 3.0);
    }
}

TEST_CASE("the fold window reaches the window unchanged", "[ui]") {
    // The operator has to be able to see the octave-fold window, because outside it the fold
    // turns a right answer into a wrong one.
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
    // and not just the handler behind it. Pairs unless "mono" is ticked (since 2026-09-28): an
    // 18-input interface offers nine, and an odd input at the end is offered alone.
    for (const bool mono : {false, true}) {
        controller.window().invoke_input_mono_toggled(mono);
        for (std::size_t i = 0; i < tracker.devices().size(); ++i) {
            controller.window().invoke_device_picked(static_cast<int>(i));
            CHECK(controller.deviceIndex() == static_cast<int>(i));
            CHECK(controller.channelIndex() == 0); // a new device starts at its first input
            const auto channels = controller.window().get_channels();
            REQUIRE(channels);
            const int inputs = tracker.devices()[i].maxInputChannels;
            const int shown = mono || inputs < 2 ? inputs : (inputs + 1) / 2;
            INFO(tracker.devices()[i].name << ": " << inputs << " inputs, mono " << mono);
            CHECK(channels->row_count() == static_cast<std::size_t>(shown));
        }
    }

    // An index the list does not have is ignored rather than crashing: a stale click from
    // a picker that was rebuilt underneath it must not take the app down.
    const int before = controller.deviceIndex();
    controller.window().invoke_device_picked(9999);
    controller.window().invoke_device_picked(-1);
    CHECK(controller.deviceIndex() == before);
}

TEST_CASE("the redraw timer runs on its own", "[ui]") {
    // The engine is meant to be drained on a UI timer. That the timer exists is one claim and
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
    // carried out a frame later.
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

TEST_CASE("an import while listening leaves the same input running and moves to another one",
          "[ui][hardware][settings]") {
    // IMPORT restores the input since 2026-10-03. A file naming the input already open must not
    // interrupt it — importing a rig's own export mid-set — and one naming another must end up
    // listening to that one, as STOP, a pick and START would. The engine counts its runs, which
    // is what says whether the stream was opened again.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device || device->maxInputChannels < 2) {
        SKIP("no input device with two inputs on this machine");
    }
    WindowController controller(tracker);
    controller.window().invoke_toggle_run();
    waitUntil(controller, [&tracker] { return tracker.running(); });
    REQUIRE(tracker.running());
    waitUntil(controller, [&controller] { return !controller.window().get_run_busy(); });
    const auto runs = [&tracker] { return tracker.engine().runNumber(); };
    const auto run = runs();
    const takt4::test::TempDir dir;

    takt4::settings::Settings same = controller.currentSettings();
    const std::filesystem::path sameFile = dir.path() / "same.json";
    REQUIRE(takt4::settings::save(same, sameFile));
    REQUIRE(controller.importFrom(sameFile));
    CHECK(tracker.running());
    CHECK(runs() == run);

    takt4::settings::Settings other = same;
    other.machine.mono = !same.machine.mono;
    const std::filesystem::path otherFile = dir.path() / "other.json";
    REQUIRE(takt4::settings::save(other, otherFile));
    REQUIRE(controller.importFrom(otherFile));
    waitUntil(controller, [&tracker, &controller] {
        return tracker.running() && !controller.window().get_run_busy();
    });
    CHECK(tracker.running());
    CHECK(runs() == run + 1);
    CHECK(controller.window().get_input_mono() == other.machine.mono);
    CHECK(controller.currentSettings().machine.mono == other.machine.mono);
    // **And said**, first, and still said once the input has opened again: it said how many
    // rules it brought, and START's own line then wrote over that.
    const std::string moved(controller.window().get_status());
    INFO(moved);
    CHECK(moved.rfind("Now listening to ", 0) == 0);
    CHECK(moved.find("Imported other.json") != std::string::npos);
    CHECK_FALSE(controller.window().get_status_is_error());

    // One naming an input this machine has not got stops the show, and says that first.
    takt4::settings::Settings missing = other;
    missing.machine.deviceName = "No Such Interface";
    const std::filesystem::path missingFile = dir.path() / "missing.json";
    REQUIRE(takt4::settings::save(missing, missingFile));
    REQUIRE(controller.importFrom(missingFile));
    for (int i = 0; i < 5; ++i) {
        controller.tick();
        slint::platform::update_timers_and_animations();
    }
    CHECK_FALSE(tracker.running());
    const std::string stopped(controller.window().get_status());
    INFO(stopped);
    CHECK(stopped.rfind("Stopped listening: No Such Interface is not on this machine", 0) == 0);
    CHECK(controller.window().get_status_is_error());

    if (tracker.running()) {
        controller.window().invoke_toggle_run();
        waitUntil(controller, [&tracker] { return !tracker.running(); });
    }
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

TEST_CASE("the status a show controller is sent carries the audio callbacks' load",
          "[ui][hardware]") {
    // The worst callback of each second, as a share of its buffer's time, measured by the stream
    // on the real interface; and nothing once nothing is open.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    CHECK(controller.remoteStatus().load == 0.0);
    controller.toggleRun();
    REQUIRE(tracker.running());
    waitUntil(controller, [&controller] { return controller.remoteStatus().load > 0.0; });
    const double load = controller.remoteStatus().load;
    INFO("the worst callback of the second: " << load);
    CHECK(load > 0.0);
    // The callback only hands the audio on: anywhere near its buffer's time is a fault.
    CHECK(load < 0.5);
    controller.toggleRun();
    REQUIRE_FALSE(tracker.running());
    CHECK(controller.remoteStatus().load == 0.0);
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
// The tracker's manual controls. Every one of these drives the window's own callback and then
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

    // The excerpt tracks at about 128, inside the default 70-140 window, so folding leaves it
    // alone — and an octave shift is exactly a halving of *what is showing*, refinement and all,
    // not of the filter's own estimate.
    const double shown = tracker.engine().state().bpm;
    const Options fold = tracker.engine().tempoOptions();
    REQUIRE(shown >= fold.minBpm);
    REQUIRE(shown < fold.maxBpm);

    controller.window().invoke_halve();
    run.applyPosted();
    CHECK_THAT(tracker.engine().state().bpm, WithinAbs(shown / 2.0, 1e-9));
    CHECK(tracker.engine().state().locked);

    controller.window().invoke_redouble();
    run.applyPosted();
    CHECK_THAT(tracker.engine().state().bpm, WithinAbs(shown, 1e-9));
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
    // The control input's learn mode, end to end through the window's own callbacks. No hardware:
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

    SECTION("and while no port is open the row says so, not that the binding is live") {
        // Off, because no port is open — the reading has to say that rather than claim a
        // binding is live when nothing is listening. Named for what it can show: it was "it
        // shows on the row", and with no port open the reading never gets as far as the binding.
        // Showing one needs a port listening, which the test sandbox refuses.
        controller.tick();
        CHECK_FALSE(controller.window().get_learning());
        CHECK_FALSE(controller.window().get_control_on());
        const std::string reading(controller.window().get_control_reading());
        INFO(reading);
        CHECK(reading.find("note 36") == std::string::npos);
        CHECK((reading == "off — pick a port" || reading == "no MIDI inputs on this machine"));
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
    // the beat spacing, which the frames applied alongside may have refined a little further.
    const double before = tracker.engine().state().bpm;
    run.applyPosted();
    CHECK(tracker.engine().state().bpm > before * 0.9);

    // The same pad again is the control doing its job: exactly half of what is showing, the
    // refinement kept.
    const double shown = tracker.engine().state().bpm;
    CHECK(controller.control().dispatch(pad));
    run.applyPosted();
    CHECK_THAT(tracker.engine().state().bpm, WithinAbs(shown / 2.0, 1e-9));
}

TEST_CASE("a learned control reaches the rules through the window", "[ui][trigger]") {
    // The other destination. The control input's `panic` is a question for the rules rather
    // than for the tracker, so a binding to it has to arrive at the window's *output runner* —
    // which is a different object, on a different thread, behind a different queue.
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
    // operator arming a rig before a set does, and which since the outputs stopped waiting for
    // the tracker reaches a runner that is running.
    CHECK(controller.control().dispatch(pad));
    CHECK(panickedNow(controller));

    SECTION("and the OSC socket moves the same latch the pad just moved") {
        // The control input's other surface, wired into the window on 2026-09-06. The pad above
        // left panic engaged, and this releases it *from the socket* — which is the whole claim:
        // one runner, two surfaces, one latch. Two runners would show up here as a release that
        // does nothing.
        //
        // Driven through `dispatch` rather than a real datagram: the socket has its own
        // tests in tests/control, and nothing here needs one to exist.
        REQUIRE(panickedNow(controller));
        // Not with `panic 0`, which engages like every panic message, but with `panic/release`.
        CHECK(controller.oscControl().dispatch("/takt4/ctl/panic", 0.0));
        CHECK(panickedNow(controller));
        CHECK(controller.oscControl().dispatch("/takt4/ctl/panic/release", std::nullopt));
        CHECK_FALSE(panickedNow(controller));

        // And a bare `/ctl/panic` engages, because a panic button panics.
        CHECK(controller.oscControl().dispatch("/takt4/ctl/panic", std::nullopt));
        CHECK(panickedNow(controller));
    }

    SECTION("the picker offers panic but not the ones that would need a rule named") {
        // The control input's `rule/<id>/…` verbs cannot be armed from a gesture: pressing a pad
        // says which button, never which rule. The list is the actions minus those.
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
    // The rules are in the settings' portable half, and this is the whole path an operator
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
    // construction ("No input device. Connect an interface and press RESCAN."), which is
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
        // The trigger engine's policy: held, shown, and refused at fire time. The alternative —
        // dropping it on load — would delete the rule an operator is half-way through fixing.
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
    // The control input's listening socket, which nothing opened until 2026-09-06 — the library
    // existed and no application built one. Three separate decisions live here and each is the
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
        // The working socket was closed first and the new number tried after, so a typo — or a
        // port another program has — left the Stream Deck with nobody listening, mid-show. A
        // fixed port rather than 0 here, since 0 rebound would be a different "any free one" and
        // the test could not tell where it came back.
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
        // The other half: saving `running()` turned a port that was only briefly busy — another
        // copy of takt4 still closing — into OSC control switched off for good, with nothing at
        // the next launch to say so.
        const takt4::testing::LoopbackReceiver taken;
        takt4::settings::Settings settings;
        settings.machine.oscControlEnabled = true;
        settings.machine.oscControlPort = taken.port();
        // And a control surface left at home, so the launch meets two problems: both are said —
        // each used to write the one before it away.
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

namespace {

/// The row of the output with this id, as the window's model holds it.
std::optional<OutputRow> outputRow(WindowController& controller, const std::string& id) {
    const auto rows = controller.window().get_outputs_list();
    for (std::size_t i = 0; i < rows->row_count(); ++i) {
        if (const std::optional<OutputRow> row = rows->row_data(i); row && std::string(row->id) == id) {
            return row;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("an OSC control port another program holds stays asked for, says why, and is taken "
          "once it is free",
          "[ui]") {
    // What is saved is what was asked for, not whether it is running, so a port that cannot be
    // bound has to say so in the window rather than fail silently. It
    // was said once, on the status line, and the next message wrote it away; the tick went back
    // to empty as if it had never been asked for, while the file went on saving it as asked for;
    // and nothing tried the port again. In the sandbox's own terms: a port a receiver of this
    // test holds, which takt4 may bind and will find taken.
    std::optional<takt4::testing::LoopbackReceiver> holder;
    holder.emplace();
    const std::uint16_t port = holder->port();
    takt4::settings::Settings settings;
    settings.machine.oscControlEnabled = true;
    settings.machine.oscControlPort = port;
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker, settings);
    controller.tick();

    CHECK_FALSE(controller.oscControl().running());
    // The tick as it was put: what was asked for, and what is saved.
    CHECK(controller.window().get_osc_control_on());
    CHECK(controller.currentSettings().machine.oscControlEnabled);
    // And beside it, in red, why it is not listening.
    CHECK(controller.window().get_osc_control_error());
    const std::string failing(controller.window().get_osc_control_reading());
    INFO(failing);
    CHECK(failing.rfind("NOT LISTENING", 0) == 0);
    CHECK(failing.find("port " + std::to_string(port) + " is in use by another program") !=
          std::string::npos);

    // **Not said once**: the status line moves on to something else, redraws come and go, the
    // port is tried again and is still taken — and the line still says so, without the status
    // line being written over by every try.
    controller.setLatencyTyped("not a number");
    const std::string status(controller.window().get_status());
    controller.setControlRetrySeconds(0.02);
    pumpTimers(std::chrono::milliseconds(150));
    for (int i = 0; i < 10; ++i) {
        controller.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK_FALSE(controller.oscControl().running());
    CHECK(controller.window().get_osc_control_error());
    CHECK(std::string(controller.window().get_osc_control_reading()).rfind("NOT LISTENING", 0) == 0);
    CHECK(std::string(controller.window().get_status()) == status);

    SECTION("the other program lets go, and it listens with nobody touching the tick") {
        holder.reset();
        waitUntil(controller, [&] { return controller.oscControl().running(); });
        REQUIRE(controller.oscControl().running());
        CHECK(controller.oscControlPort() == port);
        CHECK_FALSE(controller.window().get_osc_control_error());
        const std::string reading(controller.window().get_osc_control_reading());
        INFO(reading);
        CHECK(reading.find("NOT LISTENING") == std::string::npos);
        const std::string said(controller.window().get_status());
        INFO(said);
        CHECK(said.find("OSC control listening on " + std::to_string(port)) != std::string::npos);
    }

    SECTION("unticked, the red line goes and nothing is tried again") {
        controller.window().invoke_osc_control_toggled(false);
        CHECK_FALSE(controller.window().get_osc_control_error());
        CHECK(std::string(controller.window().get_osc_control_reading()) == "off");
        // A word, so set in Archivo with the window's other states, not in DM Mono.
        CHECK(controller.window().get_osc_control_reading_words());
        CHECK_FALSE(controller.currentSettings().machine.oscControlEnabled);
        holder.reset(); // free now, and not asked for
        for (int i = 0; i < 10; ++i) {
            controller.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        CHECK_FALSE(controller.oscControl().running());
    }
}

TEST_CASE("opening the OSC control to other machines on a port another program holds on an address "
          "of its own leaves it listening on this machine, and says so",
          "[ui]") {
    // Every address cannot be had beside a socket on any one, so ticking "allow other machines"
    // closed the working socket and then failed — a Stream Deck left with nobody listening, as
    // a new port that would not bind once did.
    namespace peer = takt4::testing::prolink_peer;
    if (!peer::canUseSecondLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2");
    }
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    std::uint16_t free = 0;
    {
        const takt4::testing::LoopbackReceiver probe;
        free = probe.port();
    }
    controller.window().invoke_osc_control_port_edited(slint::SharedString(std::to_string(free)));
    controller.window().invoke_osc_control_toggled(true);
    REQUIRE(controller.oscControl().running());
    REQUIRE(controller.oscControlPort() == free);
    REQUIRE(controller.oscControl().config().localOnly);

    // Another program on the same port at 127.0.0.2: nothing takt4 sends itself reaches it.
    peer::UdpPeer holder(peer::kDevices, free);
    controller.window().invoke_osc_control_network_toggled(true);
    CHECK(controller.oscControl().running());
    CHECK(controller.oscControlPort() == free);
    CHECK(controller.oscControl().config().localOnly);
    CHECK_FALSE(controller.window().get_osc_control_network());
    CHECK(controller.currentSettings().machine.oscControlLocalOnly);
    CHECK(controller.statusIsError());
    const std::string status(controller.window().get_status());
    INFO(status);
    CHECK(status.find("Still listening on " + std::to_string(free) + " (this machine only).") !=
          std::string::npos);
}

TEST_CASE("a MIDI control input that is not here says so on its own line until it is", "[ui]") {
    // The same for the control input's MIDI half: a controller left at home, or held by a DAW
    // that opened every input it saw (a WinMM input is one program's at a time). Its line used
    // to read "off - pick a port" with a port picked.
    takt4::settings::Settings settings;
    settings.machine.midiControlPort = "takt4 test - a controller left at home";
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker, settings);
    controller.tick();

    CHECK_FALSE(controller.control().running());
    CHECK(controller.window().get_control_error());
    const std::string reading(controller.window().get_control_reading());
    INFO(reading);
    if (reading.find("no usable MIDI API") != std::string::npos) {
        // A machine with no MIDI system at all — a Linux runner has no ALSA sequencer.
        SKIP("no MIDI system on this machine: " << reading);
    }
    CHECK(reading.rfind("NOT OPEN", 0) == 0);
    CHECK(reading.find("\"takt4 test - a controller left at home\" is not on this machine") !=
          std::string::npos);
    // And the picker names it, as not plugged in — not "select input" beside a line about it.
    {
        const auto ports = controller.window().get_midi_in_ports();
        const int picked = controller.window().get_midi_in_port_index();
        REQUIRE(picked > 0);
        REQUIRE(static_cast<std::size_t>(picked) < ports->row_count());
        CHECK(std::string(*ports->row_data(static_cast<std::size_t>(picked))) ==
              "takt4 test - a controller left at home \xE2\x80\x94 not plugged in");
        // Picking it again asks for the same port, not for the one below or nothing.
        controller.pickMidiControlPort(picked);
        CHECK(controller.control().config().port == "takt4 test - a controller left at home");
    }

    // Still said after the status line has moved on and the port has been looked for again.
    controller.setLatencyTyped("not a number");
    const std::string status(controller.window().get_status());
    controller.setControlRetrySeconds(0.02);
    for (int i = 0; i < 10; ++i) {
        controller.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(controller.window().get_control_error());
    CHECK(std::string(controller.window().get_control_reading()).rfind("NOT OPEN", 0) == 0);
    CHECK(std::string(controller.window().get_status()) == status);
    // Asked for, so still saved as asked for.
    CHECK(controller.currentSettings().machine.midiControlPort ==
          "takt4 test - a controller left at home");

    // Nothing picked, nothing failing.
    controller.pickMidiControlPort(0);
    CHECK_FALSE(controller.window().get_control_error());
    const std::string off(controller.window().get_control_reading());
    CHECK((off == "off \xE2\x80\x94 pick a port" || off == "no MIDI inputs on this machine"));
    CHECK(controller.window().get_control_reading_words());
}

TEST_CASE("an output that reaches nothing says why on its own row, for as long as it does",
          "[ui]") {
    // The status line said "outputs: desk: no MIDI device called ..." once, and whatever it said
    // next wrote it away — so a row that reached nothing sat there looking like one that worked.
    takt4::output::OutputTarget desk;
    desk.id = "o-0000de5c";
    desk.name = "desk";
    desk.kind = takt4::output::OutputTarget::Kind::Midi;
    desk.device = "takt4 test - a desk left at home";
    takt4::settings::Settings settings;
    settings.preset.outputs = {desk};
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker, settings);
    waitUntil(controller, [&] {
        const std::optional<OutputRow> row = outputRow(controller, desk.id);
        return row && !std::string(row->problem).empty();
    });
    std::optional<OutputRow> row = outputRow(controller, desk.id);
    REQUIRE(row);
    const std::string problem(row->problem);
    INFO(problem);
    if (problem.find("no usable MIDI API") != std::string::npos) {
        SKIP("no MIDI system on this machine: " << problem); // a Linux runner has no ALSA
    }
    CHECK(problem.find("no MIDI device called \"takt4 test - a desk left at home\"") !=
          std::string::npos);

    // After the status line has said something else, and a redraw or two, still on the row.
    controller.setLatencyTyped("not a number");
    for (int i = 0; i < 5; ++i) {
        controller.tick();
    }
    row = outputRow(controller, desk.id);
    REQUIRE(row);
    CHECK(std::string(row->problem) == problem);
    // And counted, for the Outputs heading to say while the section is folded: a fold must not
    // leave it reading as if all were well. The desk is on; Link, brought in by a file that did not
    // name it, is off.
    const auto enabledRows = [&controller] {
        const auto rows = controller.window().get_outputs_list();
        int on = 0;
        for (std::size_t i = 0; i < rows->row_count(); ++i) {
            on += rows->row_data(i)->enabled ? 1 : 0;
        }
        return on;
    };
    CHECK(controller.window().get_outputs_failing() == 1);
    CHECK(controller.window().get_outputs_on() == 1);
    CHECK(controller.window().get_outputs_on() == enabledRows());

    SECTION("switched off, it is not failing, and says nothing") {
        std::size_t index = 0;
        const auto rows = controller.window().get_outputs_list();
        for (std::size_t i = 0; i < rows->row_count(); ++i) {
            if (std::string(rows->row_data(i)->id) == desk.id) {
                index = i;
            }
        }
        controller.setTargetEnabled(static_cast<int>(index), false);
        waitUntil(controller, [&] {
            const std::optional<OutputRow> now = outputRow(controller, desk.id);
            return now && std::string(now->problem).empty();
        });
        row = outputRow(controller, desk.id);
        REQUIRE(row);
        CHECK(std::string(row->problem).empty());
        CHECK(controller.window().get_outputs_failing() == 0);
        CHECK(controller.window().get_outputs_on() == 0);
        CHECK(controller.window().get_outputs_on() == enabledRows());
    }

    SECTION("a list of Link peers that cannot listen says so on the Link row") {
        // The test sandbox refuses Link's port, which is exactly what a port another program
        // holds exclusively looks like from here.
        controller.toggleLinkPeers();
        CHECK_FALSE(controller.linkPeersShown());
        std::optional<OutputRow> link;
        waitUntil(controller, [&] {
            const auto rows = controller.window().get_outputs_list();
            for (std::size_t i = 0; i < rows->row_count(); ++i) {
                if (rows->row_data(i)->kind_index ==
                    static_cast<int>(takt4::output::OutputTarget::Kind::Link)) {
                    link = rows->row_data(i);
                }
            }
            return link && !std::string(link->problem).empty();
        });
        REQUIRE(link);
        INFO(std::string(link->problem));
        CHECK(std::string(link->problem).rfind("cannot list peers: ", 0) == 0);
    }
}

TEST_CASE("a driver that does not answer leaves the window working, says so, and says when it is "
          "back",
          "[ui]") {
    // PortAudio's hosts wait with no limit for a driver, and one wedged there held the window's
    // own thread — PANIC included — with nothing to press. Here the drivers do not answer RESCAN:
    // a hook on the audio thread that waits until the test says.
    LiveTracker tracker(kWeights, kStateSpace);
    LiveTracker::Limits limits;
    limits.open = std::chrono::milliseconds(300);
    limits.stop = std::chrono::milliseconds(300);
    limits.rescan = std::chrono::milliseconds(300);
    limits.list = std::chrono::milliseconds(300);
    tracker.setLimits(limits);
    WindowController controller(tracker);
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    tracker.setAudioJobHook([released](const char* what) {
        if (std::string_view(what) == "rescan") {
            released.wait();
        }
    });

    // RESCAN waits for nothing: begun, said, and the window goes on.
    const auto pressed = std::chrono::steady_clock::now();
    controller.rescanDevices();
    CHECK(std::chrono::steady_clock::now() - pressed < std::chrono::milliseconds(200));
    CHECK(controller.rescanning());
    CHECK(std::string(controller.window().get_status()).find("Looking for devices") !=
          std::string::npos);
    // Until its limit, on the redraws after it — which is when the driver is let go of.
    rescanned(controller);
    CHECK(controller.statusIsError());
    {
        const std::string trouble(controller.window().get_input_trouble());
        INFO(trouble);
        CHECK(trouble.find("audio driver not answering since it was asked to rescan") !=
              std::string::npos);
        const std::string status(controller.window().get_status());
        INFO(status);
        CHECK(status.find("stopped answering") != std::string::npos);
    }

    // **The window goes on**: PANIC engages from its own button, and the redraw runs.
    const std::uint64_t ticks = controller.ticks();
    controller.window().invoke_panic_clicked();
    CHECK(panickedNow(controller));
    controller.tick();
    CHECK(controller.ticks() > ticks);
    controller.releasePanic();

    // START asks nothing of the driver while it is held, and says why at once.
    if (!controller.devices().empty()) {
        const auto started = std::chrono::steady_clock::now();
        controller.toggleRun();
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(200));
        CHECK_FALSE(controller.wantsRunning());
        const std::string status(controller.window().get_status());
        INFO(status);
        CHECK(status.find("has not answered since it was asked to rescan") != std::string::npos);
    }

    // The drivers answer: said, and the line under the meter goes.
    release.set_value();
    waitUntil(controller, [&] { return !tracker.stuck(); });
    REQUIRE_FALSE(tracker.stuck());
    controller.tick();
    CHECK(std::string(controller.window().get_input_trouble()).empty());
    const std::string back(controller.window().get_status());
    INFO(back);
    CHECK(back.find("answering again") != std::string::npos);
}

TEST_CASE("the channel picker offers stereo pairs unless mono is ticked, and START opens what it "
          "shows",
          "[ui]") {
    // 2026-09-28: stereo unless asked otherwise. Measured over the 23 electronic tracks and
    // GiantSteps' 664, the average of a feed's two sides tracked as well as the better side
    // alone, better on downbeats, and never worse than either — and it is what the model was
    // trained on. So the picker offers the pairs an interface numbers its inputs in, and a
    // "mono" tick offers single inputs for a feed that is on one.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    int at = -1;
    for (std::size_t i = 0; i < controller.devices().size(); ++i) {
        if (controller.devices()[i].maxInputChannels >= 2 &&
            (at < 0 || controller.devices()[i].maxInputChannels >
                           controller.devices()[static_cast<std::size_t>(at)].maxInputChannels)) {
            at = static_cast<int>(i);
        }
    }
    if (at < 0) {
        SKIP("no device on this machine has two inputs");
    }
    controller.pickDevice(at);
    const InputDevice& device = controller.devices()[static_cast<std::size_t>(at)];
    INFO(device.name << ", " << device.maxInputChannels << " inputs");

    CHECK_FALSE(controller.mono());
    CHECK_FALSE(controller.window().get_input_mono());
    auto channels = controller.window().get_channels();
    CHECK(channels->row_count() == static_cast<std::size_t>((device.maxInputChannels + 1) / 2));
    CHECK(std::string(*channels->row_data(0)).rfind("In 1 + 2", 0) == 0);
    takt4::audio::ChannelSelection opened = controller.selection();
    CHECK(opened.count == 2);
    CHECK(opened.channels[0] == 0);
    CHECK(opened.channels[1] == 1);

    // Ticked: the inputs one by one, the pair's first kept.
    controller.window().invoke_input_mono_toggled(true);
    CHECK(controller.mono());
    channels = controller.window().get_channels();
    CHECK(channels->row_count() == static_cast<std::size_t>(device.maxInputChannels));
    CHECK(controller.window().get_channel_index() == 0);
    opened = controller.selection();
    CHECK(opened.count == 1);
    CHECK(opened.channels[0] == 0);
    CHECK(controller.currentSettings().machine.mono);

    // Input 2 alone, and back to stereo: the pair it is in.
    controller.window().invoke_channel_picked(1);
    CHECK(controller.channelIndex() == 1);
    controller.window().invoke_input_mono_toggled(false);
    CHECK(controller.window().get_channel_index() == 0);
    opened = controller.selection();
    CHECK(opened.count == 2);
    CHECK(opened.channels[0] == 0);
    CHECK(opened.channels[1] == 1);
    CHECK_FALSE(controller.currentSettings().machine.mono);

    if (device.maxInputChannels >= 4) {
        // The second pair is inputs 3 and 4.
        controller.window().invoke_channel_picked(1);
        opened = controller.selection();
        CHECK(opened.channels[0] == 2);
        CHECK(opened.channels[1] == 3);
    }
}

TEST_CASE("a stereo pair out of phase is said under the meter and on the status line", "[ui]") {
    // The failure an average has that one side has not: a leg wired backwards cancels the kick
    // and the bass. Measured with one side flipped: beat F 0.84 to 0.70, downbeat F 0.66 to 0.43.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    InputDevice device;
    device.name = "takt4 test stereo";
    device.hostApi = takt4::audio::HostApiKind::Wasapi;
    device.maxInputChannels = 12;
    double now = 100.0;
    controller.assumeRunningOn({device, takt4::audio::ChannelSelection::pair(10, 11)}, now);
    // Past the first seconds: until then what the window met starting is held on the status
    // line and anything else joins it (`report`) — a CI runner, with no audio input at all, starts
    // with one. SAVE says something of its own, which ends the hold, as anything the operator
    // does would. The held case has a test of its own below.
    controller.window().invoke_save_now();

    takt4::audio::StereoSums sums;
    const auto play = [&](double seconds, double both) {
        for (double t = 0.0; t < seconds; t += 0.033) {
            sums.frames += 1584;
            sums.left += 1584 * 0.01;
            sums.right += 1584 * 0.01;
            sums.both += 1584 * both;
            now += 0.033;
            controller.superviseStereo(sums, now);
            controller.superviseInput({}, {}, now);
        }
    };
    play(4.0, 0.009); // in phase, correlation 0.9
    CHECK(std::string(controller.window().get_input_trouble()).empty());
    const std::string before(controller.window().get_status());
    const bool beforeError = controller.statusIsError();

    play(4.0, -0.009); // a leg flipped
    const std::string trouble(controller.window().get_input_trouble());
    INFO(trouble);
    CHECK(trouble.find("In 11 and In 12 are out of phase") != std::string::npos);
    // What to do before why: under the meter the end of it was cut off, and it was the remedy —
    // which named mono, locked while the input runs.
    const std::size_t remedy = trouble.find("stop, then tick mono");
    CHECK(remedy != std::string::npos);
    CHECK(remedy < trouble.find("cancel the kick"));
    const std::string status(controller.window().get_status());
    INFO(status);
    CHECK(status.find("out of phase") != std::string::npos);
    CHECK(controller.statusIsError());

    // Wired back: the line goes — and so does the warning on the status line, which said it when
    // it started. It stayed there, amber, until something else was said, so a rig that had been
    // put right went on reading "out of phase" until its input was stopped and started again.
    play(4.0, 0.009);
    CHECK(std::string(controller.window().get_input_trouble()).empty());
    const std::string after(controller.window().get_status());
    INFO(after);
    CHECK(after.find("out of phase") == std::string::npos);
    CHECK(after.find("In 11 and In 12 are fine again") != std::string::npos);
    CHECK_FALSE(controller.statusIsError());

    // And "fine again" does not stick either: after ten seconds of the pair staying fine, the
    // status line has back what it said before the warning.
    play(5.0, 0.009);
    CHECK(std::string(controller.window().get_status()).find("are fine again") !=
          std::string::npos);
    play(6.0, 0.009);
    CHECK(std::string(controller.window().get_status()) == before);
    CHECK(controller.statusIsError() == beforeError);
}

TEST_CASE("a stereo warning that has passed leaves the status line, and only its own", "[ui]") {
    // Two sides that share nothing, then music: the "share almost nothing" warning goes from the
    // status line as it goes from under the meter. But a status line that has said something
    // else since is the operator's news, not the warning's, and is left as it is.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    InputDevice device;
    device.name = "takt4 test stereo";
    device.hostApi = takt4::audio::HostApiKind::Wasapi;
    device.maxInputChannels = 2;
    double now = 100.0;
    controller.assumeRunningOn({device, takt4::audio::ChannelSelection::pair(0, 1)}, now);
    // Past the first seconds: until then what the window met starting is held on the status
    // line and anything else joins it (`report`) — a CI runner, with no audio input at all, starts
    // with one. SAVE says something of its own, which ends the hold, as anything the operator
    // does would. The held case has a test of its own below.
    controller.window().invoke_save_now();

    takt4::audio::StereoSums sums;
    const auto play = [&](double seconds, double both) {
        for (double t = 0.0; t < seconds; t += 0.033) {
            sums.frames += 1584;
            sums.left += 1584 * 0.01;
            sums.right += 1584 * 0.01;
            sums.both += 1584 * both;
            now += 0.033;
            controller.superviseStereo(sums, now);
            controller.superviseInput({}, {}, now);
        }
    };
    play(10.0, 0.0); // nothing in common
    CHECK(std::string(controller.window().get_status()).find("share almost nothing") !=
          std::string::npos);
    play(10.0, 0.009); // the music, on both
    const std::string cleared(controller.window().get_status());
    INFO(cleared);
    CHECK(cleared.find("share almost nothing") == std::string::npos);
    CHECK_FALSE(controller.statusIsError());

    // Again, with news in between: that stays.
    play(10.0, 0.0);
    REQUIRE(std::string(controller.window().get_status()).find("share almost nothing") !=
            std::string::npos);
    controller.window().invoke_save_now();
    const std::string news(controller.window().get_status());
    REQUIRE(news.find("share almost nothing") == std::string::npos);
    play(10.0, 0.009);
    CHECK(std::string(controller.window().get_status()) == news);
}

TEST_CASE("a stereo warning in the first seconds joins what the window met starting, and leaves it",
          "[ui]") {
    // In the first seconds what the window met starting is held on the status line, and what it
    // finds out on its own joins it rather than replacing it (`report`). A stereo warning then is
    // joined to it, and taken back out of it when it passes — the held notice stays, alone.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    InputDevice device;
    device.name = "takt4 test stereo";
    device.hostApi = takt4::audio::HostApiKind::Wasapi;
    device.maxInputChannels = 2;
    double now = 100.0;
    controller.assumeRunningOn({device, takt4::audio::ChannelSelection::pair(0, 1)}, now);
    controller.showNotice("A notice held at startup.");
    const std::string held(controller.window().get_status());
    REQUIRE(held.find("A notice held at startup.") != std::string::npos);

    takt4::audio::StereoSums sums;
    const auto play = [&](double seconds, double both) {
        for (double t = 0.0; t < seconds; t += 0.033) {
            sums.frames += 1584;
            sums.left += 1584 * 0.01;
            sums.right += 1584 * 0.01;
            sums.both += 1584 * both;
            now += 0.033;
            controller.superviseStereo(sums, now);
            controller.superviseInput({}, {}, now);
        }
    };
    play(4.0, 0.009);
    play(4.0, -0.009); // a leg flipped
    const std::string joined(controller.window().get_status());
    INFO(joined);
    CHECK(joined.find("A notice held at startup.") != std::string::npos);
    CHECK(joined.find("are out of phase") != std::string::npos);
    play(4.0, 0.009); // wired back
    CHECK(std::string(controller.window().get_status()) == held);
}

TEST_CASE("no beat draws no number, no dots and no bar, where hunting draws its guesses",
          "[ui]") {
    // The picture itself, not the properties: the same guesses drawn twice, waiting for a beat and
    // hunting. The number is a dash, and the row the dots, the meter and the bar sit on is empty.
    auto window = MainWindow::create();
    window->set_running(true);
    TempoState state;
    state.bpm = 74.07;
    state.calledBpm = 74.07;
    state.confidence = 0.57;
    state.beatsPerBar = 4;
    state.beatInBar = 4;
    state.bars = 13;
    const auto drawn = [&](bool waiting) {
        state.waitingForPulse = waiting;
        takt4::ui::publishTempoState(*window, state);
        return takt4::tests::render(*window, 800, 934);
    };
    const takt4::tests::Shot hunting = drawn(false);
    const takt4::tests::Shot waiting = drawn(true);
    const std::vector<std::pair<int, int>> sheets = sheetsDown(hunting);
    REQUIRE(sheets.size() >= 2);
    REQUIRE(sheetsDown(waiting)[1] == sheets[1]);
    const int top = sheets[1].first; // the tempo panel
    // How many pixels are drawn bright: the panel is black under a grid of #0f0f0f and #1e1e1e
    // lines, and the words, the number and the dots are all far brighter than those.
    const auto bright = [](const takt4::tests::Shot& shot, int y0, int y1, int x0, int x1) {
        int count = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const slint::Rgb8Pixel p = shot.at(x, y);
                count += std::max({p.r, p.g, p.b}) > 0x50 ? 1 : 0;
            }
        }
        return count;
    };
    // The row of dots, the meter and the bar, right of the number and under the confidence.
    const auto dots = [&](const takt4::tests::Shot& shot) {
        return bright(shot, top + 60, top + 86, 390, 780);
    };
    // The number and its "bpm": five big digits, or a dash.
    const auto number = [&](const takt4::tests::Shot& shot) {
        return bright(shot, top + 10, top + 70, 24, 380);
    };
    INFO("hunting: dots " << dots(hunting) << " px, number " << number(hunting) << " px");
    INFO("waiting: dots " << dots(waiting) << " px, number " << number(waiting) << " px");
    CHECK(dots(hunting) > 100);
    CHECK(dots(waiting) == 0);
    CHECK(number(waiting) > 0);
    CHECK(number(waiting) * 4 < number(hunting));
}

TEST_CASE("the plot marks no beat called while it says no beat, and every one once one is heard",
          "[ui]") {
    // The hunt still calls beats while takt4 waits for one, in sound with no beat in it, and the
    // plot lit them as "beat called" beside "no beat". The trace is built here as the window and
    // takt4-shot build it (`appendToTrace`), from the real engine's frames — half of them the
    // engine's own between the network's, whose beats are carried to the next column: loud pink
    // noise with the wait; the same noise without it, the tracker as it was, whose calls are drawn;
    // and music, drawn from the moment a beat is heard. Then the plot itself, drawn.
    static const takt4::model::ModelWeights weights =
        takt4::model::ModelWeights::fromFile(kWeights);
    static const takt4::tracking::StateSpaceModel space =
        takt4::tracking::StateSpaceModel::fromFile(kStateSpace);
    struct Drawn {
        std::size_t calledWaiting = 0; // beats the hunt called while it said no beat
        std::size_t calledHeard = 0;   // and once a beat was heard
        std::size_t marked = 0;        // columns drawn as a beat called
        std::vector<TracePoint> trace = std::vector<TracePoint>(takt4::ui::kTraceLength);
    };
    const auto draw = [](const std::vector<float>& sound, bool waitForPulse) {
        takt4::engine::BeatEngine::Options options;
        options.waitForPulse = waitForPulse;
        const auto engine = std::make_unique<takt4::engine::BeatEngine>(weights, space, options);
        Drawn drawn;
        bool pending = false;
        takt4::engine::EngineFrame frame;
        takt4::engine::EngineBeat beat;
        const std::size_t hops = sound.size() / takt4::audio::kHopSize;
        for (std::size_t h = 0; h < hops; ++h) {
            engine->processHop(sound.data() + h * takt4::audio::kHopSize, h);
            (void)engine->step();
            while (engine->popFrame(frame)) {
                if (frame.beat && frame.state.waitingForPulse) {
                    ++drawn.calledWaiting;
                } else if (frame.beat) {
                    ++drawn.calledHeard;
                }
                if (takt4::ui::appendToTrace(drawn.trace, pending, frame) &&
                    drawn.trace.back().called) {
                    ++drawn.marked;
                }
            }
            while (engine->popBeat(beat)) {
            }
        }
        if (pending) {
            ++drawn.marked; // a beat on the last frame, still waiting for its column
        }
        return drawn;
    };

    const std::vector<float> noise =
        takt4::testing::beatless(takt4::testing::Beatless::PinkNoise, 12.0);
    const Drawn waiting = draw(noise, true);
    INFO("pink noise, waiting: " << waiting.calledWaiting << " called saying no beat, "
                                 << waiting.calledHeard << " after, " << waiting.marked
                                 << " drawn");
    REQUIRE(waiting.calledHeard == 0);  // no beat is heard in noise
    REQUIRE(waiting.calledWaiting > 0); // but the hunt calls them
    CHECK(waiting.marked == 0);

    const Drawn unwaiting = draw(noise, false);
    INFO("pink noise, no wait: " << unwaiting.calledHeard << " called, " << unwaiting.marked
                                 << " drawn");
    CHECK(unwaiting.calledWaiting == 0);
    CHECK(unwaiting.calledHeard > 0);
    CHECK(unwaiting.marked == unwaiting.calledHeard);

    const takt4::io::WavData music = takt4::io::readWavFile(
        std::filesystem::path(TAKT4_TEST_DATA_DIR) / "features" / "synthetic.wav");
    const Drawn heard = draw(music.samples, true);
    INFO("music: " << heard.calledWaiting << " called before a beat was heard, "
                   << heard.calledHeard << " after, " << heard.marked << " drawn");
    CHECK(heard.calledHeard > 0);
    CHECK(heard.marked == heard.calledHeard);

    // On screen: the plot's columns lit #efeee9 for a beat called, inside the plot's #111111.
    auto window = MainWindow::create();
    const auto litInPlot = [&window](const std::vector<TracePoint>& trace) {
        window->set_trace(std::make_shared<slint::VectorModel<TracePoint>>(trace));
        const takt4::tests::Shot shot = takt4::tests::render(*window, 800, 934);
        const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
        REQUIRE(sheets.size() >= 3);
        const Rgb plot{0x11, 0x11, 0x11};
        int x0 = shot.width;
        int x1 = -1;
        int y0 = shot.height;
        int y1 = -1;
        for (int y = sheets[2].first; y <= sheets[2].second; ++y) {
            for (int x = 0; x < shot.width; ++x) {
                if (is(shot, x, y, plot)) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
            }
        }
        REQUIRE(x1 - x0 > 400); // the plot, found
        REQUIRE(y1 - y0 > 40);
        int lit = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                lit += is(shot, x, y, kEdge) ? 1 : 0;
            }
        }
        return lit;
    };
    const int litWaiting = litInPlot(waiting.trace);
    const int litUnwaiting = litInPlot(unwaiting.trace);
    INFO("lit in the plot: waiting " << litWaiting << " px, without the wait " << litUnwaiting
                                     << " px");
    CHECK(litWaiting == 0);
    CHECK(litUnwaiting > 100);
}

TEST_CASE("a beat fired before a silence is not lit again while it waits for a beat", "[ui]") {
    // The dots follow the beats the output thread fired, and keep the last one lit between them.
    // After a silence takt4 waits for a beat again, and that beat — fired before the silence —
    // was lit again over "no beat", with its bar, while the tracker's own guesses were not.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    using Shown = takt4::output::OutputRunner::ShownBeat;
    Shown shown;
    shown.serial = 1; // a beat fired, its time come
    shown.beatInBar = 3;
    shown.bars = 9;
    controller.setShownBeatSource([&shown] { return shown; });
    const auto redraw = [&controller](const takt4::tracking::TempoState& drawn) {
        takt4::ui::publishTempoState(controller.window(), drawn);
        controller.publishBeatDots(drawn);
    };

    TempoState state;
    state.bpm = 128.0;
    state.locked = true;
    state.beatsPerBar = 4;
    state.beatInBar = 2;
    state.bars = 9;
    redraw(state);
    REQUIRE(controller.window().get_beat_in_bar() == 3); // the one fired, not the one heard
    REQUIRE(controller.window().get_bars() == 9);

    // A silence, and sound back with no beat yet heard in it.
    state.locked = false;
    state.waitingForPulse = true;
    redraw(state);
    CHECK(controller.window().get_beat_in_bar() == 0);
    CHECK(controller.window().get_bars() == 0);
    CHECK(controller.window().get_beats_per_bar() == 0);

    // Heard again, and a new beat fired: that one is lit.
    state.waitingForPulse = false;
    shown.serial = 2;
    shown.beatInBar = 1;
    shown.bars = 1;
    redraw(state);
    CHECK(controller.window().get_beat_in_bar() == 1);
    CHECK(controller.window().get_bars() == 1);
}

TEST_CASE("the beat dots move when the rig plays the beat, not when the window hears of it",
          "[ui]") {
    // The red dots looked about 50 ms behind outputs that were on time.
    // They moved when the tracker's state showed a beat, a detection and a redraw after its
    // moment, while every output fires at the moment plus the latency. Now they follow the beats
    // the output thread fires, on its clock: a beat fired ahead of its time is shown at that time,
    // by a timer, and not before; one whose time has come is shown at once.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);
    REQUIRE(run.untilLocked());
    const takt4::tracking::TempoState state = tracker.engine().state();
    REQUIRE(state.beatInBar >= 1);
    using Shown = takt4::output::OutputRunner::ShownBeat;
    Shown shown; // nothing fired yet: the dots are the state's
    controller.setShownBeatSource([&shown] { return shown; });
    // What the window's redraw does: the state, then the dots over it.
    const auto redraw = [&controller](const takt4::tracking::TempoState& drawn) {
        takt4::ui::publishTempoState(controller.window(), drawn);
        controller.publishBeatDots(drawn);
    };
    redraw(state);
    const int heard = controller.window().get_beat_in_bar();
    CHECK(heard == static_cast<int>(state.beatInBar));

    const auto next = [](int beat) { return beat % 4 + 1; };
    const auto runnerNow = [&controller] { return controller.outputs().elapsed(); };
    // Redraws, as the window's timer makes them, for `seconds` of wall clock.
    const auto redrawFor = [&](double seconds) {
        const double until = runnerNow() + seconds;
        while (runnerNow() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            slint::platform::update_timers_and_animations();
            redraw(state);
        }
    };

    // Fired 300 ms before the rig plays it.
    const double firedAt = runnerNow();
    shown = Shown{firedAt, firedAt + 0.3, static_cast<std::uint32_t>(next(heard)), 42, 1};
    redraw(state);
    CHECK(controller.window().get_beat_in_bar() == heard); // not yet
    redrawFor(0.2);
    CHECK(controller.window().get_beat_in_bar() == heard); // nor on the redraws before its time
    redrawFor(0.15);
    CHECK(controller.window().get_beat_in_bar() == next(heard)); // and then, by its own timer
    CHECK(controller.window().get_bars() == 42);

    // Its time already come — a beat heard late, before the tempo locks: shown at once.
    const double lateAt = runnerNow();
    shown = Shown{lateAt - 0.1, lateAt - 0.05, static_cast<std::uint32_t>(next(next(heard))), 43, 2};
    redraw(state);
    CHECK(controller.window().get_beat_in_bar() == next(next(heard)));

    // A state with no beat — stopped, reset — clears them; and what was fired before is not
    // brought back by the next state that has a beat: the state's shows until another fires.
    redraw(takt4::tracking::TempoState{});
    CHECK(controller.window().get_beat_in_bar() == 0);
    redraw(state);
    CHECK(controller.window().get_beat_in_bar() == heard);
}

TEST_CASE("a settings file from before the mono tick opens on the stereo pair and says so",
          "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    std::optional<InputDevice> multi;
    for (const InputDevice& device : tracker.devices()) {
        if (device.maxInputChannels >= 2) {
            multi = device;
            break;
        }
    }
    if (!multi) {
        SKIP("no device on this machine has two inputs");
    }
    // What `settings::load` makes of a file with a channel and no "mono" (settings_test.cpp).
    takt4::settings::Settings old;
    old.machine.deviceName = multi->name;
    old.machine.hostApiName = multi->hostApiName;
    old.machine.channel = 1;
    old.machine.stereoFromMono = true;
    WindowController controller(tracker, old);
    const takt4::audio::ChannelSelection opened = controller.selection();
    CHECK(opened.count == 2);
    CHECK(opened.channels[0] == 0);
    CHECK(opened.channels[1] == 1);
    const std::string status(controller.window().get_status());
    INFO(status);
    CHECK(status.find("Listening to In 1 + 2 as a stereo pair now") != std::string::npos);
    CHECK(status.find("Tick mono to go back to In 2 alone") != std::string::npos);
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

    // A tap *seeds*. The window moves onto an octave centred on
    // the tapped tempo — 120/sqrt(2) to 120*sqrt(2) — and the tracker keeps tracking.
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
    // runs for as long as the window does — a test taking beats off it too would be a second
    // consumer, and each would see half of them.
    const std::uint64_t pressedAt = tracker.engine().state().beats;
    REQUIRE(run.until([&] { return tracker.engine().state().beats > pressedAt; }));
    CHECK(tracker.engine().state().beats == pressedAt + 1);
    CHECK(tracker.engine().state().beatInBar == 2);
}

TEST_CASE("the downbeat button says it has been pressed", "[ui]") {
    // The downbeat button never seemed to do anything: nothing changed. The snap itself was
    // working — tests/engine proves it through
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
    // inbound OSC control will be able to.
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
    // The slider posted to the tracker, which applies the offset only to a timestamp nothing in
    // the app reads; the transports took theirs once, at construction. So dragging it moved
    // nothing on Link, the MIDI clock or OSC that night, and the value saved took effect at the
    // *next* launch.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    REQUIRE(controller.outputs().transports().latencySeconds() == 0.0);

    controller.window().invoke_latency_changed(-40.0f);
    CHECK_THAT(controller.outputs().transports().latencySeconds(), WithinAbs(-0.040, 1e-9));
    controller.window().invoke_latency_changed(25.0f);
    CHECK_THAT(controller.outputs().transports().latencySeconds(), WithinAbs(0.025, 1e-9));
}

TEST_CASE("keep for the next track reaches the tracker and the settings file", "[ui]") {
    // Whether a ÷2 or ×2 carries into the next track, set through the controller. The click on
    // the box itself is tested below with the other gestures.
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
// The outputs row. The window owns the output thread now, so these check the path
// from a control to the transports — and that the window stopped draining the beat ring,
// which is the runner's to consume.
// ---------------------------------------------------------------------------------------

TEST_CASE("the test binary never opens a file dialog", "[ui]") {
    // See `ui::fileDialogsAllowed`. These tests drive real clicks, and on 2026-09-25 a sweep of
    // them aimed at where one control used to be landed on EXPORT instead, and opened a real
    // Save dialog on the rig's desktop, again and again. REQUIRE, so that a broken guard stops
    // here rather than going on to press EXPORT itself.
    REQUIRE_FALSE(takt4::ui::fileDialogsAllowed());
    takt4::ui::FileDialogs dialogs;
    int answers = 0;
    const auto cancelled = [&answers](const takt4::ui::FileChoice& choice) {
        ++answers;
        CHECK(choice.path.empty());
        CHECK(choice.problem.empty()); // a cancel, not a problem to report
    };
    dialogs.save("takt4 test", "never.json", cancelled);
    dialogs.open("takt4 test", takt4::ui::FileKind::Preset, cancelled);
    REQUIRE(answers == 2); // at once: nothing was shown to wait for
    CHECK_FALSE(dialogs.waiting());

    // And EXPORT and IMPORT pressed are a cancel: nothing written, nothing said.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const std::string before(controller.window().get_status());
    controller.window().invoke_export_settings();
    controller.window().invoke_import_settings();
    CHECK(std::string(controller.window().get_status()) == before);
}

TEST_CASE("the window comes up sending nothing", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    // Running — the outputs live as long as the window does — and with nothing to send, and
    // the tracker not listening.
    CHECK(controller.outputs().running());
    CHECK_FALSE(controller.outputs().tracking());
    const OutputsSeen live = seen(controller);
    CHECK_FALSE(live.link);
    CHECK(live.oscTargets == 0);
    CHECK_FALSE(live.midiClock);
    // One output, and it is the Link row every set has, switched off.
    REQUIRE(live.targets.size() == 1);
    CHECK(live.targets[0].kind == takt4::output::OutputTarget::Kind::Link);
    CHECK_FALSE(live.targets[0].enabled);
    const auto rows = controller.window().get_outputs_list();
    REQUIRE(rows->row_count() == 1);
    CHECK(rows->row_data(0)->kind_index == 4);
    CHECK_FALSE(rows->row_data(0)->enabled);

    // A row's kinds: the four a row can be switched between — Link is the one row it is.
    const auto kinds = controller.window().get_output_kinds();
    REQUIRE(kinds->row_count() == 4);
    CHECK(std::string(*kinds->row_data(3)) == "MIDI clock");
    // A device dropdown offers "none" first, and says so in words rather than being an empty
    // entry, which reads as a box the application failed to fill in.
    const auto devices = controller.window().get_output_devices();
    REQUIRE(devices->row_count() == controller.midiPorts().size() + 1);
    CHECK(std::string(*devices->row_data(0)) == (controller.midiPorts().empty()
                                                     ? "no MIDI outputs on this machine"
                                                     : "select a MIDI device"));
}

TEST_CASE("the Link row's tick reaches the transports", "[ui][network]") {
    takt4::testing::skipWhileLinkIsOnTheNetwork();
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    // The tick box on the first row, as the markup sends it.
    controller.window().invoke_output_enabled_changed(0, true);
    CHECK(seen(controller).link);
    CHECK(controller.window().get_outputs_list()->row_data(0)->enabled);
    // **Joined on its own switch, whether or not the tracker is listening.** Joining publishes
    // nothing: takt4 sends Link a tempo and a phase only from a locked beat, so a peer is not
    // handed anything until there is one.
    CHECK(controller.outputs().transports().link().enabled());
    CHECK_FALSE(controller.outputs().tracking());

    controller.window().invoke_output_enabled_changed(0, false);
    CHECK_FALSE(seen(controller).link);
    CHECK_FALSE(controller.outputs().transports().link().enabled());

    // And it is never removed: there is no × on its row, and asking changes nothing — not its
    // id and not its delay, which a Link row put back in its place would not keep.
    controller.setTargetDelay(0, 25.0f);
    const std::string id = seen(controller).targets[0].id;
    controller.removeTarget(0);
    REQUIRE(seen(controller).targets.size() == 1);
    CHECK(seen(controller).targets[0].kind == takt4::output::OutputTarget::Kind::Link);
    CHECK(seen(controller).targets[0].id == id);
    CHECK(seen(controller).targets[0].delaySeconds == Catch::Approx(0.025));
    // Nor switched to another kind, nor another row switched to it.
    controller.setTargetKind(0, 0);
    CHECK(controller.window().get_outputs_list()->row_data(0)->kind_index == 4);
    controller.addTarget();
    controller.setTargetKind(1, 4);
    CHECK(controller.window().get_outputs_list()->row_data(1)->kind_index == 0);
}

#if defined(_WIN32)
namespace {

void clickAt(slint::Window& window, float x, float y); // below, with the other gestures

/// Every message a virtual MIDI port has been sent, whole — teVirtualMIDI's callback, on the
/// driver's thread.
struct Heard {
    std::mutex mutex;
    std::vector<std::vector<unsigned char>> messages;

    std::vector<std::vector<unsigned char>> copy() {
        const std::lock_guard<std::mutex> lock(mutex);
        return messages;
    }
};

void CALLBACK hearMidi(takt4::testing::VirtualMidi::Port, LPBYTE data, DWORD length,
                       DWORD_PTR instance) {
    auto* const heard = reinterpret_cast<Heard*>(instance);
    if (heard == nullptr || data == nullptr || length == 0) {
        return;
    }
    const std::lock_guard<std::mutex> lock(heard->mutex);
    heard->messages.emplace_back(data, data + length);
}

} // namespace

TEST_CASE("unticking a MIDI output sends the note off it still owes, on a real port",
          "[ui][trigger][hardware]") {
    // A MIDI output unticked closed at once, so it never sent the note off it still owed, and
    // the device kept playing that note. End to end on a
    // real MIDI port: a virtual one this test makes (teVirtualMIDI), picked on a row of the main
    // window, a rule's note on sent down it by the rule editor's TEST, and the row's "on" box
    // clicked off with the note off a minute from due. The note off must arrive before RtMidi
    // closes the port. (The core half, every kind of leaving, is output_runner_test's "an output
    // switched off or deleted is sent what it is owed first".)
    takt4::testing::VirtualMidi virtualMidi;
    if (!virtualMidi.usable()) {
        SKIP("teVirtualMIDI is not installed here (rtpMIDI and loopMIDI both bring it)");
    }
    Heard heard;
    const takt4::testing::VirtualMidi::Port port =
        virtualMidi.create(L"takt4 test note off", &hearMidi, reinterpret_cast<DWORD_PTR>(&heard),
                           65535, takt4::testing::VirtualMidi::kParseRx);
    REQUIRE(port != nullptr);

    {
        LiveTracker tracker(kWeights, kStateSpace);
        WindowController controller(tracker);
        // The port is on the device list, which is read when the window is made.
        const auto devices = controller.window().get_output_devices();
        int device = -1;
        for (std::size_t i = 0; i < devices->row_count(); ++i) {
            if (std::string(*devices->row_data(i)).rfind("takt4 test note off", 0) == 0) {
                device = static_cast<int>(i);
            }
        }
        INFO("MIDI outputs: " << Catch::Detail::stringify(controller.midiPorts()));
        REQUIRE(device > 0);
        controller.addTarget();
        controller.setTargetDevice(1, device);
        REQUIRE(seen(controller).targets.size() == 2);
        REQUIRE(seen(controller).targets[1].kind == takt4::output::OutputTarget::Kind::Midi);
        REQUIRE(seen(controller).targets[1].enabled);

        takt4::trigger::Rule::Config laser;
        laser.id = "laser";
        laser.name = "laser";
        laser.trigger = takt4::trigger::Trigger::Manual;
        laser.sendKind = takt4::trigger::Message::Kind::MidiNote;
        laser.number.kind = takt4::trigger::GeneratorKind::Fixed;
        laser.number.fixed = takt4::trigger::Value::ofInt(60);
        laser.value.kind = takt4::trigger::GeneratorKind::Fixed;
        laser.value.fixed = takt4::trigger::Value::ofInt(100);
        takt4::trigger::FollowUp release;
        release.unit = takt4::trigger::DelayUnit::Milliseconds;
        release.delaySeconds = 60.0; // a minute: only the untick can pay it out within the test
        laser.followUps = {release};
        controller.setRules({laser});
        controller.editor().pick(0);
        controller.editor().test();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (heard.copy().empty() && std::chrono::steady_clock::now() < until) {
            controller.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const std::vector<std::vector<unsigned char>> on{{0x90, 60, 100}};
        REQUIRE(heard.copy() == on);

        // The row's "on" box, clicked: 41 px in, on the row under the Link row — the places the
        // sweep of every control ("every control in the main window does what it says") uses.
        constexpr int kWidth = 800;
        const takt4::tests::Shot shot = takt4::tests::render(controller.window(), kWidth, 1200);
        auto& window = controller.window().window();
        window.dispatch_window_active_changed_event(true);
        const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
        REQUIRE(sheets.size() == 7);
        const int linkRow = sheets[5].first + 10 + 28 + 8 + 18 + 8 + 14;
        const int midiRow = linkRow + 14 + 8 + 17;
        clickAt(window, 41.0f, static_cast<float>(midiRow));
        slint::platform::update_timers_and_animations();
        REQUIRE_FALSE(seen(controller).targets[1].enabled);

        // The note off, on the port, straight away rather than in a minute — and nothing after it.
        const std::vector<std::vector<unsigned char>> onAndOff{{0x90, 60, 100}, {0x80, 60, 0}};
        const auto off = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (heard.copy().size() < 2 && std::chrono::steady_clock::now() < off) {
            controller.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(heard.copy() == onAndOff);
    }
    virtualMidi.close(port);
}
#endif

TEST_CASE("SHOW PEERS lists a Link peer in another process, never takt4 itself", "[ui][network]") {
    takt4::testing::skipWhileLinkIsOnTheNetwork();
    // The list under the Link row, end to end: Link switched on from its row, SHOW PEERS, and
    // a real Link peer — two sessions in another process, the core suite's hidden "two Link
    // sessions for another process to list" — listed at its tempo, while takt4's own Link,
    // announcing on the same network, is not.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.window().invoke_output_enabled_changed(0, true);
    REQUIRE(seen(controller).link);
    controller.window().invoke_link_peers_toggled();
    REQUIRE(controller.linkPeersShown());
    CHECK(controller.window().get_link_peers_shown());
    const auto peers = controller.window().get_link_peer_list();
    const auto rounds = [&controller](int count) {
        for (int i = 0; i < count; ++i) {
            controller.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };

    // **Only the peers on this machine count** — see support/this_machine.hpp: the rig's network
    // has Link sessions of its own, which failed this test on 2026-09-30 by being listed,
    // correctly, and say nothing about takt4 or the peer process below.
    const std::vector<std::string> ours = takt4::testing::thisMachinesAddresses();
    const auto onThisMachine = [&peers, &ours] {
        std::size_t count = 0;
        for (std::size_t i = 0; i < peers->row_count(); ++i) {
            const std::string shown(peers->row_data(i)->address);
            if (takt4::testing::onThisMachine(shown, ours)) {
                ++count;
            }
        }
        return count;
    };
    const auto listed = [&peers] {
        std::string text;
        for (std::size_t i = 0; i < peers->row_count(); ++i) {
            const auto peer = *peers->row_data(i);
            text += std::string(peer.address) + " at " + std::string(peer.tempo) + "; ";
        }
        return text;
    };

    // Two seconds of takt4's own announcements, a few a second: none of them is a peer.
    rounds(40);
    INFO("listed: " << listed());
    CHECK(onThisMachine() == 0);

#if defined(_WIN32)
    // The peer: takt4_tests, built beside this binary, running its two Link sessions.
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    const std::filesystem::path other =
        std::filesystem::path(self).parent_path() / "takt4_tests.exe";
    if (!std::filesystem::exists(other)) {
        SKIP("takt4_tests.exe is not beside this binary");
    }
    const std::string command = "\"\"" + other.string() +
                                "\" \"two Link sessions for another process to list\" >NUL 2>&1\"";
    // Which that test waits for: the network has been checked clear, and this is the one asking.
    const takt4::test::ScopedVariable peerFor("TAKT4_TEST_LINK_PEER", "1");
    std::thread peer([command] { (void)std::system(command.c_str()); });
    std::size_t most = 0;
    std::string tempo;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < until && most == 0) {
        rounds(1);
        most = std::max<std::size_t>(most, onThisMachine());
        if (peers->row_count() > 0) {
            tempo = std::string(peers->row_data(0)->tempo);
        }
    }
    peer.join();
    INFO("tempo shown: " << tempo << "; listed: " << listed());
    CHECK(most >= 1);
    CHECK(tempo.find("BPM") != std::string::npos);
#endif

    // Switched off, there is no session to list, and the list closes with it.
    controller.window().invoke_output_enabled_changed(0, false);
    controller.tick();
    CHECK_FALSE(controller.linkPeersShown());
    CHECK_FALSE(controller.window().get_link_peers_shown());

    // And HIDE PEERS closes it too.
    controller.window().invoke_output_enabled_changed(0, true);
    controller.window().invoke_link_peers_toggled();
    REQUIRE(controller.linkPeersShown());
    controller.window().invoke_link_peers_toggled();
    CHECK_FALSE(controller.linkPeersShown());
    controller.window().invoke_output_enabled_changed(0, false);
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
    REQUIRE(live().targets.size() == 2); // after the Link row, which a line of text leaves alone
    CHECK(live().targets[0].kind == takt4::output::OutputTarget::Kind::Link);
    CHECK(live().targets[1].name == "127.0.0.1:57000");

    // Several targets at once, each with a name a rule can use. One piece of text still holds
    // several — a settings line, or a rig pasted in.
    controller.setOscTargets("deck = 127.0.0.1:57000, wall = 127.0.0.1:57001");
    CHECK(live().oscTargets == 2);
    REQUIRE(live().targets.size() == 3);
    CHECK(live().targets[1].name == "deck");
    CHECK(live().targets[2].name == "wall");

    SECTION("an Art-Net row asks for a host and a port, and switching to it puts 6454 in") {
        // Every node is fed every universe the patch uses since 2026-09-25, so the row is the
        // same shape as an OSC one — and the port is Art-Net's own, put in when it is picked.
        controller.setTargetKind(1, static_cast<int>(takt4::output::OutputTarget::Kind::ArtNet));
        controller.setTargetHost(1, "192.0.2.10", true);
        REQUIRE(live().targets.size() == 3);
        CHECK(live().targets[1].kind == takt4::output::OutputTarget::Kind::ArtNet);
        CHECK(live().targets[1].host == "192.0.2.10");
        CHECK(live().targets[1].port == takt4::dmx::kArtNetPort);
        CHECK_FALSE(controller.statusIsError());
    }

    SECTION("a line an older build wrote, with a universe list, is still one target") {
        // A universe list in a pasted line or an old file is read and left out — and its commas
        // are still the list's, not new targets, so the row is not refused.
        controller.setOscTargets("node = artnet 192.0.2.10:6454 u0,1,4 +50ms, wall = 127.0.0.1:57001");
        REQUIRE(live().targets.size() == 3);
        CHECK(live().targets[1].name == "node");
        CHECK(live().targets[1].kind == takt4::output::OutputTarget::Kind::ArtNet);
        CHECK(live().targets[1].delaySeconds == Catch::Approx(0.05));
        CHECK(live().targets[2].name == "wall");
        CHECK_FALSE(controller.statusIsError());
    }

    SECTION("and a line holding several becomes a row each, name and address apart") {
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 3);
        CHECK(rows->row_data(0)->kind_index == 4);
        CHECK(std::string(rows->row_data(1)->name) == "deck");
        CHECK(std::string(rows->row_data(1)->address) == "127.0.0.1:57000");
        CHECK(std::string(rows->row_data(2)->name) == "wall");
        CHECK(std::string(rows->row_data(2)->address) == "127.0.0.1:57001");
    }

    SECTION("a target named after its own address leaves the name box empty") {
        // It is still called "127.0.0.1:57000" and a rule can still route to it by that; the
        // box the operator types a *name* into is not where to say so.
        controller.setOscTargets("127.0.0.1:57000");
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 2);
        CHECK(std::string(rows->row_data(1)->name).empty());
        CHECK(live().targets[1].name == "127.0.0.1:57000");
    }

    SECTION("each target carries its own delay, and only its own") {
        // Per-output latency: a device with a lag of its own — a winch, which has to move — is
        // offset on its own. The single latency slider moves the whole rig's timeline together,
        // which is the one adjustment a rig with two different lags in it cannot use.
        controller.setTargetDelay(2, 352.0f);
        REQUIRE(live().targets.size() == 3);
        CHECK(live().targets[1].delaySeconds == 0.0);
        CHECK(live().targets[2].delaySeconds == Catch::Approx(0.352));

        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 3);
        CHECK(rows->row_data(1)->delay_ms == Catch::Approx(0.0f));
        CHECK(rows->row_data(2)->delay_ms == Catch::Approx(352.0f));

        // The address box never shows it — the slider is where it lives, and a delay
        // appearing in the text an operator is typing into would be edited by accident.
        CHECK(std::string(rows->row_data(2)->address) == "127.0.0.1:57001");

        // Past either limit is clamped rather than refused: a slider cannot get there, but
        // inbound OSC control and a hand-edited settings file both can.
        controller.setTargetDelay(2, 5000.0f);
        CHECK(live().targets[2].delaySeconds ==
              Catch::Approx(takt4::output::kMaxOutputDelaySeconds));
        controller.setTargetDelay(2, -5000.0f);
        CHECK(live().targets[2].delaySeconds ==
              Catch::Approx(takt4::output::kMinOutputDelaySeconds));

        // Negative is a real setting now, not a clamp to zero: "this device is 300 ms slow"
        // is the sentence an operator says, and the publisher turns it into a wait.
        controller.setTargetDelay(2, -300.0f);
        CHECK(live().targets[2].delaySeconds == Catch::Approx(-0.300));
        CHECK(rows->row_data(2)->delay_ms == Catch::Approx(-300.0f));

        // And it survives the rest of the row being edited, which is what would break if the
        // delay were carried in the address text rather than beside it.
        controller.setTargetDelay(2, 120.0f);
        controller.setTargetEnabled(2, false);
        controller.setTargetEnabled(2, true);
        CHECK(live().targets[2].delaySeconds == Catch::Approx(0.12));
    }

    SECTION("dragging a delay slider does not rebuild the slider being dragged") {
        // The same fault as the trigger editor's text boxes, in the control shipped for a
        // device's own latency. `setTargetDelay` applies as it moves so the offset can be
        // found by ear, and applying republished the rows — which reset the model, which made
        // the repeater destroy and rebuild the row, which took the slider out from under the
        // pointer. The drag would have ended on its first pixel of movement.
        const auto rows = controller.window().get_outputs_list();
        const auto watch = std::make_shared<ModelWatch>();
        rows->attach_peer(watch);

        // One drag, sixty steps of it — with a redraw between each, which is where a rebuild
        // would happen: they are queued (`staleTargetRows_`) and done in `tick`, so without one
        // this could not have seen one.
        for (int step = 0; step <= 60; ++step) {
            controller.setTargetDelay(2, static_cast<float>(step) * 5.0f);
            controller.tick();
        }
        CHECK(watch->resets == 0);
        CHECK(watch->added == 0);
        CHECK(watch->removed == 0);
        // One row written per step, and never the row that was not being dragged.
        CHECK(watch->changes == 60);
        CHECK(rows->row_data(2)->delay_ms == Catch::Approx(300.0f));
        CHECK(rows->row_data(1)->delay_ms == Catch::Approx(0.0f));

        // A step that lands where the slider already is writes nothing at all, so a slider
        // resending its own position cannot churn the row it lives in.
        const int settled = watch->changes;
        controller.setTargetDelay(2, 300.0f);
        CHECK(watch->changes == settled);
    }

    SECTION("a switched-off target is kept and sends nothing") {
        controller.setTargetEnabled(2, false);
        REQUIRE(live().targets.size() == 3);
        CHECK_FALSE(live().targets[2].enabled);
        // Held in the list, so it can be switched back on — but no socket behind it.
        CHECK(live().oscTargets == 1);
        CHECK_FALSE(controller.window().get_outputs_list()->row_data(2)->enabled);

        controller.setTargetEnabled(2, true);
        CHECK(live().oscTargets == 2);
    }

    SECTION("two targets with one name is said rather than silently resolved") {
        // `resolveOutputs` would take the first, and a rule routed to the second would go
        // somewhere its operator did not choose.
        controller.acceptTarget(2, "deck", "127.0.0.1:57001");
        CHECK(controller.statusIsError());
        // And said as what it is: both addresses are fine, and calling one of them "not a
        // target" would send somebody looking at the wrong thing.
        const std::string status(controller.window().get_status());
        INFO(status);
        CHECK(status.find("two are called") != std::string::npos);
    }

    SECTION("halfway through typing, the ones that already worked survive") {
        controller.acceptTarget(2, "wall", "127.0.0.1:");
        CHECK(live().oscTargets == 1);
        CHECK(controller.statusIsError());
        CHECK(std::string(controller.window().get_status()).find("not a target") !=
              std::string::npos);
        // And the half-typed row is still there, exactly as it was typed. Losing it would
        // be the field clearing itself under somebody mid-address.
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 3);
        CHECK(std::string(rows->row_data(2)->address) == "127.0.0.1:");
    }

    SECTION("a keystroke is remembered and not applied") {
        // Applying opens and closes a socket. Doing that per character would rebuild it
        // halfway through an address.
        controller.editTarget(2, "wall", "127.0.0.1:57009");
        CHECK(live().targets[2].port == 57001);

        // And the draft still has to survive the list being republished, which is what [+]
        // does — the row is drawn from the model, so a draft kept only in the widget would go.
        const takt4::testing::LoopbackReceiver added;
        controller.setNewOutputPort(added.port());
        controller.addTarget();
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 4);
        CHECK(std::string(rows->row_data(2)->address) == "127.0.0.1:57009");
        CHECK(live().targets[2].port == 57009);
    }

    SECTION("an added row is a target already") {
        // It used to be blank, and a blank row applies nothing — so a target did not exist
        // until its whole address had been typed and entered, which is the wrong way round
        // for somebody building a rig: a rule cannot be routed to a target that is not there.
        controller.addTarget();
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 4);
        CHECK(rows->row_data(3)->kind_index == 0);
        CHECK(std::string(rows->row_data(3)->host) == "127.0.0.1");
        CHECK(std::string(rows->row_data(3)->port) == "9000");
        REQUIRE(live().targets.size() == 4);
        CHECK(live().targets[3].host == "127.0.0.1");
        CHECK(live().targets[3].port == 9000);
    }

    SECTION("a row asks for a host and a port, not for one string holding both") {
        // The two boxes are edited one at a time and merged here — see `setTargetHost`.
        controller.setTargetHost(1, "192.0.2.40", false);
        CHECK(live().targets[1].host == "127.0.0.1"); // a keystroke, not applied
        controller.setTargetPort(1, "7010", true);
        REQUIRE(live().targets.size() == 3);
        CHECK(live().targets[1].host == "192.0.2.40");
        CHECK(live().targets[1].port == 7010);
        CHECK(live().targets[1].name == "deck");
    }

    SECTION("switching a row to MIDI leaves it unfinished rather than broken") {
        // Nothing is sent until a device is picked, and that is not an error to report: the
        // row is being filled in. Naming a device it cannot open is a different thing, and
        // is reported — see "a MIDI target is a row like any other".
        controller.setTargetKind(1, 1);
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 3);
        CHECK(rows->row_data(1)->kind_index == 1);
        CHECK(std::string(rows->row_data(1)->address).empty());
        CHECK(live().targets.size() == 2);
        CHECK_FALSE(controller.statusIsError());
    }

    SECTION("a row removed is a target removed") {
        controller.removeTarget(1);
        REQUIRE(live().targets.size() == 2);
        CHECK(live().targets[1].name == "wall");
        CHECK(controller.window().get_outputs_list()->row_count() == 2);
    }

    SECTION("deleting a row takes its own element away and leaves the rest standing") {
        // The other half of the drag test above, and the case it could not cover. Updating a
        // row in place keeps its elements — which is what saves the drag — and keeps any
        // *dead* `text:` binding with them: Slint drops a binding the moment somebody types
        // into the box. Delete the first of two outputs and, rewritten in place, the second's
        // name moved up a row, so a box that had been typed into went on showing the name of
        // the target that used to be above it — and committed into it. In the trigger editor this
        // was the "editing one changes them all" bug; this is the same mechanism in the main
        // window.
        //
        // So the deleted row's own element goes, and every other element keeps its own output.
        const auto rows = controller.window().get_outputs_list();
        const auto watch = std::make_shared<ModelWatch>();
        rows->attach_peer(watch);

        // The deck's name box typed into, as the box reports it — so that box shows "deck"
        // whatever its row holds from now on.
        controller.setTargetName(1, "deck", false);
        controller.removeTarget(1);
        controller.tick();
        REQUIRE(rows->row_count() == 2);
        CHECK(std::string(rows->row_data(1)->name) == "wall");
        // That row alone — never the whole list, which took every box with it, the one holding
        // the keyboard included — and nothing rebuilt: the wall's element was always the wall's.
        CHECK(watch->resets == 0);
        CHECK(watch->removed == 1);
        CHECK(watch->added == 0);

        // And a tick that changed nothing does not keep rebuilding it: that would tear a box
        // down thirty times a second, which is the failure this started as.
        const int removed = watch->removed;
        controller.tick();
        controller.tick();
        CHECK(watch->removed == removed);
    }

    SECTION("a name committed as it was typed does not rebuild the row it was typed in") {
        // Renaming an output and pressing Enter rebuilt its row — the box holding the keyboard
        // with it — so Escape, which is PANIC, reached nothing until the next click. The box
        // already shows what it was given, so there is nothing to rebuild.
        const auto rows = controller.window().get_outputs_list();
        const auto watch = std::make_shared<ModelWatch>();
        rows->attach_peer(watch);
        controller.setTargetName(1, "desk", false);
        controller.setTargetName(1, "desk", true);
        controller.tick();
        CHECK(live().targets[1].name == "desk");
        CHECK(std::string(rows->row_data(1)->name) == "desk");
        CHECK(watch->resets == 0);
        CHECK(watch->removed == 0);
        CHECK(watch->added == 0);

        // And one the controller changed after it was typed comes back as what it is — a name that
        // is only the output's own address is shown as no name at all — **without the row being
        // built again**: the box shows its row whatever it was typed (weltformat.slint's `Entry`,
        // held to it in widget_test.cpp). It was rebuilt until the redesign of 2026-09-29.
        controller.setTargetName(1, "127.0.0.1:57000", true);
        controller.tick();
        CHECK(std::string(rows->row_data(1)->name).empty());
        CHECK(watch->resets == 0);
        CHECK(watch->removed == 0);
        CHECK(watch->added == 0);
    }

    SECTION("a port outside the range is not a port") {
        controller.setOscTargets("127.0.0.1:99999");
        CHECK(live().oscTargets == 0);
    }

    SECTION("and clearing the rows clears the outputs, all but Link") {
        controller.acceptTarget(1, "", "");
        controller.acceptTarget(2, "", "");
        REQUIRE(live().targets.size() == 1);
        CHECK(live().targets[0].kind == takt4::output::OutputTarget::Kind::Link);
        CHECK(live().oscTargets == 0);
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
        // Renaming a target — an OSC one, say — broke every rule routed to it, and each had to be
        // routed again. A rule is routed to an output's id, which the row keeps whatever is typed
        // into its name.
        controller.editor().add();
        controller.editor().setOutputs("deck");
        const std::vector<std::string> routed = controller.editor().rules().back().outputs;
        REQUIRE(routed.size() == 1);
        const std::string before = live().targets[1].id;
        CHECK(routed.front() == before);

        controller.setTargetName(1, "media server", true);
        controller.tick();
        REQUIRE(live().targets[1].name == "media server");
        CHECK(live().targets[1].id == before);
        CHECK(controller.editor().rules().back().outputs == routed);
        CHECK(std::string(controller.editor().window().get_outputs_available()) ==
              "reaches 1 output");
        CHECK(std::string(controller.editor().window().get_outputs_summary()) == "media server");
    }

    SECTION("every row is an output with an id of its own") {
        const auto rows = controller.window().get_outputs_list();
        REQUIRE(rows->row_count() == 3);
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK_FALSE(std::string(rows->row_data(i)->id).empty());
            CHECK(std::string(rows->row_data(i)->id) == live().targets[i].id);
        }
        CHECK(live().targets[1].id != live().targets[2].id);
        // A row added is one from the start.
        const takt4::testing::LoopbackReceiver added;
        controller.setNewOutputPort(added.port());
        controller.addTarget();
        CHECK_FALSE(std::string(rows->row_data(3)->id).empty());
        CHECK(std::string(rows->row_data(3)->id) == live().targets[3].id);
    }
}

TEST_CASE("a MIDI target is a row like any other", "[ui]") {
    // The targets are OSC *and* MIDI, named so a rule can pick between them. The address
    // box takes both, which is what `output::parseOutputTarget` accepts either way.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver added; // where the row points before it is MIDI
    controller.setNewOutputPort(added.port());

    controller.addTarget();
    controller.acceptTarget(1, "lights", "midi takt4 test - no such port");

    const auto rows = controller.window().get_outputs_list();
    REQUIRE(rows->row_count() == 2);
    CHECK(std::string(rows->row_data(1)->address) == "midi takt4 test - no such port");
    // The device is not on this machine, so opening it fails — and that is *said* rather
    // than dropping the row, because the rest of the rig is still sending.
    CHECK(controller.statusIsError());
    CHECK(std::string(controller.window().get_status()).find("outputs:") != std::string::npos);
    REQUIRE(seen(controller).targets.size() == 2);
    CHECK(seen(controller).targets[1].kind == takt4::output::OutputTarget::Kind::Midi);
}

TEST_CASE("a MIDI clock whose device will not open is said out loud, and kept", "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    // Through the row's address, not its dropdown: the dropdown sends an *index* into the
    // machine's own port list (a ComboBox cannot be moved by its value — Slint 11970), and no
    // index on this machine names a port that does not exist. A settings file can.
    const takt4::testing::LoopbackReceiver added; // where the row points before it is MIDI
    controller.setNewOutputPort(added.port());
    controller.addTarget();
    controller.acceptTarget(1, "drums", "midiclock takt4 test - no such port");
    CHECK(!seen(controller).midiClock);
    CHECK(controller.statusIsError());
    const std::string status(controller.window().get_status());
    INFO(status);
    CHECK(status.find("drums") != std::string::npos);
    // Kept, asking for the same device, so it is still wanted when the device is plugged in.
    REQUIRE(seen(controller).targets.size() == 2);
    CHECK(seen(controller).targets[1].kind == takt4::output::OutputTarget::Kind::MidiClock);
    CHECK(seen(controller).targets[1].device == "takt4 test - no such port");
    CHECK(controller.window().get_outputs_list()->row_data(1)->kind_index == 3);
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

    // The outputs go on — they live as long as the window — and are told the tracker has
    // stopped listening.
    CHECK(controller.outputs().running());
    REQUIRE(controller.settleOutputs());
    CHECK_FALSE(controller.outputs().tracking());
    CHECK(tracker.engine().beatsDropped() == 0);
    CHECK(controller.outputs().errors() == 0);
    // Whatever the tracker called, the output thread took all of it — a round or two after the
    // stop, since nothing drains the ring for it now — and sent every beat but those called
    // before a lock, which nothing sends (`TempoState::acquired`): four hundred milliseconds is
    // short of one, so what it called went unsent.
    const auto taken = [&] {
        return controller.outputs().transports().beats() + controller.outputs().beatsUnsent();
    };
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (taken() != tracker.engine().beatsCalled() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(taken() == tracker.engine().beatsCalled());
}

// ---------------------------------------------------------------------------------------
// The settings' two layers, machine-local and portable, from the window's side: what it
// restores on the way in and what it hands back on the way out. Where the file lives and
// what it looks like is tests/settings/settings_test.cpp's business.
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
    saved.machine.mono = true; // one input remembered, as a single input

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
    // One launch before the MOTU was powered on fell back to another device — right — and then
    // saved that device as if it had been chosen, so the next show opened on the wrong
    // interface at channel 1 even with the MOTU plugged in.
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

TEST_CASE("a remembered interface is still the one saved on a machine with no input at all",
          "[ui]") {
    // The same, with nothing to fall back to: the empty list returned before the fallback was
    // noted, so the next save wrote no input. Found on the Linux runner, which has no audio.
    LiveTracker tracker(kWeights, kStateSpace);
    if (!tracker.devices().empty()) {
        SKIP("this machine has an input device");
    }
    takt4::settings::Settings saved;
    saved.machine.deviceName = "An interface that is switched off";
    saved.machine.hostApiName = "ASIO";
    saved.machine.channel = 10;
    saved.machine.mono = true;

    WindowController controller(tracker, saved);
    CHECK(controller.deviceIndex() == -1);
    const takt4::settings::Settings out = controller.currentSettings();
    CHECK(out.machine.deviceName == "An interface that is switched off");
    CHECK(out.machine.hostApiName == "ASIO");
    CHECK(out.machine.channel == 10);
    CHECK(out.machine.mono);

    // And RESCAN looks for it, not for nothing.
    controller.window().invoke_rescan_clicked();
    rescanned(controller);
    CHECK(controller.currentSettings().machine.deviceName == "An interface that is switched off");
}

TEST_CASE("a remembered MIDI clock whose device is missing at launch is still the one saved",
          "[ui]") {
    // The same for the clock as an output: one whose drum machine was left at home is kept
    // asking for it, so it is sending again at the next launch it is plugged in for.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    // As a file written before the clock was an output holds it.
    saved.machine.midiClockPort = "takt4 test - a drum machine left at home";
    WindowController controller(tracker, saved);
    REQUIRE(!seen(controller).midiClock);
    const takt4::settings::Settings out = controller.currentSettings();
    REQUIRE(out.preset.outputs.size() == 2);
    CHECK(out.preset.outputs[1].kind == takt4::output::OutputTarget::Kind::MidiClock);
    CHECK(out.preset.outputs[1].device == "takt4 test - a drum machine left at home");
    // And where an older build looks for it.
    CHECK(takt4::settings::toJson(out).find(
              R"("midiClockPort": "takt4 test - a drum machine left at home")") !=
          std::string::npos);

    // Taking the row away is a choice too.
    controller.removeTarget(1);
    CHECK(controller.currentSettings().preset.outputs.size() == 1);
}

TEST_CASE("RESCAN reads the devices again and keeps the one chosen", "[ui]") {
    // PortAudio builds its device list once, so an interface switched on after takt4 started
    // was never offered. RESCAN restarts PortAudio — the only thing that makes it look again —
    // and must land back on what was selected, found by name.
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
    rescanned(controller);
    REQUIRE(controller.deviceIndex() >= 0);
    const InputDevice& after = controller.devices()[static_cast<std::size_t>(controller.deviceIndex())];
    CHECK(after.name == chosen);
    CHECK(after.hostApiName == api);
    CHECK_FALSE(controller.statusIsError());
    CHECK(std::string(controller.window().get_status()).find("Found ") != std::string::npos);
}

namespace {

/// A machine with one interface on it, as the window lists it: nothing in it is ever opened —
/// the tests' sandbox refuses every device, which is how a dead interface's reopen fails.
InputDevice motuAt(int index) {
    InputDevice made;
    made.index = index;
    made.name = "MOTU Pro Audio";
    made.hostApiName = "ASIO";
    made.maxInputChannels = 8;
    made.defaultSampleRate = 48000.0;
    return made;
}

} // namespace

TEST_CASE("an outage tells the outputs, which let go of the rig", "[ui]") {
    // An interface unplugged sends nothing at all, so the tracker never hears the silence that
    // says the input has gone quiet — and the lasers held their last clip, and the notes stayed
    // on, for the whole of the outage. The window tells the outputs it has lost the input; what
    // they then do is output_runner_test.cpp's "an input outage disarms the lasers and lets go of
    // the notes".
    LiveTracker tracker(kWeights, kStateSpace);
    tracker.setDeviceListHook([](std::vector<InputDevice>& list) { list = {motuAt(0)}; });
    WindowController controller(tracker);
    REQUIRE(controller.settleOutputs());
    REQUIRE(controller.outputs().inputsLost() == 0);
    controller.beginOutageOn({motuAt(0), takt4::audio::ChannelSelection::single(5)},
                             controller.secondsNow());
    REQUIRE(controller.settleOutputs());
    CHECK(controller.outputs().inputsLost() == 1);
}

#if defined(_WIN32)
TEST_CASE("an outage is still brought back while a dialog holds the window's loop", "[ui]") {
    // A file dialog, the system menu and a message box each run a message loop of their own, and
    // Slint's timers — the redraw that supervises the input — do not run inside it. A dead input,
    // a moved clock or a driver asking to be reset met while IMPORT's dialog was open waited until
    // it closed: every tempo 8.8 % off meanwhile, for a rate change. The supervision rides a
    // Windows timer as well now, which every message loop on the thread serves.
    LiveTracker tracker(kWeights, kStateSpace);
    tracker.setDeviceListHook([](std::vector<InputDevice>& list) { list = {motuAt(0)}; });
    WindowController controller(tracker);
    controller.superviseThroughModalLoops(); // as `run` does
    controller.beginOutageOn({motuAt(0), takt4::audio::ChannelSelection::single(5)},
                             controller.secondsNow());
    REQUIRE(controller.outageTries() == std::optional<int>(0));
    // What such a dialog does with the thread: its own loop, dispatching messages — never Slint's
    // timers, never `tick`.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{1600};
    MSG message{};
    while (std::chrono::steady_clock::now() < until) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        (void)MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    // A second in, the first try at opening it again.
    CHECK(controller.outageTries() == std::optional<int>(1));
}
#endif

TEST_CASE("an outage waits for no driver, and its tries come further apart the longer it lasts",
          "[ui]") {
    // An open and a look at the devices each held the window's thread — PANIC's, the meters' —
    // for up to ten and thirty seconds, at every try while the interface was gone, and the tries
    // came every two seconds all night. Here the gone interface's driver does not answer.
    LiveTracker tracker(kWeights, kStateSpace);
    LiveTracker::Limits limits;
    limits.open = std::chrono::milliseconds(300);
    limits.stop = std::chrono::milliseconds(300);
    limits.rescan = std::chrono::milliseconds(300);
    limits.list = std::chrono::milliseconds(300);
    tracker.setLimits(limits);
    tracker.setDeviceListHook([](std::vector<InputDevice>& list) { list = {motuAt(0)}; });
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    tracker.setAudioJobHook([released](const char* what) {
        if (std::string_view(what) == "open") {
            released.wait();
        }
    });
    WindowController controller(tracker);
    using Reading = takt4::audio::InputWatchdog::Reading;
    const takt4::audio::AsioDriverEvents nothing;
    controller.beginOutageOn({motuAt(0), takt4::audio::ChannelSelection::single(5)}, 100.0);
    // Asks after whatever the outage began until it has its answer, as the redraws do.
    const auto answered = [&](double now) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (controller.outageAsking() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            controller.superviseInput(Reading{}, nothing, now);
        }
        REQUIRE_FALSE(controller.outageAsking());
        return std::string(controller.window().get_status());
    };

    // The first try: begun, and the thread back at once, though the driver holds the open.
    const auto asked = std::chrono::steady_clock::now();
    controller.superviseInput(Reading{}, nothing, 101.1);
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::milliseconds(100));
    CHECK(controller.outageTries() == std::optional<int>(1));
    CHECK(controller.outageAsking());
    // PANIC meanwhile, from the window's own button.
    controller.window().invoke_panic_clicked();
    CHECK(panickedNow(controller));
    controller.releasePanic();
    {
        const std::string status = answered(101.1);
        INFO(status);
        CHECK(status.find("did not answer") != std::string::npos);
        CHECK(status.find("Trying again in 2 s") != std::string::npos);
    }

    // Then two seconds, four, eight, sixteen, and thirty from then on — never a moment early.
    const std::array<double, 6> waits = {2.0, 4.0, 8.0, 16.0, 30.0, 30.0};
    double now = 101.1;
    for (std::size_t k = 1; k < waits.size(); ++k) {
        controller.superviseInput(Reading{}, nothing, now + waits[k - 1] - 0.1);
        CHECK(controller.outageTries() == std::optional<int>(static_cast<int>(k)));
        now += waits[k - 1] + 0.05;
        controller.superviseInput(Reading{}, nothing, now);
        const std::string status = answered(now);
        INFO("try " << k + 1 << ": " << status);
        CHECK(controller.outageTries() == std::optional<int>(static_cast<int>(k + 1)));
        CHECK(status.find("Trying again in " + std::to_string(static_cast<int>(waits[k])) + " s") !=
              std::string::npos);
    }
    release.set_value();
    REQUIRE(waitFor([&] { return !tracker.stuck(); }));
    tracker.setAudioJobHook({});
}

TEST_CASE("a device table a late rescan put in place is listed before anything is opened from "
          "the old one",
          "[ui]") {
    // A rescan given up on at its limit puts its table in place whenever the drivers at last
    // answer — renumbering every device, while the window went on holding the old list. START
    // then opened whatever sat at the old index. Here the interface moves from 1 to 0.
    LiveTracker tracker(kWeights, kStateSpace);
    LiveTracker::Limits limits;
    limits.rescan = std::chrono::milliseconds(300);
    tracker.setLimits(limits);
    const auto device = [](const char* name, int index) {
        InputDevice made;
        made.index = index;
        made.name = name;
        made.hostApiName = "ASIO";
        made.maxInputChannels = 8;
        made.defaultSampleRate = 48000.0;
        return made;
    };
    std::mutex machineMutex;
    std::vector<InputDevice> machine = {device("A desk mic", 0), device("MOTU Pro Audio", 1)};
    tracker.setDeviceListHook([&](std::vector<InputDevice>& list) {
        const std::lock_guard<std::mutex> lock(machineMutex);
        list = machine;
    });
    WindowController controller(tracker);
    controller.window().invoke_device_picked(1);
    REQUIRE(controller.devices()[1].name == "MOTU Pro Audio");

    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    tracker.setAudioJobHook([released](const char* what) {
        if (std::string_view(what) == "rescan") {
            released.wait();
        }
    });
    controller.rescanDevices();
    rescanned(controller); // given up on at its limit
    CHECK(tracker.stuck());
    {
        const std::lock_guard<std::mutex> lock(machineMutex);
        machine = {device("MOTU Pro Audio", 0), device("A desk mic", 1)};
    }
    release.set_value();
    REQUIRE(waitFor([&] { return !tracker.stuck(); }));
    tracker.setAudioJobHook({});

    controller.tick();
    REQUIRE(controller.deviceIndex() >= 0);
    const InputDevice& picked =
        controller.devices()[static_cast<std::size_t>(controller.deviceIndex())];
    CHECK(picked.name == "MOTU Pro Audio");
    CHECK(picked.index == 0);
    tracker.setDeviceListHook({});
}

TEST_CASE("IMPORT after a RESCAN that could not bring PortAudio back up goes on, with no input",
          "[ui][settings]") {
    // PortAudio ended and not started again has no devices to list, and the list threw — into
    // IMPORT's callback, and out of the window. Made to happen on the audio thread.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    tracker.setAudioJobHook([](const char* what) {
        if (std::string_view(what) == "rescan") {
            Pa_Terminate();
            throw std::runtime_error("Pa_Initialize failed: made to, for the test");
        }
    });
    controller.rescanDevices();
    rescanned(controller);
    {
        const std::string status(controller.window().get_status());
        INFO(status);
        CHECK(status.find("Could not look for devices again") != std::string::npos);
        CHECK(controller.statusIsError());
    }
    tracker.setAudioJobHook({});

    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "the other rig.json";
    {
        std::ofstream out(file, std::ios::binary);
        out << R"({"machine": {"deviceName": "The other rig's interface", "hostApiName": "ASIO"}})";
    }
    bool imported = false;
    CHECK_NOTHROW(imported = controller.importFrom(file));
    CHECK(imported);
    CHECK(controller.devices().empty());
    CHECK(controller.deviceIndex() == -1);
    CHECK(controller.currentSettings().machine.deviceName == "The other rig's interface");
}

TEST_CASE("an outage that looks for the devices again finds its interface there by name",
          "[ui]") {
    // An outage long enough to reach its third reopen reads the device list again — and kept
    // the old *position* in the new list. So the picker, STOP and START, and the settings file
    // autosave wrote all meant whatever now sat where the interface used to be; and with its
    // name gone, the reopen fell back to the old entry, whose PortAudio index belonged to the
    // session before.
    //
    // The machine is the test's own list, which changes the way a replug changes one. Nothing in
    // it is ever opened: the sandbox refuses every device, which is exactly how a dead
    // interface's reopen fails.
    LiveTracker tracker(kWeights, kStateSpace);
    const auto device = [](const char* name, int index, const char* api, int channels) {
        InputDevice made;
        made.index = index;
        made.name = name;
        made.hostApiName = api;
        made.maxInputChannels = channels;
        made.defaultSampleRate = 48000.0;
        return made;
    };
    std::vector<InputDevice> machine = {device("Realtek Mic", 0, "MME", 2),
                                        device("MOTU Pro Audio", 1, "ASIO", 8),
                                        device("Speakers (loopback)", 2, "Windows WASAPI", 2)};
    tracker.setDeviceListHook([&machine](std::vector<InputDevice>& list) { list = machine; });
    takt4::settings::Settings saved;
    saved.machine.deviceName = "MOTU Pro Audio";
    saved.machine.hostApiName = "ASIO";
    saved.machine.channel = 5;
    WindowController controller(tracker, saved);
    const auto selected = [&controller] {
        return controller.deviceIndex() < 0
                   ? std::string()
                   : controller.devices()[static_cast<std::size_t>(controller.deviceIndex())].name;
    };
    REQUIRE(selected() == "MOTU Pro Audio");
    REQUIRE(controller.channelIndex() == 5);

    // Running on it, and then silent.
    double now = 100.0;
    controller.beginOutageOn({machine[1], takt4::audio::ChannelSelection::single(5)}, now);
    REQUIRE(controller.wantsRunning());

    // Plugged back in, the machine lists another device first and everything has moved.
    machine = {device("USB Mic", 0, "MME", 1), device("Realtek Mic", 1, "MME", 2),
               device("Speakers (loopback)", 2, "Windows WASAPI", 2),
               device("MOTU Pro Audio", 3, "ASIO", 8)};
    // A try each time one is due — half a minute apart at the most — with what it began, an
    // open or a look at the machine, asked after until it has its answer, as the redraws do.
    // The first opens the list there is, and fails; the second looks at the machine again.
    const auto tries = [&controller, &now](int count) {
        for (int i = 0; i < count; ++i) {
            now += 31.0;
            controller.superviseInput({}, {}, now);
            REQUIRE(waitFor([&] {
                controller.superviseInput({}, {}, now);
                return !controller.outageAsking();
            }));
        }
    };
    tries(2);
    CHECK(selected() == "MOTU Pro Audio");
    CHECK(controller.channelIndex() == 5);
    CHECK(controller.window().get_device_index() == controller.deviceIndex());
    // And that is what autosave writes, mid-outage.
    CHECK(controller.currentSettings().machine.deviceName == "MOTU Pro Audio");
    CHECK(controller.currentSettings().machine.channel == 5);

    SECTION("STOP and START open the interface, not what sits where it was") {
        controller.toggleRun();
        REQUIRE_FALSE(controller.wantsRunning());
        controller.toggleRun();
        const std::string status(controller.window().get_status());
        INFO(status);
        CHECK(status.find("Cannot open MOTU Pro Audio") != std::string::npos);
    }

    SECTION("an interface that is not there is not replaced by another") {
        machine = {device("USB Mic", 0, "MME", 1), device("Realtek Mic", 1, "MME", 2),
                   device("Speakers (loopback)", 2, "Windows WASAPI", 2)};
        // The next try looks at the machine again.
        tries(1);
        CHECK(controller.deviceIndex() == -1);
        CHECK(controller.currentSettings().machine.deviceName == "MOTU Pro Audio");
        {
            // The reopen says it is gone rather than opening the old entry.
            const std::string status(controller.window().get_status());
            INFO(status);
            CHECK(status.find("MOTU Pro Audio is not on this machine") != std::string::npos);
        }
        // STOP, and START with nothing in its place opens nothing — and asks for a pick.
        controller.toggleRun();
        controller.toggleRun();
        CHECK_FALSE(controller.wantsRunning());
        CHECK(std::string(controller.window().get_status()).find("Pick an input") !=
              std::string::npos);
        // Still the one wanted after STOP, and RESCAN finds it again by name when it is back.
        CHECK(controller.currentSettings().machine.deviceName == "MOTU Pro Audio");
        CHECK(controller.currentSettings().machine.channel == 5);
        machine.push_back(device("MOTU Pro Audio", 3, "ASIO", 8));
        controller.rescanDevices();
        rescanned(controller);
        CHECK(selected() == "MOTU Pro Audio");
        CHECK(controller.channelIndex() == 5);
    }
}

#if defined(_WIN32) // the ASIO scan and ScopedVariable are Windows'
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
        rescanned(controller); // the scan runs on the audio thread, inside these
    }
    const std::string again(controller.window().get_status());
    INFO("after a RESCAN that fell over again: " << again);
    CHECK(controller.statusIsError());
    CHECK(again.find("An ASIO driver fell over twice") != std::string::npos);

    // And a RESCAN that finds nothing wrong — with no ASIO wanted this time, as in every other
    // test — says so, and what was said goes with the problem.
    controller.window().invoke_rescan_clicked();
    rescanned(controller);
    const std::string after(controller.window().get_status());
    INFO("after RESCAN: " << after);
    CHECK(after.find("ASIO") == std::string::npos);
    // What a RESCAN with nothing wrong says, which a problem kept from before would stop.
    CHECK_FALSE(controller.statusIsError());
    CHECK(after.find("Found ") != std::string::npos);
}
#endif

TEST_CASE("STOP quiets the outputs before it waits on the driver", "[ui][hardware]") {
    // The tracker's stop waits on the driver, and the outputs were told only once it had
    // returned: they went on firing the beats they predicted meanwhile, and then blacked out.
    // Here the driver's stop is held until the outputs have been looked at.
    LiveTracker tracker(kWeights, kStateSpace);
    if (!bestInputDevice(tracker)) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    controller.toggleRun();
    REQUIRE(tracker.running());
    REQUIRE(controller.settleOutputs());
    REQUIRE(controller.outputs().tracking());
    std::atomic<bool> stopped{false};
    std::atomic<bool> quietFirst{false};
    tracker.setAudioJobHook([&controller, &stopped, &quietFirst](const char* what) {
        if (std::string_view(what) != "stop") {
            return;
        }
        // On the audio thread, with the window's thread waiting for this stop to finish.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
        while (controller.outputs().tracking() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        quietFirst = !controller.outputs().tracking();
        stopped = true;
    });
    controller.toggleRun();
    tracker.setAudioJobHook({});
    CHECK_FALSE(tracker.running());
    CHECK(stopped.load());
    CHECK(quietFirst.load());
}

TEST_CASE("a dead input says NO AUDIO and is brought back", "[ui][hardware]") {
    // A dead, reset or re-clocked input, the window's half. The watchdog's arithmetic is
    // input_watchdog_test.cpp's business; this hands the window the readings a pulled cable, a
    // moved clock and a driver reset produce, and checks what an operator would see and that the
    // input really reopens.
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

    // Not before its time, and then reopened — and not back until it has sent something, which
    // an interface does within milliseconds of opening.
    controller.superviseInput(Reading{}, nothing, 100.5);
    CHECK(controller.window().get_input_lost());
    controller.superviseInput(Reading{}, nothing, 101.1);
    // Begun, and asked after on the looks that follow until it has opened: nothing waits for the
    // driver.
    const auto opening = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (controller.outageAsking() && std::chrono::steady_clock::now() < opening) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        controller.superviseInput(Reading{}, nothing, 101.1);
    }
    CHECK(tracker.running());
    CHECK(controller.window().get_input_lost());
    CHECK(std::string(controller.window().get_status()).find("waiting for it to send audio") !=
          std::string::npos);
    const auto sent = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (tracker.stream()->counters().callbacks == 0 && std::chrono::steady_clock::now() < sent) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    controller.superviseInput(Reading{}, nothing, 101.2);
    CHECK_FALSE(controller.window().get_input_lost());
    CHECK(tracker.running());
    CHECK(std::string(controller.window().get_status()).find("Audio is back") != std::string::npos);
    // Said, and not in red: it is fixed, and a red status that asks for nothing stays red until
    // something else is said.
    CHECK_FALSE(controller.statusIsError());
    // And the latency figures with it, which a reopen can change — they were said after START
    // alone, so a buffer size changed in the driver's panel went on showing the old ones.
    CHECK(std::string(controller.window().get_status()).find("ms centred framing") !=
          std::string::npos);

    // A clock moved by another program: reopened at once, and said.
    Reading moved;
    moved.verdict = Verdict::RateChanged;
    moved.measuredRate = 48000.0;
    controller.superviseInput(moved, nothing, 102.0);
    CHECK(tracker.running());
    CHECK(std::string(controller.window().get_status()).find("moved from") != std::string::npos);
    CHECK(std::string(controller.window().get_status()).find("ms centred framing") !=
          std::string::npos);
    CHECK_FALSE(controller.statusIsError());

    // A driver asking to be reset: the same. And whatever a driver says while the stream is
    // being opened again belongs to that open — posted here as though said during it — and is
    // not left for the next redraw to answer with another reopen, which for a driver that says
    // something as it starts was a reopen on every tick.
    takt4::audio::AsioDriverEvents reset;
    reset.resetRequest = true;
    takt4::audio::postAsioDriverEvents(reset);
    controller.superviseInput(Reading{}, reset, 103.0);
    CHECK(tracker.running());
    CHECK(std::string(controller.window().get_status()).find("asked to be reset") !=
          std::string::npos);
    CHECK_FALSE(controller.statusIsError());
    CHECK_FALSE(takt4::audio::takeAsioDriverEvents().any());

    // Silent again three seconds after it came back: the same trouble, so the count goes on from
    // the one try the last outage took rather than from nothing — an input that keeps coming
    // back for a moment would otherwise never reach the look at the device list every third try.
    controller.superviseInput(silent, nothing, 104.0);
    REQUIRE(controller.window().get_input_lost());
    CHECK(controller.outageTries() == std::optional<int>(1));

    // And STOP during an outage stops, rather than reading "not running" and starting.
    controller.toggleRun();
    CHECK_FALSE(controller.wantsRunning());
    CHECK_FALSE(tracker.running());
    CHECK_FALSE(controller.window().get_input_lost());
    CHECK_FALSE(controller.window().get_running());
    CHECK_FALSE(controller.outageTries().has_value());
}

TEST_CASE("a driver that keeps asking to be reset is reopened four times a minute, then left until "
          "START",
          "[ui][hardware]") {
    // An interface on an external clock that misreports its rate was reopened every three seconds
    // all night and never settled.
    LiveTracker tracker(kWeights, kStateSpace);
    if (!bestInputDevice(tracker)) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    controller.toggleRun();
    REQUIRE(tracker.running());
    using Reading = takt4::audio::InputWatchdog::Reading;
    takt4::audio::AsioDriverEvents reset;
    reset.resetRequest = true;
    for (int i = 0; i < 4; ++i) {
        controller.superviseInput(Reading{}, reset, 100.0 + i);
        const std::string status(controller.window().get_status());
        INFO("ask " << i + 1 << ": " << status);
        CHECK(status.find("asked to be reset; reopened at") != std::string::npos);
        CHECK(tracker.running());
    }
    const takt4::audio::InputStream* const kept = tracker.stream();
    controller.superviseInput(Reading{}, reset, 104.0);
    const std::string held(controller.window().get_status());
    INFO(held);
    CHECK(held.find("reopened 4 times in a minute") != std::string::npos);
    CHECK(controller.statusIsError());
    CHECK(tracker.stream() == kept);
    // Not again, and not said again — a minute on included.
    controller.window().set_status(slint::SharedString("something else"));
    controller.superviseInput(Reading{}, reset, 200.0);
    CHECK(tracker.stream() == kept);
    CHECK(std::string(controller.window().get_status()) == "something else");
    // START is the operator's word that it is worth reopening again.
    controller.toggleRun();
    controller.toggleRun();
    REQUIRE(tracker.running());
    controller.superviseInput(Reading{}, reset, 300.0);
    CHECK(std::string(controller.window().get_status()).find("asked to be reset; reopened at") !=
          std::string::npos);
    controller.toggleRun();
}

TEST_CASE("a loopback that goes quiet is waited for, and an interface that goes quiet is reopened",
          "[ui]") {
    // Windows sends a loopback nothing at all while nothing plays on the output it captures, so
    // a paused player read as an unplugged interface: "No audio", a reopen, "Audio is back", and
    // again, every few seconds, the tracker restarting each time. A loopback's silence is said as
    // what it is and waited out; an interface's is still an outage.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    using Reading = takt4::audio::InputWatchdog::Reading;
    using Verdict = takt4::audio::InputWatchdog::Verdict;
    const auto device = [](const char* name, bool loopback) {
        InputDevice made;
        made.name = name;
        made.hostApi = takt4::audio::HostApiKind::Wasapi;
        made.hostApiName = "Windows WASAPI";
        made.maxInputChannels = 2;
        made.defaultSampleRate = 44100.0;
        made.isLoopback = loopback;
        return made;
    };
    Reading silent;
    silent.verdict = Verdict::Silent;
    silent.silentForSeconds = 0.6;
    Reading healthy;
    healthy.verdict = Verdict::Healthy;
    const auto status = [&controller] { return std::string(controller.window().get_status()); };

    SECTION("a loopback") {
        controller.assumeRunningOn({device("Speakers [Loopback]", true),
                                    takt4::audio::ChannelSelection::single(0)},
                                   100.0);
        controller.superviseInput(silent, {}, 100.6);
        const std::string said = status();
        INFO(said);
        CHECK(said.find("Nothing is playing on Speakers [Loopback]") != std::string::npos);
        CHECK_FALSE(controller.statusIsError());
        CHECK_FALSE(controller.window().get_input_lost());
        CHECK(std::string(controller.window().get_input_reading()) == "nothing playing");
        CHECK(controller.wantsRunning());
        // And it waits, however long: nothing reopened, nothing said again.
        for (double now = 101.0; now < 160.0; now += 0.5) {
            controller.superviseInput(silent, {}, now);
        }
        CHECK(status() == said);
        CHECK_FALSE(controller.window().get_input_lost());
        // Something plays.
        controller.superviseInput(healthy, {}, 160.0);
        CHECK(status().find("Sound on Speakers [Loopback] again") != std::string::npos);
        CHECK_FALSE(controller.statusIsError());
    }

    SECTION("an interface") {
        controller.assumeRunningOn({device("In 1-2 (MOTU Pro Audio)", false),
                                    takt4::audio::ChannelSelection::single(0)},
                                   100.0);
        controller.superviseInput(silent, {}, 100.6);
        CHECK(controller.window().get_input_lost());
        CHECK(status().find("No audio from In 1-2 (MOTU Pro Audio)") != std::string::npos);
        CHECK(controller.statusIsError());
    }
}

namespace {

/// An ASIO interface running at 44.1 kHz as far as the window knows — nothing is opened.
InputDevice asioInterface() {
    InputDevice made;
    made.name = "MOTU Pro Audio";
    made.hostApi = takt4::audio::HostApiKind::Asio;
    made.hostApiName = "ASIO";
    made.maxInputChannels = 8;
    made.defaultSampleRate = 44100.0;
    return made;
}

} // namespace

TEST_CASE("a driver saying its rate changed to the rate it runs at reopens nothing", "[ui]") {
    // The patched host kept every sample-rate message as "the rate changed", and drivers send it
    // for other news too — the SDK's comment names S/PDIF status — so a stream running happily
    // was reopened, and the tracker restarted, for it.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.assumeRunningOn({asioInterface(), takt4::audio::ChannelSelection::single(0)},
                               100.0);
    const auto status = [&controller] { return std::string(controller.window().get_status()); };
    const std::string before = status();
    takt4::audio::InputWatchdog::Reading healthy;
    healthy.verdict = takt4::audio::InputWatchdog::Verdict::Healthy;

    takt4::audio::AsioDriverEvents same;
    same.sampleRateChange = true;
    same.reportedRate = 44100.0;
    controller.superviseInput(healthy, same, 100.5);
    CHECK(status() == before);
    CHECK_FALSE(controller.window().get_input_lost());

    // A rate that really is another is still a reopen — which fails here, the interface being
    // imaginary, and so is an outage that says why.
    takt4::audio::AsioDriverEvents moved;
    moved.sampleRateChange = true;
    moved.reportedRate = 48000.0;
    controller.superviseInput(healthy, moved, 101.0);
    INFO(status());
    CHECK(status().find("changed its sample rate") != std::string::npos);
    CHECK(controller.window().get_input_lost());
}

TEST_CASE("a driver that asks to be reset with the same rate is still reset", "[ui]") {
    // The same-rate exception is for a rate message alone: a reset or a new buffer size that
    // arrives beside one still needs the stream built again.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.assumeRunningOn({asioInterface(), takt4::audio::ChannelSelection::single(0)},
                               100.0);
    takt4::audio::AsioDriverEvents both;
    both.sampleRateChange = true;
    both.reportedRate = 44100.0;
    both.resetRequest = true;
    controller.superviseInput({}, both, 100.5);
    const std::string said(controller.window().get_status());
    INFO(said);
    CHECK(said.find("asked to be reset") != std::string::npos);
}

TEST_CASE("a driver's resyncs are counted with the input's other trouble", "[ui]") {
    // The patch kept a driver's resync messages "so they can be shown", and nothing read them.
    // They are the interface's own word that it lost its place.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.assumeRunningOn({asioInterface(), takt4::audio::ChannelSelection::single(0)},
                               100.0);
    const auto trouble = [&controller] {
        return std::string(controller.window().get_input_trouble());
    };
    takt4::audio::InputWatchdog::Reading healthy;
    healthy.verdict = takt4::audio::InputWatchdog::Verdict::Healthy;
    takt4::audio::AsioDriverEvents resync;
    resync.resync = true;
    controller.superviseInput(healthy, resync, 100.5);
    CHECK(trouble().find("1 driver resync — the interface lost its place") != std::string::npos);
    controller.superviseInput(healthy, {}, 101.0);
    controller.superviseInput(healthy, resync, 101.5);
    CHECK(trouble().find("2 driver resyncs") != std::string::npos);
    // Not a reason to reopen on its own.
    CHECK_FALSE(controller.window().get_input_lost());
}

TEST_CASE("an outage's next look at the machine is timed from when the last one ended", "[ui]") {
    // During an outage the machine's devices are read again no more than every ten seconds —
    // timed from when the look *began*, while an ASIO driver can take the scan's whole limit to
    // answer: a look that took eight seconds made the next one due two seconds after it. Here a
    // look is begun at the first try and answered eight seconds on.
    LiveTracker tracker(kWeights, kStateSpace);
    int looks = 0;
    bool counting = false;
    tracker.setDeviceListHook([&looks, &counting](std::vector<InputDevice>& list) {
        list.clear(); // the interface is not there, so every reopen fails as a dead one does
        looks += counting ? 1 : 0;
    });
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    tracker.setAudioJobHook([released](const char* what) {
        if (std::string_view(what) == "rescan") {
            released.wait();
        }
    });
    WindowController controller(tracker);
    controller.beginOutageOn({asioInterface(), takt4::audio::ChannelSelection::single(0)}, 100.0);
    counting = true;
    // Each try's open or look asked after until it has its answer, at `now`.
    const auto at = [&controller](double now) {
        controller.superviseInput({}, {}, now);
        REQUIRE(waitFor([&] {
            controller.superviseInput({}, {}, now);
            return !controller.outageAsking();
        }));
    };

    controller.superviseInput({}, {}, 101.0); // the first try: a look, begun
    REQUIRE(controller.outageAsking());
    release.set_value();
    at(109.0); // and answered eight seconds on
    REQUIRE(looks == 1);
    tracker.setAudioJobHook({});
    // The next try, two seconds after it ended: ten after the look began, which was due — and
    // only two after it ended, which is not.
    at(111.05);
    CHECK(controller.outageTries() == std::optional<int>(2));
    CHECK(looks == 1);
    at(115.1); // four seconds on: six after it ended
    CHECK(controller.outageTries() == std::optional<int>(3));
    CHECK(looks == 1);
    at(123.15); // eight more: fourteen after it ended, and a look
    CHECK(controller.outageTries() == std::optional<int>(4));
    CHECK(looks == 2);
}

TEST_CASE("an outage looks at the machine again only once its interface has gone from the list "
          "or would not open",
          "[ui]") {
    // A look loads every ASIO driver on the machine. With the interface still listed, a try is an
    // open; only an open that failed, or the interface gone from the list, earns a look.
    LiveTracker tracker(kWeights, kStateSpace);
    tracker.setDeviceListHook([](std::vector<InputDevice>& list) { list = {asioInterface()}; });
    WindowController controller(tracker);
    std::atomic<int> looks{0};
    tracker.setAudioJobHook([&looks](const char* what) {
        if (std::string_view(what) == "rescan") {
            ++looks;
        }
    });
    controller.beginOutageOn({asioInterface(), takt4::audio::ChannelSelection::single(0)}, 100.0);
    const auto at = [&controller](double now) {
        controller.superviseInput({}, {}, now);
        REQUIRE(waitFor([&] {
            controller.superviseInput({}, {}, now);
            return !controller.outageAsking();
        }));
    };
    at(101.0); // the first try: listed, and never refused, so opened
    CHECK(controller.outageTries() == std::optional<int>(1));
    CHECK(looks == 0);
    at(103.05); // refused (the sandbox opens nothing), so the next try looks first
    CHECK(controller.outageTries() == std::optional<int>(2));
    CHECK(looks == 1);
    tracker.setAudioJobHook({});
}

namespace {

/// A loopback on this machine that nothing is playing on, found by opening each for a second
/// and a half and seeing whether anything arrives. Nothing if every one has something playing.
std::optional<InputDevice> idleLoopback(LiveTracker& tracker) {
    for (const InputDevice& device : tracker.devices()) {
        if (!device.isLoopback) {
            continue;
        }
        try {
            tracker.start(device, takt4::audio::ChannelSelection::single(0));
        } catch (const std::exception&) {
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1500});
        const bool quiet = tracker.stream()->counters().callbacks == 0;
        tracker.stop();
        if (quiet) {
            return device;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a loopback with nothing playing on it is waited for with the stream left open",
          "[ui][hardware]") {
    // End to end: the window's own redraws, the real watchdog, a real loopback of an output
    // nothing plays on. Measured on this rig on 2026-09-26: such a loopback opens and sends
    // nothing at all, 0 hops in 3 s.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> quiet = idleLoopback(tracker);
    if (!quiet) {
        SKIP("something is playing on every loopback on this machine");
    }
    INFO(quiet->name);
    WindowController controller(tracker);
    int at = -1;
    for (std::size_t i = 0; i < controller.devices().size(); ++i) {
        if (controller.devices()[i].name == quiet->name && controller.devices()[i].isLoopback) {
            at = static_cast<int>(i);
        }
    }
    REQUIRE(at >= 0);
    controller.pickDevice(at);
    controller.pickChannel(0);
    controller.toggleRun();
    REQUIRE(tracker.running());

    // Six seconds of redraws, as the window's timer makes them: past the watchdog's grace and its
    // half second of silence, and well past when a reopen used to come.
    std::set<std::string> said;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{6};
    while (std::chrono::steady_clock::now() < until) {
        controller.tick();
        said.insert(std::string(controller.window().get_status()));
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    bool nothingPlaying = false;
    for (const std::string& line : said) {
        INFO("said: " << line);
        CHECK(line.find("No audio") == std::string::npos);
        CHECK(line.find("Audio is back") == std::string::npos);
        nothingPlaying = nothingPlaying || line.find("Nothing is playing on") != std::string::npos;
    }
    CHECK(nothingPlaying);
    CHECK(tracker.running());
    CHECK_FALSE(controller.window().get_input_lost());
    controller.toggleRun();
}

TEST_CASE("an input that opens again and sends nothing is not back", "[ui][hardware]") {
    // An outage ended the moment a reopen *opened*, and said "Audio is back"; an input that
    // opens and then sends nothing began a new outage a moment later, counting its tries from
    // nothing, so the look at the device list every third try never came. The input here is an
    // idle loopback handed to the outage as though it were an interface — the one thing on a
    // machine that really does open and send nothing.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> quiet = idleLoopback(tracker);
    if (!quiet) {
        SKIP("something is playing on every loopback on this machine");
    }
    WindowController controller(tracker);
    InputDevice asInterface = *quiet;
    asInterface.isLoopback = false;
    double now = 100.0;
    controller.beginOutageOn({asInterface, takt4::audio::ChannelSelection::single(0)}, now);
    std::vector<std::string> said;
    for (int step = 0; step < 8; ++step) {
        now += 1.1;
        // A try begun is asked after until it has its answer, at the same moment, as the
        // redraws do: the open runs on the audio thread.
        controller.superviseInput({}, {}, now);
        REQUIRE(waitFor([&] {
            controller.superviseInput({}, {}, now);
            return !controller.outageAsking();
        }));
        said.emplace_back(controller.window().get_status());
        CHECK(controller.window().get_input_lost());
    }
    std::size_t reopened = 0;
    for (const std::string& line : said) {
        INFO("said: " << line);
        CHECK(line.find("Audio is back") == std::string::npos);
        reopened += line.find("waiting for it to send audio") != std::string::npos ? 1U : 0U;
    }
    CHECK(reopened >= 3); // tried, and tried again, and not taken for back
    controller.toggleRun();
    CHECK_FALSE(tracker.running());
}

TEST_CASE("the window switches the outputs back on", "[ui][network]") {
    takt4::testing::skipWhileLinkIsOnTheNetwork();
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.preset.link = true;
    saved.preset.oscPrefix = "/vj";
    saved.preset.outputs = takt4::output::oscOutputs({{"127.0.0.1", 57000}, {"127.0.0.1", 57001}});

    WindowController controller(tracker, saved);
    CHECK(seen(controller).link);
    CHECK(seen(controller).oscTargets == 2);
    CHECK(controller.outputs().transports().oscPrefix() == "/vj");
    // And into the rows that edit them, seeded once from what the runner was built with — the
    // Link row first, ticked as the file had it.
    const auto rows = controller.window().get_outputs_list();
    REQUIRE(rows->row_count() == 3);
    CHECK(rows->row_data(0)->kind_index == 4);
    CHECK(rows->row_data(0)->enabled);
    CHECK(std::string(rows->row_data(2)->address) == "127.0.0.1:57001");
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
    // Reproduced with the Release build before it was fixed: a saved output naming a MIDI
    // interface that was not plugged in threw out of the runner's constructor, out of this
    // class's, and out of `ui::run` — takt4 exited 0xC0000409 within seconds of every launch,
    // with no window and no message. A hostname that does not resolve yet did the same. The one
    // port that had been guarded was the MIDI *clock* (the test above).
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
    // OSC target of its own, looked up on a thread of its own, and is said when that look-up
    // fails — nothing waits for a name server any more.
    CHECK(seen(*controller).oscTargets == 2);
    CHECK(seen(*controller).targets.size() == 4); // the Link row, then the three
    CHECK(controller->window().get_outputs_list()->row_count() == 4);
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
    // The file was written on a clean exit and on SAVE, and a crash, a power cut or Windows
    // shutting down with the window open lost the whole session. No exit and no button here —
    // only the redraw timer, and the file appears.
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
    // (the save read the engine, which takes the change only at Start).
    controller.window().invoke_fold_min_changed(91.0f);
    for (std::uint64_t i = 0; i < 4 * WindowController::kAutosaveCheckTicks; ++i) {
        controller.tick();
    }
    CHECK_THAT(takt4::settings::load(file).preset.tempo.minBpm, WithinAbs(91.0, 1e-6));
}

TEST_CASE("the autosave builds the settings' text only when they have changed", "[ui][settings]") {
    // The text costs a large rig a dozen milliseconds of the window's thread (200 rules and 300
    // fixtures: 15 ms, measured 2026-10-22), and it was built twice a second whether or not
    // anything had changed.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "settings.json";
    controller.enableAutosave(file, 0.0);
    const auto looks = [&controller](std::uint64_t count) {
        for (std::uint64_t i = 0; i < count * WindowController::kAutosaveCheckTicks; ++i) {
            controller.tick();
        }
    };
    looks(10);
    CHECK(controller.autosaveTexts() == 1); // the first look, and none after it
    CHECK_FALSE(std::filesystem::exists(file));

    // A change is built, and written, as before.
    takt4::trigger::Rule::Config strobe;
    strobe.id = "strobe";
    strobe.address = "/composition/layers/1/clips/1/connect";
    controller.setRules({strobe});
    looks(4);
    CHECK(controller.autosaveTexts() == 2);
    REQUIRE(std::filesystem::exists(file));
    CHECK(takt4::settings::load(file).preset.rules.size() == 1);

    // And a change to any one field of a rule is one.
    strobe.every = 2;
    controller.setRules({strobe});
    looks(4);
    CHECK(controller.autosaveTexts() == 3);
    CHECK(takt4::settings::load(file).preset.rules.at(0).every == 2u);
}

TEST_CASE("a settings file held from the autosave is kept saved from the first SAVE to it",
          "[ui][settings]") {
    // A file a newer takt4 wrote is not saved over unasked, so its autosave starts off — and the
    // way out skipped it as well, so what was changed after a SAVE over it was lost at quit.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const std::filesystem::path file = takt4::settings::settingsFile(); // where SAVE writes
    REQUIRE_FALSE(file.empty());
    const std::string newer = R"({"version": 3, "machine": {"somethingNew": 7}})";
    std::filesystem::create_directories(file.parent_path());
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << newer;
    }
    REQUIRE(std::filesystem::exists(file));
    const auto contents = [&] {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    controller.holdAutosave(file, 0.0);
    CHECK_FALSE(controller.autosaving());
    CHECK(controller.window().get_autosave_held());

    takt4::trigger::Rule::Config strobe;
    strobe.id = "strobe";
    strobe.address = "/composition/layers/1/clips/1/connect";
    controller.setRules({strobe});
    for (std::uint64_t i = 0; i < 4 * WindowController::kAutosaveCheckTicks; ++i) {
        controller.tick();
    }
    CHECK(contents() == newer);

    // SAVE writes over it, as the start said it would, and from then on it is kept saved.
    REQUIRE(controller.saveNow());
    CHECK(controller.autosaving());
    CHECK_FALSE(controller.window().get_autosave_held());
    CHECK(takt4::settings::load(file).preset.rules.size() == 1);
    controller.window().invoke_fold_min_changed(91.0f);
    for (std::uint64_t i = 0; i < 4 * WindowController::kAutosaveCheckTicks; ++i) {
        controller.tick();
    }
    CHECK_THAT(takt4::settings::load(file).preset.tempo.minBpm, WithinAbs(91.0, 1e-6));
}

TEST_CASE("what the window hands back is what it was given", "[ui][network]") {
    takt4::testing::skipWhileLinkIsOnTheNetwork();
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
    REQUIRE(out.preset.outputs.size() == 2);
    CHECK(out.preset.outputs[0].kind == takt4::output::OutputTarget::Kind::Link);
    CHECK(out.preset.outputs[0].enabled);
    CHECK(out.preset.outputs[1].host == "192.0.2.40");
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
    // The exe is entirely self-contained: it relies on no weights or anything else in another
    // directory, and works anywhere.
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
    // The config can be saved without closing takt4: the only save used to be on the way out.
    // This is the round trip the buttons do.
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
    // And an output, which the name promised and nothing checked. At a receiver of the test's
    // own, so the sandbox lets what the window sends to it through.
    const takt4::testing::LoopbackReceiver deck;
    controller.setOscTargets("deck = 127.0.0.1:" + std::to_string(deck.port()));

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

    // The output, by name, host and port, alongside the Link output every set has.
    const auto outputs = seen(fresh).targets;
    const auto found = std::find_if(outputs.begin(), outputs.end(),
                                    [](const takt4::output::OutputTarget& t) { return t.name == "deck"; });
    REQUIRE(found != outputs.end());
    CHECK(found->kind == takt4::output::OutputTarget::Kind::Osc);
    CHECK(found->host == "127.0.0.1");
    CHECK(found->port == deck.port());
}

TEST_CASE("an import starts its rules afresh, whatever ids they share with the show before",
          "[ui][settings][trigger]") {
    // A rule that keeps its id across an edit carries on — its mute, its ÷2 — and so did one
    // across an IMPORT, since ids repeat from show to show (`add` numbers them rule1, rule2). A
    // show imported mid-set came in with its first rule muted and at half rate because the old
    // show's first rule had been. What happens is what a control surface does, then an IMPORT,
    // and the running rule and the editor are both asked.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::trigger::Rule::Config clip;
    clip.id = "rule1";
    clip.address = "/composition/layers/1/clips/1/connect";
    controller.setRules({clip});
    REQUIRE(controller.oscControl().dispatch("/takt4/ctl/rule/rule1/mute", 1.0));
    REQUIRE(controller.oscControl().dispatch("/takt4/ctl/rule/rule1/rate", 2.0));
    REQUIRE(controller.settleOutputs());
    const auto live = [&controller] {
        for (const auto& rule : controller.outputs().liveRules()) {
            if (rule.id == "rule1") {
                return rule;
            }
        }
        return takt4::output::OutputRunner::LiveRule{};
    };
    REQUIRE(live().id == "rule1");
    REQUIRE(live().muted);
    REQUIRE(live().rate == 2.0);
    controller.tick();
    REQUIRE(controller.editor().window().get_rule_muted());

    // Another show, whose first rule is called what this one's was.
    takt4::settings::Settings other;
    takt4::trigger::Rule::Config lights;
    lights.id = "rule1";
    lights.address = "/other/show/go";
    other.preset.rules = {lights};
    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "other.json";
    REQUIRE(takt4::settings::save(other, file));
    REQUIRE(controller.importFrom(file));
    // The editor, straight away: not the mute it last saw on a rule of that name.
    CHECK_FALSE(controller.editor().window().get_rule_muted());
    REQUIRE(controller.settleOutputs());
    CHECK(live().id == "rule1");
    CHECK_FALSE(live().muted);
    CHECK(live().rate == 1.0);
    controller.tick();
    CHECK_FALSE(controller.editor().window().get_rule_muted());
    CHECK(std::string(controller.editor().window().get_rule_rate()).empty());
}

TEST_CASE("an import brings the lighting patch with it", "[ui][settings][dmx]") {
    // IMPORT applied the rules and the outputs and left the patch alone, so every imported
    // lighting rule aimed at fixtures this rig did not have and reached nothing; and a file
    // holding only a patch was "no preset in it".
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
    // A path with characters outside ASCII, and the wider class behind it. Every string on
    // screen goes through `slint::SharedString`, which aborts the process on a byte that is not
    // UTF-8 — and the status line was built from `path.string()`, which on Windows converts
    // through the ANSI code page: "Friday’s set.json" came out as a lone 0x92 byte. A failure
    // here is not a failed assertion but the test binary dying, which is what the operator's
    // takt4 did.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / std::filesystem::path(L"Shows – 2026") /
                                       std::filesystem::path(L"Friday’s set.json");
    REQUIRE(controller.exportTo(file));
    REQUIRE(std::filesystem::exists(file));
    const std::string status(controller.window().get_status());
    CHECK(status.find("Shows \xE2\x80\x93 2026") != std::string::npos);
    CHECK(status.find("Friday\xE2\x80\x99s set.json") != std::string::npos);

    // And a name that arrived from a driver in its own encoding: an ASIO or MIDI device called
    // "µ-Port" read through the ANSI API is the byte 0xB5 on its own. The failure to open it
    // names it on the status line, which is where it used to abort.
    const takt4::testing::LoopbackReceiver added; // where the row points before it is MIDI
    controller.setNewOutputPort(added.port());
    controller.addTarget();
    controller.acceptTarget(1, "", "midiclock takt4 test \xB5-Port");
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

/// The right button, which puts a slider back to its default.
void rightClickAt(slint::Window& window, float x, float y) {
    const slint::LogicalPosition at({x, y});
    window.dispatch_pointer_move_event(at);
    window.dispatch_pointer_press_event(at, slint::PointerEventButton::Right);
    window.dispatch_pointer_release_event(at, slint::PointerEventButton::Right);
}

void press(slint::Window& window, const std::string& key) {
    window.dispatch_key_press_event(slint::SharedString(key));
    window.dispatch_key_release_event(slint::SharedString(key));
}

const std::string kEscape(1, '\x1b');

} // namespace

TEST_CASE("a double click on PANIC leaves it engaged, and RELEASE lets it go", "[ui]") {
    // PANIC was a plain toggle, so the way a button is hit in a hurry — twice — halted the rig
    // and let go of it again before it had visibly lit. Driven with real clicks, because the
    // fix is as much where RELEASE appears as what PANIC does: RELEASE shows up to PANIC's
    // *left*, so the second click of a double-click lands on PANIC again.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    constexpr float kWidth = 1000.0f;
    constexpr float kHeight = 760.0f;
    layOut(controller, kWidth, kHeight);
    auto& window = controller.window().window();
    const takt4::tests::NothingReal nothingReal;

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
    nothingReal.check();
}

TEST_CASE("the keep for the next track box is ticked by a click", "[ui]") {
    // Found on the laid-out window by clicking down a column through the box's words until the
    // box ticks — not assumed from the markup — and read back from the engine, which is what an
    // operator's click has to reach. Its words as well as its box: since 2026-09-25 they sit to
    // the right of DOWNBEAT, and a label that does nothing when clicked is not one.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    SyntheticRun run(tracker);
    layOut(controller, 1000.0f, 760.0f);
    auto& window = controller.window().window();
    const takt4::tests::NothingReal nothingReal;
    REQUIRE_FALSE(controller.window().get_keep_shift());

    // Through "keep half/double settings for next track", right of the box.
    constexpr float kWords = 700.0f;
    float boxY = 0.0f;
    for (float y = 150.0f; y < 620.0f && boxY == 0.0f; y += 3.0f) {
        clickAt(window, kWords, y);
        if (controller.window().get_keep_shift()) {
            boxY = y;
        }
    }
    INFO("no click through the words ticked the box");
    REQUIRE(boxY > 0.0f);
    run.applyPosted();
    CHECK(tracker.engine().tempoOptions().keepOctaveShift);

    clickAt(window, kWords, boxY);
    CHECK_FALSE(controller.window().get_keep_shift());
    run.applyPosted();
    CHECK_FALSE(tracker.engine().tempoOptions().keepOctaveShift);

    // And the box itself, left of its words and right of DOWNBEAT — anywhere between the two,
    // since 2026-10-08: it starts where "latency" starts under it, which moves with the width.
    bool ticked = false;
    for (float x = 440.0f; x < kWords && !ticked; x += 3.0f) {
        clickAt(window, x, boxY);
        ticked = controller.window().get_keep_shift();
    }
    CHECK(ticked);
    nothingReal.check();
}

TEST_CASE("removing an output row does not leave the keyboard dead", "[ui]") {
    // The same trap by a quieter door: the row's "−" button takes the focus on the click and is
    // then destroyed with its row, which left Escape reaching nothing until the next click.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    // Rows that send to the test's own receiver, not to this machine's 9000 (this is the test
    // that sent the operator's receiver nine datagrams).
    const takt4::testing::LoopbackReceiver added;
    controller.setNewOutputPort(added.port());
    controller.addTarget();
    controller.addTarget();
    constexpr float kWidth = 1000.0f;
    constexpr float kHeight = 1000.0f; // the rows on screen, below the inputs
    layOut(controller, kWidth, kHeight);
    auto& window = controller.window().window();
    const takt4::tests::NothingReal nothingReal;

    // The "×" at the end of a row: found by clicking **up** the column from the pinned footer until
    // a row goes. Up, because the fold arrows of Inputs and Outputs are in this column above the
    // rows, and a sweep down it folded both sections away before it reached a row (rendered).
    const float removeX = kWidth - 38.0f;
    bool removed = false;
    for (float y = kHeight - 140.0f; y > 380.0f && !removed; y -= 4.0f) {
        clickAt(window, removeX, y);
        removed = controller.window().get_outputs_list()->row_count() == 2;
    }
    REQUIRE(removed);
    press(window, kEscape);
    CHECK(panickedNow(controller));
    nothingReal.check();
}

TEST_CASE("Escape engages PANIC, except in a text box, where it only leaves the box", "[ui]") {
    // PANIC by keyboard is not negotiable for live use, and no `.slint` file handled a key at
    // all. And the other half, which is what makes Escape safe to bind: an operator who presses
    // Escape to get out of a text box must not halt the show.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver added; // the new row's, not this machine's 9000
    controller.setNewOutputPort(added.port());
    controller.addTarget();            // a row with a name box in it, after the Link row
    constexpr float kHeight = 1000.0f; // the rows on screen, below the inputs
    layOut(controller, 1000.0f, kHeight);
    auto& window = controller.window().window();
    const takt4::tests::NothingReal nothingReal;

    // Nothing clicked yet: the key reaches the window's own scope, and PANIC engages.
    press(window, kEscape);
    CHECK(panickedNow(controller));
    controller.releasePanic();
    REQUIRE_FALSE(panickedNow(controller));

    // The name box of that row, found by typing into candidates until a name commits. A miss
    // leaves Escape to engage PANIC, which is let go before the next try; the hit is the only
    // round in which Escape was pressed *inside* a box, and that is the round asserted on.
    //
    // Up from the bottom of the body, where the rows are, so the sweep meets the row before it
    // could reach the inputs above, whose "listen" box would open a real socket. Through the
    // name box's middle; ADD OUTPUT is in that column too, below the row, and a row it adds is
    // taken away again.
    bool found = false;
    bool panickedInBox = true;
    for (float y = kHeight - 128.0f; y > 380.0f && !found; y -= 4.0f) {
        clickAt(window, 110.0f, y);
        press(window, "q");
        press(window, kEscape);
        // The box commits from its `changed has-focus`, which runs a loop later.
        controller.tick();
        slint::platform::update_timers_and_animations();
        const auto rows = controller.window().get_outputs_list();
        if (rows->row_count() > 1 && std::string(rows->row_data(1)->name) == "q") {
            found = true;
            panickedInBox = panickedNow(controller);
        }
        if (panickedNow(controller)) {
            controller.releasePanic();
        }
        while (rows->row_count() > 2) {
            controller.removeTarget(static_cast<int>(rows->row_count()) - 1);
        }
    }
    INFO("no click landed in the row's name box");
    REQUIRE(found);
    CHECK_FALSE(panickedInBox);

    // And straight after leaving the box, Escape is PANIC again: leaving it put the focus
    // somewhere that still passes keys up, rather than nowhere.
    press(window, kEscape);
    CHECK(panickedNow(controller));
    nothingReal.check();
}

TEST_CASE("clicking from a row's name box into its host box keeps what is typed there", "[ui]") {
    // Leaving the name box commits it, and committing it rebuilt the whole row a redraw later —
    // the host box the operator had just clicked into with it, so what they typed next went
    // nowhere. Driven as an operator does it: a click, a letter, a click on the next box along,
    // a digit, Enter.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver added; // the new row's, not this machine's 9000
    controller.setNewOutputPort(added.port());
    controller.addTarget();
    constexpr float kHeight = 1000.0f; // the rows on screen, below the inputs
    layOut(controller, 1000.0f, kHeight);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto rows = controller.window().get_outputs_list();
    const auto live = [&controller] { return seen(controller).targets; };
    const takt4::tests::NothingReal nothingReal;

    // The row, found by its name box as the Escape test finds it: a letter and Enter that
    // name the output. A click on ADD OUTPUT on the way adds a row, which is taken away again.
    float rowY = -1.0f;
    for (float y = kHeight - 128.0f; y > 380.0f && rowY < 0.0f; y -= 4.0f) {
        clickAt(window, 110.0f, y);
        press(window, "n");
        press(window, "\n");
        settle();
        if (rows->row_count() > 1 && std::string(rows->row_data(1)->name) == "n") {
            rowY = y;
        }
        while (rows->row_count() > 2) {
            controller.removeTarget(static_cast<int>(rows->row_count()) - 1);
        }
    }
    {
        INFO("no click reached the row's name box");
        REQUIRE(rowY > 0.0f);
    }
    REQUIRE(live()[1].host == "127.0.0.1");

    clickAt(window, 110.0f, rowY);
    press(window, "a");
    settle();
    clickAt(window, 300.0f, rowY); // the host box, on the same row
    settle();                      // the name commits, and the row is redrawn
    settle();
    press(window, "9");
    press(window, "\n");
    settle();

    const std::string name = live()[1].name;
    const std::string host = live()[1].host;
    INFO("name '" << name << "', host '" << host << "'");
    CHECK(name == "a"); // the box selects what it holds when clicked into, so "a" replaced "n"
    CHECK(host.size() == std::string("127.0.0.1").size() + 1);
    CHECK(host.find('9') != std::string::npos);
    nothingReal.check();
}

namespace {

/// The middle of each output row: found from the bottom of the body up by typing a letter and
/// Enter into whatever is at x = 110 and seeing which row it named — put back straight away —
/// and taking the middle of the span of clicks that named each. The row's ×, and its other
/// boxes, sit on that line; the edge of a name box is outside the smaller × beside it. A click
/// on ADD OUTPUT on the way adds a row, which is taken away again. Row 0 is Link, which has no
/// name box, and is left at -1.
std::vector<float> outputRowsAt(WindowController& controller, float height) {
    auto& window = controller.window().window();
    const auto rows = controller.window().get_outputs_list();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const std::size_t count = rows->row_count();
    std::vector<float> bottom(count, -1.0f);
    std::vector<float> top(count, -1.0f);
    for (float y = height - 128.0f; y > 380.0f; y -= 2.0f) {
        std::vector<std::string> names;
        for (std::size_t i = 0; i < count; ++i) {
            names.emplace_back(rows->row_data(i)->name);
        }
        clickAt(window, 110.0f, y);
        press(window, "n");
        press(window, "\n");
        settle();
        while (rows->row_count() > count) {
            controller.removeTarget(static_cast<int>(rows->row_count()) - 1);
            settle();
        }
        bool hit = false;
        for (std::size_t i = 1; i < count; ++i) {
            if (std::string(rows->row_data(i)->name) != names[i]) {
                hit = true;
                if (bottom[i] < 0.0f) {
                    bottom[i] = y;
                }
                top[i] = y;
                controller.setTargetName(static_cast<int>(i), names[i], true);
                settle();
            }
        }
        // Past the top of the first row, which is the last to be met: the inputs are above.
        if (!hit && count > 1 && top[1] > 0.0f) {
            break;
        }
    }
    std::vector<float> middle(count, -1.0f);
    for (std::size_t i = 1; i < count; ++i) {
        if (bottom[i] > 0.0f) {
            middle[i] = (bottom[i] + top[i]) / 2.0f;
        }
    }
    return middle;
}

} // namespace

TEST_CASE("typing a host and then removing the row above it keeps the host on its own output",
          "[ui]") {
    // A box commits when it loses the keyboard, a turn of the event loop after the click that
    // took it — and it commits by its row's index. × on a row above rewrote the list in place,
    // so the element holding the typed host was handed the *next* output, and the late commit
    // sent that output's messages to the host typed for this one.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver added;
    controller.setNewOutputPort(added.port());
    controller.addTarget(); // A
    controller.addTarget(); // B
    controller.addTarget(); // C
    constexpr float kWidth = 1000.0f;
    constexpr float kHeight = 1100.0f;
    layOut(controller, kWidth, kHeight);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto rows = controller.window().get_outputs_list();
    const takt4::tests::NothingReal nothingReal;
    REQUIRE(rows->row_count() == 4);
    controller.setTargetName(1, "A", true);
    controller.setTargetName(2, "B", true);
    controller.setTargetName(3, "C", true);
    settle();

    const std::vector<float> at = outputRowsAt(controller, kHeight);
    INFO("A at " << at[1] << ", B at " << at[2] << ", C at " << at[3]);
    REQUIRE(at[1] > 0.0f);
    REQUIRE(at[2] > 0.0f);
    REQUIRE(at[3] > 0.0f);
    const std::string cHost(rows->row_data(3)->host);

    // B's host box, typed into and not entered; then A's ×.
    clickAt(window, 300.0f, at[2]);
    settle();
    takt4::tests::endOfText(window);
    for (int i = 0; i < 20; ++i) {
        press(window, "\b");
    }
    for (const char c : std::string("127.0.0.9")) {
        press(window, std::string(1, c));
    }
    clickAt(window, kWidth - 34.0f, at[1]);
    // What the event loop does next, in the order it does it: the box's late commit, then a
    // redraw — which would otherwise build the moved row again before the commit could land.
    slint::platform::update_timers_and_animations();
    settle();
    settle();

    const auto live = seen(controller).targets;
    REQUIRE(rows->row_count() == 3);
    const auto find = [&live](const std::string& name) {
        for (const takt4::output::OutputTarget& target : live) {
            if (target.name == name) {
                return target;
            }
        }
        return takt4::output::OutputTarget{};
    };
    CHECK(find("A").name.empty());         // gone
    CHECK(find("B").host == "127.0.0.9");  // the output it was typed for
    CHECK(find("C").host == cHost);        // and not the one that moved up under the box
    nothingReal.check();
}

TEST_CASE("an output's on tick follows its row when the rows change under it", "[ui]") {
    // A tick box sets its own `checked` when clicked, which drops its binding to the row; the
    // row was rebuilt for what its text boxes showed and not for this. So after a tick had been
    // clicked, the rows changing under it — a line of outputs pasted in, an IMPORT — left that
    // tick showing the old output's state over the new one.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver added;
    controller.setNewOutputPort(added.port());
    controller.addTarget();
    controller.addTarget();
    constexpr float kHeight = 1000.0f;
    layOut(controller, 1000.0f, kHeight);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto rows = controller.window().get_outputs_list();
    const takt4::tests::NothingReal nothingReal;
    REQUIRE(rows->row_count() == 3);

    // The last row's tick, from the bottom up: the first click after which it is off. Never
    // the Link row's, which is first and would be met last.
    float tickY = -1.0f;
    for (float y = kHeight - 128.0f; y > 380.0f && tickY < 0.0f; y -= 4.0f) {
        clickAt(window, 41.0f, y); // the middle of the "on" column, 26 to 56
        settle();
        while (rows->row_count() > 3) {
            controller.removeTarget(static_cast<int>(rows->row_count()) - 1);
            settle();
        }
        if (!rows->row_data(2)->enabled) {
            tickY = y;
        }
    }
    INFO("tick at " << tickY);
    REQUIRE(tickY > 0.0f);

    // Two new outputs in place of the old ones, both on.
    const std::string port = std::to_string(added.port());
    controller.setOscTargets("x = 127.0.0.1:" + port + ", y = 127.0.0.1:" + port);
    settle();
    settle();
    REQUIRE(rows->row_count() == 3);
    REQUIRE(std::string(rows->row_data(2)->name) == "y");
    REQUIRE(rows->row_data(2)->enabled);

    // The tick under the pointer is y's now, and shows it on — so clicking it switches y off.
    clickAt(window, 41.0f, tickY);
    settle();
    CHECK_FALSE(rows->row_data(2)->enabled);
    nothingReal.check();
}

TEST_CASE("takt4's own messages go to no OSC output until one is ticked in the outputs heading",
          "[ui]") {
    // A receiver of its rules alone logs every /takt4 address it is sent as unhandled — so since
    // 2026-10-02 they go nowhere until asked
    // for, in the outputs heading's "/takt4 global messages to" (`OutputTarget::sendsNamespace`).
    // This reads what reaches two outputs as the tempo moves, before and after the deck is ticked
    // there with clicks, as an operator would: nothing at first, then the deck and never the winch,
    // whose own cue still reaches it. (The state addresses, not the beats: they go out whenever
    // they change, stopped or not, and only START — a device — lets the window's runner send
    // beats. `/takt4/bpm` is what the receiver logged; the beats take the same road through
    // `OscPublisher`, and its tests hold them back too.)
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::testing::LoopbackReceiver deck;
    takt4::testing::LoopbackReceiver winch;
    const auto oscOutput = [](const char* id, const char* name, std::uint16_t port) {
        takt4::output::OutputTarget out;
        out.id = id;
        out.name = name;
        out.kind = takt4::output::OutputTarget::Kind::Osc;
        out.host = "127.0.0.1";
        out.port = port;
        return out;
    };
    takt4::settings::Settings saved;
    saved.preset.outputs = {oscOutput("o-00000dec", "deck", deck.port()),
                            oscOutput("o-0000e9b0", "winch", winch.port())};
    takt4::trigger::Rule::Config cue;
    cue.id = "cue";
    cue.trigger = takt4::trigger::Trigger::Manual;
    cue.address = "/winch/cue";
    cue.outputs = {"o-0000e9b0"};
    saved.preset.rules = {cue};
    WindowController controller(tracker, saved);
    constexpr int kWidth = 800;
    constexpr int kHeight = 1400; // everything in view, nothing to scroll
    const takt4::tests::Shot shot = takt4::tests::render(controller.window(), kWidth, kHeight);
    auto& window = controller.window().window();
    window.dispatch_window_active_changed_event(true);
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto rows = controller.window().get_outputs_list();
    REQUIRE(rows->row_count() == 3); // Link, the deck, the winch
    REQUIRE(std::string(rows->row_data(1)->name) == "deck");
    REQUIRE(std::string(rows->row_data(2)->name) == "winch");
    CHECK_FALSE(rows->row_data(1)->sends_namespace);
    CHECK_FALSE(rows->row_data(2)->sends_namespace);
    CHECK(std::string(controller.window().get_global_summary()) == "nothing");

    const auto heard = [](takt4::testing::LoopbackReceiver& receiver) {
        std::vector<std::string> got;
        for (std::string datagram = receiver.receive(); !datagram.empty();
             datagram = receiver.receive()) {
            got.push_back(datagram.substr(0, datagram.find('\0')));
        }
        return got;
    };
    const auto count = [](const std::vector<std::string>& got, std::string_view prefix) {
        return std::count_if(got.begin(), got.end(),
                             [prefix](const std::string& a) { return a.starts_with(prefix); });
    };

    // The committed excerpt, until the tracker locks: a tempo, a confidence and a lock, each of
    // which would be sent as it changes — to nobody, since nobody has asked.
    SyntheticRun run(tracker);
    REQUIRE(run.untilLocked());
    REQUIRE(controller.settleOutputs());
    CHECK(count(heard(deck), "/takt4") == 0);
    CHECK(count(heard(winch), "/takt4") == 0);

    // The box, at the right of the outputs heading after its label, opens the list; its lines are
    // found by probing, since where a popup opens is Slint's to say. A probe that misses the list
    // closes it — and sets off nothing under it, which the click sweep of every control holds — so
    // it is opened again for each one. Hunted outward from the box, clear of the top bar and the
    // pinned footer; any change a probe makes that is not the one looked for is put back.
    const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
    REQUIRE(sheets.size() == 7);
    const int headRow = sheets[5].first + 10 + 14;
    const auto head = occupied(shot, headRow - 13, headRow + 13, 300, kWidth - 56, kSheet, 5);
    INFO("the outputs heading: " << spans(head));
    // The label, the box, and the phrases' "in phrases of", count and "bars".
    REQUIRE(head.size() == 5);
    const float boxX = middleOf(head[1]);
    const auto flags = [&rows] {
        return std::pair{rows->row_data(1)->sends_namespace, rows->row_data(2)->sends_namespace};
    };
    const auto find = [&](std::pair<bool, bool> wanted) {
        for (int d = 0; d < 900; d += 3) {
            for (const int y : {headRow + d, headRow - d}) {
                if (y < 100 || y >= kHeight - 130) {
                    continue;
                }
                if (!controller.window().get_global_open()) {
                    clickAt(window, boxX, static_cast<float>(headRow));
                    settle();
                }
                const auto before = flags();
                // Under the box: the list hangs from its right edge (`GlobalPick`).
                clickAt(window, boxX, static_cast<float>(y));
                settle();
                const auto after = flags();
                if (after != before && after == wanted) {
                    return static_cast<float>(y);
                }
                if (after != before) {
                    controller.setTargetNamespace(1, before.first);
                    controller.setTargetNamespace(2, before.second);
                    settle();
                }
            }
        }
        return -1.0f;
    };
    const takt4::tests::NothingReal nothingReal;
    const float deckLine = find({true, false});
    INFO("the deck's line at " << deckLine);
    REQUIRE(deckLine > 0.0f);
    CHECK(std::string(controller.window().get_global_summary()) == "deck");
    CHECK(seen(controller).targets[1].sendsNamespace);
    CHECK_FALSE(seen(controller).targets[2].sendsNamespace);
    const std::string file = takt4::settings::toJson(controller.currentSettings());
    CHECK(file.find("deck = 127.0.0.1:" + std::to_string(deck.port()) + " global") !=
          std::string::npos);
    CHECK(file.find("winch = 127.0.0.1:" + std::to_string(winch.port()) + " #") !=
          std::string::npos);
    // Escape closes the list and goes no further: in this window it is PANIC.
    REQUIRE(controller.window().get_global_open());
    press(window, kEscape);
    settle();
    CHECK_FALSE(controller.window().get_global_open());
    CHECK_FALSE(panickedNow(controller));

    // The tempo moves: ÷2, then on with the excerpt. The deck hears it; the winch does not. Both
    // ports emptied first: a probe on the way to the deck's line may have ticked "every OSC
    // output" and been put back, and the winch was told the state in that moment.
    (void)heard(deck);
    (void)heard(winch);
    controller.window().invoke_halve();
    run.applyPosted();
    (void)run.until([] { return false; }); // the rest of the excerpt
    REQUIRE(controller.settleOutputs());
    const std::vector<std::string> atDeck = heard(deck);
    const std::vector<std::string> atWinch = heard(winch);
    INFO("deck heard " << atDeck.size() << ", the winch " << atWinch.size());
    CHECK(count(atDeck, "/takt4/bpm") > 0);
    CHECK(count(atWinch, "/takt4") == 0);

    // Its rule reaches it all the same: the M key fires the manual cue.
    press(window, "m");
    REQUIRE(controller.settleOutputs());
    CHECK(heard(winch) == std::vector<std::string>{"/winch/cue"});

    // "every OSC output" ticks the winch too — and it is told the state at once, though nothing
    // has moved since, as a new output is.
    const float everyLine = find({true, true});
    INFO("the every-OSC-output line at " << everyLine);
    REQUIRE(everyLine > 0.0f);
    CHECK(std::string(controller.window().get_global_summary()) == "every OSC output");
    REQUIRE(controller.settleOutputs());
    CHECK(count(heard(winch), "/takt4/bpm") > 0);
    // And again, all of them off.
    clickAt(window, boxX, everyLine);
    settle();
    CHECK(flags() == std::pair{false, false});
    CHECK(std::string(controller.window().get_global_summary()) == "nothing");
    nothingReal.check();
}

TEST_CASE("an output's delay slider follows a real drag on the running window", "[ui]") {
    // No test dragged the real slider on a window with a controller behind it — the drag tests
    // used a bare window, or called the controller. What is being guarded is the slider taken
    // away under the pointer when its row is built again; with the element destroyed the drag
    // stops following the hand after the first redraw, so what is counted is how many different
    // delays one drag produces, with a redraw between every move.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver added;
    controller.setNewOutputPort(added.port());
    controller.addTarget();
    constexpr float kWidth = 1000.0f;
    constexpr float kHeight = 1000.0f;
    layOut(controller, kWidth, kHeight);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto rows = controller.window().get_outputs_list();
    const takt4::tests::NothingReal nothingReal;
    REQUIRE(rows->row_count() == 2);
    settle();
    const std::vector<float> at = outputRowsAt(controller, kHeight);
    REQUIRE(at[1] > 0.0f);
    const auto delay = [&rows] { return rows->row_data(1)->delay_ms; };

    // The slider's track on that row: the first point along it whose click moves the delay.
    float trackX = -1.0f;
    for (float x = 420.0f; x < kWidth - 120.0f && trackX < 0.0f; x += 10.0f) {
        clickAt(window, x, at[1]);
        settle();
        if (delay() != 0.0f) {
            trackX = x;
        }
        controller.setTargetDelay(1, 0.0f);
        settle();
    }
    INFO("track at " << trackX << ", " << at[1]);
    REQUIRE(trackX > 0.0f);

    const auto watch = std::make_shared<ModelWatch>();
    rows->attach_peer(watch);
    std::set<float> seenDelays;
    const float startX = trackX + 20.0f;
    window.dispatch_pointer_move_event(slint::LogicalPosition({startX, at[1]}));
    window.dispatch_pointer_press_event(slint::LogicalPosition({startX, at[1]}),
                                        slint::PointerEventButton::Left);
    settle();
    for (int step = 1; step <= 40; ++step) {
        window.dispatch_pointer_move_event(
            slint::LogicalPosition({startX + static_cast<float>(step) * 4.0f, at[1]}));
        settle();
        seenDelays.insert(delay());
    }
    window.dispatch_pointer_release_event(
        slint::LogicalPosition({startX + 160.0f, at[1]}), slint::PointerEventButton::Left);
    settle();

    INFO(seenDelays.size() << " different delays over forty moves");
    CHECK(seenDelays.size() >= 30);
    CHECK(watch->resets == 0);
    CHECK(watch->removed == 0);
    CHECK(watch->added == 0);
    nothingReal.check();
}

TEST_CASE("EXPORT carries a host still being typed", "[ui][settings]") {
    // The other half of a box's late commit: EXPORT and SAVE write what the output thread has,
    // and a host typed and not entered was only a draft until the box let go of the keyboard — a
    // turn of the event loop after the click on EXPORT.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver added;
    controller.setNewOutputPort(added.port());
    controller.addTarget();
    controller.setTargetHost(1, "127.0.0.7", false); // a keystroke's worth, not Enter

    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "show.json";
    REQUIRE(controller.exportTo(file));
    const takt4::settings::Settings written = takt4::settings::load(file);
    bool carried = false;
    for (const takt4::output::OutputTarget& target : written.preset.outputs) {
        carried = carried || target.host == "127.0.0.7";
    }
    CHECK(carried);
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
    // The editor offered "manual hotkey" as a trigger and nothing anywhere could fire one, so a
    // rule set to it never did anything. The hotkey is M.
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

    // And the key is taught where the trigger is picked: the rule editor's list of triggers names
    // it (it was "manual hotkey", which said there was a key and not which).
    const auto triggers = controller.editor().window().get_trigger_names();
    bool taught = false;
    for (std::size_t i = 0; i < triggers->row_count(); ++i) {
        taught = taught || std::string(*triggers->row_data(i)) == "manual (M key)";
    }
    CHECK(taught);

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
    // Slint's event loop runs until the **last** window is hidden, and the rule editor is a
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
    // The same failure through the other door: the close handler hid the rule editor and
    // nothing else, so with PATCH LIGHTS open the process stayed up with the patch window alone
    // on screen — the interface, Link and the ports all still held.
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

TEST_CASE("closing the main window closes the About box with it", "[ui]") {
    // The same failure a third time. The About box is a window of its own, the close handler
    // hid the two editors and not it — and with ABOUT open, closing takt4 left it running with
    // nothing on screen but that: the tracker, Link, the MIDI clock and Art-Net still going,
    // nothing saved, the interface held.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.window().invoke_about_opened();
    REQUIRE(controller.about() != nullptr);
    REQUIRE(controller.about()->window().is_visible());

    controller.window().window().dispatch_close_requested_event();
    CHECK_FALSE(controller.about()->window().is_visible());
}

TEST_CASE("the add a rule button adds one when there are none, and only opens the editor after",
          "[ui]") {
    // Found walking the window as a new user on 2026-09-30: with no rules the triggers row's
    // button reads "add a rule", and clicking it opened an empty editor that said "pick a rule
    // on the left, or + to add one". Clicked for real, on the row where the sweep of every
    // control finds it.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    REQUIRE(controller.editor().rules().empty());
    constexpr int kWidth = 800;
    constexpr int kHeight = 1200;
    auto& window = controller.window().window();
    const auto trigButton = [&](std::size_t drawn) {
        const takt4::tests::Shot shot = takt4::tests::render(controller.window(), kWidth, kHeight);
        window.dispatch_window_active_changed_event(true);
        const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
        REQUIRE(sheets.size() == 7);
        const int row = sheets[6].first + 33;
        const auto trig = occupied(shot, row - 23, row + 22, 20, kWidth - 20, kSheet, 5);
        INFO("triggers: " << spans(trig));
        // 04, triggers, the rules button, its count once there are rules ("1 of 1 active"), the
        // fixtures button, PANIC.
        REQUIRE(trig.size() == drawn);
        return std::pair<float, float>{middleOf(trig[2]), static_cast<float>(row)};
    };
    const auto [x, y] = trigButton(5);
    clickAt(window, x, y);
    slint::platform::update_timers_and_animations();
    controller.tick();
    CHECK(controller.editor().visible());
    REQUIRE(controller.editor().rules().size() == 1);
    CHECK(controller.editor().window().get_selected() == 0);
    CHECK(controller.window().get_rules_total() == 1);

    // With a rule, the button reads "edit rules" and only opens the editor; how many are active
    // is said beside it, not on it.
    controller.editor().hide();
    const auto [again, row] = trigButton(6);
    clickAt(window, again, row);
    slint::platform::update_timers_and_animations();
    CHECK(controller.editor().visible());
    CHECK(controller.editor().rules().size() == 1);
}

TEST_CASE("an editor's own close box closes it as CLOSE does", "[ui]") {
    // The other way an editor closes: the window's close box hid it behind its controller's
    // back, so the patch editor went on redrawing its levels thirty times a second with nothing
    // on screen, and what was being typed was never committed.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.window().show();
    controller.window().invoke_fixtures_clicked();
    controller.openEditor();
    REQUIRE(controller.patchEditor().visible());
    REQUIRE(controller.editor().visible());

    controller.patchEditor().window().window().dispatch_close_requested_event();
    CHECK_FALSE(controller.patchEditor().visible());
    CHECK_FALSE(controller.patchEditor().window().window().is_visible());
    controller.editor().window().window().dispatch_close_requested_event();
    CHECK_FALSE(controller.editor().visible());
    CHECK_FALSE(controller.editor().window().window().is_visible());
    // And the main window is still up: an editor closing is not takt4 closing.
    CHECK(controller.window().window().is_visible());
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
    // it, so this is the guard.
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
    // Met on the rig: the patch editor's IDENTIFY said "Identifying..." and nothing went out,
    // because the outputs only ran while the tracker did. An operator in the truss patching a
    // rig before the doors open is exactly who presses it.
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

TEST_CASE("the end of the Windows session leaves the rig as quitting does", "[ui][dmx]") {
    // Windows shutting down ends the process once the window has answered, and nothing after it
    // ran — not the way out that lets go of what is held and puts the lights out. A par lit by
    // IDENTIFY stayed lit through the shutdown.
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
    controller.patchEditor().pick(0);
    controller.patchEditor().identify();
    // An ArtDmx frame's levels start at byte 18; the par is channels 1 to 3.
    const auto levels = [](const std::string& datagram) -> std::optional<std::array<int, 3>> {
        if (datagram.size() < 21 || datagram.compare(0, 8, std::string("Art-Net\0", 8)) != 0) {
            return std::nullopt;
        }
        return std::array<int, 3>{static_cast<std::uint8_t>(datagram[18]),
                                  static_cast<std::uint8_t>(datagram[19]),
                                  static_cast<std::uint8_t>(datagram[20])};
    };
    bool lit = false;
    for (int attempt = 0; attempt < 200 && !lit; ++attempt) {
        const std::string datagram = node.receive();
        if (datagram.empty()) {
            break;
        }
        const auto par = levels(datagram);
        lit = par && (*par)[0] > 200 && (*par)[1] > 200 && (*par)[2] > 200;
    }
    REQUIRE(lit);

    controller.endSession();
    CHECK_FALSE(controller.outputs().running());
    std::optional<std::array<int, 3>> last;
    for (std::string datagram = node.receive(); !datagram.empty(); datagram = node.receive()) {
        if (const auto par = levels(datagram)) {
            last = par;
        }
    }
    REQUIRE(last.has_value());
    CHECK((*last)[0] == 0);
    CHECK((*last)[1] == 0);
    CHECK((*last)[2] == 0);
}

TEST_CASE("IDENTIFY lights a par without a dimmer that a rule left at nothing", "[ui][dmx]") {
    // A par without a dimmer has its colour scaled by an intensity of its own, and a dimmer rule
    // that took it to nothing left it there — so IDENTIFY's colour flashes came out dark. It
    // flashes that intensity as well now.
    takt4::testing::LoopbackReceiver node;
    takt4::settings::Settings settings;
    takt4::output::OutputTarget truss;
    truss.name = "truss";
    truss.kind = takt4::output::OutputTarget::Kind::ArtNet;
    truss.host = "127.0.0.1";
    truss.port = node.port();
    settings.preset.outputs = {truss};
    settings.preset.fixtures = {takt4::dmx::fixtureFromMode("par", 1, 0, 1)};
    takt4::trigger::Rule::Config out;
    out.id = "out";
    out.trigger = takt4::trigger::Trigger::Manual;
    out.sendKind = takt4::trigger::Message::Kind::Dmx;
    out.dmx.fixtures = {"par"};
    out.dmx.effect = takt4::dmx::EffectKind::Level;
    out.dmx.role = takt4::dmx::Role::Dimmer;
    out.dmx.level.kind = takt4::trigger::GeneratorKind::Fixed;
    out.dmx.level.fixed = takt4::trigger::Value::ofInt(0);
    out.dmx.durationBeats = 0.0;
    settings.preset.rules = {out};

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker, settings);
    const auto frame = [&node] {
        const std::string datagram = node.receive();
        return datagram.size() >= 21 && datagram.compare(0, 8, std::string("Art-Net\0", 8)) == 0
                   ? std::array<int, 3>{static_cast<std::uint8_t>(datagram[18]),
                                        static_cast<std::uint8_t>(datagram[19]),
                                        static_cast<std::uint8_t>(datagram[20])}
                   : std::array<int, 3>{-1, -1, -1};
    };
    // Lit white first, so the rule has something to take to nothing.
    controller.patchEditor().pick(0);
    controller.patchEditor().identify();
    controller.editor().pick(0);
    controller.editor().test();
    bool dark = false;
    for (int attempt = 0; attempt < 400 && !dark; ++attempt) {
        const std::array<int, 3> rgb = frame();
        dark = rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0;
    }
    REQUIRE(dark);
    controller.patchEditor().identify();
    bool lit = false;
    for (int attempt = 0; attempt < 400 && !lit; ++attempt) {
        const std::array<int, 3> rgb = frame();
        lit = rgb[0] > 200 && rgb[1] > 200 && rgb[2] > 200;
    }
    CHECK(lit);
}

TEST_CASE("an Art-Net row's delay slider moves that node's delay, as every row's does", "[ui]") {
    // Rather than the delay being taken off the rows where it did nothing, every output has one
    // that works the same way, and a node's is honoured — the node is sent the lighting that
    // much later (`dmx::ArtNetPublisher`). Driven by real clicks: what is under test is what the
    // row draws and where a click on it goes.
    LiveTracker tracker(kWeights, kStateSpace);
    // Receivers of the test's own: this used to aim the node at 127.0.0.1:6454, which is where
    // the rig's own Art-Net input listens.
    const takt4::testing::LoopbackReceiver deckEnd;
    const takt4::testing::LoopbackReceiver nodeEnd;
    takt4::settings::Settings saved;
    for (const std::string& line : {"deck = 127.0.0.1:" + std::to_string(deckEnd.port()),
                                   "node = artnet 127.0.0.1:" + std::to_string(nodeEnd.port())}) {
        takt4::output::OutputTarget target;
        REQUIRE(takt4::output::parseOutputTarget(line, target));
        saved.preset.outputs.push_back(target);
    }
    WindowController controller(tracker, saved);
    layOut(controller, 1000.0f, 1400.0f);
    auto& window = controller.window().window();
    const auto rows = controller.window().get_outputs_list();
    REQUIRE(rows->row_count() == 3); // the Link row, deck, node
    REQUIRE(rows->row_data(2)->kind_index == 2);
    const takt4::tests::NothingReal nothingReal;

    // The node's slider: the first click, across the sliders' column, that moves the node's
    // delay. **Up from the footer**, where the rows are on a tall window and the node's is the
    // last: the sweep used to start at the top and click its way through every control above
    // them — START, RESCAN, the OSC "listen" box — to get here.
    float sliderY = -1.0f;
    for (float y = 1270.0f; y > 200.0f && sliderY < 0.0f; y -= 4.0f) {
        for (float x = 520.0f; x < 860.0f && sliderY < 0.0f; x += 25.0f) {
            clickAt(window, x, y);
            if (rows->row_data(2)->delay_ms != 0.0f) {
                sliderY = y;
            }
        }
    }
    {
        INFO("no click moved the Art-Net row's delay slider");
        REQUIRE(sliderY >= 0.0f);
    }
    // And the node itself has it — which is the half the old slider never reached.
    const float ms = rows->row_data(2)->delay_ms;
    CHECK(seen(controller).targets[2].delaySeconds == Catch::Approx(ms / 1000.0f).margin(1e-6));
    CHECK(seen(controller).targets[2].delaySeconds != 0.0);
    nothingReal.check();
}

TEST_CASE("a delay reading takes a typed number, and Escape leaves it as it was", "[ui]") {
    // The delay and latency readings beside the sliders are clickable, so a number can be typed
    // in directly — an underlined number like a link, not a box that is always there. Driven by
    // real clicks and keys — the reading clicked, a number typed, Enter — and read back from the
    // output thread.
    LiveTracker tracker(kWeights, kStateSpace);
    const takt4::testing::LoopbackReceiver deckEnd; // the test's own, not a port nobody holds
    takt4::settings::Settings saved;
    takt4::output::OutputTarget deck;
    REQUIRE(takt4::output::parseOutputTarget(
        "deck = 127.0.0.1:" + std::to_string(deckEnd.port()), deck));
    saved.preset.outputs = {deck};
    WindowController controller(tracker, saved);
    layOut(controller, 1000.0f, 1400.0f);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto rows = controller.window().get_outputs_list();
    REQUIRE(rows->row_count() == 2);
    const auto deckDelay = [&controller] { return seen(controller).targets[1].delaySeconds; };
    const takt4::tests::NothingReal nothingReal;

    // The deck's reading, at the right-hand end of its row: found by clicking down the reading
    // column, typing a number and Enter, until the deck's delay is that number. Each miss is
    // followed by a click on the background — never Escape, which with no box open is PANIC.
    // Below the latency reading, which is in the same column and would take the number too.
    constexpr float kReadingX = 940.0f;
    constexpr float kBackgroundX = 985.0f;
    float readingY = -1.0f;
    for (float y = 1000.0f; y < 1270.0f && readingY < 0.0f; y += 3.0f) {
        clickAt(window, kReadingX, y);
        settle();
        for (const char* key : {"1", "2", "5", "\n"}) {
            press(window, key);
        }
        settle();
        if (std::abs(deckDelay() - 0.125) < 1e-9) {
            readingY = y;
        } else {
            clickAt(window, kBackgroundX, 1080.0f);
            settle();
        }
    }
    {
        INFO("no click down the reading column took a typed number for the deck");
        REQUIRE(readingY >= 0.0f);
    }
    // The row reads it back, and so does the slider beside it — the slider's own binding is
    // dropped the first time it is dragged, and the row has to push it there itself.
    CHECK(rows->row_data(1)->delay_ms == Catch::Approx(125.0f));
    // Escape out of the box leaves the number as it was.
    clickAt(window, kReadingX, readingY);
    settle();
    for (const char* key : {"9", "0", "0"}) {
        press(window, key);
    }
    press(window, kEscape);
    settle();
    CHECK(deckDelay() == Catch::Approx(0.125));
    CHECK_FALSE(panickedNow(controller)); // and Escape in the box is not PANIC

    // A click away sets what was typed, the way every box in the window commits.
    clickAt(window, kReadingX, readingY);
    settle();
    for (const char* key : {"-", "4", "0"}) {
        press(window, key);
    }
    clickAt(window, kBackgroundX, 1080.0f);
    settle();
    settle();
    CHECK(deckDelay() == Catch::Approx(-0.040));

    // Past the end of the range is the end of the range; a word is said and changes nothing.
    controller.setTargetDelayTyped(1, "5000 ms");
    CHECK(deckDelay() == Catch::Approx(takt4::output::kMaxOutputDelaySeconds));
    controller.setTargetDelayTyped(1, "soon");
    CHECK(deckDelay() == Catch::Approx(takt4::output::kMaxOutputDelaySeconds));
    CHECK(controller.statusIsError());
    CHECK(std::string(controller.window().get_status()).find("\"soon\"") != std::string::npos);

    // And the latency reading the same way: typed, it reaches the transports and the slider.
    controller.window().invoke_latency_typed(slint::SharedString("-30"));
    CHECK_THAT(controller.window().get_latency_ms(), WithinAbs(-30.0, 1e-4));
    CHECK_THAT(controller.outputs().transports().latencySeconds(), WithinAbs(-0.030, 1e-9));
    controller.window().invoke_latency_typed(slint::SharedString("+999"));
    CHECK_THAT(controller.window().get_latency_ms(), WithinAbs(takt4::ui::kLatencyLimitMs, 1e-4));
    nothingReal.check();
}

TEST_CASE("an output's kind dropdown survives the redraws while it is open, and its pick lands",
          "[ui]") {
    // Popups inside repeaters were tested only for the two color pickers. The output rows are a
    // repeater the window builds again whenever a row cannot be updated in place, and a row
    // rebuilt with its dropdown open takes the popup away under the pointer.
    //
    // Swept only down one column, x = 208, where the row's kind dropdown sits (padding, the tick
    // box and the name come first), and only near the rows: nothing that opens a dialog or the
    // audio device is in that stretch of it.
    LiveTracker tracker(kWeights, kStateSpace);
    const takt4::testing::LoopbackReceiver deckEnd; // the test's own, not a port nobody holds
    const std::string deckLine = "deck = 127.0.0.1:" + std::to_string(deckEnd.port());
    takt4::settings::Settings saved;
    takt4::output::OutputTarget deck;
    REQUIRE(takt4::output::parseOutputTarget(deckLine, deck));
    saved.preset.outputs = {deck};
    WindowController controller(tracker, saved);
    controller.setNewOutputPort(deckEnd.port()); // a probe on [+ ADD OUTPUT] sends here
    layOut(controller, 1000.0f, 1400.0f);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto reset = [&controller, &settle, &deckLine] {
        controller.setOscTargets(deckLine);
        settle();
    };
    // The deck's row, after the Link row every set starts with.
    const auto kindOf = [&controller] {
        const auto rows = controller.window().get_outputs_list();
        return rows->row_count() == 2 ? rows->row_data(1)->kind_index : -1;
    };
    settle();
    REQUIRE(kindOf() == 0);

    // The row, found by its name box: the first click down the name column after which a typed
    // letter lands in the row's name. Not by the dropdown and an arrow key — a click on nothing
    // leaves the keyboard where an earlier probe put it, so a key proves nothing about where
    // the click went. The kind dropdown is on the same row.
    constexpr float kNameColumn = 100.0f;
    constexpr float kKindColumn = 208.0f;
    const auto nameOf = [&controller] {
        const auto rows = controller.window().get_outputs_list();
        return rows->row_count() == 2 ? std::string(rows->row_data(1)->name) : std::string();
    };
    //
    // Every probe starts with a click on the window's own background, off to the right where
    // nothing is drawn, so that the box has lost the focus and a letter can only land in it by
    // the probe's own click. Both edges are found, and the row is aimed at in the middle: the
    // dropdown is shorter than the box, and its top edge is not where the box's is.
    //
    // **Up from the footer, not down from the middle.** The deck's row is the last one, so
    // coming up from below it is met before anything above it; going down, the sweep first
    // crossed the Inputs block — whose "listen" box the Link row had pushed into its path — and
    // bound the OSC control port on the machine the show runs from, thirteen times, while the
    // test passed. Below the row there is only [+ ADD OUTPUT], and a probe that lands on it is
    // undone.
    constexpr float kBackgroundX = 985.0f;
    constexpr float kBackgroundY = 1080.0f;
    const takt4::tests::NothingReal nothingReal;
    float top = -1.0f;
    float bottom = -1.0f;
    for (float y = 1320.0f; y > 1000.0f; y -= 2.0f) {
        if (controller.window().get_outputs_list()->row_count() != 2) {
            reset(); // the last probe was [+ ADD OUTPUT]
        }
        clickAt(window, kBackgroundX, kBackgroundY);
        const std::string before = nameOf();
        clickAt(window, kNameColumn, y);
        press(window, "Q");
        // Enter, which commits the name at once. Without it the probe waited for the box to
        // lose the focus, which is the *next* probe's first click — so every probe compared the
        // row's name with itself, found nothing, and the sweep went on into ADD OUTPUT.
        press(window, "\n");
        // And a redraw here rather than hoping the window's own 30 Hz timer falls inside this
        // probe — under load it mostly did not (1 run in 32, 8 at once).
        settle();
        const bool landed = nameOf().size() > before.size();
        if (landed && bottom < 0.0f) {
            bottom = y;
        }
        if (landed) {
            top = y;
        } else if (bottom >= 0.0f) {
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

    // Then the gesture: opened, left open through ten redraws, and the same step taken. Unlike
    // the rule editor's and the patch editor's, nothing in these rows moves on its own — no
    // readout, no level — so ten redraws are all that happens to them while the list is open
    // (checked; the other two tests now move a readout under their list).
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

    // And nothing on the way opened anything real. The sandbox would have refused it, and a
    // refusal here means the sweep has wandered off the rows again.
    CHECK_FALSE(controller.oscControl().running());
    CHECK_FALSE(controller.control().running());
    nothingReal.check();
}


namespace {

const std::string kUp = "\xEF\x9C\x80";   // Key.UpArrow
const std::string kDown = "\xEF\x9C\x81"; // Key.DownArrow

/// The row an output called `name` is on, or -1.
int rowNamed(WindowController& controller, const std::string& name) {
    const auto rows = controller.window().get_outputs_list();
    for (std::size_t i = 0; i < rows->row_count(); ++i) {
        if (std::string(rows->row_data(i)->name) == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// The running output called `name`.
takt4::output::OutputTarget outputNamed(WindowController& controller, const std::string& name) {
    for (const takt4::output::OutputTarget& target : seen(controller).targets) {
        if (target.name == name) {
            return target;
        }
    }
    return {};
}

} // namespace

TEST_CASE("a kind picked again on an output row still follows the row when it changes", "[ui]") {
    // A dropdown sets its own index when it is picked from, and so stops following its row; a
    // pick is recorded so the row is built again when something else moves it. A pick of the
    // kind the row already was returned before recording it — and the dropdown went on showing
    // that kind over a row a paste or an IMPORT had changed. What it shows is read the way an
    // operator meets it: an arrow in the open list moves on from it.
    LiveTracker tracker(kWeights, kStateSpace);
    const takt4::testing::LoopbackReceiver deckEnd;
    takt4::settings::Settings saved;
    takt4::output::OutputTarget deck;
    REQUIRE(takt4::output::parseOutputTarget("deck = 127.0.0.1:" + std::to_string(deckEnd.port()),
                                             deck));
    saved.preset.outputs = {deck};
    WindowController controller(tracker, saved);
    constexpr float kHeight = 1100.0f;
    layOut(controller, 1000.0f, kHeight);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    settle();
    const takt4::tests::NothingReal nothingReal;
    const std::vector<float> at = outputRowsAt(controller, kHeight);
    REQUIRE(at.size() == 2);
    REQUIRE(at[1] > 0.0f);
    constexpr float kKindColumn = 208.0f;
    const auto kindOf = [&controller] {
        const int row = rowNamed(controller, "deck");
        return row < 0 ? -1
                       : controller.window()
                             .get_outputs_list()
                             ->row_data(static_cast<std::size_t>(row))
                             ->kind_index;
    };
    REQUIRE(kindOf() == 0); // OSC

    // OSC picked again: the list opened and closed, and the arrow that picks the entry it is on.
    clickAt(window, kKindColumn, at[1]);
    slint::platform::update_timers_and_animations();
    press(window, kEscape); // closes the list, and is not PANIC while it is open
    press(window, kUp);     // on OSC already, the first entry: picks OSC
    settle();
    REQUIRE(kindOf() == 0);
    REQUIRE_FALSE(panickedNow(controller));

    // Then the row becomes an Art-Net node from outside the dropdown — at a receiver of the
    // test's own, and nothing that changes the device list, which builds MIDI rows again for a
    // reason of its own.
    const takt4::testing::LoopbackReceiver node;
    controller.setOscTargets("deck = artnet 127.0.0.1:" + std::to_string(node.port()));
    settle();
    settle();
    REQUIRE(kindOf() == 2);

    // Down one from what the dropdown shows. Showing Art-Net, that is the MIDI clock; still
    // showing OSC, it is MIDI.
    clickAt(window, kKindColumn, at[1]);
    slint::platform::update_timers_and_animations();
    press(window, kDown);
    press(window, "\n");
    settle();
    CHECK(kindOf() == 3);
    nothingReal.check();
}

TEST_CASE("a MIDI row's device dropdown follows its row after the device list changes", "[ui]") {
    // The other half. A dropdown given a new list sets its own index (Slint's ComboBoxBase:
    // `changed model => reset-current()`), which cuts it loose from its row — so after a RESCAN,
    // which hands every MIDI row a new list, a row whose device moved in the list went on showing
    // the device that used to be there. Every MIDI row is built again with a new list. The list
    // here is the rows' own devices, none of them on this machine: taking out the row that names
    // the first moves every device after it up one.
    const auto build = [] {
        takt4::settings::Settings saved;
        for (const char* line : {"a = midi takt4 test A", "b = midi takt4 test B",
                                 "deck = midi takt4 test C", "d = midi takt4 test D"}) {
            takt4::output::OutputTarget target;
            REQUIRE(takt4::output::parseOutputTarget(line, target));
            saved.preset.outputs.push_back(target);
        }
        return saved;
    };
    constexpr float kWidth = 1000.0f;
    constexpr float kHeight = 1300.0f;
    const auto settleFor = [](WindowController& controller) {
        return [&controller] {
            controller.tick();
            slint::platform::update_timers_and_animations();
        };
    };
    const auto deviceOf = [](WindowController& controller) {
        return outputNamed(controller, "deck").device;
    };

    // Where the deck row's device dropdown is, found on a window of its own so the probing
    // leaves nothing behind on the one the gesture is made on: the first click along the row
    // after which an arrow and Enter change the device.
    float deviceX = -1.0f;
    float deckY = -1.0f;
    {
        LiveTracker tracker(kWeights, kStateSpace);
        WindowController probe(tracker, build());
        layOut(probe, kWidth, kHeight);
        const auto settle = settleFor(probe);
        settle();
        const std::vector<float> at = outputRowsAt(probe, kHeight);
        const int deck = rowNamed(probe, "deck");
        REQUIRE(deck > 0);
        deckY = at[static_cast<std::size_t>(deck)];
        REQUIRE(deckY > 0.0f);
        // A probe that lands on the row's kind dropdown instead makes it OSC, with no device; the
        // rows are put back after anything that moved them.
        const std::string lines = "a = midi takt4 test A, b = midi takt4 test B, "
                                  "deck = midi takt4 test C, d = midi takt4 test D";
        for (float x = 240.0f; x < 700.0f && deviceX < 0.0f; x += 12.0f) {
            clickAt(probe.window().window(), x, deckY);
            slint::platform::update_timers_and_animations();
            press(probe.window().window(), kUp);
            press(probe.window().window(), "\n");
            settle();
            const takt4::output::OutputTarget now = outputNamed(probe, "deck");
            if (now.kind == takt4::output::OutputTarget::Kind::Midi &&
                now.device == "takt4 test B") {
                deviceX = x;
            } else if (now.kind != takt4::output::OutputTarget::Kind::Midi ||
                       now.device != "takt4 test C") {
                probe.setOscTargets(lines);
                settle();
                settle();
            }
        }
    }
    INFO("device dropdown at " << deviceX << ", " << deckY);
    REQUIRE(deviceX > 0.0f);

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker, build());
    layOut(controller, kWidth, kHeight);
    auto& window = controller.window().window();
    const auto settle = settleFor(controller);
    settle();
    const takt4::tests::NothingReal nothingReal;
    REQUIRE(deviceOf(controller) == "takt4 test C");

    // RESCAN, and then the row naming the first device taken away: the list is B, C, D now.
    controller.rescanDevices();
    rescanned(controller);
    settle();
    controller.removeTarget(rowNamed(controller, "a"));
    settle();
    settle();
    REQUIRE(deviceOf(controller) == "takt4 test C");
    // The rows below "a" moved up one; the deck's is where "b" was.
    const std::vector<float> at = outputRowsAt(controller, kHeight);
    const float y = at[static_cast<std::size_t>(rowNamed(controller, "deck"))];

    // Up one from what the dropdown shows. Showing C, that is B; still showing the entry C used
    // to be at — D's now — it is C, and nothing moves.
    clickAt(window, deviceX, y);
    slint::platform::update_timers_and_animations();
    press(window, kUp);
    press(window, "\n");
    settle();
    CHECK(deviceOf(controller) == "takt4 test B");
    nothingReal.check();
}

TEST_CASE("an output whose MIDI device is not plugged in shows that device, and says so",
          "[ui]") {
    // The row's dropdown read "select a MIDI device" — a row with no device at all — while the
    // row went on trying to open the one it named. It shows the device now, marked as not
    // plugged in, and it is on the list for any row to pick.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    takt4::output::OutputTarget desk;
    REQUIRE(takt4::output::parseOutputTarget("desk = midi takt4 test desk at home", desk));
    saved.preset.outputs = {desk};
    WindowController controller(tracker, saved);
    const int row = rowNamed(controller, "desk");
    REQUIRE(row > 0);
    const int index = controller.window()
                          .get_outputs_list()
                          ->row_data(static_cast<std::size_t>(row))
                          ->device_index;
    REQUIRE(index > 0);
    const auto devices = controller.window().get_output_devices();
    REQUIRE(static_cast<std::size_t>(index) < devices->row_count());
    CHECK(std::string(*devices->row_data(static_cast<std::size_t>(index))) ==
          "takt4 test desk at home \xE2\x80\x94 not plugged in");
    // Still that device, as the output the runner has.
    CHECK(outputNamed(controller, "desk").device == "takt4 test desk at home");
}

TEST_CASE("a pasted line keeps a quoted name with a comma in it whole", "[ui]") {
    // A name's round trip, the paste half: a line of outputs is split at its commas, and a name
    // written in quotes — how a name with a comma in it is written — keeps its own.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver one;
    const takt4::testing::LoopbackReceiver two;
    controller.setOscTargets("\"stage, left\" = 127.0.0.1:" + std::to_string(one.port()) +
                             ", \"off stage\" = 127.0.0.1:" + std::to_string(two.port()));
    CHECK(outputNamed(controller, "stage, left").port == one.port());
    const takt4::output::OutputTarget off = outputNamed(controller, "off stage");
    CHECK(off.port == two.port());
    CHECK(off.enabled);
}

TEST_CASE("an import of a file that cannot be read says why, and changes nothing",
          "[ui][settings]") {
    // A file damaged by a hand edit, or held by another program, was "has no preset in it",
    // which sends an operator looking for the wrong thing.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::trigger::Rule::Config keep;
    keep.id = "keep";
    keep.address = "/keep";
    controller.setRules({keep});
    const takt4::test::TempDir dir;

    SECTION("damaged") {
        const std::filesystem::path file = dir.path() / "damaged.json";
        std::ofstream(file) << R"({"preset": {"rules": [ })";
        CHECK_FALSE(controller.importFrom(file));
        const std::string status(controller.window().get_status());
        INFO(status);
        CHECK(status.find("Cannot import damaged.json") != std::string::npos);
        CHECK(status.find("not a settings file takt4 can read") != std::string::npos);
        CHECK(controller.statusIsError());
    }

#if defined(_WIN32)
    SECTION("held by another program") {
        const std::filesystem::path file = dir.path() / "held.json";
        REQUIRE(takt4::settings::save(takt4::settings::Settings{}, file));
        const HANDLE held = ::CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(held != INVALID_HANDLE_VALUE);
        const bool imported = controller.importFrom(file);
        ::CloseHandle(held);
        CHECK_FALSE(imported);
        const std::string status(controller.window().get_status());
        INFO(status);
        CHECK(status.find("Cannot import held.json") != std::string::npos);
        CHECK(status.find("could not be opened") != std::string::npos);
    }
#endif

    REQUIRE(controller.rules().size() == 1);
    CHECK(controller.rules()[0].id == "keep");
}

TEST_CASE("each import decides what the next launch uses, not the one before", "[ui][settings]") {
    // A decoder and an OSC prefix cannot change while takt4 runs, so an import that brings
    // others keeps them for the next launch — and a second import that brought the running ones
    // left the first import's pending, so the next launch came up with a decoder and a prefix
    // from a file the operator had since replaced.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::tracking::Decoder running = tracker.engine().decoderKind();
    const takt4::tracking::Decoder other = running == takt4::tracking::Decoder::Forward
                                               ? takt4::tracking::Decoder::ParticleFilter
                                               : takt4::tracking::Decoder::Forward;
    const std::string prefix = controller.currentSettings().preset.oscPrefix;
    const takt4::test::TempDir dir;
    const auto importing = [&](takt4::tracking::Decoder decoder, const std::string& osc,
                               const char* name) {
        takt4::settings::Settings show;
        show.preset.decoder = decoder;
        show.preset.oscPrefix = osc;
        takt4::trigger::Rule::Config rule; // something in it, so it is a preset
        rule.id = name;
        rule.address = "/go";
        show.preset.rules = {rule};
        const std::filesystem::path file = dir.path() / (std::string(name) + ".json");
        REQUIRE(takt4::settings::save(show, file));
        REQUIRE(controller.importFrom(file));
    };

    importing(other, "/elsewhere", "first");
    CHECK(controller.currentSettings().preset.decoder == other);
    CHECK(controller.currentSettings().preset.oscPrefix == "/elsewhere");

    importing(running, prefix, "second");
    CHECK(controller.currentSettings().preset.decoder == running);
    CHECK(controller.currentSettings().preset.oscPrefix == prefix);
}

TEST_CASE("an import restores this machine's half too, and a file without one leaves it alone",
          "[ui][settings]") {
    // Export and import carry and restore everything. Import applied the preset and left the
    // input, the MIDI and OSC control and the layout as they were — by design, since they are
    // the machine's half — so a rig restored from its own export came back without them. Every
    // part of the machine half that can differ from a fresh window without hardware, from a
    // file, back out of what the window would save.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::settings::Settings before = controller.currentSettings();
    REQUIRE_FALSE(before.machine.oscControlEnabled);
    REQUIRE(before.machine.midiBindings.empty());

    takt4::settings::Settings show;
    show.machine.deviceName = "takt4 test - an interface from another machine";
    show.machine.hostApiName = "ASIO";
    show.machine.channel = 6;
    show.machine.mono = true;
    show.machine.midiControlPort = "takt4 test - a controller from another machine";
    show.machine.midiBindings = {"note 36 ch 10 -> tap"};
    show.machine.oscControlEnabled = true;
    show.machine.oscControlPort = 0; // any free one: what the sandbox lets a test bind
    show.machine.oscControlLocalOnly = true;
    show.machine.inputsFolded = true;
    show.machine.outputsFolded = true;
    show.machine.ruleSectionsFolded = {true, true, true, true};
    show.machine.ruleLogOpen = true;
    show.machine.patchSectionsFolded = {true, true, true};
    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "rig.json";
    REQUIRE(takt4::settings::save(show, file));
    REQUIRE(controller.importFrom(file));

    const takt4::settings::MachineSettings after = controller.currentSettings().machine;
    // The input is not on this machine: still the one wanted, as at a launch that cannot find it.
    CHECK(after.deviceName == show.machine.deviceName);
    CHECK(after.hostApiName == "ASIO");
    CHECK(after.channel == 6);
    CHECK(after.mono);
    CHECK(after.midiControlPort == show.machine.midiControlPort);
    CHECK(after.midiBindings == show.machine.midiBindings);
    CHECK(controller.control().bindings().size() == 1);
    CHECK(after.oscControlEnabled);
    CHECK(controller.oscControl().running());
    CHECK(after.inputsFolded);
    CHECK(after.outputsFolded);
    CHECK(controller.window().get_inputs_folded());
    CHECK(after.ruleSectionsFolded == show.machine.ruleSectionsFolded);
    CHECK(after.ruleLogOpen);
    CHECK(after.patchSectionsFolded == show.machine.patchSectionsFolded);
    const std::string status(controller.window().get_status());
    INFO(status);
    CHECK(status.find("the input, MIDI and OSC control and the layout") != std::string::npos);

    SECTION("a file with no machine section changes none of it") {
        // A preset written by hand, or shared without this half: a fresh install's would switch
        // OSC control off and forget the pads.
        const std::filesystem::path preset = dir.path() / "preset-only.json";
        std::ofstream(preset)
            << R"({"version": 1, "preset": {"rules": [{"id": "go", "address": "/go"}]}})";
        REQUIRE(controller.importFrom(preset));
        REQUIRE(controller.rules().size() == 1);
        const takt4::settings::MachineSettings kept = controller.currentSettings().machine;
        CHECK(kept.deviceName == show.machine.deviceName);
        CHECK(kept.midiBindings == show.machine.midiBindings);
        CHECK(kept.oscControlEnabled);
        CHECK(controller.oscControl().running());
        CHECK(kept.inputsFolded);
        CHECK(kept.ruleSectionsFolded == show.machine.ruleSectionsFolded);
    }

    SECTION("and the export it came from carries all of it") {
        const std::filesystem::path out = dir.path() / "exported.json";
        REQUIRE(controller.exportTo(out));
        const takt4::settings::Settings read = takt4::settings::load(out);
        CHECK(read.machine.deviceName == show.machine.deviceName);
        CHECK(read.machine.midiBindings == show.machine.midiBindings);
        CHECK(read.machine.oscControlEnabled);
        CHECK(read.machine.inputsFolded);
        CHECK(read.machine.patchSectionsFolded == show.machine.patchSectionsFolded);
    }
}

namespace {

/// An output called `name` at the address every send to fails, sent the global messages, and a
/// cue fired by hand ("manual") aimed at every output, whichever there are.
takt4::settings::Settings settingsWithHandFiredCueTo(const std::string& name) {
    takt4::settings::Settings saved;
    takt4::output::OutputTarget output;
    REQUIRE(takt4::output::parseOutputTarget(
        name + " = " + takt4::testing::kUnsendableHost + ":57000 global", output));
    saved.preset.outputs = {output};
    takt4::trigger::Rule::Config cue;
    cue.id = "cue";
    cue.trigger = takt4::trigger::Trigger::Manual;
    cue.address = "/cue";
    saved.preset.rules = {cue};
    return saved;
}

} // namespace

TEST_CASE("an output the network refuses is named in the status line", "[ui]") {
    // A network that is down, or a cable that is out, fails every send, and the window said
    // nothing — the output just went quiet. The socket refuses every send to `kUnsendableHost`,
    // the stand-in here, since no test can pull a cable. Nothing is started: the outputs run
    // from launch, and the status has to say so before START — for an output that is sent
    // something; one sent nothing has nothing to fail. A stopped window sends the global
    // messages nothing (they wait for a lock), so it is sent a cue fired by hand, as a control
    // surface fires one.
    if constexpr (!takt4::testing::kUnsendableFails) {
        SKIP(takt4::testing::kUnsendableSkip);
    }
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved = settingsWithHandFiredCueTo("deck");
    WindowController controller(tracker, saved);

    std::string status;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (status.find("deck") == std::string::npos && std::chrono::steady_clock::now() < until) {
        (void)controller.oscControl().dispatch("/takt4/ctl/manual", std::nullopt);
        controller.tick();
        status = std::string(controller.window().get_status());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO(status);
    CHECK(status.find("deck: sends are failing") != std::string::npos);
    CHECK(status.find(takt4::testing::kUnsendableReason) != std::string::npos);
    CHECK(controller.statusIsError());
}

TEST_CASE("a single problem met starting is still on the status line when the window is up",
          "[ui]") {
    // Several problems met starting were joined and said together; one was left where it was
    // and the constructor's own later news wrote over it. A set with a rule that cannot fire is
    // one such problem, and the OSC listener starting after it — on any free port — is the
    // later news.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    takt4::trigger::Rule::Config broken;
    broken.id = "broken";
    broken.address = "not an OSC address";
    REQUIRE_FALSE(takt4::trigger::Rule(broken).valid());
    saved.preset.rules = {broken};
    saved.machine.oscControlEnabled = true;
    saved.machine.oscControlPort = 0;
    WindowController controller(tracker, saved);
    REQUIRE(controller.oscControl().running());
    const std::string status(controller.window().get_status());
    INFO(status);
    CHECK(status.find("1 rule will not fire") != std::string::npos);
    CHECK(controller.statusIsError());
}

TEST_CASE("the settings notice and what the first redraws find are said together", "[ui]") {
    // The other half. A damaged settings file's notice is put up once the window exists — and
    // the first redraw that found an output it could not reach wrote over it, before anybody
    // could have read it. For its first seconds, what the window finds out on its own joins what
    // it met starting; what the operator does still says what it did.
    if constexpr (!takt4::testing::kUnsendableFails) {
        SKIP(takt4::testing::kUnsendableSkip);
    }
    LiveTracker tracker(kWeights, kStateSpace);
    // Sent a cue fired by hand, which is what fails — see the test above.
    takt4::settings::Settings saved = settingsWithHandFiredCueTo("deck");
    WindowController controller(tracker, saved);
    const std::string notice = "settings.json could not be read, so the copy from the last good "
                               "start is being used.";
    controller.showNotice(notice);

    std::string status;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (status.find("deck") == std::string::npos && std::chrono::steady_clock::now() < until) {
        (void)controller.oscControl().dispatch("/takt4/ctl/manual", std::nullopt);
        controller.tick();
        status = std::string(controller.window().get_status());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO(status);
    CHECK(status.find(notice) == 0); // first, and still there
    CHECK(status.find("deck: sends are failing") != std::string::npos);
    CHECK(controller.statusIsError());

    // The operator doing something is news the notice gives way to, and it does not come back —
    // not even when the window next finds something out on its own, still inside the hold.
    controller.saveNow(); // into this test process's own folder; says where
    const std::string after(controller.window().get_status());
    INFO(after);
    CHECK(after.find(notice) == std::string::npos);
    controller.setOscTargets(std::string("deck = ") + takt4::testing::kUnsendableHost +
                             ":57000 global, desk = " + takt4::testing::kUnsendableHost +
                             ":57001 global");
    std::string later;
    const auto until2 = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (later.find("desk") == std::string::npos && std::chrono::steady_clock::now() < until2) {
        (void)controller.oscControl().dispatch("/takt4/ctl/manual", std::nullopt);
        controller.tick();
        later = std::string(controller.window().get_status());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO(later);
    CHECK(later.find("desk: sends are failing") != std::string::npos);
    CHECK(later.find(notice) == std::string::npos);
}

TEST_CASE("a click made while START is still opening the input does not stop it again",
          "[ui][hardware]") {
    // A click queued behind a slow open, with the pointer on the button. Opening a driver holds
    // the window's thread; a click made meanwhile is delivered once it is free, and pressed STOP
    // on an input that had only just opened. Here the second click comes before the press has
    // been carried out and a third straight after it has — the two moments such a click can
    // arrive.
    LiveTracker tracker(kWeights, kStateSpace);
    if (!bestInputDevice(tracker)) {
        SKIP("no input device on this machine");
    }
    WindowController controller(tracker);
    // The window as it opens, and the top bar's buttons: 10 px of page, 12 px down the sheet, a
    // 16 px label and 3 px over the 34 px pickers they are centred on — the sweep of every control
    // ("every control in the main window does what it says") clicks the same row. (This test is
    // [hardware], so the redesign's default runs did not see it still aiming at the old top bar.)
    constexpr float kWidth = 800.0f;
    constexpr float kHeight = 934.0f;
    constexpr float kButtonY = 58.0f;
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
    // Counted when the rules change rather than on every redraw, so the count has to follow
    // every way they change: a whole set arriving, and the editor.
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

TEST_CASE("an edit is kept while the output thread has not yet got to it", "[ui]") {
    // Found by the ASan run of 2026-09-24, as the test above failing four runs in six there and
    // never in Release. The editor takes the output thread's live switches, to show what a
    // control surface changed, and it took them whenever they moved — the set before its own
    // last edit included, while the thread had not got to that edit yet. So a rule switched on
    // went off again at the next redraw, and the next edit posted it off.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::trigger::Rule::Config clips;
    clips.id = "clips";
    clips.address = "/clips";
    takt4::trigger::Rule::Config off = clips;
    off.id = "off";
    off.enabled = false;
    const std::uint64_t before = controller.outputs().liveRulesVersion();
    controller.setRules({clips, off});
    // The set applied and its switches published, with the editor yet to read them: no tick.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (controller.outputs().liveRulesVersion() == before &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(controller.outputs().liveRulesVersion() != before);

    // All of this with the output thread held between two of its rounds, so what is posted in
    // here waits: the moment the race needed a slow thread to reach, every time.
    controller.outputs().inspect([&](const auto&, const auto&, const auto&) {
        controller.editor().pick(1);
        controller.editor().setEnabled(true);
        controller.editor().tick(); // the switches it can read are the set before this edit's
        controller.editor().pick(0);
        controller.editor().setAddress("/clips/2"); // and the next edit posts the whole set
    });
    const auto applied = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!controller.outputs().liveRulesCurrent() &&
           std::chrono::steady_clock::now() < applied) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    controller.tick();

    const std::vector<takt4::trigger::Rule::Config> saved =
        controller.currentSettings().preset.rules;
    REQUIRE(saved.size() == 2);
    CHECK(saved[1].id == "off");
    CHECK(saved[1].enabled);
    CHECK(saved[0].address == "/clips/2");
    CHECK(controller.window().get_rules_active() == 2);
    // And on the output thread, which is what fires.
    CHECK(controller.outputs().inspect(
        [](const takt4::trigger::TriggerEngine& rules, const auto&, const auto&) {
            for (std::size_t i = 0; i < rules.ruleCount(); ++i) {
                if (rules.rule(i).id() == "off") {
                    return rules.rule(i).enabled();
                }
            }
            return false;
        }));
}

TEST_CASE("what the output thread lost is said under the outputs heading", "[ui]") {
    // Every rule message that reached no output was counted, and the count was shown nowhere. A
    // MIDI rule on a rig with no MIDI output, fired with TEST, the way an operator finds out
    // their laser rule goes nowhere.
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
    // At 125 % a 1920 x 1080 screen with a 40-pixel taskbar has a work area of 1536 x 832
    // logical pixels, and the main window asked for 934: its status bar and PANIC opened under
    // the taskbar. What a real small screen does cannot be seen from this desk; this is the
    // arithmetic that decides it.
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
    // A licensing gap, and the answer to it: takt4 shipped with no notices for what is
    // built into it, and GPLv3 section 5(d) asks its interface to show them. takt4 is one file,
    // so they are inside it; the About box opens them. Driven by clicks — ABOUT in the status
    // line, then the About box's own buttons — with the file opener replaced, since a test must
    // not launch a text viewer.
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
    const takt4::tests::NothingReal nothingReal;

    // ABOUT, found along the status line by what a click on it does — from the right-hand end,
    // where it is, so the sweep meets it before SAVE, EXPORT and IMPORT to its left. From the
    // left, it pressed EXPORT and IMPORT on the way, and each opened a real file dialog on the
    // rig's desktop (2026-09-25; see `ui::fileDialogsAllowed`, which now refuses them here).
    const float barY = kHeight - 17.0f;
    for (float x = kWidth - 10.0f; x > kWidth - 480.0f && controller.about() == nullptr;
         x -= 6.0f) {
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
    // Down to 480: the description above them is three lines since it names the followed sources.
    // CLOSE is at the bottom right, far below and to the right of this.
    for (float y = 280.0f; y < 480.0f && (licence.empty() || notices.empty()); y += 6.0f) {
        for (float x = 30.0f; x < 560.0f && (licence.empty() || notices.empty()); x += 20.0f) {
            opened.clear();
            clickAt(about.window(), x, y);
            slint::platform::update_timers_and_animations();
            if (opened.size() != 1) {
                continue;
            }
            const std::string name = opened.front().filename().string();
            // Written into this test process's own folder, never the temp directory's "takt4",
            // which the real takt4 uses — a test run wrote over the texts it had open there.
            std::error_code code;
            CHECK(opened.front().parent_path().parent_path().parent_path() ==
                  std::filesystem::temp_directory_path(code) / "takt4-tests");
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
    // Rust's standard library is in takt4.exe inside Slint, and was missing until it was found
    // on 2026-09-25.
    for (const char* name : {"PortAudio", "Ableton Link", "Slint", "Skia", "Steinberg ASIO SDK",
                             "The Rust standard library"}) {
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
    nothingReal.check();
}

TEST_CASE("a test process starts no text viewer and no browser, even with no opener of its own",
          "[ui]") {
    // 2026-10-08: a test that forgot `setFileOpener` swept its clicks over the About box's OPEN
    // buttons and opened the licence in the rig's text editor 140 times, until the desktop froze.
    // The sandbox refuses it now: pressed once each, with nothing replaced, nothing opens.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.openAbout();
    REQUIRE(controller.about() != nullptr);
    AboutWindow& about = *controller.about();
    about.invoke_licence_opened();
    const std::string licence(about.get_opened());
    INFO(licence);
    CHECK(licence.rfind("Opened ", 0) == std::string::npos);
    about.invoke_source_opened();
    CHECK(std::string(about.get_opened()).rfind("No browser opened", 0) == 0);
}

TEST_CASE("the About box names its author, links to the source, and its words can be copied",
          "[ui]") {
    // The source's address looked like a link and did nothing, and no word in the window could
    // be selected — the version included, which is what a bug report wants. Driven as a person
    // would: a click into the words, select all, copy; a click on the link. The browser is
    // replaced, as the text viewer is above.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    std::vector<std::string> links;
    controller.setLinkOpener([&links](const std::string& address) {
        links.push_back(address);
        return true;
    });
    // The sweep below crosses the two OPEN buttons: their files go nowhere. Without this, each
    // click opened the licence in the rig's text editor (2026-10-08, 140 windows).
    std::size_t files = 0;
    controller.setFileOpener([&files](const std::filesystem::path&) {
        ++files;
        return true;
    });
    controller.openAbout();
    REQUIRE(controller.about() != nullptr);
    AboutWindow& about = *controller.about();
    auto& window = about.window();
    window.dispatch_scale_factor_change_event(1.0f);
    window.dispatch_resize_event(slint::LogicalSize({580.0f, 640.0f}));
    window.dispatch_window_active_changed_event(true);
    slint::platform::update_timers_and_animations();
    const takt4::tests::NothingReal nothingReal;

    // Select all and copy, with the system's own shortcut key: Control, which Slint also gives
    // for Command on a Mac — Meta there is the Control key (the first macOS run, 2026-10-08).
    const slint::SharedString shortcut("\x11");
    const auto copyWhatWasClicked = [&] {
        for (const char* key : {"a", "c"}) {
            window.dispatch_key_press_event(shortcut);
            window.dispatch_key_press_event(slint::SharedString(key));
            window.dispatch_key_release_event(slint::SharedString(key));
            window.dispatch_key_release_event(shortcut);
        }
        slint::platform::update_timers_and_animations();
        return takt4::ui::headlessClipboard();
    };
    std::vector<std::string> copied;
    for (float y = 14.0f; y < 200.0f; y += 4.0f) {
        clickAt(window, 60.0f, y);
        slint::platform::update_timers_and_animations();
        const std::string got = copyWhatWasClicked();
        if (!got.empty() && (copied.empty() || copied.back() != got)) {
            copied.push_back(got);
        }
    }
    std::string seen;
    for (const std::string& one : copied) {
        seen += "[" + one + "] ";
    }
    INFO("copied: " << seen);
    const auto has = [&copied](const std::string& text) {
        return std::find(copied.begin(), copied.end(), text) != copied.end();
    };
    CHECK(has("takt4"));
    CHECK(has("Author: Jon Sands (Fohdeesha)"));
    // The version, beside the name: found along the row to the name's right. As a release says
    // it — the bare number, a few characters wide — which is what the first release run met: a
    // build of a commit after a tag says far more, and a click at one spot that hit the long one
    // on the rig missed the short one on every runner (1.3.6).
    about.set_version(slint::SharedString("1.3.6"));
    slint::platform::update_timers_and_animations();
    bool versionCopied = false;
    for (float y = 14.0f; y < 70.0f && !versionCopied; y += 4.0f) {
        for (float x = 100.0f; x < 400.0f && !versionCopied; x += 6.0f) {
            clickAt(window, x, y);
            slint::platform::update_timers_and_animations();
            versionCopied = copyWhatWasClicked() == "1.3.6";
        }
    }
    CHECK(versionCopied);

    // The link: found by the click that opens it, and nothing else in the window opens it.
    for (float y = 150.0f; y < 460.0f && links.empty(); y += 4.0f) {
        for (float x = 20.0f; x < 400.0f && links.empty(); x += 8.0f) {
            clickAt(window, x, y);
            slint::platform::update_timers_and_animations();
        }
    }
    REQUIRE(links.size() == 1);
    CHECK(links.front() == "https://github.com/Fohdeesha/takt4");
    CHECK(std::string(about.get_opened()).rfind("No browser", 0) == std::string::npos);
    nothingReal.check();
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
    // The folder this process was given: under the temp directory, in "takt4-tests". Nothing is
    // deleted here any more — the process removes its own folder on the way out
    // (tests/support/crt_dialogs.cpp) — because this used to `remove_all` whatever folder the
    // file landed in, which with TAKT4_SETTINGS_DIR set outside the tests would have been that.
    const std::filesystem::path temp = std::filesystem::temp_directory_path(code);
    CHECK(written.parent_path().parent_path() == temp / "takt4-tests");
}
#endif

TEST_CASE("the global messages box is in the outputs heading only with an OSC output to pick",
          "[ui]") {
    // Never offer a choice that cannot do anything. With no OSC output there is
    // nothing to send takt4's own messages to, and folded, the heading reads what the section
    // holds in that place instead.
    auto window = MainWindow::create();
    constexpr int kWidth = 800;
    constexpr int kHeight = 1200;
    auto rows = std::make_shared<slint::VectorModel<OutputRow>>();
    OutputRow link{};
    link.name = slint::SharedString("Link");
    link.kind_index = 4;
    rows->push_back(link);
    OutputRow clock{};
    clock.name = slint::SharedString("clock");
    clock.kind_index = 3;
    rows->push_back(clock);
    window->set_outputs_list(rows);
    takt4::ui::publishGlobalMessages(*window, {link, clock});
    // What is drawn between the heading's title and its fold arrow.
    const auto middle = [&] {
        const takt4::tests::Shot shot = takt4::tests::render(*window, kWidth, kHeight);
        const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
        REQUIRE(sheets.size() == 7);
        const int headRow = sheets[5].first + (window->get_outputs_folded() ? 6 : 10) + 14;
        return occupied(shot, headRow - 13, headRow + 13, 300, kWidth - 56, kSheet, 5);
    };
    INFO("no OSC output");
    CHECK(middle().empty());

    OutputRow deck{};
    deck.name = slint::SharedString("deck");
    deck.host = slint::SharedString("10.0.0.40");
    deck.port = slint::SharedString("7000");
    rows->push_back(deck);
    takt4::ui::publishGlobalMessages(*window, {link, clock, deck});
    CHECK(std::string(window->get_global_summary()) == "nothing");
    INFO("one OSC output: its label and its box, and the phrases' words and count after them");
    CHECK(middle().size() == 5);

    window->set_outputs_folded(true);
    window->set_outputs_on(3);
    INFO("folded, the heading's summary and nothing else, which ends before the box would begin");
    const auto folded = middle();
    CHECK((folded.empty() || folded.back().second < 540));
}

TEST_CASE("every control in the main window does what it says, once, and a switched-off one "
          "nothing",
          "[ui]") {
    // A bare window — no controller, so no device, socket or port is anywhere behind it — with a
    // rig's worth of everything in it, and every callback it has written down as it fires. Each
    // control is found on a render of the window as it is laid out (the sheets down its left
    // edge, then the ink along each row), clicked or keyed as an operator would, and the list of
    // what fired is held to exactly the one thing that control is for.
    auto window = MainWindow::create();
    std::vector<std::string> fired;
    const auto note = [&fired](std::string what) { fired.push_back(std::move(what)); };
    window->on_device_picked([&](int i) { note("device-picked " + std::to_string(i)); });
    window->on_channel_picked([&](int i) { note("channel-picked " + std::to_string(i)); });
    window->on_input_mono_toggled([&](bool on) { note(std::string("mono ") + (on ? "1" : "0")); });
    window->on_rescan_clicked([&] { note("rescan"); });
    window->on_toggle_run([&] { note("toggle-run"); });
    window->on_halve([&] { note("halve"); });
    window->on_redouble([&] { note("redouble"); });
    window->on_tap([&] { note("tap"); });
    window->on_pin_changed([&](bool on) { note(std::string("pin ") + (on ? "1" : "0")); });
    window->on_snap_downbeat([&] { note("downbeat"); });
    window->on_keep_shift_changed([&](bool on) { note(std::string("keep ") + (on ? "1" : "0")); });
    window->on_fold_on_changed([&](bool on) { note(std::string("fold-on ") + (on ? "1" : "0")); });
    window->on_fold_min_changed([&](float) { note("fold-min"); });
    window->on_fold_max_changed([&](float) { note("fold-max"); });
    window->on_latency_changed([&](float) { note("latency"); });
    window->on_latency_typed(
        [&](const slint::SharedString& t) { note("latency-typed " + std::string(t)); });
    window->on_midi_in_picked([&](int i) { note("midi-in " + std::to_string(i)); });
    window->on_timing_picked([&](int i) { note("timing " + std::to_string(i)); });
    window->on_meter_picked([&](int i) { note("meter " + std::to_string(i)); });
    window->on_clock_in_picked([&](int i) { note("clock-in " + std::to_string(i)); });
    window->on_clock_obey_toggled([&](bool on) { note(std::string("obey ") + (on ? "1" : "0")); });
    window->on_timing_beats_per_bar_edited([&](int n) { note("beats " + std::to_string(n)); });
    window->on_phrase_bars_edited([&](int n) { note("phrase " + std::to_string(n)); });
    window->on_timing_change_clicked([&] { note("timing-change"); });
    window->on_learn_action_picked([&](int i) { note("action " + std::to_string(i)); });
    window->on_learn_clicked([&] { note("learn"); });
    window->on_forget_clicked([&] { note("forget"); });
    window->on_osc_control_toggled(
        [&](bool on) { note(std::string("listen ") + (on ? "1" : "0")); });
    window->on_osc_control_port_edited(
        [&](const slint::SharedString& t) { note("osc-port " + std::string(t)); });
    window->on_osc_control_port_typed([&](const slint::SharedString&) { note("osc-port-key"); });
    window->on_osc_control_network_toggled(
        [&](bool on) { note(std::string("network ") + (on ? "1" : "0")); });
    window->on_osc_remote_setup_toggled(
        [&](bool on) { note(std::string("remote ") + (on ? "1" : "0")); });
    window->on_osc_remote_from_edited(
        [&](const slint::SharedString& t) { note("remote-from " + std::string(t)); });
    window->on_osc_remote_from_typed([&](const slint::SharedString&) { note("remote-from-key"); });
    window->on_fold_clicked([&](int s) { note("fold " + std::to_string(s)); });
    window->on_link_peers_toggled([&] { note("peers"); });
    window->on_output_enabled_changed(
        [&](int i, bool on) { note("on " + std::to_string(i) + (on ? " 1" : " 0")); });
    window->on_output_namespace_changed(
        [&](int i, bool on) { note("namespace " + std::to_string(i) + (on ? " 1" : " 0")); });
    window->on_outputs_namespace_all(
        [&](bool on) { note(std::string("namespace-all ") + (on ? "1" : "0")); });
    window->on_output_name_edited([&](int, const slint::SharedString&) { note("name-key"); });
    window->on_output_name_accepted([&](int i, const slint::SharedString& t) {
        note("name " + std::to_string(i) + " " + std::string(t));
    });
    window->on_output_host_edited([&](int, const slint::SharedString&) { note("host-key"); });
    window->on_output_host_accepted([&](int i, const slint::SharedString& t) {
        note("host " + std::to_string(i) + " " + std::string(t));
    });
    window->on_output_port_edited([&](int, const slint::SharedString&) { note("port-key"); });
    window->on_output_port_accepted([&](int i, const slint::SharedString& t) {
        note("port " + std::to_string(i) + " " + std::string(t));
    });
    window->on_output_kind_changed(
        [&](int i, int k) { note("kind " + std::to_string(i) + " " + std::to_string(k)); });
    window->on_output_device_picked(
        [&](int i, int d) { note("device " + std::to_string(i) + " " + std::to_string(d)); });
    window->on_output_delay_changed([&](int i, float) { note("delay " + std::to_string(i)); });
    window->on_output_delay_typed([&](int i, const slint::SharedString& t) {
        note("delay-typed " + std::to_string(i) + " " + std::string(t));
    });
    window->on_output_removed([&](int i) { note("remove " + std::to_string(i)); });
    window->on_output_added([&] { note("add"); });
    window->on_rules_clicked([&] { note("rules"); });
    window->on_fixtures_clicked([&] { note("fixtures"); });
    window->on_panic_clicked([&] { note("panic"); });
    window->on_panic_released([&] { note("release"); });
    window->on_save_now([&] { note("save"); });
    window->on_export_settings([&] { note("export"); });
    window->on_import_settings([&] { note("import"); });
    window->on_presets_opened([&] { note("presets"); });
    window->on_about_opened([&] { note("about"); });
    window->on_manual_fired([&] { note("manual"); });

    const auto strings = [](std::initializer_list<const char*> items) {
        auto model = std::make_shared<slint::VectorModel<slint::SharedString>>();
        for (const char* item : items) {
            model->push_back(slint::SharedString(item));
        }
        return model;
    };
    window->set_devices(strings({"ASIO / interface A", "WASAPI / interface B"}));
    window->set_channels(strings({"In 1 + 2", "In 3 + 4"}));
    window->set_midi_in_ports(strings({"select input", "pad A", "pad B"}));
    window->set_learn_actions(strings({"tap tempo", "downbeat", "halve"}));
    window->set_output_kinds(strings({"OSC", "MIDI", "Art-Net", "MIDI clock"}));
    window->set_output_devices(strings({"select a MIDI device", "synth X", "synth Y"}));
    window->set_control_on(true);
    window->set_osc_control_port(slint::SharedString("7001"));
    window->set_link_peers(2);
    window->set_fold_on(true);
    window->set_fold_min(70.0f);
    window->set_fold_max(140.0f);
    window->set_rules_total(3);
    window->set_rules_active(2);
    window->set_fixtures_total(5);
    auto rows = std::make_shared<slint::VectorModel<OutputRow>>();
    OutputRow link{};
    link.name = slint::SharedString("Link");
    link.kind_index = 4;
    link.enabled = true;
    rows->push_back(link);
    OutputRow deck{};
    deck.name = slint::SharedString("deck");
    deck.host = slint::SharedString("10.0.0.40");
    deck.port = slint::SharedString("7000");
    deck.enabled = true;
    rows->push_back(deck);
    OutputRow clock{};
    clock.name = slint::SharedString("clock");
    clock.kind_index = 3;
    clock.device_index = 1;
    clock.enabled = true;
    rows->push_back(clock);
    window->set_outputs_list(rows);
    takt4::ui::publishGlobalMessages(*window, {link, deck, clock});

    constexpr int kWidth = 800;
    constexpr int kHeight = 1200; // nothing scrolls
    auto& handle = window->window();
    const auto settle = [] { slint::platform::update_timers_and_animations(); };
    const auto click = [&](auto x, auto y) {
        clickAt(handle, static_cast<float>(x), static_cast<float>(y));
        settle();
    };
    const auto key = [&](const std::string& k) {
        press(handle, k);
        settle();
    };
    const auto expect = [&](const std::vector<std::string>& wanted, const char* what) {
        INFO(what << ": fired " << Catch::Detail::stringify(fired));
        CHECK(fired == wanted);
        fired.clear();
    };
    const auto background = [&] { click(5.0f, 5.0f); };

    // --- stopped: the top bar is live, the performance controls are not -------------------
    window->set_running(false);
    takt4::tests::Shot shot = takt4::tests::render(*window, kWidth, kHeight);
    std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
    // Top bar, tempo, activation, controls, inputs, outputs, triggers.
    REQUIRE(sheets.size() == 7);
    handle.dispatch_window_active_changed_event(true);

    // The top bar's controls: 12 px down the sheet, a 16 px label, 3 px, then 34 px pickers with
    // 40 px buttons centred on them.
    const int topRow = sheets[0].first + 48;
    const auto top = occupied(shot, topRow - 20, topRow + 20, 20, kWidth - 20, kSheet, 5);
    // The device picker, the channel picker, the mono box, its word, RESCAN, START.
    INFO("top bar: " << spans(top));
    REQUIRE(top.size() == 6);
    click(middleOf(top[0]), topRow);
    key(kDown);
    key("\n");
    expect({"device-picked 1"}, "the device picker");
    click(middleOf(top[1]), topRow);
    key(kDown);
    key("\n");
    expect({"channel-picked 1"}, "the channel picker");
    click(middleOf(top[2]), topRow);
    click(middleOf(top[3]), topRow);
    expect({"mono 1", "mono 0"}, "the mono box, then its word");
    click(middleOf(top[4]), topRow);
    expect({"rescan"}, "RESCAN");
    click(middleOf(top[5]), topRow);
    expect({"toggle-run"}, "START");

    const int buttonsRow = sheets[3].first + 26;
    const auto buttons =
        occupied(shot, buttonsRow - 16, buttonsRow + 15, 20, kWidth - 20, kSheet, 5);
    // ÷2, ×2, tap, lock, downbeat, the keep box, and the words after it.
    INFO("controls: " << spans(buttons));
    REQUIRE(buttons.size() == 7);
    for (int i = 0; i < 5; ++i) {
        click(middleOf(buttons[static_cast<std::size_t>(i)]), buttonsRow);
    }
    expect({}, "the performance controls while stopped");
    // The keep box is a setting, live while stopped: box, then words.
    click(middleOf(buttons[5]), buttonsRow);
    click(static_cast<float>(buttons[6].first + 20), buttonsRow);
    expect({"keep 1", "keep 0"}, "the keep-for-the-next-track box, then its words");

    // --- running: the performance controls are live, the top bar is locked ----------------
    window->set_running(true);
    shot = takt4::tests::render(*window, kWidth, kHeight);
    sheets = sheetsDown(shot);
    REQUIRE(sheets.size() == 7);
    handle.dispatch_window_active_changed_event(true);
    const auto lockedTop = occupied(shot, topRow - 20, topRow + 20, 20, kWidth - 20, kSheet, 5);
    // The locked pickers, the locked mono box, its word, "stop to change", STOP.
    INFO("running top bar: " << spans(lockedTop));
    REQUIRE(lockedTop.size() == 6);
    for (std::size_t i = 0; i + 1 < lockedTop.size(); ++i) {
        click(middleOf(lockedTop[i]), topRow);
        key(kDown);
        key("\n");
    }
    // Only the last one, STOP, does anything; the keys typed at the locked pickers went to the
    // window, where Down and Enter are nothing.
    expect({}, "the locked pickers, the locked mono box and the note in RESCAN's place");
    click(middleOf(lockedTop.back()), topRow);
    expect({"toggle-run"}, "STOP");

    const std::vector<std::string> perf = {"halve", "redouble", "tap", "pin 1", "downbeat"};
    for (int i = 0; i < 5; ++i) {
        click(middleOf(buttons[static_cast<std::size_t>(i)]), buttonsRow);
    }
    expect(perf, "÷2, ×2, tap, lock, downbeat");
    // And their keys, from the window's resting place.
    background();
    key("t");
    key("d");
    key("m");
    key(kEscape);
    expect({"tap", "downbeat", "manual", "panic"}, "T, D, M and Escape");

    // The settings row: the fold box, the two handles, the latency handle and its reading.
    const int settingsRow = sheets[3].first + 64;
    const auto settings =
        occupied(shot, settingsRow - 9, settingsRow + 9, 20, kWidth - 20, kSheet, 5);
    INFO("settings: " << spans(settings));
    std::vector<float> thumbs;
    for (int x = 20; x < kWidth - 20; ++x) {
        if (is(shot, x, settingsRow, kThumb) &&
            (thumbs.empty() || static_cast<float>(x) - thumbs.back() > 24.0f)) {
            thumbs.push_back(static_cast<float>(x));
        }
    }
    REQUIRE(thumbs.size() == 3); // the fold window's two ends and the latency
    // The fold box: the first run after "keep BPM in".
    REQUIRE(settings.size() >= 3);
    click(middleOf(settings[1]), settingsRow);
    expect({"fold-on 0"}, "the keep-BPM-in box");
    // Each handle dragged a little: it says so as it goes.
    for (std::size_t i = 0; i < 3; ++i) {
        const float x = thumbs[i] + 8.0f;
        handle.dispatch_pointer_move_event(
            slint::LogicalPosition({x, static_cast<float>(settingsRow)}));
        handle.dispatch_pointer_press_event(
            slint::LogicalPosition({x, static_cast<float>(settingsRow)}),
            slint::PointerEventButton::Left);
        settle();
        handle.dispatch_pointer_move_event(
            slint::LogicalPosition({x + 20.0f, static_cast<float>(settingsRow)}));
        settle();
        handle.dispatch_pointer_release_event(
            slint::LogicalPosition({x + 20.0f, static_cast<float>(settingsRow)}),
            slint::PointerEventButton::Left);
        settle();
    }
    // The fold window is off since the box was clicked: its handles are switched off.
    expect({"latency"}, "the fold window's handles while it is off, then the latency handle");
    click(middleOf(settings[1]), settingsRow);
    expect({"fold-on 1"}, "the keep-BPM-in box again");
    for (std::size_t i = 0; i < 2; ++i) {
        const float x = thumbs[i];
        handle.dispatch_pointer_move_event(
            slint::LogicalPosition({x, static_cast<float>(settingsRow)}));
        handle.dispatch_pointer_press_event(
            slint::LogicalPosition({x, static_cast<float>(settingsRow)}),
            slint::PointerEventButton::Left);
        settle();
        handle.dispatch_pointer_move_event(
            slint::LogicalPosition({x - 12.0f, static_cast<float>(settingsRow)}));
        settle();
        handle.dispatch_pointer_release_event(
            slint::LogicalPosition({x - 12.0f, static_cast<float>(settingsRow)}),
            slint::PointerEventButton::Left);
        settle();
    }
    expect({"fold-min", "fold-max"}, "the fold window's two handles");
    // The latency reading, typed into.
    click(static_cast<float>(settings.back().first + 6), settingsRow);
    key("-");
    key("7");
    key("\n");
    expect({"latency-typed -7"}, "the latency reading");

    // --- inputs ------------------------------------------------------------------------------
    // The timing row first: where the beats come from, locked while running as the audio pickers
    // are — then MIDI and OSC under it.
    const int timingRow = sheets[4].first + 73;
    const auto timing = occupied(shot, timingRow - 17, timingRow + 16, 20, kWidth - 20, kSheet, 5);
    INFO("timing: " << spans(timing));
    // timing, from, its picker, beats per bar and its box, and the words saying the audio input
    // is picked at the top.
    REQUIRE(timing.size() == 6);
    click(middleOf(timing[2]), timingRow);
    key(kDown);
    key("\n");
    click(middleOf(timing[4]), timingRow);
    key(kDown);
    key("\n");
    click(middleOf(timing[5]), timingRow);
    expect({}, "the timing picker and beats per bar while running, and the words beside them");

    const int midiRow = sheets[4].first + 117;
    const auto midi = occupied(shot, midiRow - 17, midiRow + 16, 20, kWidth - 20, kSheet, 5);
    INFO("MIDI: " << spans(midi));
    // MIDI, port, its picker, action, its picker, learn, forget (and the reading, empty here).
    REQUIRE(midi.size() == 7);
    click(middleOf(midi[2]), midiRow);
    key(kDown);
    key("\n");
    expect({"midi-in 1"}, "the MIDI port picker");
    click(middleOf(midi[4]), midiRow);
    key(kDown);
    key("\n");
    expect({"action 1"}, "the action picker");
    click(middleOf(midi[5]), midiRow);
    click(middleOf(midi[6]), midiRow);
    expect({"learn", "forget"}, "learn and forget");

    const int oscRow = sheets[4].first + 163;
    const auto osc = occupied(shot, oscRow - 17, oscRow + 17, 20, kWidth - 20, kSheet, 5);
    INFO("OSC: " << spans(osc));
    // OSC, the listen box and its word, port, the port box, the allow box and its words.
    REQUIRE(osc.size() == 7);
    click(middleOf(osc[1]), oscRow);
    expect({"listen 1"}, "the listen box");
    // Listening, the remote setup line opens under the row, and the plot above gives up the room
    // for it: the rest of the section, and the fold arrow over it, are found again.
    shot = takt4::tests::render(*window, kWidth, kHeight);
    handle.dispatch_window_active_changed_event(true);
    sheets = sheetsDown(shot);
    REQUIRE(sheets.size() == 7);
    const int listeningRow = sheets[4].first + 163;
    const auto listening =
        occupied(shot, listeningRow - 17, listeningRow + 17, 20, kWidth - 20, kSheet, 5);
    INFO("OSC, listening: " << spans(listening));
    REQUIRE(listening.size() == 7);
    // The port box: the run that holds "7001".
    float portX = -1.0f;
    for (const auto& run : listening) {
        if (run.second - run.first + 1 >= 68 && run.second - run.first + 1 <= 72) {
            portX = middleOf(run);
        }
    }
    REQUIRE(portX > 0.0f);
    click(portX, listeningRow);
    takt4::tests::endOfText(handle);
    key("2");
    key("\n");
    expect({"osc-port-key", "osc-port 70012"}, "the OSC port box: a keystroke, then Enter");
    // The allow box: the first run right of the port box.
    for (const auto& run : listening) {
        if (middleOf(run) > portX + 40.0f) {
            click(middleOf(run), listeningRow);
            break;
        }
    }
    expect({"network 1"}, "the allow-other-machines box");
    // The remote setup line: its box and its words, "only from", the box of senders — which takes
    // typing once remote setup is on and other machines are allowed — and what it allows.
    const int remoteRow = listeningRow + 19 + 10 + 19;
    const auto remoteLine =
        occupied(shot, remoteRow - 17, remoteRow + 17, 20, kWidth - 20, kSheet, 5);
    INFO("remote setup: " << spans(remoteLine));
    REQUIRE(remoteLine.size() == 5);
    click(middleOf(remoteLine[0]), remoteRow);
    expect({"remote 1"}, "the remote setup box");
    click(middleOf(remoteLine[3]), remoteRow);
    key("1");
    key("\n");
    expect({"remote-from-key", "remote-from 1"}, "the box of senders: a keystroke, then Enter");
    click(middleOf(remoteLine[2]), remoteRow);
    click(middleOf(remoteLine[4]), remoteRow);
    expect({}, "only from, and what it allows, which are words");

    // --- outputs -------------------------------------------------------------------------------
    // The Link row, 28 px, then the rows, 34 px, 8 px apart.
    const int linkRow = sheets[5].first + 10 + 28 + 8 + 18 + 8 + 14;
    const int deckRow = linkRow + 14 + 8 + 17;
    const int clockRow = deckRow + 17 + 8 + 17;
    click(41.0f, linkRow);
    expect({"on 0 0"}, "Link's on box");
    const auto linkInk = occupied(shot, linkRow - 14, linkRow + 13, 150, 500, kSheet, 5);
    INFO("Link row: " << spans(linkInk));
    REQUIRE_FALSE(linkInk.empty());
    click(middleOf(linkInk.back()), linkRow);
    expect({"peers"}, "show peers");

    click(41.0f, deckRow);
    expect({"on 1 0"}, "the deck's on box");
    click(107.0f, deckRow);
    takt4::tests::endOfText(handle);
    key("2");
    key("\n");
    expect({"name-key", "name 1 deck2"}, "the deck's name box");
    click(352.0f, deckRow);
    takt4::tests::endOfText(handle);
    key("1");
    key("\n");
    expect({"host-key", "host 1 10.0.0.401"}, "the deck's host box");
    click(470.0f, deckRow);
    takt4::tests::endOfText(handle);
    key("9");
    key("\n");
    expect({"port-key", "port 1 70009"}, "the deck's port box");
    click(211.0f, deckRow);
    key(kDown);
    key("\n");
    expect({"kind 1 1"}, "the deck's kind picker");
    click(352.0f, clockRow);
    key(kDown);
    key("\n");
    expect({"device 2 2"}, "the clock's device picker");
    // The delay handle on the deck's row, and its reading.
    float deckThumb = -1.0f;
    for (int x = 500; x < 700 && deckThumb < 0.0f; ++x) {
        if (is(shot, x, deckRow, kThumb)) {
            deckThumb = static_cast<float>(x) + 9.0f;
        }
    }
    REQUIRE(deckThumb > 0.0f);
    handle.dispatch_pointer_move_event(
        slint::LogicalPosition({deckThumb, static_cast<float>(deckRow)}));
    handle.dispatch_pointer_press_event(
        slint::LogicalPosition({deckThumb, static_cast<float>(deckRow)}),
        slint::PointerEventButton::Left);
    settle();
    handle.dispatch_pointer_move_event(
        slint::LogicalPosition({deckThumb + 15.0f, static_cast<float>(deckRow)}));
    settle();
    handle.dispatch_pointer_release_event(
        slint::LogicalPosition({deckThumb + 15.0f, static_cast<float>(deckRow)}),
        slint::PointerEventButton::Left);
    settle();
    expect({"delay 1"}, "the deck's delay handle");
    click(735.0f, deckRow);
    key("3");
    key("\n");
    expect({"delay-typed 1 3"}, "the deck's delay reading");
    click(762.0f, deckRow);
    expect({"remove 1"}, "the deck's ×");
    // Link has no ×: the same column on its row is nothing.
    click(762.0f, linkRow);
    expect({}, "the empty cell at the end of Link's row");

    // "/takt4 global messages to", at the right of the outputs heading: its label, then the box,
    // which opens a list of the one OSC output. The deck's line there asks for the deck to be sent
    // them — once, and the bare window writes nothing back, so it stays unticked.
    const int headRow = sheets[5].first + 10 + 14;
    const auto head = occupied(shot, headRow - 13, headRow + 13, 300, kWidth - 56, kSheet, 5);
    INFO("the outputs heading: " << spans(head));
    // The label, the box, and "in phrases of", its count and "bars".
    REQUIRE(head.size() == 5);
    CHECK(std::string(window->get_global_summary()) == "nothing");
    click(middleOf(head[0]), headRow);
    expect({}, "the label, which is words");
    CHECK_FALSE(window->get_global_open());
    click(middleOf(head[1]), headRow);
    CHECK(window->get_global_open());
    expect({}, "opening the list");
    // The deck's line, found by probing: where a popup opens is Slint's to say. A probe that misses
    // the list closes it, so it is opened again for each one — and a miss must set off nothing
    // under it, which this window would hear.
    float deckLine = -1.0f;
    for (int d = 0; d < 900 && deckLine < 0.0f; d += 3) {
        for (const int y : {headRow + d, headRow - d}) {
            if (y < 100 || y >= kHeight - 130 || deckLine >= 0.0f) {
                continue;
            }
            if (!window->get_global_open()) {
                click(middleOf(head[1]), headRow);
            }
            // Under the box: the list hangs from its right edge (`GlobalPick`).
            click(middleOf(head[1]), y);
            if (fired == std::vector<std::string>{"namespace 1 1"}) {
                deckLine = static_cast<float>(y);
            } else {
                INFO("a probe at 600," << y);
                expect({}, "a probe that missed the deck's line");
            }
        }
    }
    INFO("the deck's line in the list at " << deckLine);
    REQUIRE(deckLine >= 0.0f);
    fired.clear();
    // Escape closes the list, and does not reach PANIC behind it.
    CHECK(window->get_global_open());
    key(kEscape);
    CHECK_FALSE(window->get_global_open());
    expect({}, "Escape in the open list");
    // After the box, the length of a phrase: words, a number typed, words.
    click(middleOf(head[2]), headRow);
    expect({}, "in phrases of, which is words");
    click(middleOf(head[3]), headRow);
    takt4::tests::endOfText(handle);
    key("\x08");
    key("6");
    key("\n");
    expect({"phrase 6"}, "the phrase box: a number typed, then Enter");
    click(middleOf(head[4]), headRow);
    expect({}, "bars, which is words");
    window->set_phrase_bars(8);
    settle();

    // [+ add output], under the rows.
    const int addRow = clockRow + 17 + 8 + 4 + 14;
    click(80.0f, addRow);
    expect({"add"}, "+ add output");

    // The two fold arrows, at the right of each heading.
    click(760.0f, sheets[4].first + 24);
    click(760.0f, sheets[5].first + 24);
    expect({"fold 0", "fold 1"}, "the fold arrows");

    // --- triggers and the status bar ---------------------------------------------------------
    const int trigRow = sheets[6].first + 33;
    const auto trig = occupied(shot, trigRow - 23, trigRow + 22, 20, kWidth - 20, kSheet, 5);
    INFO("triggers: " << spans(trig));
    // 04, triggers, the rules button and its count ("1 of 2 active"), the fixtures button and
    // its count, PANIC. A button says what it does; the count beside it is a reading.
    REQUIRE(trig.size() == 7);
    click(middleOf(trig[2]), trigRow);
    click(middleOf(trig[4]), trigRow);
    click(middleOf(trig.back()), trigRow);
    expect({"rules", "fixtures", "panic"}, "the rules, the fixtures, PANIC");
    click(middleOf(trig[3]), trigRow);
    click(middleOf(trig[5]), trigRow);
    expect({}, "the two counts, which are readings");
    // Engaged, RELEASE appears to PANIC's left, and PANIC does not move.
    window->set_panicked(true);
    shot = takt4::tests::render(*window, kWidth, kHeight);
    handle.dispatch_window_active_changed_event(true);
    const auto engaged = occupied(shot, trigRow - 23, trigRow + 22, 20, kWidth - 20, kSheet, 5);
    INFO("triggers, panicked: " << spans(engaged));
    REQUIRE(engaged.size() == 8);
    CHECK(engaged.back().second == trig.back().second);
    click(middleOf(engaged[engaged.size() - 2]), trigRow);
    click(middleOf(engaged.back()), trigRow);
    expect({"release", "panic"}, "RELEASE, then PANIC again");

    const int statusRow = kHeight - 25;
    const auto status = occupied(shot, statusRow - 12, statusRow + 12, 400, kWidth, kPage, 5);
    INFO("status bar: " << spans(status));
    REQUIRE(status.size() == 5);
    // From the right: about, presets, import, export, save — so a sweep never meets the file
    // routes first.
    for (std::size_t i = 0; i < 5; ++i) {
        click(middleOf(status[status.size() - 1 - i]), statusRow);
    }
    expect({"about", "presets", "import", "export", "save"}, "the status bar's buttons");

    // --- following a MIDI clock, stopped: the banner, and the timing rows' own controls -------
    window->set_panicked(false);
    window->set_running(false);
    window->set_timing_index(2);
    window->set_timing_name(slint::SharedString("MIDI clock"));
    window->set_clock_in_ports(strings({"select input", "mixer A", "mixer B"}));
    window->set_clock_in_port_index(1);
    shot = takt4::tests::render(*window, kWidth, kHeight);
    sheets = sheetsDown(shot);
    REQUIRE(sheets.size() == 7);
    handle.dispatch_window_active_changed_event(true);
    // The audio pickers give way to the banner — a box round the source's name, its words and
    // CHANGE — and RESCAN and START stay where they were.
    const auto banner = occupied(shot, topRow - 20, topRow + 20, 20, kWidth - 20, kSheet, 5);
    INFO("following, top bar: " << spans(banner));
    REQUIRE(banner.size() == 3);
    CHECK(banner[1] == top[4]);
    CHECK(banner[2] == top[5]);
    // CHANGE, at the box's right end — and, stopped, the box it is in: the source's name and the
    // words. Each opens the timing list, which a click on the background closes (Escape is
    // PANIC's), so the next click is a click and not the list closing.
    for (const int x : {banner[0].second - 30, banner[0].first + 50, banner[0].first + 200}) {
        click(x, topRow);
        background();
    }
    expect({"timing-change", "timing-change", "timing-change"}, "CHANGE, and the box it is in");
    click(middleOf(banner[2]), topRow);
    expect({"toggle-run"}, "START, following");

    // From this picture's own sheets: following, the line under the strip saying the audio is not
    // in use makes the timing sheet taller, and the rows below it lower.
    const int followRow = sheets[4].first + 73;
    const auto picking = occupied(shot, followRow - 17, followRow + 16, 20, kWidth - 20, kSheet, 5);
    INFO("following, timing row: " << spans(picking));
    // timing, from, its picker, port, the port's picker — and no reading while stopped.
    REQUIRE(picking.size() == 5);
    click(middleOf(picking[4]), followRow);
    key(kDown);
    key("\n");
    expect({"clock-in 2"}, "the MIDI clock's port picker");
    click(middleOf(picking[2]), followRow);
    key(kUp);
    key("\n");
    expect({"timing 1"}, "the timing picker, stopped");
    // Put back as it was, as an owner would have it, so the rows below stay where they were drawn.
    window->set_timing_index(2);
    settle();

    const int howRow = followRow + 44;
    const auto how = occupied(shot, howRow - 17, howRow + 16, 20, kWidth - 20, kSheet, 5);
    INFO("following, the row under it: " << spans(how));
    // "beats per bar", its box, the obey box and its words, and the hint about it at the right.
    REQUIRE(how.size() == 5);
    click(middleOf(how[1]), howRow);
    takt4::tests::endOfText(handle);
    key("\x08");
    key("3");
    key("\n");
    expect({"beats 3"}, "the beats-per-bar box: a number typed, then Enter");
    click(middleOf(how[2]), howRow);
    click(static_cast<float>(how[3].first + 20), howRow);
    expect({"obey 1", "obey 0"}, "the obey box, then its words");
    window->set_timing_index(0);

    // --- beat detection, stopped: its beats per bar, live --------------------------------------
    settle();
    shot = takt4::tests::render(*window, kWidth, kHeight);
    handle.dispatch_window_active_changed_event(true);
    sheets = sheetsDown(shot);
    REQUIRE(sheets.size() == 7);
    const int detectRow = sheets[4].first + 73;
    const auto detecting =
        occupied(shot, detectRow - 17, detectRow + 16, 20, kWidth - 20, kSheet, 5);
    INFO("beat detection, stopped, timing row: " << spans(detecting));
    REQUIRE(detecting.size() == 6);
    click(middleOf(detecting[4]), detectRow);
    key(kDown);
    key("\n");
    expect({"meter 1"}, "beats per bar, stopped");
}

TEST_CASE("the main window opens 800 wide and 934 tall", "[ui]") {
    // The width cut by a fifth, and the height kept with a scroll bar.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto* const adapter = takt4::ui::headlessAdapterFor(controller.window().window());
    REQUIRE(adapter != nullptr);
    const auto asked = adapter->requested();
    REQUIRE(asked.has_value());
    CHECK(asked->width == 800);
    CHECK(asked->height == 934);
}

TEST_CASE("folding a section takes its height off the window, keeps its heading in sight, and "
          "gives the height back",
          "[ui]") {
    // A fold takes the section's height off the *window* — the trace keeps its height — but
    // never so much that the folded heading, with the arrow that opens it again, scrolls out of
    // sight; opening it gives the height back. Driven by clicks on the real arrows, with the
    // window then made the size the controller asked for, as a window manager would; and every
    // height checked against the sheets measured off a render, not the controller's figures.
    LiveTracker tracker(kWeights, kStateSpace);
    // Both sections open to start with, which a fresh install no longer is: it folds Inputs
    // (see the test after this one).
    takt4::settings::Settings bothOpen;
    bothOpen.machine.inputsFolded = false;
    WindowController controller(tracker, bothOpen);
    const takt4::testing::LoopbackReceiver added;
    controller.setNewOutputPort(added.port());
    // Outputs tall enough that folding them would take the window above their own heading.
    for (int i = 0; i < 4; ++i) {
        controller.addTarget();
    }
    constexpr int kWidth = 800;
    auto* const adapter = takt4::ui::headlessAdapterFor(controller.window().window());
    REQUIRE(adapter != nullptr);
    auto& window = controller.window().window();
    const takt4::tests::NothingReal nothingReal;
    int height = 934;
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto shoot = [&] {
        const takt4::tests::Shot shot = takt4::tests::render(controller.window(), kWidth, height);
        window.dispatch_window_active_changed_event(true);
        return shot;
    };
    const auto look = [&] { return sheetsDown(shoot()); };
    constexpr int kPinned = 10 + 66 + 51; // the triggers row and the status bar, which never scroll
    // Whether the body has anything left to scroll: its bar, in the page's right-hand margin.
    const auto scrolls = [&](const takt4::tests::Shot& shot) {
        for (int y = 0; y < shot.height - kPinned; ++y) {
            if (is(shot, kWidth - 5, y, Rgb{0x55, 0x55, 0x53})) {
                return true;
            }
        }
        return false;
    };
    // The size the controller asked for, taken as a window manager takes it.
    const auto obey = [&] {
        const auto asked = adapter->requested();
        REQUIRE(asked.has_value());
        height = static_cast<int>(asked->height);
        CHECK(asked->width == static_cast<std::uint32_t>(kWidth));
    };
    const auto clickArrow = [&](const std::pair<int, int>& sheet) {
        clickAt(window, static_cast<float>(kWidth - 26 - 14), static_cast<float>(sheet.first + 24));
        settle();
    };
    // Each section's own height, from a window tall enough to show all of it: in a shorter one the
    // lowest sheet is cut off where the view ends. A section's height is the same in any window
    // (only the trace takes spare height).
    height = 2000;
    const std::vector<std::pair<int, int>> whole = look();
    REQUIRE(whole.size() == 7);
    const int inputsOpen = whole[4].second - whole[4].first + 1;
    const int outputsOpen = whole[5].second - whole[5].first + 1;
    INFO("inputs " << inputsOpen << " px open, outputs " << outputsOpen << " px open");

    // At 934 this rig's content is taller than the view, so it is laid out at its least — each
    // sheet's top is where it stays when a fold shortens what is below it. (In a window with
    // height to spare the trace stretches and pushes every sheet under it to the bottom: the
    // first build read a heading's place from that, and a fold of Outputs moved nothing.)
    height = 934;
    const takt4::tests::Shot first = shoot();
    REQUIRE(scrolls(first));
    const std::vector<std::pair<int, int>> open = sheetsDown(first);
    REQUIRE(open.size() == 7);
    REQUIRE(open[4].second - open[4].first + 1 == inputsOpen);

    SECTION("Inputs, folded and opened") {
        clickArrow(open[4]);
        CHECK(controller.window().get_inputs_folded());
        obey();
        const std::vector<std::pair<int, int>> folded = look();
        REQUIRE(folded.size() == 7);
        CHECK(folded[4] == std::make_pair(open[4].first, open[4].first + 39));
        // Shorter by exactly what the fold took off the section: its heading is far above the
        // bottom.
        CHECK(height == 934 - (inputsOpen - 40));
        clickArrow(folded[4]);
        CHECK_FALSE(controller.window().get_inputs_folded());
        obey();
        CHECK(height == 934);
        const std::vector<std::pair<int, int>> reopened = look();
        CHECK(reopened[4].second - reopened[4].first + 1 == inputsOpen);
    }

    SECTION("Outputs, folded: the window stops at its heading") {
        // Their whole height off the window would put the heading under the triggers row. The
        // window stops where the heading's bottom edge meets what is pinned — shorter than it
        // was, and with nothing left to scroll.
        const int stop = open[5].first + 40 + kPinned;
        REQUIRE(934 - (outputsOpen - 40) < stop);
        REQUIRE(stop < 934);
        clickArrow(open[5]);
        CHECK(controller.window().get_outputs_folded());
        obey();
        CHECK(height == stop);
        const takt4::tests::Shot shot = shoot();
        const std::vector<std::pair<int, int>> folded = sheetsDown(shot);
        REQUIRE(folded.size() == 7);
        CHECK(folded[5] == std::make_pair(open[5].first, open[5].first + 39));
        CHECK_FALSE(scrolls(shot));
        // And it is there to be clicked: the arrow opens the section again, and the height the
        // fold took comes back.
        clickArrow(folded[5]);
        CHECK_FALSE(controller.window().get_outputs_folded());
        obey();
        CHECK(height == 934);
        CHECK(scrolls(shoot()));
    }

    SECTION("both, and opened in the other order") {
        clickArrow(open[4]);
        obey();
        const int afterInputs = height;
        CHECK(afterInputs == 934 - (inputsOpen - 40));
        std::vector<std::pair<int, int>> now = look();
        clickArrow(now[5]);
        obey();
        // Outputs' heading has come up by what Inputs gave; the window stops at it again.
        const int stop = open[5].first - (inputsOpen - 40) + 40 + kPinned;
        REQUIRE(afterInputs - (outputsOpen - 40) < stop);
        CHECK(height == stop);
        CHECK(height < afterInputs);
        const takt4::tests::Shot shot = shoot();
        now = sheetsDown(shot);
        REQUIRE(now.size() == 7);
        CHECK(now[4].second - now[4].first + 1 == 40);
        CHECK(now[5] == std::make_pair(stop - kPinned - 40, stop - kPinned - 1));
        CHECK_FALSE(scrolls(shot));
        clickArrow(now[4]);
        obey();
        CHECK(height == stop + (inputsOpen - 40));
        now = look();
        clickArrow(now[5]);
        obey();
        CHECK(height == 934);
    }

    nothingReal.check();
}

TEST_CASE("a window opens folded as it was left, shorter for it, and opening gives the height "
          "back",
          "[ui][settings]") {
    // Folds are remembered across launches. The second window
    // must open at the height the first reached by clicks, from a file read back as the next
    // launch reads it.
    constexpr int kWidth = 800;
    const takt4::testing::LoopbackReceiver added;
    takt4::settings::Settings saved;
    int foldedHeight = 0;
    {
        LiveTracker tracker(kWeights, kStateSpace);
        takt4::settings::Settings bothOpen; // a fresh install folds Inputs: see the test after
        bothOpen.machine.inputsFolded = false;
        WindowController first(tracker, bothOpen);
        first.setNewOutputPort(added.port());
        for (int i = 0; i < 4; ++i) {
            first.addTarget();
        }
        auto* const adapter = takt4::ui::headlessAdapterFor(first.window().window());
        REQUIRE(adapter != nullptr);
        (void)takt4::tests::render(first.window(), kWidth, 934);
        CHECK_FALSE(first.currentSettings().machine.inputsFolded);
        first.toggleFold(0);
        (void)takt4::tests::render(first.window(), kWidth,
                                   static_cast<int>(adapter->requested()->height));
        first.toggleFold(1);
        foldedHeight = static_cast<int>(adapter->requested()->height);
        REQUIRE(foldedHeight < 934 - 100);
        saved = first.currentSettings();
    }
    CHECK(saved.machine.inputsFolded);
    CHECK(saved.machine.outputsFolded);
    saved = takt4::settings::fromJson(takt4::settings::toJson(saved));

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController second(tracker, saved);
    auto* const adapter = takt4::ui::headlessAdapterFor(second.window().window());
    REQUIRE(adapter != nullptr);
    // Folded from the start, so nothing is drawn open and then shut. The height waits for the
    // window to be shown: before that Slint has not built the output rows, and the content
    // measures 230 px short (the first build worked it out in the constructor, and opened this
    // window at 774).
    CHECK(second.window().get_inputs_folded());
    CHECK(second.window().get_outputs_folded());
    REQUIRE(adapter->requested().has_value());
    CHECK(adapter->requested()->height == 934);
    second.show();
    CHECK(static_cast<int>(adapter->requested()->height) == foldedHeight);
    CHECK(second.window().get_inputs_folded());
    CHECK(second.window().get_outputs_folded());
    const std::vector<std::pair<int, int>> sheets =
        sheetsDown(takt4::tests::render(second.window(), kWidth, foldedHeight));
    REQUIRE(sheets.size() == 7);
    CHECK(sheets[4].second - sheets[4].first + 1 == 40);
    CHECK(sheets[5].second - sheets[5].first + 1 == 40);

    // Opening both gives back what folding them took at the launch.
    second.toggleFold(1);
    (void)takt4::tests::render(second.window(), kWidth,
                               static_cast<int>(adapter->requested()->height));
    second.toggleFold(0);
    CHECK(adapter->requested()->height == 934);
    // And what the next launch is told is what the window is now.
    CHECK_FALSE(second.currentSettings().machine.inputsFolded);
    CHECK_FALSE(second.currentSettings().machine.outputsFolded);
}


TEST_CASE("a fresh window opens with the control inputs folded and the outputs open", "[ui]") {
    // MIDI and OSC control are set once for a rig and then only looked at, so a fresh install
    // opens with that section folded to its heading, which still says what each is doing.
    // Outputs, which an operator works in, stay open.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    CHECK(controller.window().get_inputs_folded());
    CHECK_FALSE(controller.window().get_outputs_folded());
    CHECK(controller.currentSettings().machine.inputsFolded);
}

TEST_CASE("opening a section takes its height from what the trace has to spare before the window "
          "grows, and folding it gives that back",
          "[ui]") {
    // A window made taller gave the height to the trace, and opening Inputs then made the window
    // taller still, or scrolled, rather than taking it back from the trace — the one part that
    // stretches, and the one worth least. Driven by clicks on the real arrow, from a window
    // opened folded as a launch opens it; every height off a render.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker); // a fresh install: Inputs folded, Outputs open
    REQUIRE(controller.window().get_inputs_folded());
    auto* const adapter = takt4::ui::headlessAdapterFor(controller.window().window());
    REQUIRE(adapter != nullptr);
    auto& window = controller.window().window();
    const takt4::tests::NothingReal nothingReal;
    constexpr int kWidth = 800;
    controller.show();
    const int opened = static_cast<int>(adapter->requested()->height);
    int height = opened;
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto shoot = [&] {
        const takt4::tests::Shot shot = takt4::tests::render(controller.window(), kWidth, height);
        window.dispatch_window_active_changed_event(true);
        return shot;
    };
    const auto span = [](const std::pair<int, int>& sheet) {
        return sheet.second - sheet.first + 1;
    };
    constexpr int kPinned = 10 + 66 + 51; // the triggers row and the status bar, which never scroll
    // Whether the body has anything left to scroll: its bar, in the page's right-hand margin.
    const auto scrolls = [&](const takt4::tests::Shot& shot) {
        for (int y = 0; y < shot.height - kPinned; ++y) {
            if (is(shot, kWidth - 5, y, Rgb{0x55, 0x55, 0x53})) {
                return true;
            }
        }
        return false;
    };
    const auto clickArrow = [&](const std::pair<int, int>& sheet) {
        clickAt(window, static_cast<float>(kWidth - 26 - 14), static_cast<float>(sheet.first + 24));
        settle();
    };
    // The trace at its least: in a window too short for everything the body scrolls, and the
    // trace has nothing to spare.
    height = 600;
    const takt4::tests::Shot shortest = shoot();
    REQUIRE(scrolls(shortest));
    // (The sheets below the view are cut off where it ends; the trace's is the third from the top.)
    const std::vector<std::pair<int, int>> least = sheetsDown(shortest);
    REQUIRE(least.size() >= 3);
    const int traceLeast = span(least[2]);

    SECTION("a window made taller lends the section the trace's spare, and does not grow") {
        // Dragged 500 px taller: everything fits, and the trace takes the rest.
        height = opened + 500;
        const std::vector<std::pair<int, int>> tall = sheetsDown(shoot());
        REQUIRE(tall.size() == 7);
        const int trace = span(tall[2]);
        REQUIRE(span(tall[4]) == 40);
        clickArrow(tall[4]);
        CHECK_FALSE(controller.window().get_inputs_folded());
        // No height asked for: the last size the window was asked to be is still the launch's.
        CHECK(static_cast<int>(adapter->requested()->height) == opened);
        const takt4::tests::Shot shot = shoot();
        const std::vector<std::pair<int, int>> open = sheetsDown(shot);
        REQUIRE(open.size() == 7);
        const int section = span(open[4]) - 40;
        INFO("the trace " << trace << " px, " << trace - traceLeast << " to spare; Inputs "
                          << section << " px taller open");
        REQUIRE(section > 0);
        REQUIRE(trace - traceLeast >= section);
        CHECK(span(open[2]) == trace - section);
        CHECK_FALSE(scrolls(shot));
        // Folded again: the trace has its height back, and the window is the size it was made.
        clickArrow(open[4]);
        CHECK(controller.window().get_inputs_folded());
        CHECK(static_cast<int>(adapter->requested()->height) == opened);
        const std::vector<std::pair<int, int>> folded = sheetsDown(shoot());
        REQUIRE(folded.size() == 7);
        CHECK(span(folded[2]) == trace);
        CHECK(span(folded[4]) == 40);
    }

    SECTION("what the spare cannot cover makes the window taller, and folding takes that off") {
        height = opened + 50;
        const std::vector<std::pair<int, int>> tall = sheetsDown(shoot());
        REQUIRE(tall.size() == 7);
        const int trace = span(tall[2]);
        const int spare = trace - traceLeast;
        REQUIRE(spare > 0);
        const int made = height;
        clickArrow(tall[4]);
        height = static_cast<int>(adapter->requested()->height);
        const takt4::tests::Shot shot = shoot();
        const std::vector<std::pair<int, int>> open = sheetsDown(shot);
        REQUIRE(open.size() == 7);
        const int section = span(open[4]) - 40;
        INFO("the trace " << trace << " px, " << spare << " to spare; Inputs " << section
                          << " px taller open; the window " << made << " then " << height);
        REQUIRE(section > spare);
        // Taller by what the spare could not cover, and not a pixel more: the trace at its least,
        // and nothing to scroll.
        CHECK(height - made == section - spare);
        CHECK(span(open[2]) == traceLeast);
        CHECK_FALSE(scrolls(shot));
        // Folded again: the window gives up what it grew, and the trace has its spare back.
        clickArrow(open[4]);
        height = static_cast<int>(adapter->requested()->height);
        CHECK(height == made);
        const std::vector<std::pair<int, int>> folded = sheetsDown(shoot());
        REQUIRE(folded.size() == 7);
        CHECK(span(folded[2]) == trace);
    }

    nothingReal.check();
}

TEST_CASE("the rule editor opens folded as it was left, with its log as it was",
          "[ui][settings][trigger]") {
    // The rule editor's folds, A to D and the event log, and the log's height are the window's
    // and remembered across launches, like the main window's. Saved by one window, read back as
    // the next launch reads it.
    takt4::settings::Settings saved;
    {
        LiveTracker tracker(kWeights, kStateSpace);
        WindowController first(tracker);
        const takt4::settings::MachineSettings before = first.currentSettings().machine;
        CHECK(before.ruleSectionsFolded == std::array<bool, 4>{false, true, false, false});
        CHECK_FALSE(before.ruleLogOpen);
        first.editor().toggleFold(0);
        first.editor().toggleFold(1);
        first.editor().toggleFold(4);
        first.editor().window().set_log_lines_height(144.0f);
        saved = first.currentSettings();
    }
    CHECK(saved.machine.ruleSectionsFolded == std::array<bool, 4>{true, false, false, false});
    CHECK(saved.machine.ruleLogOpen);
    CHECK(saved.machine.ruleLogHeight == Catch::Approx(144.0));
    saved = takt4::settings::fromJson(takt4::settings::toJson(saved));

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController second(tracker, saved);
    RulesWindow& editor = second.editor().window();
    CHECK(editor.get_when_folded());
    CHECK_FALSE(editor.get_only_if_folded());
    CHECK_FALSE(editor.get_send_folded());
    CHECK_FALSE(editor.get_then_folded());
    CHECK(editor.get_log_open());
    CHECK(editor.get_log_lines_height() == Catch::Approx(144.0f));
}

TEST_CASE("an output edit typed and not entered stays off the outputs an import brings in",
          "[ui][settings]") {
    // Found reviewing, 2026-10-01: a box that had the keyboard commits a turn of the event loop
    // after the click that took it — IMPORT's — and IMPORT replaces the rows under the boxes. When
    // row i of the imported rig held what the box was shown, nothing changed the box, and its late
    // commit put what was typed onto the imported output. Driven as the markup sends it: the
    // keystroke, the import, then the box's own commit.
    takt4::settings::Settings show;
    takt4::output::OutputTarget deck;
    deck.name = "deck";
    deck.kind = takt4::output::OutputTarget::Kind::Osc;
    deck.host = "10.0.0.40";
    deck.port = 7000;
    show.preset.outputs = {deck};
    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "show.json";
    REQUIRE(takt4::settings::save(show, file));

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker, show); // the same show on screen, as a re-import has it
    auto& window = controller.window();
    // A copy: `currentSettings` is built afresh on every call.
    const auto deckNow = [&controller]() -> takt4::output::OutputTarget {
        const std::vector<takt4::output::OutputTarget> outputs =
            controller.currentSettings().preset.outputs;
        const auto found = std::find_if(outputs.begin(), outputs.end(),
                                        [](const auto& target) { return target.name == "deck"; });
        REQUIRE(found != outputs.end());
        return *found;
    };
    const auto rows = window.get_outputs_list();
    int row = -1;
    for (std::size_t i = 0; i < rows->row_count(); ++i) {
        if (std::string(rows->row_data(i)->name) == "deck") {
            row = static_cast<int>(i);
        }
    }
    REQUIRE(row >= 0);

    window.invoke_output_host_edited(row, "10.0.0.99"); // typed, not entered
    REQUIRE(controller.importFrom(file));
    window.invoke_output_host_accepted(row, "10.0.0.99"); // the box's own commit, late
    CHECK(deckNow().host == "10.0.0.40");

    window.invoke_output_delay_keyed(row);
    REQUIRE(controller.importFrom(file));
    window.invoke_output_delay_typed(row, "33");
    REQUIRE(controller.settleOutputs());
    CHECK(deckNow().delaySeconds == 0.0);

    // With no import in between, what is typed lands.
    window.invoke_output_host_edited(row, "10.0.0.77");
    window.invoke_output_host_accepted(row, "10.0.0.77");
    CHECK(deckNow().host == "10.0.0.77");
    window.invoke_output_delay_keyed(row);
    window.invoke_output_delay_typed(row, "33");
    REQUIRE(controller.settleOutputs());
    CHECK(deckNow().delaySeconds == Catch::Approx(0.033));
}

TEST_CASE("the patch editor opens folded as it was left", "[ui][settings][dmx]") {
    // The patch editor's A, B and C since its redesign (2026-09-30), remembered as the rule
    // editor's are: saved by one window, read back as the next launch reads it.
    takt4::settings::Settings saved;
    {
        LiveTracker tracker(kWeights, kStateSpace);
        WindowController first(tracker);
        CHECK(first.currentSettings().machine.patchSectionsFolded ==
              std::array<bool, 3>{false, false, false});
        first.patchEditor().fold(0);
        first.patchEditor().fold(2);
        saved = first.currentSettings();
    }
    CHECK(saved.machine.patchSectionsFolded == std::array<bool, 3>{true, false, true});
    saved = takt4::settings::fromJson(takt4::settings::toJson(saved));

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController second(tracker, saved);
    FixturesWindow& patch = second.patchEditor().window();
    CHECK(patch.get_where_folded());
    CHECK_FALSE(patch.get_channels_folded());
    CHECK(patch.get_moves_folded());
}

TEST_CASE("folding a section while one of its boxes is being typed in keeps the edit", "[ui]") {
    // A box commits a turn of the event loop after it loses the keyboard — which the click on
    // the fold arrow takes. A section removed by the fold took the box, and the edit, with it;
    // a folded section's body is clipped to nothing instead, so the commit lands. A bare window,
    // with its owner folding as the controller does.
    auto window = MainWindow::create();
    std::vector<std::string> fired;
    window->on_osc_control_port_edited(
        [&](const slint::SharedString& t) { fired.push_back("osc-port " + std::string(t)); });
    window->on_output_host_accepted([&](int i, const slint::SharedString& t) {
        fired.push_back("host " + std::to_string(i) + " " + std::string(t));
    });
    window->on_fold_clicked([&](int section) {
        if (section == 0) {
            window->set_inputs_folded(!window->get_inputs_folded());
        } else {
            window->set_outputs_folded(!window->get_outputs_folded());
        }
    });
    window->set_osc_control_port(slint::SharedString("7001"));
    auto rows = std::make_shared<slint::VectorModel<OutputRow>>();
    OutputRow deck{};
    deck.name = slint::SharedString("deck");
    deck.host = slint::SharedString("10.0.0.40");
    deck.port = slint::SharedString("7000");
    deck.enabled = true;
    rows->push_back(deck);
    window->set_outputs_list(rows);

    constexpr int kWidth = 800;
    constexpr int kHeight = 1200;
    auto& handle = window->window();
    const auto settle = [] { slint::platform::update_timers_and_animations(); };
    const takt4::tests::Shot shot = takt4::tests::render(*window, kWidth, kHeight);
    const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
    REQUIRE(sheets.size() == 7);
    handle.dispatch_window_active_changed_event(true);

    // The OSC port box: on the OSC row — under the timing row and the MIDI row — the run of ink
    // 70 px wide.
    const int oscRow = sheets[4].first + 163;
    float portX = -1.0f;
    for (const auto& run : inkAlong(shot, oscRow, 20, kWidth - 20, kSheet, 3)) {
        if (run.second - run.first + 1 >= 68 && run.second - run.first + 1 <= 72) {
            portX = middleOf(run);
        }
    }
    REQUIRE(portX > 0.0f);
    clickAt(handle, portX, static_cast<float>(oscRow));
    settle();
    takt4::tests::endOfText(handle);
    press(handle, "5");
    settle();
    // The arrow, and the edit is not lost with the section.
    clickAt(handle, static_cast<float>(kWidth - 40), static_cast<float>(sheets[4].first + 24));
    settle();
    settle();
    CHECK(window->get_inputs_folded());
    CHECK(fired == std::vector<std::string>{"osc-port 70015"});
    fired.clear();

    // The same for an output's host, with Outputs folded. Its sheet moved up with the fold.
    const takt4::tests::Shot now = takt4::tests::render(*window, kWidth, kHeight);
    const std::vector<std::pair<int, int>> after = sheetsDown(now);
    REQUIRE(after.size() == 7);
    handle.dispatch_window_active_changed_event(true);
    const int deckRow = after[5].first + 10 + 28 + 8 + 18 + 8 + 17;
    clickAt(handle, 352.0f, static_cast<float>(deckRow));
    settle();
    takt4::tests::endOfText(handle);
    press(handle, "7");
    settle();
    clickAt(handle, static_cast<float>(kWidth - 40), static_cast<float>(after[5].first + 24));
    settle();
    settle();
    CHECK(window->get_outputs_folded());
    CHECK(fired == std::vector<std::string>{"host 0 10.0.0.407"});
}

TEST_CASE("the preset's library of fixture definitions is saved, grows with an import, and is "
          "restored by IMPORT",
          "[ui][dmx][import]") {
    // Found wiring the import (2026-10-05): this window kept the patch for saving and not the
    // library, so a library read at startup was gone at the next save, and every fixture's link
    // to it at the launch after.
    std::string problem;
    std::optional<takt4::fixtures::FixtureProfile> diablo =
        takt4::fixtures::importProfile(std::filesystem::path(TAKT4_TEST_DATA_DIR) / "fixtures" /
                                           "ofl" / "ayrton" / "diablo-s.json",
                                       problem);
    INFO(problem);
    REQUIRE(diablo);
    takt4::settings::Settings loaded;
    takt4::fixtures::addProfile(loaded.preset.library, *diablo);
    loaded.preset.fixtures =
        takt4::fixtures::makeFixtures(loaded.preset.library[0], "Standard", 1, 0, 1, {}).fixtures;
    REQUIRE(loaded.preset.fixtures.size() == 1);

    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker, loaded);
    takt4::settings::Settings saved = controller.currentSettings();
    REQUIRE(saved.preset.library.size() == 1);
    CHECK(saved.preset.library[0].id == loaded.preset.library[0].id);
    REQUIRE(saved.preset.fixtures.size() == 1);
    CHECK(saved.preset.fixtures[0].profile.id == saved.preset.library[0].id);
    // And through the file, links and all.
    const takt4::settings::Settings again =
        takt4::settings::fromJson(takt4::settings::toJson(saved));
    REQUIRE(again.preset.library.size() == 1);
    CHECK(again.preset.fixtures[0].profile.linked());
    CHECK(controller.patchEditor().library().size() == 1);

    SECTION("an import in the patch editor reaches the file") {
        auto& patch = controller.patchEditor();
        patch.openImport();
        patch.importFile(std::filesystem::path(TAKT4_TEST_DATA_DIR) / "fixtures" / "ofl" / "adb" /
                         "alc4.json");
        patch.confirmImport();
        saved = controller.currentSettings();
        CHECK(saved.preset.library.size() == 2);
        CHECK(saved.preset.fixtures.size() == 2);
    }
    SECTION("IMPORT of a settings file brings its library with its fixtures") {
        const takt4::test::TempDir dir;
        const std::filesystem::path file = dir.path() / "no-library.json";
        takt4::settings::Settings bare = saved;
        bare.preset.library.clear();
        bare.preset.fixtures.clear();
        REQUIRE(takt4::settings::save(bare, file));
        REQUIRE(controller.importFrom(file));
        CHECK(controller.currentSettings().preset.library.empty());
        CHECK(controller.patchEditor().library().empty());

        const std::filesystem::path back = dir.path() / "with-library.json";
        REQUIRE(takt4::settings::save(saved, back));
        REQUIRE(controller.importFrom(back));
        CHECK(controller.currentSettings().preset.library.size() == 1);
        CHECK(controller.patchEditor().library().size() == 1);
        CHECK(controller.currentSettings().preset.fixtures[0].profile.linked());
    }
}

TEST_CASE("a right-click puts each of the main window's sliders back to a fresh preset's", "[ui]") {
    // A right-click on any slider sets it back to its default —
    // here what a fresh preset has (`settings::freshTempoOptions`), and no delay on an output.
    // Each is moved off it, and the whole window is right-clicked on a grid: every slider must
    // come back, each to its own default, and nothing real is reached on the way.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver added;
    controller.setNewOutputPort(added.port());
    controller.addTarget();
    constexpr float kWidth = 1000.0f;
    constexpr float kHeight = 1400.0f;
    layOut(controller, kWidth, kHeight);
    auto& window = controller.window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const takt4::tests::NothingReal nothingReal;
    const Options fresh = takt4::settings::freshTempoOptions();
    const auto rows = window.get_outputs_list();
    REQUIRE(rows->row_count() == 2);
    const auto delay = [&rows] { return rows->row_data(1)->delay_ms; };
    const auto moveOff = [&] {
        controller.setFoldEnabled(true);
        controller.setFoldMax(190.0);
        controller.setFoldMin(100.0);
        controller.setLatencyMs(-30.0);
        controller.setTargetDelay(1, 40.0f);
        settle();
    };
    moveOff();
    REQUIRE(window.get_fold_min() == Catch::Approx(100.0f));
    REQUIRE(window.get_fold_max() == Catch::Approx(190.0f));
    REQUIRE(window.get_latency_ms() == Catch::Approx(-30.0f));
    REQUIRE(delay() == Catch::Approx(40.0f));

    std::set<std::string> reset;
    std::vector<std::string> wrong;
    // 16 x 12 px: every slider here is 24 px tall and wider than 100 (see the rule editor's
    // sweep for why no finer).
    for (float y = 4.0f; y < kHeight; y += 12.0f) {
        for (float x = 4.0f; x < kWidth; x += 16.0f) {
            rightClickAt(window.window(), x, y);
            settle();
            const std::string where = " at " + std::to_string(static_cast<int>(x)) + ", " +
                                      std::to_string(static_cast<int>(y));
            bool moved = false;
            if (window.get_latency_ms() != Catch::Approx(-30.0f)) {
                moved = true;
                if (window.get_latency_ms() ==
                    Catch::Approx(fresh.latencyOffsetSeconds * 1000.0).margin(0.01)) {
                    reset.insert("latency");
                } else {
                    wrong.push_back("latency to " + std::to_string(window.get_latency_ms()) +
                                    where);
                }
            }
            if (window.get_fold_min() != Catch::Approx(100.0f) ||
                window.get_fold_max() != Catch::Approx(190.0f)) {
                moved = true;
                if (window.get_fold_min() == Catch::Approx(fresh.minBpm) &&
                    window.get_fold_max() == Catch::Approx(fresh.maxBpm)) {
                    reset.insert("fold");
                } else {
                    wrong.push_back("keep BPM in to " + std::to_string(window.get_fold_min()) +
                                    " - " + std::to_string(window.get_fold_max()) + where);
                }
            }
            if (delay() != Catch::Approx(40.0f)) {
                moved = true;
                if (delay() == 0.0f) {
                    reset.insert("delay");
                } else {
                    wrong.push_back("delay to " + std::to_string(delay()) + where);
                }
            }
            if (rows->row_count() != 2 || !window.get_fold_on()) {
                wrong.push_back("something else changed" + where);
                moved = true;
            }
            if (moved) {
                moveOff();
            }
        }
    }
    INFO("wrong: " << (wrong.empty() ? std::string("nothing") : wrong.front()) << " ("
                   << wrong.size() << ")");
    CHECK(wrong.empty());
    CHECK(reset == std::set<std::string>{"delay", "fold", "latency"});
    nothingReal.check();
}

TEST_CASE("the Liberation preset patches its zones, adds Liberation's output and switches Link on",
          "[ui][liberation][network]") {
    takt4::testing::skipWhileLinkIsOnTheNetwork();
    // Tagged for the network because adding switches Link on, which joins the session on the LAN.
    // The Art-Net output is aimed at a listener of the test's own rather than Art-Net's port, so a
    // Liberation running on this machine never hears it (`RulesController::setLiberationPort`).
    takt4::testing::LoopbackReceiver node;
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::ui::RulesController& editor = controller.editor();
    editor.setLiberationPort(node.port());
    editor.openLiberation();
    editor.setLaserCount(2);
    editor.addLiberation();
    REQUIRE_FALSE(editor.liberationOpen());

    const takt4::settings::Settings saved = controller.currentSettings();
    // Two zones, in the patch the file keeps and the patch editor shows.
    std::vector<std::string> zones;
    for (const takt4::dmx::Fixture& fixture : saved.preset.fixtures) {
        if (takt4::dmx::liberation::isZone(fixture)) {
            zones.push_back(fixture.id);
        }
    }
    REQUIRE(zones.size() == 2);
    CHECK(controller.patchEditor().fixtures().size() == saved.preset.fixtures.size());
    // An Art-Net output to Liberation, on — and Link, on.
    const auto& outputs = saved.preset.outputs;
    const auto artNet = [&outputs, &node] {
        return std::count_if(outputs.begin(), outputs.end(), [&node](const auto& target) {
            return target.kind == takt4::output::OutputTarget::Kind::ArtNet &&
                   target.host == "127.0.0.1" && target.port == node.port();
        });
    };
    REQUIRE(artNet() == 1);
    for (const takt4::output::OutputTarget& target : outputs) {
        if (target.kind == takt4::output::OutputTarget::Kind::ArtNet) {
            CHECK(target.enabled);
            CHECK(target.name == "Liberation");
        }
    }
    REQUIRE_FALSE(outputs.empty());
    CHECK(outputs.front().kind == takt4::output::OutputTarget::Kind::Link);
    CHECK(outputs.front().enabled);
    // The rules, aimed at the zones.
    REQUIRE(saved.preset.rules.size() == 2);
    CHECK(saved.preset.rules[0].dmx.fixtures == std::vector<std::string>{zones[0]});
    CHECK(saved.preset.rules[1].dmx.fixtures == std::vector<std::string>{zones[1]});

    // And the zones reach the node: both disarmed until a clip fires.
    bool heard = false;
    for (int attempt = 0; attempt < 200 && !heard; ++attempt) {
        const std::string datagram = node.receive();
        if (datagram.size() >= 18 + 64 &&
            datagram.compare(0, 8, std::string("Art-Net\0", 8)) == 0) {
            heard = static_cast<std::uint8_t>(datagram[18]) == 0 &&
                    static_cast<std::uint8_t>(datagram[18 + 32]) == 0 &&
                    static_cast<std::uint8_t>(datagram[18 + 9]) == 255; // laser 1's scale
        }
    }
    CHECK(heard);

    // Asked again for the same Liberation, it reuses the output and the zones.
    editor.openLiberation();
    editor.addLiberation();
    const takt4::settings::Settings again = controller.currentSettings();
    CHECK(std::count_if(again.preset.outputs.begin(), again.preset.outputs.end(),
                        [&node](const auto& target) {
                            return target.kind == takt4::output::OutputTarget::Kind::ArtNet &&
                                   target.port == node.port();
                        }) == 1);
    CHECK(again.preset.fixtures.size() == saved.preset.fixtures.size());
    CHECK(again.preset.rules.size() == 4);
}

TEST_CASE("a name typed into an output and left for its delay handle is applied", "[ui]") {
    // A box commits a turn after it loses the keyboard, and only what differs from its row. A
    // press on the delay's track takes the keyboard and moves the delay in the same event, and the
    // move wrote the rows — the name being typed with them — so the name box, let go of a turn
    // later, found its row already holding what it held and committed nothing: the name was never
    // applied. Driven as an operator does it: a click in the name box, a letter, a click on the
    // track beside the handle.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver added; // the new row's, not this machine's 9000
    controller.setNewOutputPort(added.port());
    controller.addTarget();
    constexpr int kSize = 1000;
    layOut(controller, static_cast<float>(kSize), static_cast<float>(kSize));
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const takt4::tests::NothingReal nothingReal;
    const std::vector<float> rows = outputRowsAt(controller, static_cast<float>(kSize));
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[1] > 0.0f);
    const float y = rows[1];
    REQUIRE(seen(controller).targets[1].name != "a");

    // The handle, on a picture of the window as it is.
    const takt4::tests::Shot shot = takt4::tests::render(controller.window(), kSize, kSize);
    float thumb = -1.0f;
    for (int x = 400; x < kSize - 20 && thumb < 0.0f; ++x) {
        if (is(shot, x, static_cast<int>(y), kThumb)) {
            thumb = static_cast<float>(x) + 9.0f;
        }
    }
    INFO("the delay handle at x=" << thumb << ", y=" << y);
    REQUIRE(thumb > 0.0f);

    clickAt(window, 110.0f, y);
    press(window, "a"); // the box selects what it holds when clicked into
    settle();
    // Off the handle, so the press itself moves the delay — no turn of the loop between the
    // keyboard leaving the box and the rows being written.
    clickAt(window, thumb + 40.0f, y);
    settle();
    settle();

    const takt4::output::OutputTarget after = seen(controller).targets[1];
    INFO("name '" << after.name << "', delay " << after.delaySeconds);
    CHECK(after.delaySeconds > 0.0); // the drag reached the output
    CHECK(after.name == "a");
    nothingReal.check();
}

TEST_CASE("closing the main window applies what is being typed, the OSC control port's included",
          "[ui]") {
    // The settings are written once the event loop has returned, and a click anywhere finishes a
    // box — but closing the window clicked nothing: a name typed into an output and the window
    // closed on it was not in them, and neither was a port typed into OSC control's box, which
    // only Enter or a click away committed.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::testing::LoopbackReceiver added;
    controller.setNewOutputPort(added.port());
    controller.addTarget();
    // A keystroke in each, as the boxes say them, and nothing committed.
    controller.window().invoke_output_name_edited(1, slint::SharedString("deck"));
    controller.window().invoke_osc_control_port_typed(slint::SharedString("7044"));
    REQUIRE(controller.currentSettings().preset.outputs.size() == 2);
    REQUIRE(controller.currentSettings().preset.outputs[1].name != "deck");
    REQUIRE(controller.currentSettings().machine.oscControlPort != 7044);

    // The real gesture, dispatched into the window.
    controller.window().window().dispatch_close_requested_event();
    const takt4::settings::Settings saved = controller.currentSettings();
    CHECK(saved.preset.outputs[1].name == "deck");
    CHECK(saved.machine.oscControlPort == 7044);
    CHECK(controller.oscControlPort() == 0); // a number, not a socket: listening was off
}

TEST_CASE("an import with other meters puts them in between runs, and says so under one",
          "[ui][settings]") {
    // The forward filter is built for its bars. An import that carried others kept them for the
    // next launch, and before that said nothing, so the bars went on being counted in the old
    // ones with nothing on screen to say why. Now they go in at once while stopped, and under a
    // run as it ends — said then, since until START the old ones go on.
    using Meters = std::array<std::uint8_t, 4>;
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::test::TempDir dir;
    const Meters four = tracker.engine().meters();
    REQUIRE(four == Meters{4, 0, 0, 0});
    const auto fileWith = [&](const Meters& meters, const char* name) {
        takt4::settings::Settings settings = controller.currentSettings();
        settings.preset.meters = meters;
        const std::filesystem::path file = dir.path() / name;
        REQUIRE(takt4::settings::save(settings, file));
        return file;
    };

    // Stopped: in place, and nothing to say about it.
    REQUIRE(controller.importFrom(fileWith(Meters{3, 4, 0, 0}, "waltz.json")));
    {
        const std::string said(controller.window().get_status());
        INFO(said);
        CHECK(said.find("Restart") == std::string::npos);
        CHECK(said.find("Beats per bar") == std::string::npos);
    }
    CHECK(tracker.engine().meters() == Meters{3, 4, 0, 0});
    CHECK(controller.currentSettings().preset.meters == Meters{3, 4, 0, 0});
    CHECK(controller.window().get_meter_index() == 1);

    // Under a run — its engine started, as a START starts it — they wait for its end, and it
    // says when they come.
    tracker.engine().start();
    REQUIRE(controller.importFrom(fileWith(four, "four.json")));
    {
        const std::string said(controller.window().get_status());
        INFO(said);
        CHECK(said.find("Beats per bar change at the next START.") != std::string::npos);
    }
    CHECK(tracker.engine().meters() == Meters{3, 4, 0, 0});
    CHECK(controller.currentSettings().preset.meters == four);
    CHECK(controller.window().get_meter_index() == 0);
    tracker.stop();
    CHECK(tracker.engine().meters() == four);

    // And a file with the bars already in place says nothing about them, running or not.
    tracker.engine().start();
    REQUIRE(controller.importFrom(fileWith(four, "four-again.json")));
    {
        const std::string said(controller.window().get_status());
        INFO(said);
        CHECK(said.find("Beats per bar") == std::string::npos);
    }
    tracker.stop();
}

TEST_CASE("beats per bar offers four, three or four and three, and a file's own list as itself",
          "[ui][settings]") {
    // A set with waltzes used to mean editing the settings file. The picker offers what a set
    // needs, and a file's list that is none of them is shown as what it is rather than as one of
    // the three it is not.
    using Meters = std::array<std::uint8_t, 4>;
    LiveTracker tracker(kWeights, kStateSpace);
    const auto choices = [](WindowController& controller) {
        std::vector<std::string> out;
        const auto model = controller.window().get_meter_choices();
        for (std::size_t i = 0; i < model->row_count(); ++i) {
            out.emplace_back(*model->row_data(i));
        }
        return out;
    };
    {
        WindowController controller(tracker);
        CHECK(choices(controller) == std::vector<std::string>{"4", "3 or 4", "3"});
        CHECK(controller.window().get_meter_index() == 0);
        CHECK(controller.window().get_meter_offered());

        controller.window().invoke_meter_picked(1);
        CHECK(tracker.engine().meters() == Meters{3, 4, 0, 0});
        CHECK(controller.window().get_meter_index() == 1);
        CHECK(controller.currentSettings().preset.meters == Meters{3, 4, 0, 0});
        controller.window().invoke_meter_picked(2);
        CHECK(tracker.engine().meters() == Meters{3, 0, 0, 0});
        CHECK(controller.window().get_meter_index() == 2);
        // Past the end of the list, as nothing on screen can send: nothing changes.
        controller.window().invoke_meter_picked(7);
        controller.window().invoke_meter_picked(-1);
        CHECK(tracker.engine().meters() == Meters{3, 0, 0, 0});
        CHECK(controller.window().get_meter_index() == 2);
        controller.window().invoke_meter_picked(0);
        CHECK(tracker.engine().meters() == Meters{4, 0, 0, 0});
    }
    {
        takt4::settings::Settings settings;
        settings.preset.meters = {2, 3, 4, 0};
        WindowController controller(tracker, settings);
        CHECK(choices(controller) == std::vector<std::string>{"4", "3 or 4", "3", "2, 3 or 4"});
        CHECK(controller.window().get_meter_index() == 3);
        // The window was built with the file's bars, as the application's engine is.
        CHECK(tracker.engine().meters() == Meters{2, 3, 4, 0});
        // Picking it again is picking what is in force.
        controller.window().invoke_meter_picked(3);
        CHECK(tracker.engine().meters() == Meters{2, 3, 4, 0});
        CHECK(controller.window().get_meter_index() == 3);
        // Another, and the file's own leaves the list: it is no longer anything to go back to.
        controller.window().invoke_meter_picked(1);
        CHECK(tracker.engine().meters() == Meters{3, 4, 0, 0});
        CHECK(choices(controller) == std::vector<std::string>{"4", "3 or 4", "3"});
        CHECK(controller.window().get_meter_index() == 1);
    }
}

TEST_CASE("beats per bar is not offered where the tracker keeps its own bars", "[ui][settings]") {
    // The particle filter, picked in a settings file, models its bars from its own state space:
    // a box that changed nothing is a box that lies.
    LiveTracker::Options options;
    options.engine.decoder = takt4::tracking::Decoder::ParticleFilter;
    LiveTracker tracker(kWeights, kStateSpace, options);
    WindowController controller(tracker);
    CHECK_FALSE(controller.window().get_meter_offered());
    const auto before = tracker.engine().meters();
    controller.window().invoke_meter_picked(1);
    CHECK(tracker.engine().meters() == before);
}

TEST_CASE("beats per bar is picked by clicking it, and not while a run holds it locked",
          "[ui][settings]") {
    // The box itself, clicked where the picture puts it and picked from with the keys, as the
    // timing list is: a pick the controller takes proves the controller, not the box.
    using Meters = std::array<std::uint8_t, 4>;
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    if (controller.window().get_inputs_folded()) {
        controller.toggleFold(0);
    }
    controller.tick();
    auto& window = controller.window().window();
    const int height = 1400;
    takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, height);
    window.dispatch_window_active_changed_event(true);
    const float top = controller.window().get_timing_row_y();
    REQUIRE(top > 0.0f);
    // The boxes in the timing row by their top edges, which no word reaches: the source's list
    // and then beats per bar.
    const int edge = static_cast<int>(top) + 2;
    const auto boxes = occupied(shot, edge, edge + 3, 90, 790, kSheet, 5);
    INFO("box edges along the timing row: " << spans(boxes));
    REQUIRE(boxes.size() >= 2);
    const float x = middleOf(boxes[1]);
    const float y = top + 17.0f;
    CHECK(boxes[1].second - boxes[1].first + 1 >= 80);
    CHECK(boxes[1].second - boxes[1].first + 1 <= 120);

    clickAt(window, x, y);
    slint::platform::update_timers_and_animations();
    press(window, kDown);
    press(window, "\n");
    slint::platform::update_timers_and_animations();
    controller.tick();
    CHECK(tracker.engine().meters() == Meters{3, 4, 0, 0});
    CHECK(controller.window().get_meter_index() == 1);

    // Running, the box is the run's and opens to nothing: the same click and keys pick nothing.
    controller.window().set_running(true);
    shot = takt4::tests::render(controller.window(), 800, height);
    clickAt(window, x, y);
    slint::platform::update_timers_and_animations();
    press(window, kDown);
    press(window, "\n");
    slint::platform::update_timers_and_animations();
    CHECK(tracker.engine().meters() == Meters{3, 4, 0, 0});
    CHECK(controller.window().get_meter_index() == 1);
}

TEST_CASE("the Liberation preset says it drops a half-done fixture import before it does",
          "[ui][liberation]") {
    // ADD lays a patch, and a new patch drops a fixture import half done in the patch editor —
    // which it did without a word. It says so first, adds nothing, and ADD again goes on.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::ui::RulesController& editor = controller.editor();
    takt4::ui::FixturesController& patch = controller.patchEditor();
    const takt4::testing::LoopbackReceiver node;
    const auto ask = [&] {
        editor.openLiberation();
        editor.setLaserCount(1);
        editor.setLiberationHost("127.0.0.1");
        editor.setLiberationPort(static_cast<int>(node.port()));
        editor.setLiberationUniverse(1);
        editor.setLiberationAddress(1);
        REQUIRE(std::string(editor.window().get_liberation_problem()).empty());
    };

    SECTION("with an import half done") {
        patch.openImport();
        REQUIRE(patch.importing());
        ask();
        editor.addLiberation();
        const std::string warning(editor.window().get_liberation_warning());
        INFO(warning);
        CHECK(warning.find("fixture import") != std::string::npos);
        CHECK(std::string(editor.window().get_liberation_problem()).empty()); // ADD still works
        CHECK(editor.liberationOpen());
        CHECK(patch.importing());
        CHECK(editor.rules().empty());
        CHECK(patch.fixtures().empty());

        // Closed and opened again, it says it again: what was said was said to that prompt.
        editor.closeLiberation();
        ask();
        CHECK(std::string(editor.window().get_liberation_warning()).empty());
        editor.addLiberation();
        CHECK(editor.liberationOpen());
        CHECK(patch.importing());

        editor.addLiberation();
        CHECK_FALSE(editor.liberationOpen());
        CHECK_FALSE(patch.importing());
        CHECK(editor.rules().size() == 1);
        CHECK(patch.fixtures().size() == 1);
    }

    SECTION("with none, it adds at once") {
        ask();
        editor.addLiberation();
        CHECK_FALSE(editor.liberationOpen());
        CHECK(editor.rules().size() == 1);
        CHECK(patch.fixtures().size() == 1);
    }
}

TEST_CASE("an import naming an input this machine has not got, while stopped, keeps it asked for "
          "and says nothing of listening",
          "[ui][settings]") {
    // The import's way through the input list, on whatever this machine has: on a runner with no
    // audio the list is empty, which is where a list's first entry has been read before now.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::test::TempDir dir;
    takt4::settings::Settings other = controller.currentSettings();
    other.machine.deviceName = "An interface that is switched off";
    other.machine.hostApiName = "ASIO";
    const std::filesystem::path file = dir.path() / "other.json";
    REQUIRE(takt4::settings::save(other, file));
    REQUIRE(controller.importFrom(file));

    CHECK_FALSE(tracker.running());
    const std::string said(controller.window().get_status());
    INFO(said);
    CHECK(said.rfind("Imported other.json", 0) == 0);
    CHECK(said.find("listening") == std::string::npos);
    // Still the one wanted, for RESCAN and the next launch to find.
    CHECK(controller.currentSettings().machine.deviceName == "An interface that is switched off");
}

// --- the timing input --------------------------------------------------------------------------

namespace {

/// Link's clock as the window reads it, in microseconds — what a followed beat is stamped on.
std::int64_t linkNow(const WindowController& controller) {
    return controller.outputs().transports().link().now().count();
}

/// The main window laid out at 800 x 1480, nothing scrolled, Inputs open — and the sheets down its
/// left edge on a render of it.
std::vector<std::pair<int, int>> laidOutWithInputs(WindowController& controller) {
    controller.window().set_inputs_folded(false);
    layOut(controller, 800.0f, 1480.0f);
    controller.tick();
    slint::platform::update_timers_and_animations();
    const takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, 1480);
    controller.window().window().dispatch_window_active_changed_event(true);
    return sheetsDown(shot);
}

/// What the row `dy` below the inputs sheet's top holds, on a render of the window as it is now.
std::vector<std::pair<int, int>> inputsRow(WindowController& controller, int dy, int& y) {
    const takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, 1480);
    controller.window().window().dispatch_window_active_changed_event(true);
    const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
    REQUIRE(sheets.size() == 7);
    y = sheets[4].first + dy;
    return occupied(shot, y - 17, y + 16, 20, 780, kSheet, 5);
}

} // namespace

TEST_CASE("where the beats come from is picked with the pointer, and the window follows it",
          "[ui][follow]") {
    // The timing row's picker, opened by a click and stepped with the keys as an operator would —
    // and what the pick does: the audio pickers give way to the banner, START waits for a port,
    // and the transports stop any MIDI clock out to the clock's own device.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    REQUIRE(laidOutWithInputs(controller).size() == 7);
    REQUIRE(controller.timingIndex() == 0);
    const takt4::tests::NothingReal nothingReal;

    int y = 0;
    const auto audio = inputsRow(controller, 73, y);
    INFO("timing row: " << spans(audio));
    // timing, from, the picker, beats per bar and its box, and the words saying the audio input is
    // picked at the top.
    REQUIRE(audio.size() == 6);
    clickAt(window, middleOf(audio[2]), static_cast<float>(y));
    slint::platform::update_timers_and_animations();
    press(window, kDown);
    press(window, kDown);
    press(window, "\n");
    settle();
    CHECK(controller.timingIndex() == 2);
    CHECK(controller.window().get_following());
    CHECK(std::string(controller.window().get_timing_name()) == "MIDI clock");
    // No port yet: START has nothing to start.
    CHECK_FALSE(controller.window().get_timing_ready());

    // The banner, in the audio pickers' place: one box, then RESCAN and START.
    {
        const takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, 1480);
        const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
        REQUIRE(sheets.size() == 7);
        const int topRow = sheets[0].first + 48;
        const auto top = occupied(shot, topRow - 20, topRow + 20, 20, 780, kSheet, 5);
        INFO("top bar, following: " << spans(top));
        CHECK(top.size() == 3);
    }

    // And back, by the same picker, two steps up: the audio pickers come back.
    const auto following = inputsRow(controller, 73, y);
    INFO("timing row, following: " << spans(following));
    // timing, from, the picker, port, the port's picker — and, no input picked for the clock,
    // what START needs.
    REQUIRE(following.size() == 6);
    CHECK(std::string(controller.window().get_timing_reading()) == "pick the clock's input");
    clickAt(window, middleOf(following[2]), static_cast<float>(y));
    slint::platform::update_timers_and_animations();
    press(window, kUp);
    press(window, kUp);
    press(window, "\n");
    settle();
    CHECK(controller.timingIndex() == 0);
    CHECK_FALSE(controller.window().get_following());
    {
        const takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, 1480);
        const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
        const int topRow = sheets[0].first + 48;
        const auto top = occupied(shot, topRow - 20, topRow + 20, 20, 780, kSheet, 5);
        INFO("top bar, back on the audio: " << spans(top));
        CHECK(top.size() == 6);
    }
    // Nothing was opened, listened to or joined: a MIDI clock opens its port at START.
    nothingReal.check();
}

TEST_CASE("picking Link hands the session to the follower at once, and picking again gives it back",
          "[ui][follow][link]") {
    // A session of takt4's own, switched on and alone while the operator sets up, would be the
    // older one when the DJ's app arrives — and the DJ's app would take takt4's tempo. So Link is
    // handed over the moment it is picked, not at START; the Link output's switch is kept.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.preset.link = true;
    WindowController controller(tracker, saved);
    const auto linkState = [&controller] {
        REQUIRE(controller.settleOutputs());
        return controller.outputs().inspect([](const takt4::trigger::TriggerEngine&,
                                               const takt4::output::Transports& transports,
                                               const takt4::output::RuleSink&) {
            return std::make_pair(transports.linkFollowing(), transports.linkEnabled());
        });
    };
    CHECK(linkState() == std::make_pair(false, true));
    controller.pickTiming(1);
    CHECK(linkState() == std::make_pair(true, true));
    CHECK(std::string(controller.window().get_timing_name()) == "Ableton Link");
    controller.pickTiming(0);
    CHECK(linkState() == std::make_pair(false, true));
    CHECK_FALSE(controller.window().get_following());
}

TEST_CASE("following, START runs on the source: its beats reach the plot and the outputs, tap and "
          "lock do nothing, and STOP lets go",
          "[ui][follow]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    // A source in place of the Link session, which the sandbox will not join: four beats a second
    // on Link's own clock, its bar starting a tenth of a second from now.
    takt4::testing::FakeGridSource source([&controller] { return linkNow(controller); }, 240.0,
                                          static_cast<double>(linkNow(controller)) + 100'000.0);
    controller.followForTests(&source);
    controller.pickTiming(1);
    std::vector<std::pair<int, int>> sheets = laidOutWithInputs(controller);
    REQUIRE(sheets.size() == 7);

    // START, the last thing in the top bar.
    takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, 1480);
    window.dispatch_window_active_changed_event(true);
    const int topRow = sheets[0].first + 48;
    const auto top = occupied(shot, topRow - 20, topRow + 20, 20, 780, kSheet, 5);
    INFO("top bar: " << spans(top));
    REQUIRE(top.size() == 3);
    clickAt(window, middleOf(top.back()), static_cast<float>(topRow));
    waitUntil(controller, [&] { return controller.followed().running(); });
    REQUIRE(controller.followed().running());
    CHECK(controller.window().get_running());
    CHECK_FALSE(tracker.running()); // the audio is not opened at all
    CHECK(source.begun() == 1);

    // A second and a half of it: the beats on the plot, a bar's first in red, and at the outputs.
    waitUntil(controller, [&] { return controller.followed().beatsSent() >= 6; });
    settle();
    const auto trace = controller.window().get_trace();
    int beatsDrawn = 0;
    int downbeatsDrawn = 0;
    bool inBeat = false;
    for (std::size_t i = 0; i < trace->row_count(); ++i) {
        const TracePoint point = *trace->row_data(i);
        if (point.called && !inBeat) {
            ++beatsDrawn;
            downbeatsDrawn += point.downbeat > 0.5f ? 1 : 0;
        }
        inBeat = point.called;
    }
    CHECK(beatsDrawn >= 5);
    CHECK(downbeatsDrawn >= 1);
    CHECK(controller.window().get_bpm() == Catch::Approx(240.0).margin(0.01));
    CHECK(controller.window().get_acquired());
    CHECK(controller.window().get_strip_beat() >= 1);
    CHECK(std::string(controller.window().get_timing_reading()).find("240.00 BPM") !=
          std::string::npos);
    waitUntil(controller, [&] { return controller.window().get_beats_sent() > 0; });
    CHECK(controller.window().get_beats_sent() > 0);

    // The controls: ÷2 halves the followed grid; tap and lock are switched off — clicked, and T
    // from the keyboard, they send nothing at all.
    shot = takt4::tests::render(controller.window(), 800, 1480);
    window.dispatch_window_active_changed_event(true);
    sheets = sheetsDown(shot);
    const int buttonsRow = sheets[3].first + 26;
    const auto buttons = occupied(shot, buttonsRow - 16, buttonsRow + 15, 20, 780, kSheet, 5);
    INFO("controls: " << spans(buttons));
    // ÷2, ×2, tap, lock, downbeat, and the words saying tap and lock are off.
    REQUIRE(buttons.size() == 6);
    // T from the keyboard first, on its own: a tap clicked a moment before would swallow it as a
    // bounce, and say nothing about the key.
    press(window, "t");
    settle();
    CHECK(controller.taps() == 0);
    clickAt(window, middleOf(buttons[2]), static_cast<float>(buttonsRow));
    clickAt(window, middleOf(buttons[3]), static_cast<float>(buttonsRow));
    settle();
    CHECK(controller.taps() == 0);
    CHECK(controller.followed().commandsRefused() == 0); // nothing was even asked of it
    clickAt(window, middleOf(buttons[0]), static_cast<float>(buttonsRow));
    waitUntil(controller, [&] { return controller.followed().state().octaveShift == -1; });
    CHECK(controller.followed().state().octaveShift == -1);
    CHECK(controller.followed().state().bpm == Catch::Approx(120.0));

    // STOP: the follower let go of, the run over.
    clickAt(window, middleOf(top.back()), static_cast<float>(topRow));
    waitUntil(controller, [&] { return !controller.followed().running(); });
    CHECK_FALSE(controller.followed().running());
    settle();
    CHECK_FALSE(controller.window().get_running());
    CHECK(source.ended() == 1);
    controller.followForTests(nullptr);
}

TEST_CASE("the beats-per-bar box takes a number when it is clicked away from, and the run counts "
          "in it",
          "[ui][follow]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window().window();
    takt4::testing::FakeGridSource source([&controller] { return linkNow(controller); }, 240.0,
                                          static_cast<double>(linkNow(controller)));
    controller.followForTests(&source);
    controller.pickTiming(2);
    REQUIRE(laidOutWithInputs(controller).size() == 7);
    const takt4::tests::NothingReal nothingReal;

    int y = 0;
    const auto how = inputsRow(controller, 73 + 44, y);
    INFO("the row under the timing row: " << spans(how));
    // "beats per bar", its box, the obey box and its words, and the hint about it at the right.
    REQUIRE(how.size() == 5);
    clickAt(window, middleOf(how[1]), static_cast<float>(y));
    takt4::tests::endOfText(window);
    press(window, "\x08");
    press(window, "3");
    // Clicked away from — on the window's own background, at the right of the inputs heading
    // row's empty middle — not entered.
    clickAt(window, 400.0f, static_cast<float>(y - 73 - 44 + 14));
    controller.tick();
    slint::platform::update_timers_and_animations();
    CHECK(controller.window().get_timing_beats_per_bar() == 3);
    CHECK(controller.currentSettings().preset.externalBeatsPerBar == 3);

    controller.toggleRun();
    REQUIRE(controller.followed().running());
    waitUntil(controller, [&] { return controller.followed().beatsSent() >= 4; });
    CHECK(controller.followed().state().beatsPerBar == 3);
    CHECK(source.beatsPerBar() == 3);
    // Fixed for the run: the box is not live while it runs.
    controller.setExternalBeatsPerBar(5);
    CHECK(controller.window().get_timing_beats_per_bar() == 3);
    controller.toggleRun();
    CHECK_FALSE(controller.followed().running());
    controller.followForTests(nullptr);
    nothingReal.check();
}

TEST_CASE("an import while a source is followed leaves the run alone, and its bar is the next "
          "START's",
          "[ui][follow]") {
    // A file with the source being followed and another MIDI clock
    // input was applied as if stopped — the Link listener or the CDJs' network opened on this
    // thread while the timing worker had it — and its beats to the bar went on screen while the
    // run went on counting in its own.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::testing::FakeGridSource source([&controller] { return linkNow(controller); }, 240.0,
                                          static_cast<double>(linkNow(controller)));
    controller.followForTests(&source);
    controller.pickTiming(1);
    controller.toggleRun();
    REQUIRE(controller.followed().running());
    waitUntil(controller, [&] { return controller.followed().beatsSent() >= 2; });

    const takt4::test::TempDir dir;
    takt4::settings::Settings file = controller.currentSettings();
    file.machine.midiClockInPort = "takt4 test - another clock";
    file.preset.externalBeatsPerBar = 3;
    const std::filesystem::path path = dir.path() / "rig.json";
    REQUIRE(takt4::settings::save(file, path));
    // The Link listener is the timing worker's while it runs: the import does not so much as try
    // to open it (which the sandbox would refuse, and count).
    const std::uint64_t listens = takt4::sandbox::refusals(takt4::sandbox::Refused::Link);
    REQUIRE(controller.importFrom(path));
    controller.tick();
    CHECK(takt4::sandbox::refusals(takt4::sandbox::Refused::Link) == listens);
    // Still following, the same run, counting in four.
    CHECK(controller.followed().running());
    CHECK(source.begun() == 1);
    CHECK(source.ended() == 0);
    CHECK(controller.followed().state().beatsPerBar == 4);
    CHECK(controller.window().get_timing_beats_per_bar() == 4);
    CHECK(std::string(controller.window().get_timing_note()) ==
          "3 beats to the bar from the next START");
    // And what the file asked for kept, for the next START and the next save.
    CHECK(controller.currentSettings().preset.externalBeatsPerBar == 3);
    CHECK(controller.currentSettings().machine.midiClockInPort == "takt4 test - another clock");
    controller.toggleRun();
    controller.toggleRun();
    REQUIRE(controller.followed().running());
    waitUntil(controller, [&] { return controller.followed().beatsSent() >= 2; });
    CHECK(controller.followed().state().beatsPerBar == 3);
    controller.tick();
    CHECK(std::string(controller.window().get_timing_note()).empty());
    controller.toggleRun();
    controller.followForTests(nullptr);
}

TEST_CASE("where the beats come from, and how, is saved and comes back picked and not started",
          "[ui][follow][settings]") {
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    {
        WindowController controller(tracker);
        controller.pickTiming(2);
        controller.setClockPort("DJM-A9 1");
        controller.setClockObey(true);
        controller.setExternalBeatsPerBar(7);
        // The slider, following, is the MIDI clock's own latency — beat detection's is left alone.
        const double detection = controller.settings().latencyOffsetSeconds;
        controller.setLatencyMs(-12.0);
        CHECK(controller.window().get_latency_ms() == Catch::Approx(-12.0f));
        CHECK(controller.outputs().transports().latencySeconds() == Catch::Approx(-0.012));
        CHECK(controller.settings().latencyOffsetSeconds == Catch::Approx(detection));
        // Back on the audio, the slider and the outputs have beat detection's again.
        controller.pickTiming(0);
        CHECK(controller.outputs().transports().latencySeconds() == Catch::Approx(detection));
        CHECK(controller.window().get_latency_ms() == Catch::Approx(detection * 1000.0));
        controller.pickTiming(2);
        CHECK(controller.window().get_latency_ms() == Catch::Approx(-12.0f));
        saved = controller.currentSettings();
    }
    CHECK(saved.machine.timingSource == "midi-clock");
    CHECK(saved.machine.midiClockInPort == "DJM-A9 1");
    CHECK(saved.machine.midiClockObeyStartStop);
    CHECK(saved.preset.externalBeatsPerBar == 7);
    CHECK(saved.preset.timingLatency.midiClock == Catch::Approx(-0.012));
    CHECK(saved.preset.timingLatency.link == Catch::Approx(0.0));

    // Through the file's text, and into a window opened from it.
    const takt4::settings::Settings loaded =
        takt4::settings::fromJson(takt4::settings::toJson(saved));
    WindowController again(tracker, loaded);
    CHECK(again.timingIndex() == 2);
    CHECK(again.window().get_following());
    CHECK(again.window().get_clock_obey());
    CHECK(again.window().get_timing_beats_per_bar() == 7);
    CHECK(again.window().get_latency_ms() == Catch::Approx(-12.0f));
    // The port, not on this machine, still the one picked — and said to be missing.
    const int picked = again.window().get_clock_in_port_index();
    const auto ports = again.window().get_clock_in_ports();
    REQUIRE(picked > 0);
    REQUIRE(static_cast<std::size_t>(picked) < ports->row_count());
    CHECK(std::string(*ports->row_data(static_cast<std::size_t>(picked))) ==
          "DJM-A9 1 \xE2\x80\x94 not plugged in");
    // Picked, and not started: START is pressed as ever.
    CHECK_FALSE(again.followed().running());
    CHECK_FALSE(again.window().get_running());
}

TEST_CASE("a MIDI clock output to the device the clock is followed from says on its row that it "
          "is not sent",
          "[ui][follow][midi]") {
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    takt4::output::OutputTarget back;
    back.id = "o-d1a9";
    back.name = "DJM clock";
    back.kind = takt4::output::OutputTarget::Kind::MidiClock;
    back.device = "DJM-A9 2";
    saved.preset.outputs = {back};
    saved.machine.timingSource = "midi-clock";
    saved.machine.midiClockInPort = "DJM-A9 1";
    WindowController controller(tracker, saved);
    controller.tick();
    std::optional<OutputRow> row = outputRow(controller, "o-d1a9");
    REQUIRE(row);
    CHECK(std::string(row->problem) ==
          "not sent \xE2\x80\x94 the MIDI clock is followed from this device, and could come "
          "straight back round");
    // Followed from somewhere else, the row is only a device that is not on this machine.
    controller.setClockPort("another mixer");
    REQUIRE(controller.settleOutputs());
    controller.tick();
    row = outputRow(controller, "o-d1a9");
    REQUIRE(row);
    CHECK(std::string(row->problem).find("followed from this device") == std::string::npos);
}

TEST_CASE("what the window says about the source followed, in each state it can be in",
          "[ui][follow]") {
    // `describeTiming` is what the window and takt4-shot both say, so the words are pinned here.
    using takt4::ui::describeTiming;
    using takt4::ui::TimingFacts;
    TimingFacts facts;
    // Beat detection: nothing to say — the tracker's readouts say it all.
    CHECK(describeTiming(facts).name.empty());
    CHECK(describeTiming(facts).reading.empty());

    // Link, stopped: heard, not joined.
    facts.index = 1;
    facts.peersHeard = 3;
    facts.heardBpm = 127.98;
    CHECK(describeTiming(facts).name == "Ableton Link");
    CHECK(describeTiming(facts).shortName == "Link");
    CHECK(describeTiming(facts).reading == "3 peers seen \xC2\xB7 127.98 BPM");
    facts.peersHeard = 0;
    CHECK(describeTiming(facts).reading == "no Link session heard");
    facts.canListen = false;
    CHECK(describeTiming(facts).reading == "cannot listen for Link sessions on this network");
    CHECK_FALSE(describeTiming(facts).waiting); // stopped is not waiting

    // Link, running: waiting for a session, then following it.
    facts.running = true;
    facts.canListen = true;
    takt4::ui::TimingView view = describeTiming(facts);
    CHECK(view.waiting);
    CHECK(view.reading == "no Link session heard \xE2\x80\x94 waiting for one");
    facts.state.locked = true;
    facts.state.rawBpm = 128.0;
    facts.state.beatInBar = 3;
    facts.peers = 1;
    view = describeTiming(facts);
    CHECK_FALSE(view.waiting);
    CHECK(view.reading == "1 peer \xC2\xB7 128.00 BPM");
    CHECK(view.detail == "1 peer \xC2\xB7 4 beats to the bar");
    CHECK(view.stripBeat == 3);

    // A MIDI clock, running: ticking with no Start, started, coasting, stopped, gone.
    facts = TimingFacts{};
    facts.index = 2;
    facts.running = true;
    facts.port = "DJM-A9";
    facts.ticking = true;
    view = describeTiming(facts);
    CHECK(view.waiting);
    CHECK(view.reading == "ticking \xE2\x80\x94 measuring the tempo");
    facts.state.locked = true;
    facts.state.rawBpm = 127.9;
    facts.tickInBeat = 13;
    view = describeTiming(facts);
    CHECK(view.reading == "ticking \xC2\xB7 127.90 BPM");
    CHECK(view.detail == "DJM-A9 \xC2\xB7 no start yet");
    CHECK(view.note == "no start received \xE2\x80\x94 press downbeat on the one");
    CHECK(view.stripTicks == 14);
    facts.placed = true;
    view = describeTiming(facts);
    CHECK(view.detail == "DJM-A9 \xC2\xB7 started");
    CHECK(view.note.empty());
    facts.coasting = true;
    facts.ticking = false;
    view = describeTiming(facts);
    CHECK(view.coasting);
    CHECK_FALSE(view.waiting);
    CHECK(view.reading == "no clock \xE2\x80\x94 coasting at 127.90 BPM");
    CHECK(view.stripTicks == 0);
    facts.coasting = false;
    // An obeyed Stop lets go of the lock at once (`BeatGrid::goQuiet`).
    facts.state.locked = false;
    facts.stopObeyed = true;
    CHECK(describeTiming(facts).reading == "stopped \xE2\x80\x94 waiting for Start");
    facts.stopObeyed = false;
    CHECK(describeTiming(facts).reading == "no clock \xE2\x80\x94 waiting for one");
    // Its device pulled out: not "waiting for one", which nothing
    // would ever bring — said not to be plugged in, whatever else the clock was doing.
    facts.portLost = true;
    view = describeTiming(facts);
    CHECK(view.reading == "NOT OPEN \xE2\x80\x94 DJM-A9 is not plugged in");
    CHECK(view.detail == "DJM-A9 \xC2\xB7 not plugged in");
    facts.state.locked = true;
    facts.coasting = true;
    CHECK(describeTiming(facts).reading == "NOT OPEN \xE2\x80\x94 DJM-A9 is not plugged in");

    // An import while running sets the bar for the next START, and the run goes on in its own:
    // the box shows the run's, and says the next.
    facts = TimingFacts{};
    facts.index = 1;
    facts.running = true;
    facts.state.locked = true;
    facts.state.rawBpm = 128.0;
    facts.state.beatInBar = 2;
    facts.peers = 2;
    facts.beatsPerBar = 3;
    facts.runBeatsPerBar = 4;
    view = describeTiming(facts);
    CHECK(view.beatsPerBar == 4);
    CHECK(view.note == "3 beats to the bar from the next START");
    CHECK(view.detail == "2 peers \xC2\xB7 4 beats to the bar");
    CHECK(view.stripLabel == "beat 2 of 4");
    facts.runBeatsPerBar = 3;
    CHECK(describeTiming(facts).note.empty());
}

TEST_CASE("a MIDI control input pulled out says so on its line, and is heard again when it is back",
          "[ui]") {
    // A pad's cable knocked out mid-set went on showing its port and
    // its bindings, and PANIC from it had stopped arriving with no sign; plugged back in, it was
    // never heard again. The machine's list is looked at every few seconds; here it is handed in.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.midiInputs().attachWithoutPort("Pad 1", controller.control());
    controller.control().listenWithoutPort();
    controller.tick();
    REQUIRE_FALSE(controller.window().get_control_error());

    controller.recheckMidiInputs({"MOTU Pro Audio 0"});
    controller.tick();
    CHECK(controller.window().get_control_error());
    CHECK(std::string(controller.window().get_control_reading()) ==
          "NOT OPEN \xE2\x80\x94 Pad 1 is not plugged in. Listening again as soon as it is.");
    CHECK(std::string(controller.window().get_status())
              .find("MIDI input Pad 1 is not plugged in any more") != std::string::npos);
    CHECK(controller.statusIsError());

    // Back, under another number now that Windows has counted its inputs again.
    controller.recheckMidiInputs({"MOTU Pro Audio 0", "Pad 2"});
    controller.tick();
    CHECK_FALSE(controller.window().get_control_error());
    CHECK(std::string(controller.window().get_control_reading()).rfind("NOT OPEN", 0) ==
          std::string::npos);
    CHECK(std::string(controller.window().get_status()).find("MIDI input Pad 2 is back") !=
          std::string::npos);
    // And what it sends reaches the control input again.
    const std::vector<unsigned char> note{0x90, 36, 100};
    controller.midiInputs().deliver("Pad 2", std::span<const unsigned char>(note));
    CHECK(controller.control().lastEvent().has_value());
}

TEST_CASE("while a source is followed, a rule that needs the audio says it will not fire and is "
          "not counted active",
          "[ui][follow][trigger]") {
    // Nothing is heard while a source is followed, so a rule on onsets never fires: the editor
    // says why where a rule says why it will not, its WHEN list marks the triggers that need the
    // audio, and the TRIGGERS row counts it apart — and all of it goes once the audio is back.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::trigger::Rule::Config clips;
    clips.id = "clips";
    clips.address = "/clips";
    takt4::trigger::Rule::Config hits = clips;
    hits.id = "hits";
    hits.trigger = takt4::trigger::Trigger::Onset;
    controller.setRules({clips, hits});
    controller.tick();
    CHECK(controller.window().get_rules_active() == 2);
    CHECK(controller.window().get_rules_need_audio() == 0);
    const auto problemOf = [&controller](std::size_t row) {
        return std::string(controller.editor().window().get_rules()->row_data(row)->problem);
    };
    const auto whenList = [&controller] {
        std::vector<std::string> names;
        const auto model = controller.editor().window().get_trigger_names();
        for (std::size_t i = 0; i < model->row_count(); ++i) {
            names.push_back(std::string(*model->row_data(i)));
        }
        return names;
    };
    CHECK(problemOf(1).empty());

    controller.pickTiming(2);
    controller.tick();
    CHECK(controller.window().get_rules_active() == 1);
    CHECK(controller.window().get_rules_need_audio() == 1);
    CHECK(problemOf(0).empty());
    CHECK(problemOf(1) == "timing comes from MIDI clock \xE2\x80\x94 needs beat detection");
    controller.editor().pick(1);
    CHECK(std::string(controller.editor().window().get_rule_problem()) == problemOf(1));
    const std::vector<std::string> marked = whenList();
    CHECK(std::count_if(marked.begin(), marked.end(), [](const std::string& name) {
              return name.ends_with(" (needs beat detection)");
          }) == 2);

    controller.pickTiming(0);
    controller.tick();
    CHECK(controller.window().get_rules_active() == 2);
    CHECK(controller.window().get_rules_need_audio() == 0);
    CHECK(problemOf(1).empty());
    const std::vector<std::string> plain = whenList();
    CHECK(std::none_of(plain.begin(), plain.end(), [](const std::string& name) {
        return name.find("needs beat detection") != std::string::npos;
    }));
}

// --- the CDJs ------------------------------------------------------------------------------------

TEST_CASE("the CDJs are picked with the pointer: the follow list fills with the players heard, a "
          "player picked from it is the one followed, and nothing is announced until START",
          "[ui][follow][prolink]") {
    namespace peer = takt4::testing::prolink_peer;
    if (!peer::canUseSecondLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2");
    }
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    peer::SimulatedRig rig;
    const takt4::timing::prolink::Network::Config config =
        peer::testConfig(rig.announcePort(), [&controller] { return linkNow(controller); });
    controller.useCdjNetworkForTests(config);
    rig.start(config);
    REQUIRE(laidOutWithInputs(controller).size() == 7);

    // The timing picker, three steps down: the CDJs.
    int y = 0;
    const auto audio = inputsRow(controller, 73, y);
    INFO("timing row: " << spans(audio));
    REQUIRE(audio.size() == 6);
    clickAt(window, middleOf(audio[2]), static_cast<float>(y));
    slint::platform::update_timers_and_animations();
    press(window, kDown);
    press(window, kDown);
    press(window, kDown);
    press(window, "\n");
    settle();
    CHECK(controller.timingIndex() == 3);
    CHECK(controller.window().get_following());
    CHECK(std::string(controller.window().get_timing_name()) == "CDJs");
    CHECK(controller.window().get_timing_ready());

    // Listening: the player and the mixer heard, the player on the follow list.
    waitUntil(controller,
              [&] { return controller.window().get_cdj_follow_choices()->row_count() == 2; });
    const auto choices = controller.window().get_cdj_follow_choices();
    REQUIRE(choices->row_count() == 2);
    CHECK(std::string(*choices->row_data(0)) == "tempo master");
    CHECK(std::string(*choices->row_data(1)) == "player 2 \xC2\xB7 CDJ-3000");
    CHECK(std::string(controller.window().get_timing_reading()) == "1 player, 1 mixer heard");
    // Automatic, and the card they are heard on named, as what START joins on.
    CHECK(std::string(controller.window().get_timing_network()) ==
          "loopback \xC2\xB7 127.0.0.1, at START");

    // The row: timing, from, the picker, follow, the follow picker, and the reading.
    const auto row = inputsRow(controller, 73, y);
    INFO("timing row, the CDJs: " << spans(row));
    REQUIRE(row.size() == 6);
    clickAt(window, middleOf(row[4]), static_cast<float>(y));
    slint::platform::update_timers_and_animations();
    press(window, kDown);
    press(window, "\n");
    settle();
    CHECK(controller.window().get_cdj_follow_index() == 1);
    CHECK(controller.currentSettings().machine.proLinkFollow == 2);
    waitUntil(controller, [&] { return controller.cdjNetwork().snapshot().follow == 2; });
    CHECK(controller.cdjNetwork().snapshot().follow == 2);

    // The list rebuilt while it stands still would take an open picker from under the hand: it
    // is published again only when it changes.
    const auto before = controller.window().get_cdj_follow_choices();
    for (int i = 0; i < 20; ++i) {
        settle();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(controller.window().get_cdj_follow_choices().get() == before.get());

    // Nothing announced while stopped.
    CHECK_FALSE(rig.announceSocket().receive(300).has_value());
    CHECK(controller.cdjNetwork().snapshot().phase ==
          takt4::timing::prolink::VirtualPlayer::Phase::Listening);
    rig.stop();
}

TEST_CASE("following the CDJs, START joins as a player and the master's beats reach the plot and "
          "the tempo; STOP stops announcing",
          "[ui][follow][prolink][slow]") {
    namespace peer = takt4::testing::prolink_peer;
    if (!peer::canUseSecondLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2");
    }
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    peer::SimulatedRig rig;
    const takt4::timing::prolink::Network::Config config =
        peer::testConfig(rig.announcePort(), [&controller] { return linkNow(controller); });
    controller.useCdjNetworkForTests(config);
    rig.start(config);
    controller.pickTiming(3);
    std::vector<std::pair<int, int>> sheets = laidOutWithInputs(controller);
    REQUIRE(sheets.size() == 7);
    waitUntil(controller, [&] { return !controller.cdjNetwork().snapshot().devices.empty(); });

    // START, the last thing in the top bar.
    takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, 1480);
    window.dispatch_window_active_changed_event(true);
    const int topRow = sheets[0].first + 48;
    const auto top = occupied(shot, topRow - 20, topRow + 20, 20, 780, kSheet, 5);
    INFO("top bar: " << spans(top));
    REQUIRE(top.size() == 3);
    clickAt(window, middleOf(top.back()), static_cast<float>(topRow));
    waitUntil(controller, [&] { return controller.followed().running(); });
    REQUIRE(controller.followed().running());
    CHECK_FALSE(tracker.running()); // the audio is not opened at all
    waitUntil(controller, [&] {
        return controller.cdjNetwork().snapshot().phase ==
               takt4::timing::prolink::VirtualPlayer::Phase::Joined;
    });
    CHECK(controller.cdjNetwork().snapshot().number == 7);
    settle(); // the window's next round, which reads what the network now says
    CHECK(std::string(controller.window().get_timing_network()) ==
          "loopback \xC2\xB7 127.0.0.1 \xC2\xB7 takt4 is player 7");
    // Joined, and the deck not yet playing: nothing to follow, said in the terms it chooses by.
    CHECK(std::string(controller.window().get_timing_reading()) ==
          "no deck playing \xE2\x80\x94 waiting");

    // The player plays, and is master: its beats on the plot, its tempo the window's.
    rig.setPlaying(true);
    waitUntil(controller, [&] { return controller.followed().beatsSent() >= 5; });
    settle();
    const auto trace = controller.window().get_trace();
    int beatsDrawn = 0;
    bool inBeat = false;
    for (std::size_t i = 0; i < trace->row_count(); ++i) {
        const TracePoint point = *trace->row_data(i);
        beatsDrawn += point.called && !inBeat ? 1 : 0;
        inBeat = point.called;
    }
    CHECK(beatsDrawn >= 4);
    CHECK(controller.window().get_bpm() == Catch::Approx(128.0).margin(0.05));
    const std::string reading(controller.window().get_timing_reading());
    INFO(reading);
    CHECK(reading.rfind("following player 2 (master) \xC2\xB7 128.00 BPM", 0) == 0);
    CHECK(std::string(controller.window().get_strip_label()).rfind("player 2 \xC2\xB7 ", 0) == 0);
    CHECK_FALSE(controller.window().get_timing_waiting());

    // STOP: the run over, the keep-alives stopped.
    rig.setPlaying(false);
    shot = takt4::tests::render(controller.window(), 800, 1480);
    window.dispatch_window_active_changed_event(true);
    const auto running = occupied(shot, topRow - 20, topRow + 20, 20, 780, kSheet, 5);
    REQUIRE_FALSE(running.empty());
    clickAt(window, middleOf(running.back()), static_cast<float>(topRow));
    waitUntil(controller, [&] { return !controller.followed().running(); });
    CHECK_FALSE(controller.followed().running());
    waitUntil(controller, [&] {
        return controller.cdjNetwork().snapshot().phase ==
               takt4::timing::prolink::VirtualPlayer::Phase::Listening;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    while (rig.announceSocket().receive(10).has_value()) {
    }
    CHECK_FALSE(rig.announceSocket().receive(500).has_value());
    rig.stop();
}

TEST_CASE("following the CDJs, the window looks for the card they were joined on every few "
          "seconds, and when its address has gone the network joins again on the one they are on",
          "[ui][follow][prolink][slow]") {
    namespace peer = takt4::testing::prolink_peer;
    if (!peer::canUseSecondLoopbackAddress() || !peer::canUseThirdLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2 or 127.0.0.3");
    }
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.setControlRetrySeconds(0.2);
    peer::SimulatedRig rig;
    const auto cards = std::make_shared<peer::Cards>(peer::loopbackCard());
    takt4::timing::prolink::Network::Config config =
        peer::testConfig(rig.announcePort(), [&controller] { return linkNow(controller); });
    config.interfaces = [cards] { return cards->get(); };
    controller.useCdjNetworkForTests(config);
    rig.start(config);
    controller.pickTiming(3);
    waitUntil(controller, [&] { return !controller.cdjNetwork().snapshot().devices.empty(); });
    controller.toggleRun();
    waitUntil(controller, [&] { return controller.followed().running(); });
    REQUIRE(controller.followed().running());
    const auto joinedOn = [&](std::uint32_t ip) {
        const takt4::timing::prolink::Network::Snapshot s = controller.cdjNetwork().snapshot();
        return s.interfaceIp == ip &&
               s.phase == takt4::timing::prolink::VirtualPlayer::Phase::Joined;
    };
    waitUntil(controller, [&] { return joinedOn(peer::kHere); });
    REQUIRE(joinedOn(peer::kHere));

    // The card's address changes, and nothing else is done: the window's own look finds it.
    cards->set(peer::movedCard());
    waitUntil(controller, [&] { return joinedOn(peer::kMoved); });
    CHECK(joinedOn(peer::kMoved));
    CHECK(controller.cdjNetwork().snapshot().rejoins == 1);
    controller.tick();
    slint::platform::update_timers_and_animations();
    CHECK(std::string(controller.window().get_timing_network()) ==
          "loopback \xC2\xB7 127.0.0.3 \xC2\xB7 takt4 is player 7");
    controller.toggleRun();
    waitUntil(controller, [&] { return !controller.followed().running(); });
    rig.stop();
}

TEST_CASE("with nothing at all coming in on the CDJs' ports, their row names the firewall",
          "[ui][follow][prolink]") {
    namespace peer = takt4::testing::prolink_peer;
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    // Ports the sandbox gave this test, which nothing sends to.
    takt4::timing::prolink::Network::Config config =
        peer::testConfig(peer::allowedPort(), [&controller] { return linkNow(controller); });
    config.quietMicros = 200'000;
    controller.useCdjNetworkForTests(config);
    controller.pickTiming(3);
    const std::string firewall = "nothing at all is coming in \xE2\x80\x94 a firewall may be "
                                 "blocking takt4: allow it on Public networks too";
    waitUntil(controller,
              [&] { return std::string(controller.window().get_timing_note()) == firewall; });
    CHECK(std::string(controller.window().get_timing_note()) == firewall);
    CHECK(std::string(controller.window().get_timing_reading()) ==
          "no CDJs heard on this network yet");
}

TEST_CASE("the CDJs' port held by another program is said on the timing row in the alarm colour, "
          "and START says why it cannot follow them",
          "[ui][follow][prolink]") {
    namespace peer = takt4::testing::prolink_peer;
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);

    SECTION("in the test sandbox, Pro DJ Link's own port") {
        controller.pickTiming(3);
        controller.tick();
        const std::string reading(controller.window().get_timing_reading());
        INFO(reading);
        CHECK(reading == "cannot listen for CDJs");
        // And the whole of why on its own line.
        CHECK(std::string(controller.window().get_timing_problem()) ==
              "cannot listen for CDJs: not listening for CDJs in the test sandbox");
        CHECK(controller.window().get_timing_waiting());
    }
    SECTION("a port another program has") {
        peer::UdpPeer holder(0);
        takt4::timing::prolink::Network::Config config =
            peer::testConfig(1, [&controller] { return linkNow(controller); });
        config.own.announce = holder.port();
        controller.useCdjNetworkForTests(config);
        controller.pickTiming(3);
        controller.tick();
        const std::string problem(controller.window().get_timing_problem());
        INFO(problem);
        CHECK(std::string(controller.window().get_timing_reading()) == "cannot listen for CDJs");
        CHECK(problem.find("is in use by another program") != std::string::npos);
        CHECK(problem.find("rekordbox") != std::string::npos);
        CHECK(controller.window().get_timing_waiting());
        controller.toggleRun();
        CHECK_FALSE(controller.followed().running());
        const std::string said(controller.window().get_status());
        INFO(said);
        CHECK(said.rfind("Cannot follow the CDJs: cannot listen for CDJs: ", 0) == 0);
        CHECK(controller.window().get_status_is_error());
    }
}

TEST_CASE("the CDJs keep a latency of their own, and whom to follow and the card are saved with "
          "the desk",
          "[ui][follow][prolink][settings]") {
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.machine.timingSource = std::string(takt4::settings::kTimingProLink);
    saved.machine.proLinkFollow = 4;
    saved.machine.proLinkInterface = "169.254.30.1";
    // As a hand might write them: kept as the window writes them.
    saved.machine.proLinkInterfaceMac = "74-5E-1C-00-00-09";
    saved.machine.proLinkInterfaceName = "DJ\nLAN";
    saved.preset.timingLatency.link = -0.010;
    saved.preset.timingLatency.proLink = 0.022;
    WindowController controller(tracker, saved);
    CHECK(controller.timingIndex() == 3);
    // The latency slider is the CDJs' own while they are picked.
    CHECK(controller.window().get_latency_ms() == Catch::Approx(22.0));
    controller.setLatencyMs(-31.0);
    takt4::settings::Settings out = controller.currentSettings();
    CHECK(out.preset.timingLatency.proLink == Catch::Approx(-0.031));
    CHECK(out.preset.timingLatency.link == Catch::Approx(-0.010));
    CHECK(out.machine.timingSource == "prolink");
    CHECK(out.machine.proLinkFollow == 4);
    CHECK(out.machine.proLinkInterface == "169.254.30.1");
    CHECK(out.machine.proLinkInterfaceMac == "74:5e:1c:00:00:09");
    CHECK(out.machine.proLinkInterfaceName == "DJ LAN");
    // The card is not on this computer: kept, picked, and said to be missing.
    controller.tick();
    const auto cards = controller.window().get_cdj_card_choices();
    REQUIRE(cards->row_count() >= 2);
    const int picked = controller.window().get_cdj_card_index();
    REQUIRE(picked == static_cast<int>(cards->row_count()) - 1);
    CHECK(std::string(*cards->row_data(static_cast<std::size_t>(picked))) ==
          "not on this computer: DJ LAN \xC2\xB7 169.254.30.1");
    // Player 4 is not on any network here: kept on the list, said not to be heard.
    controller.tick();
    const auto choices = controller.window().get_cdj_follow_choices();
    REQUIRE(choices->row_count() == 2);
    CHECK(std::string(*choices->row_data(1)) == "player 4 \xE2\x80\x94 not heard");
    CHECK(controller.window().get_cdj_follow_index() == 1);
    // Link picked: its own latency back on the slider.
    controller.pickTiming(1);
    CHECK(controller.window().get_latency_ms() == Catch::Approx(-10.0));
    controller.pickTiming(3);
    CHECK(controller.window().get_latency_ms() == Catch::Approx(-31.0));
    // Following the master again, picked from the list: saved as such.
    controller.pickCdjFollow(0);
    CHECK(controller.currentSettings().machine.proLinkFollow == 0);
}

TEST_CASE("what the window says about the CDJs, in each state they can be in", "[ui][follow]") {
    using takt4::ui::describeTiming;
    using takt4::ui::TimingFacts;
    TimingFacts facts;
    facts.index = 3;
    facts.beatsPerBar = 7; // the setting is not the CDJs': they count four
    takt4::ui::TimingView view = describeTiming(facts);
    CHECK(view.name == "CDJs");
    CHECK(view.shortName == "CDJ");
    CHECK(view.beatsPerBar == 4);
    CHECK(view.reading == "no CDJs heard on this network yet");
    CHECK(view.network == "the card a CDJ is first heard on, at START");
    CHECK_FALSE(view.waiting);
    facts.cdjPlayers = 3;
    facts.cdjMixers = 1;
    CHECK(describeTiming(facts).reading == "3 players, 1 mixer heard");
    facts.cdjPlayers = 1;
    facts.cdjMixers = 0;
    CHECK(describeTiming(facts).reading == "1 player heard");
    // Heard, the card is the one they are heard on — not "picked when a CDJ is heard" under "1
    // player heard" — taken at START, or being taken.
    CHECK(describeTiming(facts).network == "the card the CDJs are heard on, at START");
    facts.running = true;
    CHECK(describeTiming(facts).network == "the card the CDJs are heard on");
    facts.running = false;
    // Automatic, and the card they are heard on known: named, as what START joins on.
    facts.cdjHeardOn = "Ethernet 2 \xC2\xB7 169.254.20.7";
    CHECK(describeTiming(facts).network == "Ethernet 2 \xC2\xB7 169.254.20.7, at START");
    // A card picked, and here: the picker says it, and nothing is read after it.
    facts.cdjPicked = "Ethernet 2 \xC2\xB7 169.254.20.7";
    CHECK(describeTiming(facts).network.empty());
    // A card picked on another computer: what is joined instead, stopped and running.
    facts.cdjPicked = "DJ LAN \xC2\xB7 10.9.9.9";
    facts.cdjPickMissing = true;
    CHECK(describeTiming(facts).network == "instead: Ethernet 2 \xC2\xB7 169.254.20.7, at START");
    facts.running = true;
    CHECK(describeTiming(facts).network == "instead: Ethernet 2 \xC2\xB7 169.254.20.7");
    facts.cdjHeardOn.clear();
    CHECK(describeTiming(facts).network == "instead: the card the CDJs are heard on");
    facts.running = false;
    facts.cdjPicked.clear();
    facts.cdjPickMissing = false;
    facts.cdjProblem = "cannot listen for CDJs: port 50000 is in use by another program";
    view = describeTiming(facts);
    // A few words on the row, the whole of it on a line of its own under the rows.
    CHECK(view.reading == "cannot listen for CDJs");
    CHECK(view.problem == facts.cdjProblem);
    CHECK(view.waiting); // in the alarm colour, stopped or not
    CHECK(view.wait == takt4::ui::TimingView::Wait::Absent);

    // Running: joining, then following, coasting, waiting for a master.
    facts.cdjProblem.clear();
    facts.running = true;
    view = describeTiming(facts);
    CHECK(view.waiting);
    CHECK(view.waitingWord == "joining");
    CHECK(view.reading == "joining as a player\xE2\x80\xA6");
    facts.cdjJoined = true;
    facts.cdjNumber = 7;
    facts.cdjMaster = 2;
    facts.cdjFollowed = 2;
    facts.cdjInterface = "Ethernet 2 \xC2\xB7 169.254.20.7";
    facts.state.locked = true;
    facts.state.rawBpm = 126.0;
    facts.state.beatInBar = 3;
    view = describeTiming(facts);
    CHECK_FALSE(view.waiting);
    CHECK(view.reading == "following player 2 (master) \xC2\xB7 126.00 BPM");
    CHECK(view.detail == "player 2 (master) \xC2\xB7 4 beats to the bar");
    CHECK(view.network == "Ethernet 2 \xC2\xB7 169.254.20.7 \xC2\xB7 takt4 is player 7");
    CHECK(view.stripBeat == 3);
    CHECK(view.stripLabel == "player 2 \xC2\xB7 3 of 4");
    // A player named rather than the master.
    facts.cdjFollow = 3;
    facts.cdjFollowed = 3;
    CHECK(describeTiming(facts).reading == "following player 3 \xC2\xB7 126.00 BPM");
    facts.coasting = true;
    view = describeTiming(facts);
    CHECK(view.coasting);
    CHECK(view.reading == "beats stopped \xE2\x80\x94 coasting at 126.00 BPM");
    CHECK(view.detail == "player 3 stopped");
    facts.coasting = false;
    facts.state.locked = false;
    view = describeTiming(facts);
    CHECK(view.waiting);
    // The deck asked for, paused: the DJ's to start, grey as a quiet room is, not a fault.
    CHECK(view.waitingWord == "not playing");
    CHECK(view.wait == takt4::ui::TimingView::Wait::Stopped);
    CHECK(view.reading == "player 3 not playing \xE2\x80\x94 waiting");
    facts.cdjFollow = 0;
    facts.cdjFollowed = 0;
    facts.cdjMasterWithoutBeats = true;
    view = describeTiming(facts);
    // The master playing a track with no grid leads nothing: a fault to put right, in red.
    CHECK(view.waitingWord == "no beat grid");
    CHECK(view.wait == takt4::ui::TimingView::Wait::Absent);
    CHECK(view.reading == "no deck playing \xE2\x80\x94 waiting");
    CHECK(view.note == "the master's track has no beat grid, so it sends no beats");
    CHECK(view.stripLabel.empty());
}

TEST_CASE("the CDJs' row reads the network's own snapshot: the join, the decks that could be "
          "followed, the mixer's on-air, nothing heard, the sends failing and the card's note",
          "[ui][follow]") {
    using takt4::timing::prolink::DeviceKind;
    using takt4::timing::prolink::Network;
    using takt4::timing::prolink::VirtualPlayer;
    using takt4::ui::describeTiming;
    using takt4::ui::TimingFacts;
    const auto factsOf = [](const Network::Snapshot& snapshot) {
        TimingFacts facts;
        facts.index = 3;
        facts.running = true;
        takt4::ui::readCdjFacts(snapshot, facts);
        return facts;
    };
    Network::Snapshot snapshot;
    snapshot.open = true;
    Network::DeviceView deck;
    deck.kind = DeviceKind::Player;
    deck.canLead = true;
    deck.number = 1;
    snapshot.devices.push_back(deck);
    deck.number = 2;
    snapshot.devices.push_back(deck);
    Network::DeviceView mixer;
    mixer.kind = DeviceKind::Mixer;
    mixer.number = 33;
    mixer.canLead = true; // the master: not a deck playing
    snapshot.devices.push_back(mixer);

    snapshot.phase = VirtualPlayer::Phase::AwaitingMixer;
    TimingFacts facts = factsOf(snapshot);
    CHECK(facts.cdjPlayers == 2);
    CHECK(facts.cdjMixers == 1);
    CHECK(facts.cdjLeaders == 2);
    CHECK(describeTiming(facts).reading == "the mixer is giving takt4 a player number\xE2\x80\xA6");
    snapshot.phase = VirtualPlayer::Phase::NoNumber;
    CHECK(describeTiming(factsOf(snapshot)).waitingWord == "no number");

    snapshot.phase = VirtualPlayer::Phase::Joined;
    snapshot.number = 7;
    snapshot.interfaceName = "Ethernet 2";
    snapshot.interfaceIp = takt4::testing::ipv4(169, 254, 20, 7);
    facts = factsOf(snapshot);
    CHECK(facts.cdjInterface == "Ethernet 2 \xC2\xB7 169.254.20.7");
    CHECK(describeTiming(facts).reading ==
          "2 decks playing and none is the master \xE2\x80\x94 pick one under follow");
    snapshot.devices[0].canLead = false;
    snapshot.devices[1].canLead = false;
    snapshot.onAirTold = true;
    CHECK(describeTiming(factsOf(snapshot)).reading ==
          "no deck playing on air \xE2\x80\x94 waiting");
    snapshot.sendsFailing = true;
    CHECK(describeTiming(factsOf(snapshot)).note ==
          "takt4's packets to the CDJs are not getting out \xE2\x80\x94 joining again");
    snapshot.sendsFailing = false;
    snapshot.note = "the card picked for the CDJs, 10.9.9.9, is not on this computer";
    CHECK(describeTiming(factsOf(snapshot)).note == snapshot.note);

    // Listening, and not one datagram read: the firewall named.
    snapshot = Network::Snapshot{};
    snapshot.open = true;
    snapshot.nothingHeard = true;
    facts = factsOf(snapshot);
    facts.running = false;
    CHECK(describeTiming(facts).note ==
          "nothing at all is coming in \xE2\x80\x94 a firewall may be blocking takt4: allow it on "
          "Public networks too");
}

TEST_CASE("what the window says about the CDJs while joining, with nothing coming in, with the "
          "master paused or two decks playing, and with takt4's packets not getting out",
          "[ui][follow]") {
    using takt4::ui::describeTiming;
    using takt4::ui::TimingFacts;
    TimingFacts facts;
    facts.index = 3;
    // Nothing at all read since the network opened: the firewall is named, stopped or running,
    // for as long as no CDJ is heard.
    facts.cdjNothingHeard = true;
    const std::string firewall = "nothing at all is coming in \xE2\x80\x94 a firewall may be "
                                 "blocking takt4: allow it on Public networks too";
    takt4::ui::TimingView view = describeTiming(facts);
    CHECK(view.reading == "no CDJs heard on this network yet");
    CHECK(view.note == firewall);
    facts.running = true;
    view = describeTiming(facts);
    CHECK(view.reading == "no CDJs heard on this network \xE2\x80\x94 waiting for one");
    CHECK(view.note == firewall);
    facts.cdjNothingHeard = false;
    CHECK(describeTiming(facts).note.empty());

    // Joining: a mixer giving the number, and every number taken.
    facts.cdjPlayers = 2;
    facts.cdjMixers = 1;
    facts.cdjAwaitingMixer = true;
    view = describeTiming(facts);
    CHECK(view.waiting);
    CHECK(view.waitingWord == "joining");
    CHECK(view.reading == "the mixer is giving takt4 a player number\xE2\x80\xA6");
    facts.cdjAwaitingMixer = false;
    facts.cdjNoNumber = true;
    view = describeTiming(facts);
    CHECK(view.waitingWord == "no number");
    CHECK(view.reading ==
          "players 7 to 15 are all taken \xE2\x80\x94 waiting for one to come free");
    CHECK(view.detail == "no player number free");
    facts.cdjNoNumber = false;

    // Joined, following the tempo master, and nobody to follow: no deck playing (on the air, when
    // the mixer says which are), or two and no telling which.
    facts.cdjJoined = true;
    facts.cdjNumber = 7;
    facts.cdjMaster = 1;
    view = describeTiming(facts);
    CHECK(view.waitingWord == "no deck");
    CHECK(view.reading == "no deck playing \xE2\x80\x94 waiting");
    facts.cdjOnAirTold = true;
    CHECK(describeTiming(facts).reading == "no deck playing on air \xE2\x80\x94 waiting");
    facts.cdjLeaders = 2;
    view = describeTiming(facts);
    CHECK(view.reading ==
          "2 decks playing and none is the master \xE2\x80\x94 pick one under follow");
    CHECK(view.detail == "2 decks playing and none is the master");
    // One deck it can follow, and none of its beats yet.
    facts.cdjLeaders = 1;
    facts.cdjFollowed = 2;
    view = describeTiming(facts);
    CHECK(view.waitingWord == "no beats");
    CHECK(view.reading == "player 2 playing, no beats yet \xE2\x80\x94 waiting");

    // What stops nothing comes last: the card picked not here, then takt4's packets failing.
    facts.cdjNote = "the card picked for the CDJs, 10.9.9.9, is not on this computer \xE2\x80\x94 "
                    "joined on Ethernet 2 \xC2\xB7 169.254.20.7";
    CHECK(describeTiming(facts).note == facts.cdjNote);
    facts.cdjSendsFailing = true;
    CHECK(describeTiming(facts).note ==
          "takt4's packets to the CDJs are not getting out \xE2\x80\x94 joining again");
    // And the master's beatless track before either.
    facts.cdjFollowed = 0;
    facts.cdjLeaders = 0;
    facts.cdjMasterWithoutBeats = true;
    CHECK(describeTiming(facts).note ==
          "the master's track has no beat grid, so it sends no beats");
}

// --- the timing row's colours, its CHANGE and its words (2026-10-22) ----------------------------

namespace {

/// The alarm's own pixels (#ff5646) and the amber's (#ffb541) in a part of a picture.
struct Coloured {
    int alarm = 0;
    int amber = 0;
};

Coloured coloured(const takt4::tests::Shot& shot, int y0, int y1, int x0, int x1) {
    Coloured seen;
    for (int y = std::max(0, y0); y < std::min(shot.height, y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(shot.width, x1); ++x) {
            seen.alarm += shot.is(x, y, 0xff, 0x56, 0x46) ? 1 : 0;
            seen.amber += shot.is(x, y, 0xff, 0xb5, 0x41) ? 1 : 0;
        }
    }
    return seen;
}

} // namespace

TEST_CASE("a followed source is red only when it is not there: a stop obeyed is grey, one on its "
          "way amber",
          "[ui][follow]") {
    // A DAW's Stop, obeyed, turned the banner, the timing row and the folded inputs heading the
    // alarm red, as an unplugged clock does; so did a clock's first beats while its tempo was
    // measured, Link's peers being joined, and the CDJs at every START. Counted on the picture:
    // the alarm's own pixels and the amber's in the banner's right half — the state word and its
    // dot — on the timing row's right, where the reading is, and on the folded heading right of
    // its number, which is the same red and is not counted.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window();
    struct Drawn {
        Coloured banner;
        Coloured row;
        Coloured heading;
    };
    const auto drawn = [&](const takt4::ui::TimingFacts& facts) {
        window.set_running(facts.running);
        takt4::ui::publishTimingView(window, takt4::ui::describeTiming(facts));
        window.set_inputs_folded(false);
        takt4::tests::Shot shot = takt4::tests::render(window, 800, 1480);
        std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
        REQUIRE(sheets.size() == 7);
        Drawn out;
        out.banner = coloured(shot, sheets[1].first, sheets[1].second, 400, 800);
        const int row = static_cast<int>(window.get_timing_row_y());
        out.row = coloured(shot, row, row + 34, 400, 800);
        window.set_inputs_folded(true);
        shot = takt4::tests::render(window, 800, 1480);
        sheets = sheetsDown(shot);
        REQUIRE(sheets.size() == 7);
        out.heading = coloured(shot, sheets[4].first, sheets[4].second, 100, 740);
        window.set_inputs_folded(false);
        return out;
    };

    takt4::ui::TimingFacts clock;
    clock.index = 2;
    clock.running = true;
    clock.port = "DJM-A9";
    clock.obey = true;
    {
        INFO("no clock at all: red, wherever it is said");
        const Drawn none = drawn(clock);
        CHECK(none.banner.alarm > 0);
        CHECK(none.row.alarm > 0);
        CHECK(none.heading.alarm > 0);
    }
    clock.stopObeyed = true;
    {
        INFO("the DAW's Stop, obeyed: grey, not one pixel of the alarm or the amber");
        const Drawn stopped = drawn(clock);
        CHECK(stopped.banner.alarm == 0);
        CHECK(stopped.banner.amber == 0);
        CHECK(stopped.row.alarm == 0);
        CHECK(stopped.row.amber == 0);
        CHECK(stopped.heading.alarm == 0);
    }
    clock.stopObeyed = false;
    clock.ticking = true;
    {
        INFO("ticking, its tempo being measured: amber, and the heading not red");
        const Drawn measuring = drawn(clock);
        CHECK(measuring.banner.alarm == 0);
        CHECK(measuring.banner.amber > 0);
        CHECK(measuring.row.alarm == 0);
        CHECK(measuring.row.amber > 0);
        CHECK(measuring.heading.alarm == 0);
    }

    takt4::ui::TimingFacts cdjs;
    cdjs.index = 3;
    cdjs.running = true;
    cdjs.cdjPlayers = 2;
    {
        INFO("the CDJs being joined at START: amber");
        const Drawn joining = drawn(cdjs);
        CHECK(joining.banner.alarm == 0);
        CHECK(joining.banner.amber > 0);
        CHECK(joining.heading.alarm == 0);
    }
    cdjs.cdjJoined = true;
    {
        INFO("joined, and no deck playing: the DJ's pause, grey");
        const Drawn paused = drawn(cdjs);
        CHECK(paused.banner.alarm == 0);
        CHECK(paused.banner.amber == 0);
        CHECK(paused.heading.alarm == 0);
    }

    takt4::ui::TimingFacts link;
    link.index = 1;
    link.running = true;
    link.peers = 3;
    {
        INFO("Link: three peers heard, the session being joined — amber, not 'no peers' in red");
        const Drawn joining = drawn(link);
        CHECK(joining.banner.alarm == 0);
        CHECK(joining.banner.amber > 0);
        CHECK(joining.row.alarm == 0);
    }
    link.peers = 0;
    {
        INFO("Link: nobody heard at all — red");
        const Drawn alone = drawn(link);
        CHECK(alone.banner.alarm > 0);
        CHECK(alone.row.alarm > 0);
    }
}

TEST_CASE("CHANGE brings the timing row into view with its list open, and is not there while "
          "running",
          "[ui][follow]") {
    // CHANGE only unfolded the inputs: with them open, or the window scrolled, it did nothing at
    // all, and while running it sat beside "stop to change" and led to a picker that would not
    // open. Here the window is short, so the row is below the fold; CHANGE is clicked where the
    // picture puts it, and the keys that follow pick from the list it opened.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window().window();
    takt4::testing::FakeGridSource source([&controller] { return linkNow(controller); }, 120.0,
                                          static_cast<double>(linkNow(controller)));
    controller.followForTests(&source);
    controller.pickTiming(1);
    controller.tick();
    // Folded, as a window opens: CHANGE has to unfold the section too.
    REQUIRE(controller.window().get_inputs_folded());
    const int height = 520;
    const takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, height);
    window.dispatch_window_active_changed_event(true);
    REQUIRE(controller.window().get_timing_row_y() > static_cast<float>(height));
    const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
    REQUIRE_FALSE(sheets.empty());
    const int topRow = sheets[0].first + 48;
    const auto banner = occupied(shot, topRow - 20, topRow + 20, 20, 780, kSheet, 5);
    INFO("following, stopped, top bar: " << spans(banner));
    REQUIRE(banner.size() == 3);

    clickAt(window, static_cast<float>(banner[0].second - 30), static_cast<float>(topRow));
    slint::platform::update_timers_and_animations();
    CHECK_FALSE(controller.window().get_inputs_folded());
    const float y = controller.window().get_timing_row_y();
    INFO("the timing row, after CHANGE, at y " << y);
    CHECK(y >= 0.0f);
    CHECK(y + 34.0f <= static_cast<float>(height));
    // Its list open: one step down is the MIDI clock.
    press(window, kDown);
    press(window, "\n");
    controller.tick();
    CHECK(controller.timingIndex() == 2);

    // Running: the box is the source's name and words, and START's STOP; no CHANGE.
    controller.pickTiming(1);
    controller.toggleRun();
    REQUIRE(controller.followed().running());
    controller.tick();
    {
        const std::string said(controller.window().get_status());
        INFO(said);
        CHECK(said.rfind("following ", 0) == 0);
        CHECK(said.find("audio not in use") == std::string::npos);
    }
    // Rendered tall, where the window has nothing to scroll: CHANGE left it scrolled down to the
    // timing row, and at the short height the picture is of that row and not of the bar — where
    // there is nothing at the box's right end whether CHANGE is drawn or not.
    const takt4::tests::Shot running = takt4::tests::render(controller.window(), 800, 1480);
    const std::vector<std::pair<int, int>> runningSheets = sheetsDown(running);
    REQUIRE_FALSE(runningSheets.empty());
    REQUIRE(runningSheets[0].first == sheets[0].first);
    const auto runningBanner = occupied(running, topRow - 20, topRow + 20, 20, 780, kSheet, 5);
    INFO("following, running, top bar: " << spans(runningBanner));
    REQUIRE_FALSE(runningBanner.empty());
    CHECK(runningBanner[0].first == banner[0].first);
    // The box, "stop to change", STOP — and nothing in the box at its right end.
    const auto inBox = occupied(running, topRow - 12, topRow + 12, banner[0].second - 80,
                                banner[0].second - 4, kSheet, 5);
    CHECK(inBox.empty());
    // A click where CHANGE was does nothing, the list staying shut.
    clickAt(window, static_cast<float>(banner[0].second - 30), static_cast<float>(topRow));
    slint::platform::update_timers_and_animations();
    press(window, kDown);
    press(window, "\n");
    controller.tick();
    CHECK(controller.timingIndex() == 1);
    controller.toggleRun();
    controller.followForTests(nullptr);
}

TEST_CASE("a MIDI clock with no input picked says what START needs, in amber, until one is",
          "[ui][follow]") {
    // START is greyed out until the clock has an input, and the status line said "press Start" —
    // which could not be done. Said instead, and on the row, in amber.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.window().set_inputs_folded(false);
    controller.pickTiming(2);
    controller.tick();
    CHECK_FALSE(controller.window().get_timing_ready());
    CHECK(std::string(controller.window().get_status()) ==
          "Pick the MIDI input the clock comes in on, then press Start.");
    CHECK(std::string(controller.window().get_timing_reading()) == "pick the clock's input");
    {
        const takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, 1480);
        const int row = static_cast<int>(controller.window().get_timing_row_y());
        CHECK(coloured(shot, row, row + 34, 400, 800).amber > 0);
        CHECK(coloured(shot, shot.height - 51, shot.height, 0, 400).amber > 0); // the status line
    }
    controller.setClockPort("DJM-A9 1");
    controller.tick();
    CHECK(controller.window().get_timing_ready());
    const std::string said(controller.window().get_status());
    INFO(said);
    CHECK(said.rfind("press Start to follow", 0) == 0);
    CHECK_FALSE(controller.window().get_status_is_error());
    CHECK(std::string(controller.window().get_timing_reading()).empty());
}

TEST_CASE("with no audio input on the machine, a source picked says what its START needs, and the "
          "audio picked back says there is no input",
          "[ui][follow]") {
    // A followed source does not open the audio at all, yet on a machine with no input — every
    // CI runner — the audio's "No input device" stayed up over the MIDI clock's "pick its input",
    // and Link's "press Start" over the audio picked back.
    LiveTracker tracker(kWeights, kStateSpace);
    tracker.setDeviceListHook([](std::vector<InputDevice>& list) { list.clear(); });
    WindowController controller(tracker);
    REQUIRE(controller.devices().empty());
    const std::string noInput = "No input device. Connect an interface and press RESCAN.";
    REQUIRE(std::string(controller.window().get_status()) == noInput);
    controller.pickTiming(2);
    controller.tick();
    CHECK(std::string(controller.window().get_status()) ==
          "Pick the MIDI input the clock comes in on, then press Start.");
    CHECK(controller.window().get_status_is_error());
    controller.setClockPort("DJM-A9 1");
    controller.tick();
    CHECK(std::string(controller.window().get_status()) == "press Start to follow MIDI clock.");
    CHECK_FALSE(controller.window().get_status_is_error());
    controller.pickTiming(1);
    controller.tick();
    CHECK(std::string(controller.window().get_status()) == "press Start to follow Ableton Link.");
    // The audio picked back has nothing to start on, and says so again.
    controller.pickTiming(0);
    controller.tick();
    CHECK(std::string(controller.window().get_status()) == noInput);
    CHECK(controller.window().get_status_is_error());
}

TEST_CASE("the MIDI clock's amber line gives way to the audio's when the audio is picked back",
          "[ui][follow]") {
    // "Pick the MIDI input the clock comes in on" is an error line, and nothing said the audio's
    // own over an error: it stayed up with the audio picked, asking for a clock nobody wanted.
    LiveTracker tracker(kWeights, kStateSpace);
    tracker.setDeviceListHook([](std::vector<InputDevice>& list) { list = {motuAt(0)}; });
    WindowController controller(tracker);
    controller.pickTiming(2);
    controller.tick();
    REQUIRE(std::string(controller.window().get_status()) ==
            "Pick the MIDI input the clock comes in on, then press Start.");
    controller.pickTiming(0);
    controller.tick();
    CHECK(std::string(controller.window().get_status()) == "pick an input and press Start.");
    CHECK_FALSE(controller.window().get_status_is_error());
}

TEST_CASE("a machine with no audio input that follows a source opens saying what START needs, "
          "not that there is no input",
          "[ui][follow]") {
    // A show PC that follows Link with nothing plugged in can have no capture device at all, and
    // was told to connect an interface it does not use.
    LiveTracker tracker(kWeights, kStateSpace);
    tracker.setDeviceListHook([](std::vector<InputDevice>& list) { list.clear(); });
    takt4::settings::Settings saved;
    saved.machine.timingSource = "link";
    WindowController controller(tracker, saved);
    controller.tick();
    REQUIRE(controller.devices().empty());
    CHECK(std::string(controller.window().get_status()) == "press Start to follow Ableton Link.");
    CHECK_FALSE(controller.window().get_status_is_error());
    // And a RESCAN that finds none still says what it found, not a fault.
    controller.rescanDevices();
    rescanned(controller);
    const std::string said(controller.window().get_status());
    INFO(said);
    CHECK(said.rfind("Found 0 inputs", 0) == 0);
    CHECK_FALSE(controller.window().get_status_is_error());
}

TEST_CASE("following, halving or doubling never leaves the banner telling the operator to press "
          "the other one",
          "[ui][follow]") {
    // The beats' own rate is the median of the last eight gaps, and ÷2 kept the old grid's: the
    // banner read 128 going out against 64 shown and said "press ×2 to match" for up to four
    // seconds, and the operator who obeyed undid the halve and was told to press ÷2. Watched on
    // the picture: the advice's amber, on the banner's line under the number, never drawn.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window().window();
    takt4::testing::FakeGridSource source([&controller] { return linkNow(controller); }, 128.0,
                                          static_cast<double>(linkNow(controller)));
    controller.followForTests(&source);
    controller.pickTiming(1);
    REQUIRE(laidOutWithInputs(controller).size() == 7);
    controller.toggleRun();
    REQUIRE(controller.followed().running());
    waitUntil(controller, [&] { return controller.window().get_beats_bpm() > 0.0f; });
    REQUIRE(controller.window().get_beats_bpm() == Catch::Approx(128.0).margin(0.5));

    const takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, 1480);
    window.dispatch_window_active_changed_event(true);
    const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
    REQUIRE(sheets.size() == 7);
    const int buttonsRow = sheets[3].first + 26;
    const auto buttons = occupied(shot, buttonsRow - 16, buttonsRow + 15, 20, 780, kSheet, 5);
    INFO("controls: " << spans(buttons));
    REQUIRE(buttons.size() == 6); // ÷2, ×2, tap, lock, downbeat, and that tap and lock are off
    // The line under the number: "truth source: Ableton Link" in ink while the two agree, the
    // advice in amber when they are an octave apart. "÷2 applied", amber too, is beside the
    // number, above it; the source's own word is in the right half.
    const auto adviceDrawn = [&controller] {
        const takt4::tests::Shot now = takt4::tests::render(controller.window(), 800, 1480);
        const std::vector<std::pair<int, int>> down = sheetsDown(now);
        REQUIRE(down.size() == 7);
        return coloured(now, down[1].first + 70, down[1].first + 92, 20, 360).amber;
    };
    REQUIRE(adviceDrawn() == 0);
    const auto watch = [&controller, &adviceDrawn](std::chrono::milliseconds lasting) {
        int most = 0;
        const auto until = std::chrono::steady_clock::now() + lasting;
        while (std::chrono::steady_clock::now() < until) {
            controller.tick();
            slint::platform::update_timers_and_animations();
            most = std::max(most, adviceDrawn());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return most;
    };

    clickAt(window, middleOf(buttons[0]), static_cast<float>(buttonsRow));
    waitUntil(controller, [&] { return controller.followed().state().octaveShift == -1; });
    REQUIRE(controller.followed().state().octaveShift == -1);
    CHECK(watch(std::chrono::milliseconds{5000}) == 0);
    // And measured again, on the new grid.
    waitUntil(controller, [&] { return controller.window().get_beats_bpm() > 0.0f; });
    CHECK(controller.window().get_beats_bpm() == Catch::Approx(64.0).margin(0.5));
    CHECK(adviceDrawn() == 0);

    clickAt(window, middleOf(buttons[1]), static_cast<float>(buttonsRow));
    waitUntil(controller, [&] { return controller.followed().state().octaveShift == 0; });
    REQUIRE(controller.followed().state().octaveShift == 0);
    CHECK(watch(std::chrono::milliseconds{3000}) == 0);
    waitUntil(controller, [&] { return controller.window().get_beats_bpm() > 0.0f; });
    CHECK(controller.window().get_beats_bpm() == Catch::Approx(128.0).margin(0.5));

    controller.toggleRun();
    controller.followForTests(nullptr);
}

TEST_CASE("what stops the CDJs is said whole on a red line of its own under the timing rows",
          "[ui][follow][prolink]") {
    // At the reading's width the end of it — "is rekordbox running on this computer?", what to
    // do — was cut off. The row says it in a few words; the line under the rows says all of it.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.window().set_inputs_folded(false);
    controller.pickTiming(3);
    controller.tick();
    REQUIRE(std::string(controller.window().get_timing_problem()) ==
            "cannot listen for CDJs: not listening for CDJs in the test sandbox");
    const takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, 1480);
    const int row = static_cast<int>(controller.window().get_timing_row_y());
    // The timing row, the row under it, and then the problem's own line.
    const Coloured line =
        coloured(shot, row + 34 + 10 + 34 + 10, row + 34 + 10 + 34 + 10 + 20, 74, 780);
    CHECK(line.alarm > 0);
    // Gone with the problem: back on the audio, nothing red there.
    controller.pickTiming(0);
    controller.tick();
    CHECK(std::string(controller.window().get_timing_problem()).empty());
}

TEST_CASE("tap and lock say in the MIDI learn list that they are off while a source is followed",
          "[ui][follow]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const auto words = [&controller] {
        std::vector<std::string> out;
        const auto model = controller.window().get_learn_actions();
        for (std::size_t i = 0; i < model->row_count(); ++i) {
            out.emplace_back(*model->row_data(i));
        }
        return out;
    };
    const std::string tap(takt4::control::labelOf(takt4::control::ControlAction::Tap));
    const std::string lock(takt4::control::labelOf(takt4::control::ControlAction::Lock));
    const std::string halve(takt4::control::labelOf(takt4::control::ControlAction::TempoHalve));
    const auto has = [](const std::vector<std::string>& list, const std::string& word) {
        return std::find(list.begin(), list.end(), word) != list.end();
    };
    std::vector<std::string> list = words();
    CHECK(has(list, tap));
    CHECK(has(list, lock));
    const auto model = controller.window().get_learn_actions();
    controller.pickTiming(1);
    controller.tick();
    list = words();
    CHECK(has(list, tap + " (off while following)"));
    CHECK(has(list, lock + " (off while following)"));
    CHECK(has(list, halve)); // the rest as they were
    // Rows written in place: the same list, which an open dropdown is not taken from under.
    CHECK(controller.window().get_learn_actions().get() == model.get());
    controller.pickTiming(0);
    controller.tick();
    list = words();
    CHECK(has(list, tap));
    CHECK(has(list, lock));
}

TEST_CASE("a beat run on at the last tempo after the source went is drawn dimmed", "[ui][follow]") {
    // A coasted beat is a guess, and was drawn exactly like one that came.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::testing::FakeGridSource source([&controller] { return linkNow(controller); }, 240.0,
                                          static_cast<double>(linkNow(controller)));
    controller.followForTests(&source);
    controller.pickTiming(1);
    controller.toggleRun();
    REQUIRE(controller.followed().running());
    waitUntil(controller, [&] { return controller.followed().beatsSent() >= 4; });
    source.setPresent(false);
    waitUntil(controller, [&] { return controller.followed().coasting(); });
    const auto coastedSent = controller.followed().beatsSent();
    waitUntil(controller, [&] { return controller.followed().beatsSent() >= coastedSent + 2; });
    controller.tick();
    // The window's trace: beats that came, and beats coasted, each marked as it was.
    const auto trace = controller.window().get_trace();
    int came = 0;
    int coasted = 0;
    for (std::size_t i = 0; i < trace->row_count(); ++i) {
        const TracePoint point = *trace->row_data(i);
        came += point.called && !point.coasted ? 1 : 0;
        coasted += point.called && point.coasted ? 1 : 0;
    }
    CHECK(came > 0);
    CHECK(coasted > 0);
    controller.toggleRun();
    controller.followForTests(nullptr);

    // And drawn so: the same picture with every beat coasted has fewer full-bright pixels in the
    // timing sheet than with none.
    const auto bright = [&](bool asCoasted) {
        auto points = std::make_shared<slint::VectorModel<TracePoint>>();
        for (int i = 0; i < 300; ++i) {
            points->push_back(TracePoint{1.0f, 0.0f, i % 25 < 2, asCoasted});
        }
        controller.window().set_trace(points);
        const takt4::tests::Shot shot = takt4::tests::render(controller.window(), 800, 1480);
        const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
        REQUIRE(sheets.size() == 7);
        int count = 0;
        for (int y = sheets[2].first; y < sheets[2].second; ++y) {
            for (int x = 20; x < 780; ++x) {
                const slint::Rgb8Pixel p = shot.at(x, y);
                count += p.r > 0xe0 && p.g > 0xe0 && p.b > 0xe0 ? 1 : 0;
            }
        }
        return count;
    };
    controller.pickTiming(1); // following, so the plot draws beats as columns
    const int full = bright(false);
    const int dimmed = bright(true);
    INFO(full << " full-bright pixels drawn as they came, " << dimmed << " coasted");
    CHECK(full > dimmed + 1000);
}

TEST_CASE("the CDJs' follow list is not rebuilt under the pointer while it is open",
          "[ui][follow][prolink]") {
    // A player joining while the list was open moved its rows under the pointer, and the click
    // meant for one deck picked another. Held until the list closes, then written.
    namespace peer = takt4::testing::prolink_peer;
    if (!peer::canUseSecondLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2");
    }
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    peer::SimulatedRig rig;
    const takt4::timing::prolink::Network::Config config =
        peer::testConfig(rig.announcePort(), [&controller] { return linkNow(controller); });
    controller.useCdjNetworkForTests(config);
    rig.start(config);
    controller.pickTiming(3);
    waitUntil(controller,
              [&] { return controller.window().get_cdj_follow_choices()->row_count() == 2; });
    REQUIRE(laidOutWithInputs(controller).size() == 7);
    int y = 0;
    const auto row = inputsRow(controller, 73, y);
    INFO("timing row, the CDJs: " << spans(row));
    REQUIRE(row.size() == 6);
    clickAt(window, middleOf(row[4]), static_cast<float>(y));
    slint::platform::update_timers_and_animations();
    REQUIRE(controller.window().get_cdj_follow_open());
    const auto before = controller.window().get_cdj_follow_choices();

    // A second player announces itself while the list is open.
    peer::UdpPeer other(peer::kDevices);
    const takt4::testing::prolink_sim::Mac mac{0x74, 0x5e, 0x1c, 0, 0, 3};
    const auto announced = [&] {
        for (const auto& device : controller.cdjNetwork().snapshot().devices) {
            if (device.number == 3) {
                return true;
            }
        }
        return false;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!announced() && std::chrono::steady_clock::now() < deadline) {
        other.sendTo(peer::kHere, config.own.announce,
                     takt4::testing::prolink_sim::keepAlive(3, peer::kDevices, mac, 1, 3));
        settle();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(announced());
    for (int i = 0; i < 10; ++i) {
        settle();
    }
    CHECK(controller.window().get_cdj_follow_choices().get() == before.get());
    CHECK(controller.window().get_cdj_follow_choices()->row_count() == 2);

    // Closed, the next redraw writes it.
    press(window, kEscape);
    slint::platform::update_timers_and_animations();
    REQUIRE_FALSE(controller.window().get_cdj_follow_open());
    waitUntil(controller,
              [&] { return controller.window().get_cdj_follow_choices()->row_count() == 3; });
    CHECK(controller.window().get_cdj_follow_choices()->row_count() == 3);
    rig.stop();
}

TEST_CASE("Link's row says what the follower is doing: peers heard and being joined, not reached, "
          "or not to be heard at all",
          "[ui][follow]") {
    // Running, Link's row said "no peers — waiting for a Link session" in red for the second or
    // two a join takes, while the peers were heard; and nothing at all of a join that reached
    // nobody, or of discovery that could not be listened to.
    using Wait = takt4::ui::TimingView::Wait;
    takt4::ui::TimingFacts facts;
    facts.index = 1;
    facts.running = true;
    facts.peers = 3;
    takt4::ui::TimingView view = takt4::ui::describeTiming(facts);
    CHECK(view.waiting);
    CHECK(view.wait == Wait::Coming);
    CHECK(view.waitingWord == "joining");
    CHECK(view.reading == "3 peers heard \xE2\x80\x94 joining\xE2\x80\xA6");
    facts.linkFailedJoins = 2;
    facts.linkRetryIn = 8.0;
    view = takt4::ui::describeTiming(facts);
    CHECK(view.wait == Wait::Absent);
    CHECK(view.waitingWord == "not reached");
    CHECK(view.reading == "3 peers heard, not reached \xE2\x80\x94 trying again in 8 s");
    facts.linkFailedJoins = 0;
    facts.linkRetryIn = 0.0;
    facts.linkPhase = takt4::timing::LinkFollower::Phase::Joining;
    view = takt4::ui::describeTiming(facts);
    CHECK(view.wait == Wait::Coming);
    CHECK(view.reading == "joining the Link session\xE2\x80\xA6");
    facts.linkPhase = takt4::timing::LinkFollower::Phase::Listening;
    facts.canListen = false;
    facts.linkProblem = "no network card is up";
    view = takt4::ui::describeTiming(facts);
    CHECK(view.wait == Wait::Absent);
    CHECK(view.reading == "cannot listen for Link sessions");
    CHECK(view.problem == "no network card is up");
    facts.canListen = true;
    facts.linkProblem.clear();
    facts.coasting = true;
    facts.state.rawBpm = 127.98;
    view = takt4::ui::describeTiming(facts);
    CHECK_FALSE(view.waiting);
    CHECK(view.reading == "no peers \xE2\x80\x94 coasting at 127.98 BPM");
}

namespace {

/// The CDJs' second row on a render of the window as it is now — "network card", the card picker,
/// the reading after it, the note — and the picker's run: the first wider than a label.
std::pair<int, int> cdjCardPicker(WindowController& controller, int& y) {
    const auto row = inputsRow(controller, 117, y);
    INFO("the CDJs' second row: " << spans(row));
    for (const auto& run : row) {
        if (run.second - run.first >= 110) {
            return run;
        }
    }
    FAIL("no card picker on the CDJs' second row");
    return {};
}

constexpr takt4::timing::prolink::Mac kSecondCardMac{0x02, 0x00, 0x00, 0x00, 0x00, 0x03};

/// The machine's cards, as a test has them: the loopback the simulated CDJs are heard on, and a
/// second card on the same network, at 127.0.0.3.
std::vector<takt4::timing::prolink::Interface> twoCards() {
    namespace peer = takt4::testing::prolink_peer;
    std::vector<takt4::timing::prolink::Interface> cards = peer::loopbackCard();
    takt4::timing::prolink::Interface second = cards[0];
    second.name = "second";
    second.ip = peer::kMoved;
    second.mac = kSecondCardMac;
    cards.push_back(second);
    return cards;
}

/// Whether a string handed to the window is one it can show: UTF-8, and nothing that breaks a line
/// or reorders the text around it.
bool fitToShow(const std::string& text) {
    if (!takt4::io::isValidUtf8(text)) {
        return false;
    }
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f) {
            return false;
        }
    }
    for (const char* hidden : {"\xE2\x80\xAE", "\xE2\x80\xAD", "\xE2\x81\xA6", "\xE2\x81\xA9",
                               "\xE2\x80\x8F", "\xE2\x80\xA8", "\xC2\x85"}) {
        if (text.find(hidden) != std::string::npos) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("the CDJs' card list offers only cards that can reach a CDJ, marks where they are heard, "
          "and keeps a card picked that is not one of those, saying why",
          "[ui][prolink]") {
    using takt4::testing::ipv4;
    using takt4::timing::prolink::CardPick;
    using takt4::timing::prolink::Interface;
    using takt4::timing::prolink::Mac;
    using takt4::ui::cdjCardList;
    const auto card = [](const char* name, std::uint32_t ip, std::uint32_t mask, Mac mac,
                         std::uint32_t index) {
        Interface made;
        made.name = name;
        made.ip = ip;
        made.mask = mask;
        made.mac = mac;
        made.index = index;
        return made;
    };
    const Mac cableMac{0x00, 0x1b, 0x21, 0x3a, 0x4c, 0x5d};
    const Mac wifiMac{0x3c, 0x22, 0xfb, 0x01, 0x02, 0x03};
    Interface loopback =
        card("Loopback Pseudo-Interface 1", ipv4(127, 0, 0, 1), 0xff000000u, {}, 1);
    loopback.loopback = true;
    const Interface cable = card("Ethernet 2", ipv4(169, 254, 20, 7), 0xffff0000u, cableMac, 12);
    const Interface vpn = card("WireGuard", ipv4(10, 64, 0, 2), 0xffffffffu, {}, 30);
    const Interface wifi = card("Wi-Fi", ipv4(192, 168, 1, 23), 0xffffff00u, wifiMac, 7);
    const std::vector<Interface> cards{loopback, cable, vpn, wifi};
    const std::string dot = " \xC2\xB7 ";

    // Automatic, the CDJs heard on the cable: the cable and the Wi-Fi offered, the loopback and a
    // network of one address not, and the cable marked.
    auto list = cdjCardList(cards, std::nullopt, cable.ip);
    CHECK(list.entries == std::vector<std::string>{"automatic", "Ethernet 2" + dot + "169.254.20.7",
                                                   "Wi-Fi" + dot + "192.168.1.23"});
    CHECK(list.asides ==
          std::vector<std::string>{"the card the CDJs are heard on", "CDJs heard here", ""});
    CHECK(list.index == 0);
    REQUIRE(list.picks.size() == 3);
    CHECK_FALSE(list.picks[0].has_value());
    CHECK(list.picks[1] == CardPick{cable.ip, cableMac, "Ethernet 2"});
    // Nothing heard: nothing marked.
    CHECK(cdjCardList(cards, std::nullopt, 0).asides[1].empty());

    // A card picked: its own line.
    CHECK(cdjCardList(cards, CardPick{wifi.ip, wifiMac, "Wi-Fi"}, cable.ip).index == 2);
    // The loopback, picked by hand in a file: kept, and said to reach no CDJ — first.
    list = cdjCardList(cards, CardPick{loopback.ip, {}, ""}, cable.ip);
    CHECK(list.entries.back() == "reaches no CDJ: Loopback Pseudo-Interface 1" + dot + "127.0.0.1");
    CHECK(list.index == 3);
    CHECK(list.picks.back() == CardPick{loopback.ip, {}, ""});
    // Another computer's card: kept as it was picked, said not to be here.
    const CardPick elsewhere{ipv4(10, 9, 9, 9), {0x00, 0x0e, 0xc6, 0x11, 0x22, 0x33}, "DJ LAN"};
    list = cdjCardList(cards, elsewhere, cable.ip);
    CHECK(list.entries.back() == "not on this computer: DJ LAN" + dot + "10.9.9.9");
    CHECK(list.index == 3);
    CHECK(list.picks.back() == elsewhere);

    // The cable on a new address: found by its hardware address — its own line lit, no other added.
    std::vector<Interface> moved = cards;
    moved[1].ip = ipv4(169, 254, 99, 3);
    list = cdjCardList(moved, CardPick{cable.ip, cableMac, "Ethernet 2"}, 0);
    REQUIRE(list.entries.size() == 3);
    CHECK(list.entries[static_cast<std::size_t>(list.index)] ==
          "Ethernet 2" + dot + "169.254.99.3");

    // Two cards on one address: the one picked by its hardware address is the one lit, as it is
    // the one a join takes — not the first with the address.
    Interface twin = wifi;
    twin.name = "USB LAN";
    twin.mac = {0x00, 0x24, 0x9b, 0x00, 0x00, 0x05};
    twin.index = 15;
    // Either of them, wherever it is listed.
    list = cdjCardList({wifi, twin}, CardPick{wifi.ip, twin.mac, "USB LAN"}, 0);
    CHECK(list.entries[static_cast<std::size_t>(list.index)] == "USB LAN" + dot + "192.168.1.23");
    list = cdjCardList({wifi, twin}, CardPick{wifi.ip, wifiMac, "Wi-Fi"}, 0);
    CHECK(list.entries[static_cast<std::size_t>(list.index)] == "Wi-Fi" + dot + "192.168.1.23");
}

TEST_CASE("the CDJs' network card is picked with the pointer from this machine's cards, saved with "
          "the desk, and the one START joins on; locked while running; automatic again by the keys",
          "[ui][follow][prolink][slow]") {
    namespace peer = takt4::testing::prolink_peer;
    using takt4::timing::prolink::CardPick;
    using takt4::timing::prolink::VirtualPlayer;
    if (!peer::canUseSecondLoopbackAddress() || !peer::canUseThirdLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2 or 127.0.0.3");
    }
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    peer::SimulatedRig rig;
    takt4::timing::prolink::Network::Config config =
        peer::testConfig(rig.announcePort(), [&controller] { return linkNow(controller); });
    config.interfaces = twoCards;
    controller.useCdjNetworkForTests(config);
    rig.start(config);
    controller.pickTiming(3);
    REQUIRE(laidOutWithInputs(controller).size() == 7);
    const takt4::tests::NothingReal nothingReal;

    // Automatic, the CDJs heard on the loopback: the list names both cards, and marks where they
    // are heard; the reading after the box says which START would join on.
    waitUntil(controller, [&] {
        return std::string(controller.window().get_timing_network()) ==
               "loopback \xC2\xB7 127.0.0.1, at START";
    });
    CHECK(std::string(controller.window().get_timing_network()) ==
          "loopback \xC2\xB7 127.0.0.1, at START");
    const auto choices = controller.window().get_cdj_card_choices();
    REQUIRE(choices->row_count() == 3);
    CHECK(std::string(*choices->row_data(0)) == "automatic");
    CHECK(std::string(*choices->row_data(1)) == "loopback \xC2\xB7 127.0.0.1");
    CHECK(std::string(*choices->row_data(2)) == "second \xC2\xB7 127.0.0.3");
    const auto asides = controller.window().get_cdj_card_asides();
    REQUIRE(asides->row_count() == 3);
    CHECK(std::string(*asides->row_data(1)) == "CDJs heard here");
    CHECK(std::string(*asides->row_data(2)).empty());
    CHECK(controller.window().get_cdj_card_index() == 0);

    // The second card's line of the open list, clicked: found by probing outward from the box,
    // since where a popup opens is Slint's to say. A probe that misses closes the list and sets
    // off nothing under it, so the list is opened again for each; one that picks another card is
    // put back.
    int y = 0;
    const auto box = cdjCardPicker(controller, y);
    const auto clickLine = [&](int wanted) {
        for (int d = 21; d < 700; d += 3) {
            for (const int probe : {y + d, y - d}) {
                if (probe < 100 || probe >= 1480 - 130) {
                    continue;
                }
                if (!controller.window().get_cdj_card_open()) {
                    clickAt(window, middleOf(box), static_cast<float>(y));
                    settle();
                }
                const int before = controller.window().get_cdj_card_index();
                const std::optional<CardPick> pickBefore = controller.cdjCard();
                clickAt(window, middleOf(box), static_cast<float>(probe));
                settle();
                const int after = controller.window().get_cdj_card_index();
                if (after != before && after == wanted) {
                    return true;
                }
                if (after != before) {
                    controller.setCdjCard(pickBefore);
                    settle();
                }
            }
        }
        return false;
    };
    REQUIRE(clickLine(2));
    settle();
    CHECK_FALSE(controller.window().get_cdj_card_open());
    REQUIRE(controller.cdjCard().has_value());
    CHECK(*controller.cdjCard() == CardPick{peer::kMoved, kSecondCardMac, "second"});
    const takt4::settings::MachineSettings saved = controller.currentSettings().machine;
    CHECK(saved.proLinkInterface == "127.0.0.3");
    CHECK(saved.proLinkInterfaceMac == "02:00:00:00:00:03");
    CHECK(saved.proLinkInterfaceName == "second");
    // The box says which card: nothing to read after it.
    waitUntil(controller,
              [&] { return std::string(controller.window().get_timing_network()).empty(); });
    CHECK(std::string(controller.window().get_timing_network()).empty());
    CHECK(controller.window().get_cdj_card_index() == 2);

    // START joins on the card picked, though the CDJs are heard on the other.
    controller.toggleRun();
    waitUntil(controller, [&] {
        return controller.cdjNetwork().snapshot().phase == VirtualPlayer::Phase::Joined;
    });
    REQUIRE(controller.cdjNetwork().snapshot().phase == VirtualPlayer::Phase::Joined);
    CHECK(controller.cdjNetwork().snapshot().interfaceIp == peer::kMoved);
    settle();
    CHECK(std::string(controller.window().get_timing_network()) ==
          "second \xC2\xB7 127.0.0.3 \xC2\xB7 takt4 is player 7");

    // Running, the box is locked: a click opens nothing, and a pick made another way is refused
    // and said.
    const auto locked = cdjCardPicker(controller, y);
    clickAt(window, middleOf(locked), static_cast<float>(y));
    settle();
    CHECK_FALSE(controller.window().get_cdj_card_open());
    controller.pickCdjCard(0);
    settle();
    REQUIRE(controller.cdjCard().has_value());
    CHECK(controller.cdjCard()->ip == peer::kMoved);
    CHECK(controller.window().get_cdj_card_index() == 2);
    CHECK(std::string(controller.window().get_status()).rfind("Stop first", 0) == 0);
    CHECK(controller.cdjNetwork().snapshot().interfaceIp == peer::kMoved);

    // Stopped, automatic picked again with the keys: two lines up from the card picked. The
    // network seen to have left first, so the join waited for below is the next run's.
    controller.toggleRun();
    waitUntil(controller, [&] {
        return !controller.followed().running() &&
               controller.cdjNetwork().snapshot().phase == VirtualPlayer::Phase::Listening;
    });
    REQUIRE_FALSE(controller.followed().running());
    REQUIRE(controller.cdjNetwork().snapshot().phase == VirtualPlayer::Phase::Listening);
    const auto stopped = cdjCardPicker(controller, y);
    clickAt(window, middleOf(stopped), static_cast<float>(y));
    settle();
    REQUIRE(controller.window().get_cdj_card_open());
    press(window, kUp);
    press(window, kUp);
    press(window, "\n");
    settle();
    CHECK_FALSE(controller.cdjCard().has_value());
    CHECK(controller.currentSettings().machine.proLinkInterface.empty());
    CHECK(controller.currentSettings().machine.proLinkInterfaceMac.empty());
    CHECK(controller.currentSettings().machine.proLinkInterfaceName.empty());
    // And START joins where the CDJs are heard.
    controller.toggleRun();
    waitUntil(controller, [&] {
        return controller.cdjNetwork().snapshot().phase == VirtualPlayer::Phase::Joined;
    });
    CHECK(controller.cdjNetwork().snapshot().interfaceIp == peer::kHere);
    controller.toggleRun();
    waitUntil(controller, [&] { return !controller.followed().running(); });
    rig.stop();
    nothingReal.check();
}

TEST_CASE("a card picked on another computer is kept on the card list, said not to be here, and "
          "START joins on the card the CDJs are heard on instead, saying so; picking another "
          "drops it",
          "[ui][follow][prolink][slow]") {
    // A settings file beside a takt4 on a shared drive is every computer's that runs it: the card
    // one of them picked is not on the next, and is still on the one it was picked on.
    namespace peer = takt4::testing::prolink_peer;
    using takt4::timing::prolink::VirtualPlayer;
    if (!peer::canUseSecondLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2");
    }
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings saved;
    saved.machine.proLinkInterface = "10.9.9.9";
    saved.machine.proLinkInterfaceMac = "00:0e:c6:11:22:33";
    saved.machine.proLinkInterfaceName = "DJ LAN";
    WindowController controller(tracker, saved);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    peer::SimulatedRig rig;
    const takt4::timing::prolink::Network::Config config =
        peer::testConfig(rig.announcePort(), [&controller] { return linkNow(controller); });
    controller.useCdjNetworkForTests(config);
    rig.start(config);
    controller.pickTiming(3);
    REQUIRE(laidOutWithInputs(controller).size() == 7);

    waitUntil(controller, [&] {
        return std::string(controller.window().get_timing_network()) ==
               "instead: loopback \xC2\xB7 127.0.0.1, at START";
    });
    CHECK(std::string(controller.window().get_timing_network()) ==
          "instead: loopback \xC2\xB7 127.0.0.1, at START");
    const auto choices = controller.window().get_cdj_card_choices();
    REQUIRE(choices->row_count() == 3);
    CHECK(std::string(*choices->row_data(1)) == "loopback \xC2\xB7 127.0.0.1");
    CHECK(std::string(*choices->row_data(2)) == "not on this computer: DJ LAN \xC2\xB7 10.9.9.9");
    CHECK(controller.window().get_cdj_card_index() == 2);
    // Kept as it was, for the computer it is on.
    CHECK(controller.currentSettings().machine.proLinkInterface == "10.9.9.9");
    CHECK(controller.currentSettings().machine.proLinkInterfaceMac == "00:0e:c6:11:22:33");
    CHECK(controller.currentSettings().machine.proLinkInterfaceName == "DJ LAN");

    // START: joined where they are heard, and said so on the row.
    controller.toggleRun();
    waitUntil(controller, [&] {
        return controller.cdjNetwork().snapshot().phase == VirtualPlayer::Phase::Joined;
    });
    REQUIRE(controller.cdjNetwork().snapshot().phase == VirtualPlayer::Phase::Joined);
    CHECK(controller.cdjNetwork().snapshot().interfaceIp == peer::kHere);
    // The window's next round, which reads what the network now says: a wait looks before it
    // ticks, so the round it ended on was the one before the join.
    settle();
    CHECK(std::string(controller.window().get_timing_note()) ==
          "the card picked for the CDJs, DJ LAN \xC2\xB7 10.9.9.9, is not on this computer "
          "\xE2\x80\x94 joined on loopback \xC2\xB7 127.0.0.1");
    CHECK(std::string(controller.window().get_timing_network()) ==
          "loopback \xC2\xB7 127.0.0.1 \xC2\xB7 takt4 is player 7");
    CHECK(controller.currentSettings().machine.proLinkInterface == "10.9.9.9");
    controller.toggleRun();
    waitUntil(controller, [&] { return !controller.followed().running(); });

    // The loopback picked with the keys, one line up: the other computer's card drops off the
    // list once it has closed.
    int y = 0;
    const auto box = cdjCardPicker(controller, y);
    clickAt(window, middleOf(box), static_cast<float>(y));
    settle();
    REQUIRE(controller.window().get_cdj_card_open());
    press(window, kUp);
    press(window, "\n");
    settle();
    REQUIRE(controller.cdjCard().has_value());
    CHECK(controller.cdjCard()->ip == peer::kHere);
    waitUntil(controller,
              [&] { return controller.window().get_cdj_card_choices()->row_count() == 2; });
    CHECK(controller.window().get_cdj_card_choices()->row_count() == 2);
    CHECK(controller.window().get_cdj_card_index() == 1);
    CHECK(controller.currentSettings().machine.proLinkInterface == "127.0.0.1");
    rig.stop();
}

TEST_CASE(
    "the CDJs' card list is not rebuilt under the pointer while it is open: a card plugged in "
    "is listed once it closes",
    "[ui][follow][prolink]") {
    namespace peer = takt4::testing::prolink_peer;
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.setControlRetrySeconds(0.05);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto cards = std::make_shared<peer::Cards>(peer::loopbackCard());
    takt4::timing::prolink::Network::Config config =
        peer::testConfig(peer::allowedPort(), [&controller] { return linkNow(controller); });
    config.interfaces = [cards] { return cards->get(); };
    controller.useCdjNetworkForTests(config);
    controller.pickTiming(3);
    REQUIRE(laidOutWithInputs(controller).size() == 7);
    waitUntil(controller,
              [&] { return controller.window().get_cdj_card_choices()->row_count() == 2; });
    REQUIRE(controller.window().get_cdj_card_choices()->row_count() == 2);

    int y = 0;
    const auto box = cdjCardPicker(controller, y);
    clickAt(window, middleOf(box), static_cast<float>(y));
    settle();
    REQUIRE(controller.window().get_cdj_card_open());
    const auto before = controller.window().get_cdj_card_choices();

    // A USB card plugged in while the list is open, and listed — many times over at this rate.
    takt4::timing::prolink::Interface usb;
    usb.name = "USB LAN";
    usb.ip = takt4::testing::ipv4(10, 0, 0, 5);
    usb.mask = 0xff000000u;
    usb.broadcast = takt4::testing::ipv4(10, 255, 255, 255);
    usb.mac = {0x00, 0x24, 0x9b, 0x00, 0x00, 0x05};
    std::vector<takt4::timing::prolink::Interface> plugged = peer::loopbackCard();
    plugged.push_back(usb);
    cards->set(plugged);
    for (int i = 0; i < 30; ++i) {
        settle();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(controller.window().get_cdj_card_open());
    CHECK(controller.window().get_cdj_card_choices().get() == before.get());
    CHECK(controller.window().get_cdj_card_choices()->row_count() == 2);

    // Closed, the next redraw writes it.
    press(window, kEscape);
    settle();
    REQUIRE_FALSE(controller.window().get_cdj_card_open());
    waitUntil(controller,
              [&] { return controller.window().get_cdj_card_choices()->row_count() == 3; });
    REQUIRE(controller.window().get_cdj_card_choices()->row_count() == 3);
    CHECK(std::string(*controller.window().get_cdj_card_choices()->row_data(2)) ==
          "USB LAN \xC2\xB7 10.0.0.5");
    CHECK(controller.window().get_cdj_card_index() == 0);
}

TEST_CASE("cards named anything the system allows — bytes that are not UTF-8, line breaks, a "
          "right-to-left override, hundreds of characters — are listed, picked, saved and joined "
          "on without harm",
          "[ui][follow][prolink][slow]") {
    // A byte that is not UTF-8, handed to the window, aborts the process: a test that gets to its
    // end has shown every one of these.
    namespace peer = takt4::testing::prolink_peer;
    using takt4::timing::prolink::VirtualPlayer;
    if (!peer::canUseSecondLoopbackAddress() || !peer::canUseThirdLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2 or 127.0.0.3");
    }
    std::string euros;
    for (int i = 0; i < 300; ++i) {
        euros += "\xE2\x82\xAC";
    }
    const auto strange = [euros] {
        std::vector<takt4::timing::prolink::Interface> cards = twoCards();
        cards[0].name = std::string("eth\xff\xfe\r\n\xE2\x80\xAE"
                                    "NAL JD");
        cards[1].name = euros;
        return cards;
    };
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    peer::SimulatedRig rig;
    takt4::timing::prolink::Network::Config config =
        peer::testConfig(rig.announcePort(), [&controller] { return linkNow(controller); });
    config.interfaces = strange;
    controller.useCdjNetworkForTests(config);
    rig.start(config);
    controller.pickTiming(3);
    REQUIRE(laidOutWithInputs(controller).size() == 7);

    const std::string first = "eth\xEF\xBF\xBD\xEF\xBF\xBD NAL JD";
    std::string cut;
    for (std::size_t i = 0; i + 1 < takt4::timing::prolink::kCardNameLength; ++i) {
        cut += "\xE2\x82\xAC";
    }
    cut += "\xE2\x80\xA6";
    waitUntil(controller, [&] {
        return std::string(controller.window().get_timing_network()) ==
               first + " \xC2\xB7 127.0.0.1, at START";
    });
    CHECK(std::string(controller.window().get_timing_network()) ==
          first + " \xC2\xB7 127.0.0.1, at START");
    const auto choices = controller.window().get_cdj_card_choices();
    REQUIRE(choices->row_count() == 3);
    CHECK(std::string(*choices->row_data(1)) == first + " \xC2\xB7 127.0.0.1");
    CHECK(std::string(*choices->row_data(2)) == cut + " \xC2\xB7 127.0.0.3");
    for (std::size_t i = 0; i < choices->row_count(); ++i) {
        INFO("entry " << i);
        CHECK(fitToShow(std::string(*choices->row_data(i))));
    }

    // The box at the window's narrowest: no wider than it may be, the long name cut inside it.
    int y = 0;
    const auto box = cdjCardPicker(controller, y);
    clickAt(window, middleOf(box), static_cast<float>(y));
    settle();
    REQUIRE(controller.window().get_cdj_card_open());
    press(window, kDown);
    press(window, kDown);
    press(window, "\n");
    settle();
    REQUIRE(controller.cdjCard().has_value());
    CHECK(controller.cdjCard()->name == cut);
    const auto picked = cdjCardPicker(controller, y);
    CHECK(picked.second - picked.first + 1 >= 120);
    CHECK(picked.second - picked.first + 1 <= 262);

    // Saved and read back: the same card, the name as it was made fit.
    const takt4::test::TempDir dir;
    const std::filesystem::path file = dir.path() / "settings.json";
    REQUIRE(takt4::settings::save(controller.currentSettings(), file));
    const takt4::settings::Settings back = takt4::settings::load(file);
    CHECK(back.machine.proLinkInterface == "127.0.0.3");
    CHECK(back.machine.proLinkInterfaceMac == "02:00:00:00:00:03");
    CHECK(back.machine.proLinkInterfaceName == cut);

    // Joined on it, and named on the row.
    controller.toggleRun();
    waitUntil(controller, [&] {
        return controller.cdjNetwork().snapshot().phase == VirtualPlayer::Phase::Joined;
    });
    REQUIRE(controller.cdjNetwork().snapshot().interfaceIp == peer::kMoved);
    settle();
    const std::string joined(controller.window().get_timing_network());
    CHECK(joined == cut + " \xC2\xB7 127.0.0.3 \xC2\xB7 takt4 is player 7");
    CHECK(fitToShow(joined));
    controller.toggleRun();
    waitUntil(controller, [&] { return !controller.followed().running(); });
    rig.stop();
}

TEST_CASE("an IMPORT naming a card picks it, here or not, and one naming none puts automatic back",
          "[ui][follow][prolink][settings]") {
    using takt4::timing::prolink::CardPick;
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.pickTiming(3);
    const takt4::test::TempDir dir;
    takt4::settings::Settings other = controller.currentSettings();
    other.machine.proLinkInterface = "10.9.9.9";
    other.machine.proLinkInterfaceMac = "00:0e:c6:11:22:33";
    other.machine.proLinkInterfaceName = "DJ LAN";
    const std::filesystem::path file = dir.path() / "other.json";
    REQUIRE(takt4::settings::save(other, file));
    REQUIRE(controller.importFrom(file));
    REQUIRE(controller.cdjCard().has_value());
    CHECK(*controller.cdjCard() == CardPick{takt4::testing::ipv4(10, 9, 9, 9),
                                            {0x00, 0x0e, 0xc6, 0x11, 0x22, 0x33},
                                            "DJ LAN"});
    controller.tick();
    slint::platform::update_timers_and_animations();
    const auto choices = controller.window().get_cdj_card_choices();
    const int picked = controller.window().get_cdj_card_index();
    REQUIRE(picked >= 1);
    CHECK(std::string(*choices->row_data(static_cast<std::size_t>(picked))) ==
          "not on this computer: DJ LAN \xC2\xB7 10.9.9.9");

    other.machine.proLinkInterface.clear();
    REQUIRE(takt4::settings::save(other, file));
    REQUIRE(controller.importFrom(file));
    CHECK_FALSE(controller.cdjCard().has_value());
    controller.tick();
    slint::platform::update_timers_and_animations();
    CHECK(controller.window().get_cdj_card_index() == 0);
}

TEST_CASE("the folded inputs heading says what a followed source is waiting for, and calls the "
          "MIDI input beside a clock the MIDI control",
          "[ui][follow]") {
    // Folded, the heading turned red for a clock that had stopped coming and said nothing of why,
    // and "MIDI:" beside "MIDI clock on DJM-A9" read as the clock's input said twice.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    auto& window = controller.window();
    const std::string dot = " \xC2\xB7 ";
    std::string summary(window.get_inputs_summary());
    INFO(summary);
    CHECK(summary.rfind("timing: audio" + dot + "MIDI: ", 0) == 0);

    takt4::ui::TimingFacts clock;
    clock.index = 2;
    clock.port = "DJM-A9";
    takt4::ui::publishClockPorts(window, {"DJM-A9"}, "DJM-A9");
    takt4::ui::publishTimingView(window, takt4::ui::describeTiming(clock));
    summary = std::string(window.get_inputs_summary());
    INFO(summary);
    CHECK(summary.rfind("timing: MIDI clock on DJM-A9" + dot + "MIDI control: ", 0) == 0);
    // Running, and no clock coming: what is wrong, on the heading it turns red.
    clock.running = true;
    window.set_running(true);
    takt4::ui::publishTimingView(window, takt4::ui::describeTiming(clock));
    summary = std::string(window.get_inputs_summary());
    INFO(summary);
    const std::string noClock = "timing: MIDI clock on DJM-A9 \xE2\x80\x94 no clock";
    CHECK(summary.rfind(noClock + dot + "MIDI control: ", 0) == 0);
    window.set_running(false);
}

TEST_CASE("STOP while following Link listens again for the peers the stopped row lists",
          "[ui][follow]") {
    // The follower closes the listener as its run ends, and only picking Link opened it: after
    // every STOP the row said "cannot listen for Link sessions" and counted no peers. In the
    // sandbox the open is refused, and counted, which is how it is seen to be asked for.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::testing::FakeGridSource source([&controller] { return linkNow(controller); }, 120.0,
                                          static_cast<double>(linkNow(controller)));
    controller.followForTests(&source);
    controller.pickTiming(1);
    controller.toggleRun();
    REQUIRE(controller.followed().running());
    std::uint64_t asked = takt4::sandbox::refusals(takt4::sandbox::Refused::Link);
    controller.toggleRun();
    REQUIRE_FALSE(controller.followed().running());
    CHECK(takt4::sandbox::refusals(takt4::sandbox::Refused::Link) == asked + 1);
    // Not after a run on another source, which has no such row.
    controller.pickTiming(2);
    controller.toggleRun();
    REQUIRE(controller.followed().running());
    asked = takt4::sandbox::refusals(takt4::sandbox::Refused::Link);
    controller.toggleRun();
    REQUIRE_FALSE(controller.followed().running());
    CHECK(takt4::sandbox::refusals(takt4::sandbox::Refused::Link) == asked);
    controller.followForTests(nullptr);
}

// --- remote status and control, from a show controller -----------------------------------------

namespace {

/// The window listening for OSC control on a port the system picks, with remote setup on — what a
/// show controller on this machine talks to.
void listenForRemote(WindowController& controller) {
    controller.window().invoke_osc_control_port_edited(slint::SharedString("0"));
    controller.window().invoke_osc_control_toggled(true);
    REQUIRE(controller.oscControl().running());
    controller.setRemoteSetup(true);
    controller.tick(); // the window's status written: one that has not is busy
}

/// `message` sent from `showController` to the window's control port, and the window run as its
/// timer would until an answer comes back.
std::vector<takt4::testing::OscMsg> ask(WindowController& controller,
                                        takt4::testing::LoopbackReceiver& showController,
                                        takt4::output::OscMessage message) {
    REQUIRE(showController.send(controller.oscControlPort(), message.packet()));
    std::vector<takt4::testing::OscMsg> answer;
    waitUntil(controller, [&] {
        if (showController.ready(0)) {
            answer = takt4::testing::oscMessagesIn(showController.receive());
        }
        return !answer.empty();
    });
    return answer;
}

/// A `/takt4/reply` as its request number, "ok" or "refused", and its note; a request number of -1
/// for anything else.
struct Reply {
    std::int32_t seq = -1;
    std::string verdict;
    std::string note;
};

Reply replyIn(const std::vector<takt4::testing::OscMsg>& messages) {
    if (messages.size() != 1 || messages[0].address != "/takt4/reply" ||
        messages[0].args.size() != 3) {
        return {};
    }
    return {messages[0].args[0].i, messages[0].args[1].s, messages[0].args[2].s};
}

/// `address` with a string or two and a request number, as a show controller sends a change.
/// A literal address only: the message keeps a view of it.
takt4::output::OscMessage
numbered(const char* address, std::initializer_list<std::string_view> words, std::int32_t seq) {
    takt4::output::OscMessage message(address);
    for (const std::string_view word : words) {
        message.addString(word);
    }
    message.addInt(seq);
    return message;
}

} // namespace

TEST_CASE("whatever a show controller sends, the window stays up, answers and saves what it was "
          "left with",
          "[ui][remote][fuzz]") {
    // Everything on the control port came off the network. Remote setup on, which is when a
    // message can reach the window's own functions: well-formed changes, half-right ones, every
    // argument of every type in every place, strings that are not UTF-8 — which Slint aborts the
    // whole process on — and bytes that are not OSC at all. The bar is the show's: takt4 still up
    // afterwards, still answering, and what it was left with saved and read back.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    listenForRemote(controller);
    takt4::testing::LoopbackReceiver showController;
    const std::filesystem::path file = takt4::settings::settingsFile();
    REQUIRE_FALSE(file.empty());

    // A longer campaign, or another one: TAKT4_FUZZ_MESSAGES and TAKT4_FUZZ_SEED.
    const auto envNumber = [](const char* name, std::uint32_t otherwise) {
        std::string text;
#if defined(_MSC_VER)
        char buffer[32] = {};
        std::size_t length = 0;
        if (getenv_s(&length, buffer, sizeof buffer, name) == 0 && length > 1) {
            text = buffer;
        }
#else
        if (const char* value = std::getenv(name)) {
            text = value;
        }
#endif
        return text.empty() ? otherwise : static_cast<std::uint32_t>(std::stoul(text));
    };
    const std::uint32_t messages = envNumber("TAKT4_FUZZ_MESSAGES", 400);
    std::uint32_t state = envNumber("TAKT4_FUZZ_SEED", 0x9E3779B9U) | 1U;
    INFO("seed " << state);
    const auto next = [&state] {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    };
    const auto pick = [&next](std::size_t count) {
        return static_cast<std::size_t>(next() % count);
    };

    const std::array<std::string_view, 28> verbs = {"status",
                                                    "audio/devices",
                                                    "outputs",
                                                    "audio/start",
                                                    "audio/stop",
                                                    "audio/restart",
                                                    "audio/device",
                                                    "audio/channel",
                                                    "source",
                                                    "output/add",
                                                    "output/1/enable",
                                                    "output/1/delay",
                                                    "output/1/global",
                                                    "output//enable",
                                                    "output/",
                                                    "output/\xff/delay",
                                                    "tap",
                                                    "downbeat",
                                                    "tempo/halve",
                                                    "tempo/double",
                                                    "lock",
                                                    "panic",
                                                    "panic/release",
                                                    "manual",
                                                    "rule/x/enable",
                                                    "",
                                                    "audio",
                                                    "bar"};
    const std::array<std::string, 22> words = {"",
                                               "audio",
                                               "link",
                                               "prolink",
                                               "midiclock",
                                               "osc",
                                               "artnet",
                                               "127.0.0.1",
                                               "256.1.1.1",
                                               "localhost",
                                               "::1",
                                               "\xff\xfe",
                                               "\xc0\x80",
                                               "\xed\xa0\x80",
                                               "abc\x80",
                                               "\xf4\x90\x80\x80",
                                               "\t\r\n\x01",
                                               "\xF0\x9F\x8E\xB5 \xE2\x80\xAE",
                                               "a/b%s%n",
                                               std::string(200, 'A'),
                                               std::string(300, '\xff'),
                                               "ASIO"};
    const std::array<std::int32_t, 9> ints = {0,
                                              1,
                                              -1,
                                              2,
                                              65535,
                                              65536,
                                              std::numeric_limits<std::int32_t>::min(),
                                              std::numeric_limits<std::int32_t>::max(),
                                              7};
    const std::array<float, 9> floats = {0.0f,
                                         1.0f,
                                         -1.0f,
                                         std::numeric_limits<float>::quiet_NaN(),
                                         std::numeric_limits<float>::infinity(),
                                         -std::numeric_limits<float>::infinity(),
                                         1e30f,
                                         1e-40f,
                                         0.5f};

    // A message byte by byte, so any type tag can be sent — T and F included, which the
    // builder takt4 sends with has no call for.
    const auto padded = [](std::vector<std::byte>& out, std::string_view text) {
        for (const char c : text) {
            out.push_back(static_cast<std::byte>(c));
        }
        do {
            out.push_back(std::byte{0});
        } while (out.size() % 4 != 0);
    };
    const auto word32 = [](std::vector<std::byte>& out, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    };
    // The ids of the outputs the window has now, so a change can name a real one.
    const auto someId = [&]() -> std::string {
        const auto rows = controller.window().get_outputs_list();
        if (rows->row_count() == 0 || pick(4) == 0) {
            return "o-" + std::to_string(next() % 100);
        }
        return std::string(rows->row_data(pick(rows->row_count()))->id);
    };
    // Half are the shape a change takes — the right types in the right places — with values
    // chosen to hurt; the rest anything at all.
    const auto shaped = [&]() {
        std::vector<std::byte> out;
        std::string tags = ",";
        std::vector<std::byte> payload;
        const auto text = [&] {
            tags += 's';
            padded(payload, words[pick(words.size())]);
        };
        const auto whole = [&] {
            tags += 'i';
            word32(payload, static_cast<std::uint32_t>(ints[pick(ints.size())]));
        };
        const auto real = [&] {
            tags += 'f';
            std::uint32_t bits = 0;
            const float value = floats[pick(floats.size())];
            std::memcpy(&bits, &value, sizeof bits);
            word32(payload, bits);
        };
        const auto seq = [&] {
            if (pick(2) == 0) {
                tags += 'i';
                word32(payload, next() % 1000);
            }
        };
        std::string address = "/takt4/ctl/";
        switch (pick(8)) {
        case 0:
            address +=
                std::array<const char*, 3>{"audio/start", "audio/stop", "audio/restart"}[pick(3)];
            seq();
            break;
        case 1:
            address += "audio/device";
            text();
            text();
            seq();
            break;
        case 2:
            address += "audio/channel";
            whole();
            if (pick(2) == 0) {
                tags += 'i';
                word32(payload, next() % 2);
            }
            seq();
            break;
        case 3:
            address += "source";
            text();
            seq();
            break;
        case 4:
        case 5:
            address += "output/add";
            text();
            text();
            text();
            whole();
            seq();
            break;
        case 6:
            address += "output/" + someId() + "/delay";
            real();
            seq();
            break;
        default:
            address += "output/" + someId() + (pick(2) == 0 ? "/enable" : "/global");
            tags += 'i';
            word32(payload, next() % 2);
            seq();
            break;
        }
        padded(out, address);
        padded(out, tags);
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    };
    const auto message = [&]() {
        if (pick(2) == 0) {
            return shaped();
        }
        std::vector<std::byte> out;
        const std::size_t roll = pick(20);
        if (roll == 0) {
            // Not OSC at all.
            const std::size_t length = pick(300);
            for (std::size_t i = 0; i < length; ++i) {
                out.push_back(static_cast<std::byte>(next() & 0xFFU));
            }
            return out;
        }
        std::string address = roll == 1 ? "/takt4/" : roll == 2 ? "/elsewhere/ctl/" : "/takt4/ctl/";
        address += verbs[pick(verbs.size())];
        padded(out, address);
        std::string tags = ",";
        std::vector<std::byte> payload;
        const std::size_t count = pick(7);
        for (std::size_t a = 0; a < count; ++a) {
            switch (pick(5)) {
            case 0:
                tags += 'i';
                word32(payload, static_cast<std::uint32_t>(ints[pick(ints.size())]));
                break;
            case 1: {
                tags += 'f';
                std::uint32_t bits = 0;
                const float value = floats[pick(floats.size())];
                std::memcpy(&bits, &value, sizeof bits);
                word32(payload, bits);
                break;
            }
            case 2:
                tags += 's';
                padded(payload, words[pick(words.size())]);
                break;
            case 3:
                tags += 'T';
                break;
            default:
                tags += 'F';
                break;
            }
        }
        padded(out, tags);
        out.insert(out.end(), payload.begin(), payload.end());
        if (pick(10) == 0 && !out.empty()) {
            // A byte of a well-formed message changed in flight.
            out[pick(out.size())] = static_cast<std::byte>(next() & 0xFFU);
        }
        return out;
    };

    const auto seen = [&controller] {
        const auto& osc = controller.oscControl();
        return osc.handled() + osc.ignored() + osc.queriesAnswered() + osc.commandsQueued() +
               osc.remoteRefused();
    };
    for (std::uint32_t sent = 0; sent < messages; ++sent) {
        const std::vector<std::byte> packet = message();
        if (packet.empty()) {
            continue;
        }
        const std::uint64_t before = seen();
        REQUIRE(showController.send(controller.oscControlPort(), packet));
        // Until the control thread has counted it, and the window has carried out whatever it
        // queued; the answers drained so the sender's socket never fills.
        for (int turn = 0; turn < 40 && (turn < 2 || seen() == before); ++turn) {
            controller.tick();
            slint::platform::update_timers_and_animations();
            while (showController.ready(0)) {
                (void)showController.receive();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    for (int turn = 0; turn < 20; ++turn) {
        controller.tick();
        slint::platform::update_timers_and_animations();
    }
    while (showController.ready(0)) {
        (void)showController.receive();
    }
    INFO(controller.oscControl().handled()
         << " handled, " << controller.oscControl().ignored() << " ignored, "
         << controller.oscControl().commandsQueued() << " changes queued, "
         << controller.oscControl().remoteRefused() << " refused");
    // Every one of them was looked at: none slipped past the counts.
    CHECK(controller.oscControl().commandsQueued() > 0);
    CHECK(controller.oscControl().remoteRefused() > 0);

    // Still answering, the status it gives is well-formed, and what it was left with saves and
    // reads back — every name in it UTF-8, whatever bytes it arrived as.
    takt4::output::OscMessage status("/takt4/ctl/status");
    CHECK_FALSE(ask(controller, showController, status).empty());
    controller.setRemoteSetup(false);
    REQUIRE(controller.saveNow());
    const takt4::settings::Settings saved = takt4::settings::load(file);
    for (const auto& output : saved.preset.outputs) {
        INFO("output " << output.id);
        CHECK(takt4::io::validUtf8(output.name) == output.name);
        CHECK(takt4::io::validUtf8(output.host) == output.host);
    }
}

TEST_CASE("a show controller's change of source while the CDJs' lists are open holds neither list "
          "back afterwards",
          "[ui][follow][prolink][remote]") {
    // Each list's box goes with the CDJs' rows, open or not, and never says it closed: its flag
    // stayed set, and every later list was held back as if it were still open under the pointer.
    namespace peer = takt4::testing::prolink_peer;
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.setControlRetrySeconds(0.05);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto cards = std::make_shared<peer::Cards>(peer::loopbackCard());
    takt4::timing::prolink::Network::Config config =
        peer::testConfig(peer::allowedPort(), [&controller] { return linkNow(controller); });
    config.interfaces = [cards] { return cards->get(); };
    controller.useCdjNetworkForTests(config);
    listenForRemote(controller);
    takt4::testing::LoopbackReceiver showController;
    controller.pickTiming(3);
    REQUIRE(laidOutWithInputs(controller).size() == 7);
    waitUntil(controller,
              [&] { return controller.window().get_cdj_card_choices()->row_count() == 2; });
    REQUIRE(controller.window().get_cdj_card_choices()->row_count() == 2);

    // One list or the other held open.
    int y = 0;
    SECTION("the card list") {
        const auto box = cdjCardPicker(controller, y);
        clickAt(window, middleOf(box), static_cast<float>(y));
        settle();
        REQUIRE(controller.window().get_cdj_card_open());
    }
    SECTION("the follow list") {
        const auto row = inputsRow(controller, 73, y);
        INFO("timing row, the CDJs: " << spans(row));
        REQUIRE(row.size() == 6);
        clickAt(window, middleOf(row[4]), static_cast<float>(y));
        settle();
        REQUIRE(controller.window().get_cdj_follow_open());
    }

    // Away to the audio and back, over the wire — and the window drawn after each, as it is live:
    // drawing is what takes the CDJs' rows, and an open list with them, away.
    const auto draw = [&controller] { (void)takt4::tests::render(controller.window(), 800, 1480); };
    CHECK(replyIn(ask(controller, showController, numbered("/takt4/ctl/source", {"audio"}, 1)))
              .verdict == "ok");
    settle();
    draw();
    CHECK(controller.timingIndex() == 0);
    CHECK(replyIn(ask(controller, showController, numbered("/takt4/ctl/source", {"prolink"}, 2)))
              .verdict == "ok");
    settle();
    draw();
    REQUIRE(controller.timingIndex() == 3);
    CHECK_FALSE(controller.window().get_cdj_card_open());
    CHECK_FALSE(controller.window().get_cdj_follow_open());

    // A card plugged in now is listed, with nobody opening or closing anything.
    takt4::timing::prolink::Interface usb = peer::loopbackCard()[0];
    usb.name = "USB LAN";
    usb.ip = takt4::testing::ipv4(10, 0, 0, 5);
    usb.mask = 0xff000000u;
    usb.mac = {0x00, 0x24, 0x9b, 0x00, 0x00, 0x05};
    std::vector<takt4::timing::prolink::Interface> plugged = peer::loopbackCard();
    plugged.push_back(usb);
    cards->set(plugged);
    waitUntil(controller,
              [&] { return controller.window().get_cdj_card_choices()->row_count() == 3; });
    CHECK(controller.window().get_cdj_card_choices()->row_count() == 3);
    // And the card list opens on the first click — nothing left open swallows it — and closes.
    const auto again = cdjCardPicker(controller, y);
    clickAt(window, middleOf(again), static_cast<float>(y));
    settle();
    CHECK(controller.window().get_cdj_card_open());
    press(window, kEscape);
    settle();
    CHECK_FALSE(controller.window().get_cdj_card_open());
}

TEST_CASE("remote setup has a line of its own under the OSC row while listening, and a list of "
          "senders with a mistake in it changes nothing",
          "[ui][remote]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.window().set_inputs_folded(false);
    auto& window = controller.window().window();
    const auto inputsSheet = [&controller](const takt4::tests::Shot& shot) {
        const int osc = static_cast<int>(controller.window().get_osc_row_y());
        std::pair<int, int> found{-1, -1};
        for (const auto& sheet : sheetsDown(shot)) {
            if (sheet.first <= osc && osc <= sheet.second) {
                found = sheet;
            }
        }
        REQUIRE(found.first >= 0);
        return found;
    };

    // Not listening: nothing to set up, so nothing offered.
    const takt4::tests::Shot off = takt4::tests::render(controller.window(), 800, 1480);
    const std::pair<int, int> sheetOff = inputsSheet(off);

    controller.window().invoke_osc_control_port_edited(slint::SharedString("0"));
    controller.window().invoke_osc_control_toggled(true);
    REQUIRE(controller.oscControl().running());
    controller.tick();
    const takt4::tests::Shot on = takt4::tests::render(controller.window(), 800, 1480);
    const std::pair<int, int> sheetOn = inputsSheet(on);
    // A line of its own, 38 px and the section's spacing, under the OSC row: the section that
    // much taller (the plot above gives the room up, the window being the size it is).
    INFO("sheets off: " << spans(sheetsDown(off)) << "; on: " << spans(sheetsDown(on))
                        << "; the OSC row at " << controller.window().get_osc_row_y());
    CHECK((sheetOn.second - sheetOn.first) - (sheetOff.second - sheetOff.first) == 48);
    const int line = static_cast<int>(controller.window().get_osc_row_y()) + 38 + 10 + 19;
    const auto parts = occupied(on, line - 9, line + 9, 20, 780, kSheet, 5);
    INFO("the remote setup line: " << spans(parts));
    // The tick's box and its words, "only from", its box and what it allows.
    REQUIRE(parts.size() == 5);
    CHECK(parts[0].first >= 74);

    // Ticked by a click on its box — and said on the inputs heading, folded or not.
    CHECK_FALSE(controller.remoteSetup());
    CHECK(std::string(controller.window().get_inputs_summary()).find("remote setup") ==
          std::string::npos);
    clickAt(window, static_cast<float>(parts[0].first + 10), static_cast<float>(line));
    controller.tick();
    CHECK(controller.remoteSetup());
    CHECK(std::string(controller.window().get_inputs_summary())
              .find("OSC: listening on port " + std::to_string(controller.oscControlPort()) +
                    ", remote setup on") != std::string::npos);
    CHECK(controller.oscControl().remoteSetup().allowed);
    CHECK(controller.window().get_osc_remote_setup());
    CHECK(controller.currentSettings().machine.oscRemoteSetup);

    // The list is for other machines, so it is open once they are allowed.
    controller.setOscControlNetwork(true);
    controller.tick();
    const auto typeInto = [&](const std::string& text) {
        clickAt(window, middleOf(parts[3]), static_cast<float>(line));
        for (const char c : text) {
            press(window, std::string(1, c));
        }
        press(window, "\n");
        controller.tick();
        slint::platform::update_timers_and_animations();
    };

    // A letter O for a nought: refused whole, and said — what is left would have been empty,
    // and an empty list lets every sender in.
    typeInto("192.168.1.9O");
    CHECK(controller.remoteFrom().empty());
    CHECK(controller.oscControl().remoteSetup().from.empty());
    CHECK(std::string(controller.window().get_osc_remote_from()).empty());
    CHECK(controller.statusIsError());
    {
        const std::string said(controller.window().get_status());
        INFO(said);
        CHECK(said.find("\"192.168.1.9O\" is not an address") != std::string::npos);
        CHECK(said.find("the list is as it was") != std::string::npos);
    }

    // Two addresses, as they are typed: kept as the window keeps them, and in force.
    typeInto("192.168.1.50  10.0.0.2");
    CHECK(controller.remoteFrom() == "192.168.1.50, 10.0.0.2");
    CHECK(std::string(controller.window().get_osc_remote_from()) == "192.168.1.50, 10.0.0.2");
    CHECK(controller.oscControl().remoteSetup().from ==
          std::vector<std::uint32_t>{0xC0A80132u, 0x0A000002u});
    CHECK_FALSE(controller.statusIsError());
    CHECK(std::string(controller.window().get_status()) ==
          "Remote setup from 192.168.1.50, 10.0.0.2 and this machine only.");
    CHECK(controller.currentSettings().machine.oscRemoteFrom == "192.168.1.50, 10.0.0.2");

    // And a typo after that leaves those two as they were.
    controller.setRemoteFrom("192.168.1.50, 10.0.0");
    CHECK(controller.remoteFrom() == "192.168.1.50, 10.0.0.2");
    CHECK(controller.oscControl().remoteSetup().from.size() == 2);
}

TEST_CASE("a show controller's change over the wire is carried out by the window's own "
          "functions, said with who asked, and answered",
          "[ui][remote]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    listenForRemote(controller);
    takt4::testing::LoopbackReceiver showController;

    const Reply reply =
        replyIn(ask(controller, showController, numbered("/takt4/ctl/source", {"link"}, 3)));
    CHECK(reply.seq == 3);
    CHECK(reply.verdict == "ok");
    CHECK(reply.note == "timing from Ableton Link");
    // As the picker would have: the picker shows it, and it is what is saved.
    CHECK(controller.timingIndex() == 1);
    CHECK(controller.window().get_timing_index() == 1);
    CHECK(controller.currentSettings().machine.timingSource == "link");
    {
        const std::string said(controller.window().get_status());
        INFO(said);
        CHECK(said == "Remote, from 127.0.0.1:" + std::to_string(showController.port()) +
                          ": /takt4/ctl/source \xE2\x80\x94 timing from Ableton Link");
        CHECK_FALSE(controller.statusIsError());
    }

    SECTION("a source takt4 does not know is refused, and nothing changes") {
        const Reply refused = replyIn(
            ask(controller, showController, numbered("/takt4/ctl/source", {"midiclock"}, 4)));
        CHECK(refused.verdict == "refused");
        CHECK(refused.note.find("midi-clock") != std::string::npos);
        CHECK(controller.timingIndex() == 1);
        CHECK(controller.statusIsError());
    }

    SECTION("without a request number, a change is carried out and only a refusal is said") {
        takt4::output::OscMessage back("/takt4/ctl/source");
        back.addString("audio");
        REQUIRE(showController.send(controller.oscControlPort(), back.packet()));
        waitUntil(controller, [&] { return controller.timingIndex() == 0; });
        CHECK(controller.timingIndex() == 0);
        CHECK(showController.receive().empty());
    }

    SECTION("remote setup off, nothing changes, and the sender is told how to allow it") {
        controller.setRemoteSetup(false);
        const Reply refused =
            replyIn(ask(controller, showController, numbered("/takt4/ctl/source", {"audio"}, 5)));
        CHECK(refused.verdict == "refused");
        CHECK(refused.note.find("allow remote setup") != std::string::npos);
        CHECK(controller.timingIndex() == 1);
    }

    SECTION("an input and a source are changed only while stopped") {
        takt4::testing::FakeGridSource source([&controller] { return linkNow(controller); }, 120.0,
                                              static_cast<double>(linkNow(controller)));
        controller.followForTests(&source);
        controller.toggleRun();
        REQUIRE(controller.followed().running());
        const Reply refused =
            replyIn(ask(controller, showController, numbered("/takt4/ctl/source", {"audio"}, 6)));
        CHECK(refused.verdict == "refused");
        CHECK(refused.note.rfind("stop first", 0) == 0);
        CHECK(controller.timingIndex() == 1);
        takt4::output::OscMessage channel("/takt4/ctl/audio/channel");
        channel.addInt(1);
        channel.addInt(1);
        channel.addInt(7);
        const Reply notNow = replyIn(ask(controller, showController, channel));
        CHECK(notNow.verdict == "refused");
        CHECK(notNow.note.rfind("stop first", 0) == 0);
        controller.toggleRun();
        controller.followForTests(nullptr);
    }
}

TEST_CASE("START and STOP from the network are switches, answered once the run has settled",
          "[ui][remote][follow]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::testing::FakeGridSource source([&controller] { return linkNow(controller); }, 120.0,
                                          static_cast<double>(linkNow(controller)));
    controller.followForTests(&source);
    controller.pickTiming(1);
    listenForRemote(controller);
    takt4::testing::LoopbackReceiver showController;
    const auto askRun = [&](const char* address, std::int32_t seq) {
        takt4::output::OscMessage message(address);
        message.addInt(seq);
        return replyIn(ask(controller, showController, message));
    };

    const Reply started = askRun("/takt4/ctl/audio/start", 4);
    CHECK(started.seq == 4);
    CHECK(started.verdict == "ok");
    CHECK(started.note == "running");
    CHECK(controller.followed().running());
    CHECK(controller.window().get_running());

    // Again: running already, and still running — a switch, never a toggle.
    const Reply again = askRun("/takt4/ctl/audio/start", 5);
    CHECK(again.verdict == "ok");
    CHECK(again.note == "running already");
    CHECK(controller.followed().running());

    // STOP, once the button takes presses again, as it does for a hand.
    waitUntil(controller, [&] { return !controller.window().get_run_busy(); });
    const Reply stopped = askRun("/takt4/ctl/audio/stop", 6);
    CHECK(stopped.verdict == "ok");
    CHECK(stopped.note == "stopped");
    CHECK_FALSE(controller.followed().running());
    CHECK_FALSE(controller.window().get_running());
    const Reply stillStopped = askRun("/takt4/ctl/audio/stop", 7);
    CHECK(stillStopped.note == "stopped already");

    SECTION("a START the window cannot carry out is refused with what the window says") {
        controller.pickTiming(2); // a MIDI clock, with no input picked for it
        controller.tick();
        REQUIRE_FALSE(controller.window().get_timing_ready());
        const Reply refused = askRun("/takt4/ctl/audio/start", 8);
        CHECK(refused.verdict == "refused");
        CHECK(refused.note ==
              "it cannot start: Pick the MIDI input the clock comes in on, then press Start.");
        CHECK_FALSE(controller.followed().running());
    }
    controller.followForTests(nullptr);
}

TEST_CASE("an output added from the network is a row like one added in the window, changed by "
          "its id, and hears the heartbeat once it takes the global messages",
          "[ui][remote]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    listenForRemote(controller);
    takt4::testing::LoopbackReceiver showController;
    takt4::testing::LoopbackReceiver studio; // where the new output sends

    takt4::output::OscMessage add("/takt4/ctl/output/add");
    add.addString("show controller");
    add.addString("osc");
    add.addString("127.0.0.1");
    add.addInt(studio.port());
    add.addInt(7);
    const Reply added = replyIn(ask(controller, showController, add));
    REQUIRE(added.verdict == "ok");
    const std::string id = added.note;
    CHECK(id.rfind("o-", 0) == 0);
    const std::optional<OutputRow> row = outputRow(controller, id);
    REQUIRE(row.has_value());
    CHECK(std::string(row->name) == "show controller");
    CHECK(std::string(row->host) == "127.0.0.1");
    CHECK(std::string(row->port) == std::to_string(studio.port()));
    CHECK(row->kind_index == 0);
    CHECK(row->enabled);
    CHECK_FALSE(row->sends_namespace);
    // And on the output thread, as the window's own rows are.
    bool running = false;
    for (const takt4::output::OutputTarget& target : seen(controller).targets) {
        running = running || (target.id == id && target.port == studio.port());
    }
    CHECK(running);

    // Ticked for takt4's own messages: the heartbeat reaches it. (Each address held by name: an
    // `OscMessage` keeps a view of its address, which a temporary would leave dangling.)
    const std::string globalAddress = "/takt4/ctl/output/" + id + "/global";
    takt4::output::OscMessage global(globalAddress);
    global.addInt(1);
    global.addInt(9);
    const Reply global1 = replyIn(ask(controller, showController, global));
    CHECK(global1.verdict == "ok");
    CHECK(outputRow(controller, id)->sends_namespace);
    bool heard = false;
    waitUntil(controller, [&] {
        while (studio.ready(0)) {
            const auto messages = takt4::testing::oscMessagesIn(studio.receive());
            heard = heard || takt4::testing::findOsc(messages, "/takt4/status/alive") != nullptr;
        }
        return heard;
    });
    CHECK(heard);

    // Its delay, by id, through the slider's own path.
    const std::string delayAddress = "/takt4/ctl/output/" + id + "/delay";
    takt4::output::OscMessage delay(delayAddress);
    delay.addFloat(25.0f);
    delay.addInt(10);
    const Reply delayed = replyIn(ask(controller, showController, delay));
    CHECK(delayed.verdict == "ok");
    CHECK(delayed.note == "25 ms");
    CHECK(outputRow(controller, id)->delay_ms == Catch::Approx(25.0f));
    // And what the status line said of it, to the outputs taking takt4's own messages — who asked
    // included.
    const std::string line = "Remote, from 127.0.0.1:" + std::to_string(showController.port()) +
                             ": /takt4/ctl/output/" + id + "/delay \xE2\x80\x94 25 ms";
    bool said = false;
    waitUntil(controller, [&] {
        while (studio.ready(0)) {
            for (const takt4::testing::OscMsg& message :
                 takt4::testing::oscMessagesIn(studio.receive())) {
                said =
                    said || (message.address == "/takt4/status/log" && message.args.size() == 2 &&
                             message.args[0].s == "info" && message.args[1].s == line);
            }
        }
        return said;
    });
    CHECK(said);

    // Off.
    const std::string enableAddress = "/takt4/ctl/output/" + id + "/enable";
    takt4::output::OscMessage enable(enableAddress);
    enable.addInt(0);
    enable.addInt(11);
    const Reply off = replyIn(ask(controller, showController, enable));
    CHECK(off.verdict == "ok");
    CHECK(off.note == "off");
    CHECK_FALSE(outputRow(controller, id)->enabled);

    // An id that is no output; a second Link; the global messages for a row that is not OSC.
    takt4::output::OscMessage nobody("/takt4/ctl/output/o-00000000/enable");
    nobody.addInt(1);
    nobody.addInt(12);
    const Reply none = replyIn(ask(controller, showController, nobody));
    CHECK(none.verdict == "refused");
    CHECK(none.note.rfind("no output with the id \"o-00000000\"", 0) == 0);
    takt4::output::OscMessage secondLink("/takt4/ctl/output/add");
    secondLink.addString("another link");
    secondLink.addString("link");
    secondLink.addString("");
    secondLink.addInt(0);
    secondLink.addInt(13);
    CHECK(replyIn(ask(controller, showController, secondLink)).note ==
          "Link is always there, as the first output");
    const std::string linkId(controller.window().get_outputs_list()->row_data(0)->id);
    const std::string linkGlobalAddress = "/takt4/ctl/output/" + linkId + "/global";
    takt4::output::OscMessage linkGlobal(linkGlobalAddress);
    linkGlobal.addInt(1);
    linkGlobal.addInt(14);
    const Reply notOsc = replyIn(ask(controller, showController, linkGlobal));
    CHECK(notOsc.verdict == "refused");
    CHECK(notOsc.note == "only an OSC output can be sent takt4's own messages");
}

TEST_CASE("a change that waited behind a busy window is refused, not carried out late",
          "[ui][remote]") {
    // A dialog open stops the redraws, and with them the commands. One asked for before the
    // window stopped and found after is two seconds stale: the show controller has moved on.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    listenForRemote(controller);
    takt4::testing::LoopbackReceiver showController;
    takt4::remote::Command command;
    command.kind = takt4::remote::Command::Kind::Source;
    command.address = "/takt4/ctl/source";
    command.text = "link";
    command.seq = 2;
    command.from = {0x7F000001u, showController.port()};
    REQUIRE(controller.remoteCommands().push(command));
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    controller.tick();
    const Reply refused = replyIn(takt4::testing::oscMessagesIn(showController.receive()));
    CHECK(refused.seq == 2);
    CHECK(refused.verdict == "refused");
    CHECK(refused.note.rfind("busy:", 0) == 0);
    CHECK(controller.timingIndex() == 0);
}

TEST_CASE("the status a show controller is sent says what the window shows",
          "[ui][remote][follow]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    controller.tick();
    const takt4::remote::Status stopped = controller.remoteBoard().read();
    CHECK_FALSE(stopped.running);
    CHECK(stopped.state == "stopped");
    CHECK(stopped.state == std::string(controller.window().get_lock_word()));
    CHECK(stopped.source == "audio");
    CHECK(stopped.version == std::string(controller.window().get_version()));
    CHECK(stopped.levelDb == takt4::remote::Status::kSilentDb);
    const auto rows = controller.window().get_outputs_list();
    REQUIRE(stopped.outputs.size() == rows->row_count());
    CHECK(stopped.outputs[0].kind == "link");
    CHECK(stopped.outputs[0].id == std::string(rows->row_data(0)->id));

    takt4::testing::FakeGridSource source([&controller] { return linkNow(controller); }, 120.0,
                                          static_cast<double>(linkNow(controller)));
    controller.followForTests(&source);
    controller.pickTiming(1);
    // No session to follow yet, so the panel waits for as long as this likes, however slow the
    // machine; then the session, and the panel goes on to "following" in one of the redraws
    // watched.
    source.setPresent(false);
    controller.toggleRun();
    REQUIRE(controller.followed().running());
    // After every redraw, while the panel's word goes from waiting to following: the status says
    // the word the window has just put up, never the one before it. (From a redraw on: START has
    // changed the word since the last one.)
    controller.tick();
    std::vector<std::string> words;
    int disagreed = 0;
    const auto watch = [&] {
        const std::string board = controller.remoteBoard().read().state;
        const std::string shown(controller.window().get_lock_word());
        if (words.empty() || words.back() != shown) {
            words.push_back(shown);
        }
        disagreed += board == shown ? 0 : 1;
        return shown;
    };
    for (int round = 0; round < 5; ++round) {
        controller.tick();
        (void)watch();
    }
    source.setPresent(true);
    waitUntil(controller,
              [&] { return watch() == "following" && controller.remoteBoard().read().locked; });
    INFO("the panel said, in turn: " << Catch::Detail::stringify(words));
    CHECK(disagreed == 0);
    REQUIRE(words.size() >= 2); // it did change while watched
    CHECK(words.back() == "following");
    const takt4::remote::Status following = controller.remoteBoard().read();
    CHECK(following.running);
    CHECK(following.locked);
    CHECK_FALSE(following.hunting);
    CHECK(following.source == "link");
    CHECK(following.sourceOk);
    CHECK(following.bpm == Catch::Approx(120.0).margin(0.5));
    CHECK(following.state == std::string(controller.window().get_lock_word()));
    CHECK(following.sourceDetail == std::string(controller.window().get_timing_detail()));
    controller.toggleRun();
    controller.followForTests(nullptr);
    controller.tick();
    CHECK_FALSE(controller.remoteBoard().read().running);
}

TEST_CASE("after a launch that fell back from the interface it remembered, a pick from the network "
          "is the one saved",
          "[ui][remote]") {
    // A launch that could not find its interface keeps saving that one, until somebody picks
    // another: a show the night after a launch with the interface switched off must not move to
    // the stand-in. A pick sent from the network is somebody picking.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::settings::Settings settings;
    settings.machine.deviceName = "Gone Interface";
    settings.machine.hostApiName = "ASIO";
    settings.machine.channel = 4;

    SECTION("the device") {
        InputDevice microphone;
        microphone.index = 1;
        microphone.name = "Microphone";
        microphone.hostApiName = "Windows WASAPI";
        microphone.maxInputChannels = 2;
        microphone.defaultSampleRate = 48000.0;
        tracker.setDeviceListHook(
            [&microphone](std::vector<InputDevice>& list) { list = {motuAt(0), microphone}; });
        WindowController controller(tracker, settings);
        REQUIRE(controller.currentSettings().machine.deviceName == "Gone Interface");
        listenForRemote(controller);
        takt4::testing::LoopbackReceiver showController;
        CHECK(replyIn(ask(controller, showController,
                          numbered("/takt4/ctl/audio/device", {"Windows WASAPI", "Microphone"}, 1)))
                  .verdict == "ok");
        CHECK(controller.currentSettings().machine.deviceName == "Microphone");
    }

    SECTION("the channel, on the interface it fell back to") {
        tracker.setDeviceListHook([](std::vector<InputDevice>& list) { list = {motuAt(0)}; });
        WindowController controller(tracker, settings);
        REQUIRE(controller.currentSettings().machine.deviceName == "Gone Interface");
        listenForRemote(controller);
        takt4::testing::LoopbackReceiver showController;
        takt4::output::OscMessage channel("/takt4/ctl/audio/channel");
        channel.addInt(3);
        channel.addInt(0);
        channel.addInt(2);
        CHECK(replyIn(ask(controller, showController, channel)).verdict == "ok");
        const takt4::settings::Settings saved = controller.currentSettings();
        CHECK(saved.machine.deviceName == "MOTU Pro Audio");
        CHECK(saved.machine.channel == 2);
    }
}

TEST_CASE("remote setup is saved and comes back, and a list in the file with a word in it leaves "
          "it off",
          "[ui][remote]") {
    takt4::settings::Settings settings;
    settings.machine.oscRemoteSetup = true;
    settings.machine.oscRemoteFrom = "192.168.1.50,10.0.0.2";
    LiveTracker tracker(kWeights, kStateSpace);

    SECTION("as it was saved") {
        WindowController controller(tracker, settings);
        CHECK(controller.remoteSetup());
        CHECK(controller.remoteFrom() == "192.168.1.50, 10.0.0.2");
        CHECK(controller.oscControl().remoteSetup().allowed);
        CHECK(controller.oscControl().remoteSetup().from ==
              std::vector<std::uint32_t>{0xC0A80132u, 0x0A000002u});
        CHECK(controller.currentSettings().machine.oscRemoteSetup);
        CHECK(controller.currentSettings().machine.oscRemoteFrom == "192.168.1.50, 10.0.0.2");
    }

    SECTION("a list with something else in it opens nothing") {
        settings.machine.oscRemoteFrom = "192.168.1.50, controller";
        WindowController controller(tracker, settings);
        CHECK_FALSE(controller.remoteSetup());
        CHECK_FALSE(controller.oscControl().remoteSetup().allowed);
        CHECK(controller.remoteFrom() == "192.168.1.50");
        CHECK(controller.statusIsError());
        const std::string said(controller.window().get_status());
        INFO(said);
        CHECK(said.find("Remote setup is off") != std::string::npos);
        CHECK(said.find("\"controller\"") != std::string::npos);
        CHECK_FALSE(controller.currentSettings().machine.oscRemoteSetup);
    }
}

TEST_CASE("an input picked from the network is the one the pickers show, and its channel is one "
          "the window offers",
          "[ui][remote]") {
    LiveTracker tracker(kWeights, kStateSpace);
    InputDevice microphone;
    microphone.index = 1;
    microphone.name = "Microphone";
    microphone.hostApiName = "Windows WASAPI";
    microphone.maxInputChannels = 2;
    microphone.defaultSampleRate = 48000.0;
    tracker.setDeviceListHook(
        [&microphone](std::vector<InputDevice>& list) { list = {motuAt(0), microphone}; });
    WindowController controller(tracker);
    listenForRemote(controller);
    takt4::testing::LoopbackReceiver showController;

    // By the names the devices list gives, host first: as the picker would, and saved as a pick.
    const Reply picked =
        replyIn(ask(controller, showController,
                    numbered("/takt4/ctl/audio/device", {"Windows WASAPI", "Microphone"}, 1)));
    CHECK(picked.verdict == "ok");
    CHECK(picked.note == "Windows WASAPI / Microphone");
    CHECK(controller.window().get_device_index() == 1);
    CHECK(controller.currentSettings().machine.deviceName == "Microphone");
    CHECK(controller.currentSettings().machine.hostApiName == "Windows WASAPI");

    // A device this machine has not got changes nothing, and says where to look.
    const Reply missing =
        replyIn(ask(controller, showController,
                    numbered("/takt4/ctl/audio/device", {"ASIO", "Nothing Here"}, 2)));
    CHECK(missing.verdict == "refused");
    CHECK(missing.note.find("ask /takt4/ctl/audio/devices") != std::string::npos);
    CHECK(controller.window().get_device_index() == 1);

    // The MOTU's eight: inputs 5 and 6 as the pair they are, then 7 alone.
    CHECK(replyIn(ask(controller, showController,
                      numbered("/takt4/ctl/audio/device", {"ASIO", "MOTU Pro Audio"}, 3)))
              .verdict == "ok");
    const auto channel = [&](std::int32_t first, std::int32_t mono, std::int32_t seq) {
        takt4::output::OscMessage message("/takt4/ctl/audio/channel");
        message.addInt(first);
        message.addInt(mono);
        message.addInt(seq);
        return replyIn(ask(controller, showController, message));
    };
    const Reply pair = channel(5, 0, 4);
    CHECK(pair.verdict == "ok");
    CHECK(pair.note == "inputs 5 and 6");
    CHECK(controller.selection().count == 2);
    CHECK(controller.selection().channels[0] == 4);
    CHECK(controller.window().get_channel_index() == 2); // "5 + 6", third in the pairs' list
    CHECK_FALSE(controller.window().get_input_mono());

    // The second of a pair is no pair the window offers: refused, with the one it meant.
    const Reply even = channel(6, 0, 5);
    CHECK(even.verdict == "refused");
    CHECK(even.note.find("ask for 5") != std::string::npos);
    CHECK(controller.selection().channels[0] == 4);

    const Reply single = channel(7, 1, 6);
    CHECK(single.verdict == "ok");
    CHECK(single.note == "input 7, mono");
    CHECK(controller.selection().count == 1);
    CHECK(controller.selection().channels[0] == 6);
    CHECK(controller.window().get_input_mono());
    CHECK(controller.window().get_channel_index() == 6);
    CHECK(controller.currentSettings().machine.mono);
    CHECK(controller.currentSettings().machine.channel == 6);

    const Reply past = channel(9, 1, 7);
    CHECK(past.verdict == "refused");
    CHECK(past.note == "input 9 is not on MOTU Pro Audio, which has 8");

    // And the status says what is picked, as the window shows it.
    controller.tick();
    const takt4::remote::Status status = controller.remoteBoard().read();
    CHECK(status.audioHost == "ASIO");
    CHECK(status.audioDevice == "MOTU Pro Audio");
    CHECK(status.audioChannel == 7);
    CHECK(status.audioMono);
    REQUIRE(status.devices.size() == 2);
    CHECK(status.devices[1].device == "Microphone");
    CHECK(status.devices[1].inputs == 2);
}

TEST_CASE("the bars of a phrase typed in the outputs heading are what /takt4/phrase counts in, and "
          "are saved",
          "[ui][bars]") {
    // The end of the heading's sentence, "global messages to … in phrases of 8 bars": typed into
    // as an operator would, and read back where it acts — the output thread's publisher — and
    // where it is kept.
    LiveTracker tracker(kWeights, kStateSpace);
    takt4::testing::LoopbackReceiver deck;
    takt4::settings::Settings saved;
    takt4::output::OutputTarget out;
    out.id = "o-00000dec";
    out.name = "deck";
    out.kind = takt4::output::OutputTarget::Kind::Osc;
    out.host = "127.0.0.1";
    out.port = deck.port();
    saved.preset.outputs = {out};
    saved.preset.phraseBars = 4; // as a file holding it would
    WindowController controller(tracker, saved);
    constexpr int kWidth = 800;
    constexpr int kHeight = 1400;
    const takt4::tests::Shot shot = takt4::tests::render(controller.window(), kWidth, kHeight);
    auto& window = controller.window().window();
    window.dispatch_window_active_changed_event(true);
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    const auto publisherBars = [&controller] {
        REQUIRE(controller.settleOutputs());
        return controller.outputs().inspect(
            [](const auto&, const takt4::output::Transports& transports, const auto&) {
                return transports.osc().phraseBars();
            });
    };
    CHECK(controller.window().get_phrase_bars() == 4);
    CHECK(publisherBars() == 4u);

    const std::vector<std::pair<int, int>> sheets = sheetsDown(shot);
    REQUIRE(sheets.size() == 7);
    const int headRow = sheets[5].first + 10 + 14;
    const auto head = occupied(shot, headRow - 13, headRow + 13, 300, kWidth - 56, kSheet, 5);
    INFO("the outputs heading: " << spans(head));
    REQUIRE(head.size() == 5);
    clickAt(window, middleOf(head[3]), static_cast<float>(headRow));
    settle();
    takt4::tests::endOfText(window);
    press(window, "\x08");
    press(window, "1");
    press(window, "6");
    press(window, "\n");
    settle();
    CHECK(controller.window().get_phrase_bars() == 16);
    CHECK(publisherBars() == 16u);

    // And a number past the end is the longest there is, not refused into nothing.
    clickAt(window, middleOf(head[3]), static_cast<float>(headRow));
    settle();
    takt4::tests::endOfText(window);
    press(window, "\x08");
    press(window, "\x08");
    press(window, "9");
    press(window, "9");
    press(window, "9");
    press(window, "\n");
    settle();
    CHECK(controller.window().get_phrase_bars() == 256);
    CHECK(publisherBars() == 256u);

    const std::filesystem::path file = takt4::settings::settingsFile();
    REQUIRE(controller.saveNow());
    CHECK(takt4::settings::load(file).preset.phraseBars == 256u);
}

TEST_CASE("the main window's LEARN and a rule's own are told apart, and a rule's pad is saved",
          "[ui][midi]") {
    // One MIDI learn behind two windows: the rule editor arms it for a rule, the main window for
    // one of its own actions. Each window lights and gives up its own — and a pad bound to a
    // rule is kept with the machine's other controls.
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    takt4::control::MidiControl& midi = controller.control();
    midi.listenWithoutPort();
    takt4::ui::RulesController& editor = controller.editor();
    editor.add();
    const std::string id = editor.rules().front().id;
    const takt4::control::ControlTarget mute(takt4::control::ControlAction::RuleMute, id);

    editor.learnRemote(1);
    controller.tick();
    REQUIRE(midi.learning() == mute);
    // Not this window's: its LEARN is not lit and its line is not waiting.
    CHECK_FALSE(controller.window().get_learning());
    CHECK(std::string(controller.window().get_control_reading()).find("waiting") ==
          std::string::npos);
    // Another action picked here, or its FORGET, leaves the editor's armed.
    controller.pickLearnAction(1);
    controller.forgetLearned();
    CHECK(midi.learning() == mute);
    // Its own LEARN pressed is its own, over the editor's: the last LEARN pressed is the one meant.
    controller.toggleLearn();
    REQUIRE(midi.learning().has_value());
    CHECK_FALSE(takt4::control::takesRuleId(midi.learning()->action));
    CHECK(controller.window().get_learning());
    controller.toggleLearn();
    CHECK_FALSE(midi.learning().has_value());

    // A pad for the rule, saved as the machine's controls are, and back with a new window.
    editor.learnRemote(1);
    takt4::control::MidiEvent pad;
    pad.kind = takt4::control::MidiEvent::Kind::ControlChange;
    pad.channel = 2;
    pad.number = 21;
    pad.value = 127;
    REQUIRE(midi.dispatch(pad));
    const takt4::settings::Settings saved = controller.currentSettings();
    const std::string line = "cc 21 ch 2 -> rule/" + id + "/mute";
    CHECK(std::find(saved.machine.midiBindings.begin(), saved.machine.midiBindings.end(), line) !=
          saved.machine.midiBindings.end());

    LiveTracker again(kWeights, kStateSpace);
    WindowController reopened(again, saved);
    reopened.editor().tick();
    const auto rows = reopened.editor().window().get_remote_rows();
    REQUIRE(rows->row_count() == 5);
    CHECK(std::string(rows->row_data(1)->reading) == "cc 21 ch 2");
}

TEST_CASE("a show controller loads a preset by name, its show and never this desk's input or "
          "controls, and is told the names when it asks for one there is not",
          "[ui][remote]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::test::TempDir temp;
    controller.usePresetsFolder(temp.path() / "presets");
    listenForRemote(controller);
    takt4::testing::LoopbackReceiver showController;
    const std::uint16_t port = controller.oscControlPort();

    // A show saved as "Show A": phrases of 12 bars and an OSC output.
    controller.setPhraseBars(12);
    controller.window().invoke_output_added();
    const std::size_t outputs = controller.currentSettings().preset.outputs.size();
    REQUIRE(controller.savePreset("  Show A "));
    CHECK(controller.presetNames() == std::vector<std::string>{"Show A"});
    // And a file someone dropped in the folder: a whole EXPORT, machine half and all, whose
    // machine half would stop the very socket the switch comes in on.
    takt4::settings::Settings full = controller.currentSettings();
    full.preset.phraseBars = 5;
    full.machine.oscControlEnabled = false;
    full.machine.oscControlPort = 1;
    full.machine.oscRemoteSetup = false;
    REQUIRE(takt4::settings::save(full, temp.path() / "presets" / "Full.json"));

    // Another show in the window. Saved as "Show A" a moment ago, so the show was that, and
    // now it has changed.
    CHECK(controller.remoteStatus().loadedPreset == "Show A");
    controller.setPhraseBars(16);
    controller.window().invoke_output_added();
    REQUIRE(controller.currentSettings().preset.outputs.size() == outputs + 1);
    const auto rounds = [&controller](int count) { // the window's own, 20 ms apart
        for (int turn = 0; turn < count; ++turn) {
            controller.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };
    rounds(40);
    CHECK(controller.remoteStatus().presetModified);

    // Loaded over the wire, by its name in another case.
    Reply reply =
        replyIn(ask(controller, showController, numbered("/takt4/ctl/preset", {"show a"}, 1)));
    INFO("the reply: " << reply.note);
    CHECK(reply.seq == 1);
    CHECK(reply.verdict == "ok");
    CHECK(reply.note.starts_with("loaded preset \"Show A\": "));
    CHECK(controller.currentSettings().preset.phraseBars == 12);
    CHECK(controller.currentSettings().preset.outputs.size() == outputs);
    CHECK(std::string(controller.window().get_status()).find("/takt4/ctl/preset") !=
          std::string::npos);

    // The preset the show is now, said in the status, and not changed by nothing happening.
    rounds(40);
    CHECK(controller.remoteStatus().loadedPreset == "Show A");
    CHECK_FALSE(controller.remoteStatus().presetModified);
    const auto statusSays = [&](std::string_view address) {
        for (const takt4::testing::OscMsg& message :
             ask(controller, showController, takt4::output::OscMessage("/takt4/ctl/status"))) {
            if (message.address == address && !message.args.empty()) {
                return message.args[0];
            }
        }
        return takt4::testing::OscArg{};
    };
    CHECK(statusSays("/takt4/status/loaded_preset").s == "Show A");
    CHECK(statusSays("/takt4/status/preset_modified").type == 'i');
    CHECK(statusSays("/takt4/status/preset_modified").i == 0);
    // A show setting changed: modified, the name kept.
    controller.setPhraseBars(9);
    rounds(40);
    CHECK(controller.remoteStatus().presetModified);
    CHECK(statusSays("/takt4/status/preset_modified").i == 1);
    CHECK(statusSays("/takt4/status/loaded_preset").s == "Show A");
    controller.setPhraseBars(12);

    // The dropped-in export: its show, and none of its machine half.
    reply = replyIn(ask(controller, showController, numbered("/takt4/ctl/preset", {"Full"}, 2)));
    CHECK(reply.verdict == "ok");
    CHECK(controller.currentSettings().preset.phraseBars == 5);
    CHECK(controller.remoteStatus().loadedPreset == "Full");
    CHECK_FALSE(controller.remoteStatus().presetModified);
    CHECK(controller.oscControl().running());
    CHECK(controller.oscControlPort() == port);
    CHECK(controller.currentSettings().machine.oscRemoteSetup);

    // One there is not: refused, with the names there are.
    reply = replyIn(ask(controller, showController, numbered("/takt4/ctl/preset", {"Nope"}, 3)));
    CHECK(reply.verdict == "refused");
    CHECK(reply.note == "no preset \"Nope\"; there are Full, Show A");
    CHECK(controller.currentSettings().preset.phraseBars == 5);

    // Asked for: each by name, then how many, then the answer.
    takt4::output::OscMessage list("/takt4/ctl/presets");
    list.addInt(4);
    REQUIRE(showController.send(port, list.packet()));
    std::vector<takt4::testing::OscMsg> said;
    waitUntil(controller, [&] {
        while (showController.ready(0)) {
            for (takt4::testing::OscMsg& message :
                 takt4::testing::oscMessagesIn(showController.receive())) {
                said.push_back(std::move(message));
            }
        }
        return said.size() >= 4;
    });
    REQUIRE(said.size() == 4);
    CHECK(said[0].address == "/takt4/status/preset");
    CHECK(said[0].args[0].s == "Full");
    CHECK(said[1].args[0].s == "Show A");
    CHECK(said[2].address == "/takt4/status/presets");
    CHECK(said[2].args[0].i == 2);
    CHECK(replyIn({said[3]}).verdict == "ok");

    // Remote setup off: a switch of show is refused, the list still answered.
    controller.setRemoteSetup(false);
    reply = replyIn(ask(controller, showController, numbered("/takt4/ctl/preset", {"Show A"}, 5)));
    CHECK(reply.verdict == "refused");
    CHECK(controller.currentSettings().preset.phraseBars == 5);
}

TEST_CASE("a preset is saved, replaced, loaded and deleted from the window, and a bad name is "
          "refused saying why",
          "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::test::TempDir temp;
    controller.usePresetsFolder(temp.path() / "presets");
    CHECK(controller.presetNames().empty());
    CHECK_FALSE(controller.loadPreset("anything"));
    CHECK(std::string(controller.window().get_status()) == "No presets are saved yet.");

    controller.setPhraseBars(12);
    REQUIRE(controller.savePreset("Friday"));
    CHECK(std::string(controller.window().get_status()) == "Saved the show as preset \"Friday\".");
    controller.setPhraseBars(13);
    REQUIRE(controller.savePreset("friday")); // the same one, in another case: replaced
    CHECK(controller.presetNames() == std::vector<std::string>{"Friday"});
    controller.setPhraseBars(4);
    REQUIRE(controller.loadPreset("Friday"));
    CHECK(controller.currentSettings().preset.phraseBars == 13);
    CHECK(std::string(controller.window().get_status()).starts_with("Loaded preset \"Friday\": "));

    CHECK_FALSE(controller.savePreset("a/b"));
    CHECK(std::string(controller.window().get_status()) ==
          "Could not save the preset: a preset's name cannot hold /.");
    CHECK_FALSE(controller.savePreset("   "));
    CHECK(controller.presetNames() == std::vector<std::string>{"Friday"});

    REQUIRE(controller.deletePreset("Friday"));
    CHECK(controller.presetNames().empty());
    CHECK_FALSE(controller.deletePreset("Friday"));
}

TEST_CASE("PRESETS opens its window, whose own box and buttons save, load and delete a preset, "
          "and a double click on its x never deletes one",
          "[ui]") {
    LiveTracker tracker(kWeights, kStateSpace);
    WindowController controller(tracker);
    const takt4::test::TempDir temp;
    controller.usePresetsFolder(temp.path() / "presets");
    constexpr float kWidth = 1000.0f;
    constexpr float kHeight = 760.0f;
    layOut(controller, kWidth, kHeight);
    auto& window = controller.window().window();
    const auto settle = [&controller] {
        controller.tick();
        slint::platform::update_timers_and_animations();
    };
    settle();
    const takt4::tests::NothingReal nothingReal;

    // PRESETS, found along the status line by what a click on it does: from the right, past ABOUT,
    // which opens its own window on the way and nothing else.
    const float barY = kHeight - 17.0f;
    for (float x = kWidth - 10.0f; x > kWidth - 480.0f && controller.presetsWindow() == nullptr;
         x -= 6.0f) {
        clickAt(window, x, barY);
        settle();
    }
    REQUIRE(controller.presetsWindow() != nullptr);
    PresetsWindow& presets = *controller.presetsWindow();
    CHECK(presets.window().is_visible());
    constexpr int kW = static_cast<int>(takt4::ui::kPresetsWindowWidth);
    constexpr int kH = static_cast<int>(takt4::ui::kPresetsWindowHeight);
    presets.window().dispatch_scale_factor_change_event(1.0f);
    presets.window().dispatch_resize_event(slint::LogicalSize({1.0f * kW, 1.0f * kH}));
    presets.window().dispatch_window_active_changed_event(true);
    slint::platform::update_timers_and_animations();
    auto& inside = presets.window();
    const auto shoot = [&presets] { return takt4::tests::render(presets, kW, kH); };

    // SAVE: the one button filled off-white above CLOSE's row — its face, found down the right
    // half; the name box is left of it on the same line.
    const auto saveButton = [&](const takt4::tests::Shot& shot) {
        for (int y = 0; y < kH - 60; ++y) {
            int run = 0;
            for (int x = kW / 2; x < kW - 12; ++x) {
                run = is(shot, x, y, kEdge) ? run + 1 : 0;
                if (run >= 50) {
                    int bottom = y;
                    while (bottom + 1 < kH && is(shot, x, bottom + 1, kEdge)) {
                        ++bottom;
                    }
                    return std::pair<std::pair<int, int>, float>{
                        {x - run + 1, x}, static_cast<float>(y + bottom) / 2.0f};
                }
            }
        }
        return std::pair<std::pair<int, int>, float>{{-1, -1}, -1.0f};
    };
    takt4::tests::Shot shot = shoot();
    const auto [saveSpan, saveY] = saveButton(shot);
    INFO("save: " << saveSpan.first << ".." << saveSpan.second << " at " << saveY);
    REQUIRE(saveSpan.first > 0);
    controller.setPhraseBars(12);
    clickAt(inside, static_cast<float>(saveSpan.first - 80), saveY); // into the name box
    for (const char c : std::string("Encore")) {
        press(inside, std::string(1, c));
    }
    settle();
    CHECK(std::string(presets.get_name()) == "Encore"); // the box's text, as the owner holds it
    CHECK_FALSE(presets.get_replacing());
    clickAt(inside, middleOf(saveSpan), saveY);
    settle();
    CHECK(controller.presetNames() == std::vector<std::string>{"Encore"});
    CHECK(std::filesystem::exists(temp.path() / "presets" / "Encore.json"));
    CHECK(std::string(presets.get_said()) == "Saved the show as preset \"Encore\".");
    CHECK(presets.get_replacing()); // SAVE now says "replace"
    REQUIRE(presets.get_presets()->row_count() == 1);

    // The row's buttons: its x's right-hand edge is a tall off-white run down the column just
    // inside the sheet's right-hand padding; LOAD and x are what is there, projected over its
    // height.
    shot = shoot();
    const int edgeX = kW - 28;
    float rowY = -1.0f;
    for (int y = 0, run = 0; y < static_cast<int>(saveY) - 30; ++y) {
        run = is(shot, edgeX, y, kEdge) ? run + 1 : 0;
        if (run >= 20) {
            int bottom = y;
            while (is(shot, edgeX, bottom + 1, kEdge)) {
                ++bottom;
            }
            rowY = static_cast<float>(y - run + 1 + bottom) / 2.0f;
            break;
        }
    }
    REQUIRE(rowY > 0.0f);
    // Over the buttons' whole 32 px, edges and all, and short of the next row 46 px down.
    const auto buttons = occupied(shot, static_cast<int>(rowY) - 17, static_cast<int>(rowY) + 17,
                                  kW / 2, kW - 20, kSheet, 4);
    INFO("the row at " << rowY << ": " << spans(buttons));
    REQUIRE(buttons.size() == 2); // LOAD and x

    controller.setPhraseBars(3);
    clickAt(inside, middleOf(buttons[0]), rowY);
    settle();
    CHECK(controller.currentSettings().preset.phraseBars == 12);
    CHECK(std::string(presets.get_said()).starts_with("Loaded preset \"Encore\": "));

    // A double click on x arms it and nothing more; a press after it deletes the file.
    clickAt(inside, middleOf(buttons[1]), rowY);
    clickAt(inside, middleOf(buttons[1]), rowY);
    settle();
    CHECK(presets.get_presets()->row_data(0)->armed);
    CHECK(std::filesystem::exists(temp.path() / "presets" / "Encore.json"));
    std::this_thread::sleep_for(takt4::ui::DeleteGuard::interval() + std::chrono::milliseconds(50));
    clickAt(inside, middleOf(buttons[1]), rowY);
    settle();
    CHECK_FALSE(std::filesystem::exists(temp.path() / "presets" / "Encore.json"));
    CHECK(presets.get_presets()->row_count() == 0);
    CHECK(std::string(presets.get_said()) == "Deleted preset \"Encore\".");
    nothingReal.check();
}
