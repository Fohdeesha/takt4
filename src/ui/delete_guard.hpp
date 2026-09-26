#pragma once

#include <chrono>
#include <optional>

namespace takt4::ui {

/// Tells a row's × pressed on purpose from the second click of a double-click on it.
///
/// Deleting a row moves the one below it up under the pointer, so the second click of a
/// double-click on × landed on the next row's × and deleted that one too — two rows for one
/// gesture, with no undo (the 2026-09-25 audit's L10). A press on the same row's mark within
/// the system's double-click time of the last press is taken for that second click and
/// ignored; a press after it, or on another row, is an operator deleting on purpose.
///
/// On the click's path only — the markup's callback — so a controller's own `removeAt` still
/// does what it is asked, however quickly it is asked.
class DeleteGuard {
public:
    using Clock = std::chrono::steady_clock;

    /// Whether this press on row `index`'s × deletes it. Every press restarts the window, so a
    /// triple click is one deletion as well.
    bool press(int index, Clock::time_point now = Clock::now()) noexcept;

    /// The system's double-click time; 500 ms where it cannot be asked.
    static Clock::duration interval() noexcept;

private:
    std::optional<Clock::time_point> last_;
    int lastIndex_ = -1;
};

} // namespace takt4::ui
