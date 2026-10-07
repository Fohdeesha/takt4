#pragma once

#include <cstdint>
#include <string_view>

namespace takt4::ui {

/// The two things a Slint window cannot do for itself, both of them Windows' own behaviour
/// and both reported from a rig.
///
/// Slint's C++ `Window` offers `show`, `hide`, `is_visible`, `position` and `request_redraw`
/// — and no way to reach the platform window under it (there is no handle accessor in 1.17).
/// Everything here therefore finds the window by its title among this thread's own top-level
/// windows, which is exact enough: takt4 opens two, and their titles do not collide.
///
/// **Every function is a no-op away from Windows**, returning as though it found nothing. The
/// two problems are Win32's, and neither is worth an approximation elsewhere: a hide-then-show
/// to fake a raise would flicker and would risk the event loop's last-window-closed rule.
/// They are also no-ops under Slint's testing backend, which has no platform windows at all.

/// Brings this process's window whose title contains `titleContains` to the front, restoring
/// it if it has been minimised. False when there is no such window.
///
/// `show()` on a window that is already up does **nothing** on Windows: it stays exactly where
/// it was in the Z order. So pressing TRIGGERS on the main window while the editor sat behind
/// it looked like a button that did not work — reported on 2026-09-08. Matched on a substring
/// so the caller need not spell the em dash in the editor's title.
bool bringWindowToFront(std::string_view titleContains);

/// Called from a window's own message loop while it is being dragged or resized. It is handed
/// whatever `user` was registered with.
using DragPump = void (*)(void* user);

/// Keeps `pump` running while any of this process's windows is being dragged or resized.
///
/// **Why this exists.** Dragging a window on Windows enters a *modal* message loop inside
/// `DefWindowProc`, and that loop runs until the mouse is released. Slint's timers are the
/// event loop's, and the event loop is not running during it — so every readout froze for as
/// long as the window was being moved, and caught up in a jump afterwards. The audio, the
/// tracker and the output thread never stopped; it was only ever the window, which is why it
/// is a UI fix and not an engine one.
///
/// The standard answer, and the one here: subclass the window, start a `SetTimer` when the
/// modal loop begins and stop it when it ends. `WM_TIMER` *is* dispatched inside that loop, so
/// the timer is the one thing that still ticks. The pump redraws; the paint that follows is
/// dispatched by the same loop.
///
/// Idempotent and cheap: it walks this thread's windows and subclasses the ones it has not
/// already, which is how a window opened later — the rule editor — gets covered too. Call it
/// from the redraw timer every second or so.
void keepPaintingWhileDragged(DragPump pump, void* user);

/// Calls `pump(user)` every `periodMs` from **any** message loop on the calling thread — Slint's
/// own, and the ones Windows runs for a file dialog, a menu or a message box, which Slint's
/// timers do not reach. A thread timer: every loop that dispatches messages serves it. Returns a
/// handle for `stopPumping`, or 0 where there is none (not Windows).
std::uintptr_t pumpThroughModalLoops(DragPump pump, void* user, unsigned periodMs);
/// Stops what `pumpThroughModalLoops` started. Nothing for 0.
void stopPumping(std::uintptr_t handle);

/// A size in Slint's logical pixels, kept free of Slint's own types so this header is not.
struct LogicalExtent {
    float width = 0.0f;
    float height = 0.0f;
};

/// What a window asking to open at `wanted` may open at inside a screen whose work area — the
/// screen without its taskbar — is `work` logical pixels: `wanted`, or less where the work
/// area is smaller, keeping room for the title bar and frame. A zero work area is an unknown
/// one, and leaves `wanted` alone. Pure arithmetic, for `fitToScreen` and its test.
LogicalExtent fitWithin(LogicalExtent wanted, LogicalExtent work) noexcept;

/// `wanted`, fitted to the primary monitor's work area — once `fitWindowsToScreen(true)` has
/// been said, which the application does and the tests do not, so a test window opens at the
/// size its layout assertions were written for.
///
/// **The audit's M26.** The main window opened at 1000 x 934 logical pixels, which at 125 %
/// scaling is 1168 physical: taller than a 1080-line screen once its taskbar is off it, so the
/// status bar and PANIC opened under the taskbar on exactly the laptop a show runs from. The
/// rule and patch editors (872 and 800 tall) the same on a smaller screen. Only the first show
/// is sized this way; a window the operator drags larger stays larger.
///
/// Away from Windows, or when the screen cannot be asked, `wanted` unchanged.
LogicalExtent fitToScreen(LogicalExtent wanted) noexcept;
void fitWindowsToScreen(bool fit) noexcept;

/// Says something that stops takt4 starting, where somebody will see it.
///
/// A window application has no console (see src/main.cpp), so the `std::cerr` this used to be
/// went nowhere: a binary whose embedded assets failed their checksum simply exited without a
/// window and without a word. Rare — it means the executable is damaged — and exactly the case
/// that must not be silent. A message box on Windows, `std::cerr` everywhere else, where a
/// terminal is the normal way to start it.
void reportFatal(std::string_view message);

} // namespace takt4::ui
