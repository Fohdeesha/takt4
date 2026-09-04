#pragma once

#include <filesystem>

namespace takt4::ui {

/// Shows the main window and runs the event loop on the calling thread.
/// Returns when the window is closed.
int run();

/// Renders the main window to a 24-bit BMP at `out`, with no window system involved.
///
/// HANDOFF §5.9's window is worked on from sessions that have no display, where every
/// screen-capture route fails; this one does not need one. See `shot.cpp`.
int renderShot(const std::filesystem::path& out, int width, int height);

} // namespace takt4::ui
