#pragma once

#include <filesystem>

namespace takt4::ui {

/// Shows the main window and runs the event loop on the calling thread.
/// Returns when the window is closed.
int run();

struct ShotOptions {
    /// The size the window opens at (`kMainWindowWidth` and `kMainWindowHeight` in
    /// window_state.hpp, which shot.cpp holds these to), so the picture is the window an
    /// operator first sees. They were 900 by 836, which no window opened at (the audit of
    /// 2026-09-25, L34).
    int width = 1000;
    int height = 934;
    /// Draw the window as it looks with a tracker running. False renders the idle
    /// window — blank readouts, every manual control disabled — which is what the app
    /// looks like the moment it opens, and is therefore worth being able to look at.
    bool running = true;
    /// Render §5.9's rule editor instead of the main window. A second window needs looking
    /// at as much as the first — more, being newer — and it is the same problem: no display
    /// in these sessions, and a Slint layout bug that is silent rather than a compile error.
    bool rules = false;
    /// Render the About box instead.
    bool about = false;
    /// Render the lighting patch editor instead. The third window, and the same argument:
    /// a Slint layout bug is silent rather than a compile error, and nobody can see this one
    /// from a session with no display either.
    bool fixtures = false;
    /// With `--rules`, draw the lighting half of the rule editor rather than the OSC one.
    /// The two are alternatives in the markup — `if root.sends-dmx` — so only one of them is
    /// ever on screen, and only a render says whether the other fits.
    bool dmx = false;
};

/// The size the rule editor opens at, kept beside the main window's for the same reason, and
/// held to `kRulesWindowWidth` and `kRulesWindowHeight` the same way.
inline constexpr int kRulesShotWidth = 1164;
inline constexpr int kRulesShotHeight = 872;

/// And the patch editor's (`kFixturesWindowWidth`, `kFixturesWindowHeight`).
inline constexpr int kFixturesShotWidth = 1080;
inline constexpr int kFixturesShotHeight = 800;

/// Renders the main window to a 24-bit BMP at `out`, with no window system involved.
///
/// HANDOFF §5.9's window is worked on from sessions that have no display, where every
/// screen-capture route fails; this one does not need one. See `shot.cpp`.
int renderShot(const std::filesystem::path& out, const ShotOptions& options);

} // namespace takt4::ui
