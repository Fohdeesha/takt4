#include "ui/file_dialog.hpp"
#include "ui/native_window.hpp"
#include "ui/single_instance.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <cderr.h>

#include <cwctype>
#endif

using takt4::ui::SingleInstance;

TEST_CASE("one takt4 to a settings folder: a second is refused it until the first lets go",
          "[ui]") {
    // Two takt4s on one folder fought over everything — the patched universes, Link, the global
    // OSC messages, the log and the settings file, the last writer winning. The second is refused
    // the folder however it is spelled, and another folder is another rig.
    const takt4::test::TempDir dir;
    const std::filesystem::path folder = dir.path() / "rig";
    const std::filesystem::path other = dir.path() / "another rig";
    std::filesystem::create_directories(folder);
    std::filesystem::create_directories(other);
    {
        const SingleInstance first(folder);
        CHECK(first.claimed());
        const SingleInstance second(folder);
        CHECK_FALSE(second.claimed());
        const SingleInstance spelled(folder / ".");
        CHECK_FALSE(spelled.claimed());
        CHECK(SingleInstance::keyOf(folder / ".") == SingleInstance::keyOf(folder));
#if defined(_WIN32)
        // One folder on Windows whatever the case it is typed in.
        std::wstring upper = folder.wstring();
        for (wchar_t& c : upper) {
            c = static_cast<wchar_t>(std::towupper(static_cast<std::wint_t>(c)));
        }
        CHECK(SingleInstance::keyOf(upper) == SingleInstance::keyOf(folder));
        const SingleInstance shouted{std::filesystem::path(upper)};
        CHECK_FALSE(shouted.claimed());
#endif
        const SingleInstance elsewhere(other);
        CHECK(elsewhere.claimed());
        CHECK(SingleInstance::keyOf(other) != SingleInstance::keyOf(folder));
        // The first has not said which window is its own yet — still starting, as far as a second
        // launch can tell — so there is nothing to bring to the front.
        CHECK_FALSE(SingleInstance::bringOtherToFront(folder));
    }
    // Let go of with the first, as when it quits, or falls over.
    const SingleInstance after(folder);
    CHECK(after.claimed());
}

#if defined(_WIN32)
TEST_CASE("the end of the Windows session is passed on once, from whichever window hears of it",
          "[ui]") {
    // Shutting down, restarting or signing out ends the process once its windows have answered
    // WM_ENDSESSION, and nothing after that ran: the last edits unsaved, the lights left as they
    // were, and a next launch that said takt4 had not closed normally. Two windows that are never
    // shown, taken over as the redraw takes takt4's own over.
    WNDCLASSW kind{};
    kind.lpfnWndProc = DefWindowProcW;
    kind.hInstance = GetModuleHandleW(nullptr);
    kind.lpszClassName = L"takt4.test.session";
    (void)RegisterClassW(&kind);
    const auto make = [&] {
        return CreateWindowExW(0, kind.lpszClassName, L"session test", WS_OVERLAPPEDWINDOW, 0, 0,
                               100, 100, nullptr, nullptr, kind.hInstance, nullptr);
    };
    const HWND first = make();
    const HWND second = make();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    takt4::ui::keepPaintingWhileDragged(nullptr, nullptr);
    int ended = 0;
    takt4::ui::whenSessionEnds([](void* user) { ++*static_cast<int*>(user); }, &ended);

    // Asked first, and nothing here holds a shutdown up.
    CHECK(SendMessageW(first, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF) != 0);
    CHECK(ended == 0);
    // Another program refused: the session goes on, and takt4 with it.
    SendMessageW(first, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF);
    CHECK(ended == 0);
    // It ends.
    SendMessageW(first, WM_ENDSESSION, TRUE, ENDSESSION_LOGOFF);
    CHECK(ended == 1);
    // Every window is told, and it is done once.
    SendMessageW(second, WM_ENDSESSION, TRUE, ENDSESSION_LOGOFF);
    CHECK(ended == 1);

    takt4::ui::whenSessionEnds(nullptr, nullptr);
    DestroyWindow(first);
    DestroyWindow(second);
}

TEST_CASE("a file dialog that could not do what was asked says so, and a cancel says nothing",
          "[ui]") {
    // Windows' dialogs come back with nothing for a cancel and for a failure alike, and every
    // failure read as a cancel: IMPORT pressed, a path too long for the dialog chosen, and
    // nothing happened with nothing said.
    CHECK(takt4::ui::fileDialogProblem(0).empty());
    const std::string tooLong = takt4::ui::fileDialogProblem(FNERR_BUFFERTOOSMALL);
    CHECK(tooLong.find("path that long") != std::string::npos);
    const std::string other = takt4::ui::fileDialogProblem(CDERR_INITIALIZATION);
    CHECK(other.find("could not be shown") != std::string::npos);
    CHECK(other.find(std::to_string(CDERR_INITIALIZATION)) != std::string::npos);
}
#endif
