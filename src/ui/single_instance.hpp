#pragma once

#include <filesystem>
#include <string>

namespace takt4::ui {

/// **One takt4 per settings folder.** A second launch from the same
/// folder brings the first one's window to the front and goes.
///
/// Two of them on one folder fought over everything: the second's output thread started at once
/// and sent the patched universes its own idle frames, both sent the global OSC messages and
/// joined Link, the second's log truncated the first's, and both autosaved one settings file, the
/// last writer winning. A second launch is natural — the window takes up to half a minute to
/// appear while the audio drivers are listed — and so is a watchdog that relaunches takt4 while
/// the old one still lives.
///
/// Claimed before anything else `ui::run` does. On Windows a named mutex, keyed on the folder;
/// elsewhere an exclusive lock on a file in it. Let go of when this is destroyed, or when the
/// process ends however it ends.
class SingleInstance {
public:
    explicit SingleInstance(const std::filesystem::path& folder);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    /// This process has the folder: no other takt4 on it was running.
    bool claimed() const noexcept { return claimed_; }

    /// Says which window a later launch is to bring to the front: `window`, a native handle —
    /// the starting splash, while the devices are listed — or, given none, this process's own
    /// main one, its first visible top-level window whose title begins "takt4". False while
    /// there is no such window yet: asked again until there is, since the window is shown only
    /// some turns of the event loop after it is made. True away from Windows, where there is
    /// nothing to do.
    bool announceWindow(void* window = nullptr);

    /// Brings the main window of the takt4 that has `folder` to the front, restored if it was
    /// minimised. False when it has said none yet (still starting) or is not there.
    static bool bringOtherToFront(const std::filesystem::path& folder);

    /// The name the folder is known by between processes — the same for every spelling of it.
    static std::string keyOf(const std::filesystem::path& folder);

private:
    bool claimed_ = false;
    std::string key_;
    /// Windows: the mutex and the mapping the window is announced in. POSIX: the lock file. Each
    /// unused on the other.
    [[maybe_unused]] void* mutex_ = nullptr;
    [[maybe_unused]] void* mapping_ = nullptr;
    [[maybe_unused]] int lockFile_ = -1;
};

} // namespace takt4::ui
