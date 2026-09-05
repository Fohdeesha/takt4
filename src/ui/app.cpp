#include "ui/app.hpp"

#include "core/engine/live_tracker.hpp"
#include "core/settings/settings.hpp"
#include "ui/window_controller.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>

namespace takt4::ui {
namespace {

std::filesystem::path weightsPath() {
    return std::filesystem::path(TAKT4_WEIGHTS_DIR) / "generic.bin";
}

std::filesystem::path stateSpacePath() {
    return std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin";
}

} // namespace

int run() {
    // Whatever the last run left (Q7). Never fails: a settings file that is missing or
    // unreadable gives the defaults, because it must not be the reason the app will not
    // open. The tracker's own tuning goes in here rather than being applied afterwards,
    // so the engine is built with it and the window's sliders show it from the first
    // frame instead of jumping a moment later.
    const std::filesystem::path settingsPath = settings::settingsFile();
    const settings::Settings saved = settings::load(settingsPath);
    engine::LiveTracker::Options options;
    options.engine.tempo = saved.preset.tempo;

    // The assets are read before the window opens, so a broken install says which file is
    // missing rather than showing an empty window that will not start. Phase 7's
    // first-run flow is where this becomes something friendlier than a line on stderr.
    std::unique_ptr<engine::LiveTracker> tracker;
    try {
        tracker = std::make_unique<engine::LiveTracker>(weightsPath(), stateSpacePath(), options);
    } catch (const std::exception& e) {
        std::cerr << "takt4: " << e.what() << '\n';
        return 1;
    }

    WindowController controller(*tracker, saved);
    controller.run();
    // The window has gone; stop the audio before the tracker goes with it.
    tracker->stop();

    // On the way out rather than on every change: a slider drag would otherwise write the
    // file sixty times a second. What is lost to a crash is one session's tweaks, which is
    // the right trade for a file nobody is waiting on.
    if (!settingsPath.empty() && !settings::save(controller.currentSettings(), settingsPath)) {
        std::cerr << "takt4: could not write " << settingsPath.string()
                  << "; this session's settings were not kept\n";
    }
    return 0;
}

} // namespace takt4::ui
