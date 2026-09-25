#pragma once

#include <filesystem>
#include <string>

namespace takt4::ui {

/// A native "open" / "save as" dialog, for §9's import and export.
///
/// **Why a platform call rather than a Slint dialog.** Slint has no file picker, and a
/// hand-drawn one would be a file browser to write and maintain — with no idea of the
/// desktop's places, shortcuts, or the network drive the operator keeps their shows on.
/// This is `GetOpenFileNameW` / `GetSaveFileNameW` on Windows, which is what every other
/// application on that desktop opens.
///
/// Both return an **empty path when the operator cancels**, which is not an error and must
/// not be reported as one. On a platform with no implementation yet they also return empty,
/// so the caller's "cancelled, do nothing" path is the safe default rather than a crash —
/// see the note in the .cpp about what that costs when the port happens.
///
/// Must be called from the UI thread: a native modal dialog runs its own message loop.
std::filesystem::path askOpenFile(const std::string& title, const std::string& suggested);
std::filesystem::path askSaveFile(const std::string& title, const std::string& suggested);

/// Whether a dialog may be shown at all: false while `TAKT4_NO_FILE_DIALOGS` is set, when both
/// of the above return empty — a cancel — without showing anything.
///
/// **The test binaries set it** (`tests/support/crt_dialogs.cpp`). A window test drives real
/// clicks, and one whose clicks land on EXPORT or IMPORT opened a real modal dialog on the
/// desktop the tests run on — which is the rig — and left it there for somebody to dismiss,
/// again for every click. It happened on 2026-09-25, when the status bar's buttons moved under
/// a sweep of clicks aimed at where something else used to be.
bool fileDialogsAllowed();

} // namespace takt4::ui
