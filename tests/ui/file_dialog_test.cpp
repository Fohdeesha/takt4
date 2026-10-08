// The Linux file dialog: the desktop's own program, waited for without stopping the window. Not
// built elsewhere — Windows' and macOS's dialogs are modal system calls a test cannot drive.

#if !defined(_WIN32) && !defined(__APPLE__)

#include "support/scoped_env.hpp"
#include "support/temp_dir.hpp"
#include "ui/file_dialog.hpp"

#include <slint-platform.h>

#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

using takt4::ui::FileChoice;
using takt4::ui::FileDialogs;
using takt4::ui::FileKind;

namespace {

/// A stand-in for zenity, first on the PATH: it writes its arguments, one a line, and its process
/// id beside itself, waits `seconds`, prints `answer` and leaves with `status` — what zenity does
/// with a file chosen (0) or a cancel (1). `exec` so the process waited for is the one killed.
class FakeZenity {
public:
    FakeZenity(const std::string& answer, int status, double seconds = 0.2)
        : path_("PATH", (folder_.path().string() + ":/usr/bin:/bin").c_str()),
          dialogs_("TAKT4_NO_FILE_DIALOGS", nullptr), desktop_("XDG_CURRENT_DESKTOP", nullptr) {
        const std::filesystem::path script = folder_.path() / "zenity";
        std::ofstream out(script);
        out << "#!/bin/sh\n"
            << "here=$(dirname \"$0\")\n"
            << "echo $$ > \"$here/pid\"\n"
            << "printf '%s\\n' \"$@\" > \"$here/args\"\n"
            << "sleep " << seconds << "\n"
            << "echo '" << answer << "'\n"
            << "exit " << status << "\n";
        out.close();
        ::chmod(script.c_str(), 0755);
    }

    std::string arguments() const { return read("args"); }
    int pid() const {
        const std::string text = read("pid");
        return text.empty() ? 0 : std::stoi(text);
    }

private:
    std::string read(const char* name) const {
        std::ifstream in(folder_.path() / name);
        std::stringstream all;
        all << in.rdbuf();
        return all.str();
    }

    takt4::test::TempDir folder_;
    takt4::test::ScopedVariable path_;
    takt4::test::ScopedVariable dialogs_;
    takt4::test::ScopedVariable desktop_;
};

/// Turns the event loop, as the window's own does, until `done` or two seconds.
template <typename Done>
void waitFor(Done done) {
    for (int i = 0; i < 200 && !done(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        slint::platform::update_timers_and_animations();
    }
}

} // namespace

TEST_CASE("on Linux a file is chosen in the desktop's own dialog, and the window keeps running",
          "[ui]") {
    SECTION("a file chosen comes back from the event loop, not inside the click") {
        const FakeZenity zenity("/home/someone/shows/friday.json", 0);
        FileDialogs dialogs;
        std::optional<FileChoice> got;
        dialogs.open("Import takt4 settings", FileKind::Preset,
                     [&got](const FileChoice& choice) { got = choice; });
        // Not answered yet: the window would have stopped drawing for as long as it was up.
        CHECK_FALSE(got.has_value());
        CHECK(dialogs.waiting());
        waitFor([&] { return got.has_value(); });
        REQUIRE(got.has_value());
        CHECK(got->path == std::filesystem::path("/home/someone/shows/friday.json"));
        CHECK(got->problem.empty());
        CHECK_FALSE(dialogs.waiting());
        const std::string arguments = zenity.arguments();
        INFO(arguments);
        CHECK(arguments.find("--file-selection\n") != std::string::npos);
        CHECK(arguments.find("--title=Import takt4 settings\n") != std::string::npos);
        CHECK(arguments.find("--file-filter=presets (*.json) | *.json\n") != std::string::npos);
        CHECK(arguments.find("--save") == std::string::npos);
    }

    SECTION("a save asks before it replaces a file, and suggests the name") {
        const FakeZenity zenity("/tmp/takt4-settings.json", 0);
        FileDialogs dialogs;
        std::optional<FileChoice> got;
        dialogs.save("Export takt4 settings", "takt4-settings.json",
                     [&got](const FileChoice& choice) { got = choice; });
        waitFor([&] { return got.has_value(); });
        REQUIRE(got.has_value());
        CHECK(got->path == std::filesystem::path("/tmp/takt4-settings.json"));
        const std::string arguments = zenity.arguments();
        CHECK(arguments.find("--save\n") != std::string::npos);
        CHECK(arguments.find("--confirm-overwrite\n") != std::string::npos);
        CHECK(arguments.find("--filename=takt4-settings.json\n") != std::string::npos);
    }

    SECTION("a cancel is a cancel, not a problem") {
        const FakeZenity zenity("", 1);
        FileDialogs dialogs;
        std::optional<FileChoice> got;
        dialogs.open("Import a fixture's definition", FileKind::FixtureDefinition,
                     [&got](const FileChoice& choice) { got = choice; });
        waitFor([&] { return got.has_value(); });
        REQUIRE(got.has_value());
        CHECK(got->path.empty());
        CHECK(got->problem.empty());
        CHECK(zenity.arguments().find("GDTF (*.gdtf) | *.gdtf") != std::string::npos);
    }

    SECTION("one dialog at a time") {
        const FakeZenity zenity("/tmp/a.json", 0, 1.0);
        FileDialogs dialogs;
        int answers = 0;
        dialogs.open("first", FileKind::Preset, [&answers](const FileChoice&) { ++answers; });
        std::optional<FileChoice> second;
        dialogs.open("second", FileKind::Preset,
                     [&second](const FileChoice& choice) { second = choice; });
        REQUIRE(second.has_value()); // answered at once
        CHECK(second->path.empty());
        CHECK(second->problem == "A file dialog is already open.");
        waitFor([&] { return answers == 1; });
        CHECK(answers == 1);
    }

    SECTION("a window closed with its dialog up closes the dialog, and hears nothing") {
        const FakeZenity zenity("/tmp/never.json", 0, 30.0);
        bool heard = false;
        int pid = 0;
        {
            FileDialogs dialogs;
            dialogs.open("Import takt4 settings", FileKind::Preset,
                         [&heard](const FileChoice&) { heard = true; });
            waitFor([&] { return zenity.pid() != 0; });
            pid = zenity.pid();
            REQUIRE(pid > 0);
            CHECK(::kill(pid, 0) == 0); // up
        }
        // Gone with the window, and waited for, so not even a zombie is left.
        CHECK(::kill(pid, 0) == -1);
        CHECK(errno == ESRCH);
        waitFor([] { return false; }); // the timer's turn, had it been left running
        CHECK_FALSE(heard);
    }

    SECTION("no dialog program says so, rather than doing nothing") {
        const takt4::test::ScopedVariable path("PATH", "/nonexistent");
        const takt4::test::ScopedVariable dialogs_allowed("TAKT4_NO_FILE_DIALOGS", nullptr);
        FileDialogs dialogs;
        std::optional<FileChoice> got;
        dialogs.open("Import takt4 settings", FileKind::Preset,
                     [&got](const FileChoice& choice) { got = choice; });
        REQUIRE(got.has_value());
        CHECK(got->path.empty());
        CHECK(got->problem.find("zenity") != std::string::npos);
    }
}

#endif
