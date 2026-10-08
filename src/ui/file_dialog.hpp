#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace takt4::ui {

/// A native "open" / "save as" dialog, for §9's import and export.
///
/// **Why a platform call rather than a Slint dialog.** Slint has no file picker, and a
/// hand-drawn one would be a file browser to write and maintain — with no idea of the
/// desktop's places, shortcuts, or the network drive the operator keeps their shows on.
/// This is `GetOpenFileNameW` / `GetSaveFileNameW` on Windows and `NSOpenPanel` / `NSSavePanel`
/// on macOS, which is what every other application on those desktops opens. Linux has no
/// dialog of its own below the toolkits, so there it is the desktop's own dialog program:
/// zenity (GNOME's, and what most desktops have) or kdialog (KDE's, asked first on KDE).
///
/// `kind` is what the dialog lists first: a takt4 preset (`.json`), or a fixture's definition —
/// a GDTF file or an Open Fixture Library file (`.gdtf`, `.json`) — for the patch editor's import.
enum class FileKind { Preset, FixtureDefinition };

/// What came back: the file chosen, or an empty path for a cancel — which is not an error and
/// must not be reported as one — and, when no dialog could be shown at all, `problem`: the
/// sentence the window shows instead, so a press of IMPORT is never silence.
struct FileChoice {
    std::filesystem::path path;
    std::string problem;
};

/// A window's file dialogs. **The answer comes to `chosen` on the UI thread**, and these must
/// be called from it:
///
///   * On Windows and macOS before `open` or `save` returns. The dialog is modal and runs its
///     own message loop, which goes on drawing the window behind it.
///   * On Linux later, from a Slint timer. The dialog is another program, and waiting for it
///     inside a click would stop the window drawing while it is up — and a desktop that pings
///     a window which has stopped answering offers to kill it.
///
/// So a caller must not assume the answer has arrived when the call returns, and must read
/// whatever it needs at the moment it does. **An answer still on its way when this is destroyed
/// is dropped**, and the dialog program closed: `chosen` is never called on a window that has
/// gone. One dialog at a time; asking while one is up is answered with a `problem`.
class FileDialogs {
public:
    using Chosen = std::function<void(const FileChoice&)>;

    FileDialogs();
    ~FileDialogs();
    FileDialogs(const FileDialogs&) = delete;
    FileDialogs& operator=(const FileDialogs&) = delete;

    void open(const std::string& title, FileKind kind, Chosen chosen);
    void save(const std::string& title, const std::string& suggested, Chosen chosen);

    /// Whether a dialog is up and its answer not yet in.
    bool waiting() const;

private:
    struct Pending;
    std::unique_ptr<Pending> pending_;
};

/// Whether a dialog may be shown at all: false while `TAKT4_NO_FILE_DIALOGS` is set, when every
/// dialog is answered with a cancel without showing anything.
///
/// **The test binaries set it** (`tests/support/crt_dialogs.cpp`). A window test drives real
/// clicks, and one whose clicks land on EXPORT or IMPORT opened a real modal dialog on the
/// desktop the tests run on — which is the rig — and left it there for somebody to dismiss,
/// again for every click. It happened on 2026-09-25, when the status bar's buttons moved under
/// a sweep of clicks aimed at where something else used to be.
bool fileDialogsAllowed();

} // namespace takt4::ui
