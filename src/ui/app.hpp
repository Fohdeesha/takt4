#pragma once

#include <filesystem>

namespace takt4::ui {

/// Shows the main window and runs the event loop on the calling thread.
/// Returns when the window is closed.
int run();

struct ShotOptions {
    int width = 900;
    int height = 640;
    /// Draw the window as it looks with a tracker running. False renders the idle
    /// window — blank readouts, every manual control disabled — which is what the app
    /// looks like the moment it opens, and is therefore worth being able to look at.
    bool running = true;
};

/// Renders the main window to a 24-bit BMP at `out`, with no window system involved.
///
/// HANDOFF §5.9's window is worked on from sessions that have no display, where every
/// screen-capture route fails; this one does not need one. See `shot.cpp`.
int renderShot(const std::filesystem::path& out, const ShotOptions& options);

} // namespace takt4::ui
