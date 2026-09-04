#include "ui/app.hpp"

#include "core/engine/live_tracker.hpp"
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
    // The assets are read before the window opens, so a broken install says which file is
    // missing rather than showing an empty window that will not start. Phase 7's
    // first-run flow is where this becomes something friendlier than a line on stderr.
    std::unique_ptr<engine::LiveTracker> tracker;
    try {
        tracker = std::make_unique<engine::LiveTracker>(weightsPath(), stateSpacePath());
    } catch (const std::exception& e) {
        std::cerr << "takt4: " << e.what() << '\n';
        return 1;
    }

    WindowController controller(*tracker);
    controller.run();
    // The window has gone; stop the audio before the tracker goes with it.
    tracker->stop();
    return 0;
}

} // namespace takt4::ui
