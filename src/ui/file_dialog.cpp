#include "ui/file_dialog.hpp"

#if defined(_WIN32)
// <windows.h> before <commdlg.h>, and NOMINMAX because this translation unit is compiled
// with the same warning flags as the rest and `max` as a macro breaks <algorithm> for
// anything that includes this later.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <commdlg.h>

#include <cstddef>
#elif !defined(__APPLE__)
#include <slint.h>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

extern char** environ; // the process's environment, which <unistd.h> leaves undeclared
#endif

#include <cstdlib>
#include <initializer_list>
#include <string>

namespace takt4::ui {

bool fileDialogsAllowed() {
#if defined(_MSC_VER)
    std::size_t length = 0;
    return getenv_s(&length, nullptr, 0, "TAKT4_NO_FILE_DIALOGS") != 0 || length == 0;
#else
    const char* const value = std::getenv("TAKT4_NO_FILE_DIALOGS");
    return value == nullptr || *value == '\0';
#endif
}

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          needed);
    return out;
}

/// The filter is a list of null-separated label/pattern pairs, ended by a second null —
/// which is why it is built rather than written as a literal: a string literal stops at the
/// first embedded null and the dialog would see one truncated entry.
std::wstring filterOf(std::initializer_list<const wchar_t*> parts) {
    std::wstring out;
    for (const wchar_t* part : parts) {
        out.append(part);
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

const std::wstring& filter(FileKind kind) {
    static const std::wstring presets =
        filterOf({L"presets (*.json)", L"*.json", L"all files", L"*.*"});
    static const std::wstring definitions =
        filterOf({L"fixture definitions (*.gdtf, *.json)", L"*.gdtf;*.json", L"GDTF (*.gdtf)",
                  L"*.gdtf", L"Open Fixture Library (*.json)", L"*.json", L"all files", L"*.*"});
    return kind == FileKind::FixtureDefinition ? definitions : presets;
}

OPENFILENAMEW baseOf(std::wstring& buffer, const std::wstring& title, FileKind kind) {
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ::GetActiveWindow();
    ofn.lpstrFilter = filter(kind).c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = title.empty() ? nullptr : title.c_str();
    ofn.lpstrDefExt = kind == FileKind::Preset ? L"json" : nullptr;
    return ofn;
}

std::wstring bufferWith(const std::string& suggested) {
    std::wstring buffer(1024, L'\0');
    const std::wstring wide = widen(suggested);
    // Leave room for the terminator the dialog writes.
    for (std::size_t i = 0; i < wide.size() && i + 1 < buffer.size(); ++i) {
        buffer[i] = wide[i];
    }
    return buffer;
}

std::filesystem::path showOpen(const std::string& title, FileKind kind) {
    std::wstring buffer = bufferWith("");
    const std::wstring wideTitle = widen(title);
    OPENFILENAMEW ofn = baseOf(buffer, wideTitle, kind);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetOpenFileNameW(&ofn) == 0) {
        return {}; // cancelled, or the dialog could not be shown; both mean "do nothing"
    }
    return std::filesystem::path(buffer.c_str());
}

std::filesystem::path showSave(const std::string& title, const std::string& suggested) {
    std::wstring buffer = bufferWith(suggested);
    const std::wstring wideTitle = widen(title);
    OPENFILENAMEW ofn = baseOf(buffer, wideTitle, FileKind::Preset);
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetSaveFileNameW(&ofn) == 0) {
        return {};
    }
    return std::filesystem::path(buffer.c_str());
}

} // namespace

// A modal dialog answers before it returns, so there is never anything pending.
struct FileDialogs::Pending {};

FileDialogs::FileDialogs() = default;
FileDialogs::~FileDialogs() = default;

bool FileDialogs::waiting() const {
    return false;
}

void FileDialogs::open(const std::string& title, FileKind kind, Chosen chosen) {
    chosen(FileChoice{fileDialogsAllowed() ? showOpen(title, kind) : std::filesystem::path{}, {}});
}

void FileDialogs::save(const std::string& title, const std::string& suggested, Chosen chosen) {
    chosen(FileChoice{fileDialogsAllowed() ? showSave(title, suggested) : std::filesystem::path{},
                      {}});
}

#elif defined(__APPLE__)

// NSOpenPanel and NSSavePanel are Objective-C: see file_dialog_mac.mm.

#else

namespace {

/// `name`'s full path where the PATH finds it, or empty.
std::string onPath(const std::string& name) {
    const char* const path = std::getenv("PATH");
    if (path == nullptr) {
        return {};
    }
    std::string_view rest(path);
    for (;;) {
        const std::size_t colon = rest.find(':');
        const std::string_view folder = rest.substr(0, colon);
        const std::string candidate =
            (folder.empty() ? std::string(".") : std::string(folder)) + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
        if (colon == std::string_view::npos) {
            return {};
        }
        rest.remove_prefix(colon + 1);
    }
}

bool onKde() {
    const char* const desktop = std::getenv("XDG_CURRENT_DESKTOP");
    return desktop != nullptr && std::string_view(desktop).find("KDE") != std::string_view::npos;
}

/// A dialog program and what to tell it.
struct Command {
    std::string name; // for a sentence about it
    std::string program;
    std::vector<std::string> arguments; // the program's name first, as a process sees it
};

/// zenity's filters, one argument each: `label | pattern pattern`.
std::vector<std::string> zenityFilters(FileKind kind) {
    if (kind == FileKind::FixtureDefinition) {
        return {"--file-filter=fixture definitions (*.gdtf, *.json) | *.gdtf *.json",
                "--file-filter=GDTF (*.gdtf) | *.gdtf",
                "--file-filter=Open Fixture Library (*.json) | *.json",
                "--file-filter=all files | *"};
    }
    return {"--file-filter=presets (*.json) | *.json", "--file-filter=all files | *"};
}

/// kdialog's filter, one argument: `pattern pattern|label` per line.
std::string kdialogFilter(FileKind kind) {
    if (kind == FileKind::FixtureDefinition) {
        return "*.gdtf *.json|fixture definitions (*.gdtf, *.json)\n*.gdtf|GDTF (*.gdtf)\n"
               "*.json|Open Fixture Library (*.json)\n*|all files";
    }
    return "*.json|presets (*.json)\n*|all files";
}

/// The dialog to run, or none when neither program is installed. KDE's own first on KDE, where
/// zenity is a GTK program in a Qt desktop; zenity first everywhere else.
std::optional<Command> dialogCommand(bool saving, const std::string& title,
                                     const std::string& suggested, FileKind kind) {
    const std::string zenity = onPath("zenity");
    const std::string kdialog = onPath("kdialog");
    const bool useKdialog = !kdialog.empty() && (zenity.empty() || onKde());
    if (useKdialog) {
        Command command{"kdialog", kdialog, {"kdialog", "--title", title}};
        command.arguments.push_back(saving ? "--getsavefilename" : "--getopenfilename");
        command.arguments.push_back(saving ? suggested : std::string("."));
        command.arguments.push_back(kdialogFilter(kind));
        return command;
    }
    if (zenity.empty()) {
        return std::nullopt;
    }
    Command command{"zenity", zenity, {"zenity", "--file-selection", "--title=" + title}};
    if (saving) {
        command.arguments.push_back("--save");
        // Asked for, since zenity 3 replaces a file without asking. zenity 4 always asks and
        // takes this as a no-op.
        command.arguments.push_back("--confirm-overwrite");
        command.arguments.push_back("--filename=" + suggested);
    }
    for (std::string& filter : zenityFilters(kind)) {
        command.arguments.push_back(std::move(filter));
    }
    return command;
}

constexpr auto kPollEvery = std::chrono::milliseconds(100);

} // namespace

/// The dialog program while it is up: its process, the pipe its answer comes down, and who
/// to give the answer to. Read by a timer on the UI thread, so nothing here is shared with
/// another thread.
struct FileDialogs::Pending {
    pid_t child = -1;
    int output = -1; // the read end of the child's standard output
    std::string answer;
    std::string name;
    Chosen chosen;
    slint::Timer poll;

    ~Pending() { stop(); }

    bool up() const { return child > 0; }

    /// Starts `command` with its standard output to a pipe this end can read without
    /// blocking, and its standard input from nothing.
    bool start(const Command& command) {
        int ends[2] = {-1, -1};
        if (::pipe(ends) != 0) {
            return false;
        }
        // Neither end leaks into any other child; the dup onto 1 below is not close-on-exec.
        (void)::fcntl(ends[0], F_SETFD, FD_CLOEXEC);
        (void)::fcntl(ends[1], F_SETFD, FD_CLOEXEC);
        (void)::fcntl(ends[0], F_SETFL, O_NONBLOCK);
        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        std::vector<char*> argv;
        argv.reserve(command.arguments.size() + 1);
        for (const std::string& argument : command.arguments) {
            argv.push_back(const_cast<char*>(argument.c_str())); // posix_spawn's own signature
        }
        argv.push_back(nullptr);
        pid_t started = -1;
        const int result = ::posix_spawn(&started, command.program.c_str(), &actions, nullptr,
                                         argv.data(), environ);
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(ends[1]);
        if (result != 0) {
            ::close(ends[0]);
            return false;
        }
        child = started;
        output = ends[0];
        answer.clear();
        name = command.name;
        return true;
    }

    void drain() {
        char buffer[4096];
        for (;;) {
            const ssize_t got = ::read(output, buffer, sizeof buffer);
            if (got <= 0) {
                return; // nothing more yet, the end of it, or an error: the exit says which
            }
            answer.append(buffer, static_cast<std::size_t>(got));
        }
    }

    /// The child's answer once it has gone, or nothing while it is still up.
    std::optional<FileChoice> check() {
        drain();
        int status = 0;
        const pid_t gone = ::waitpid(child, &status, WNOHANG);
        if (gone == 0) {
            return std::nullopt;
        }
        drain(); // whatever it wrote just before it went
        ::close(output);
        output = -1;
        child = -1;
        poll.stop();
        FileChoice choice;
        if (gone > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            const std::size_t end = answer.find('\n');
            choice.path = std::filesystem::path(answer.substr(0, end));
        } else if (gone < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 1) {
            // 1 is both programs' cancel; anything else is a dialog that never showed.
            choice.problem = name + " could not show a file dialog.";
        }
        return choice;
    }

    /// Shows `command`'s dialog and has the timer hand its answer to `then`; or answers at once
    /// when there is to be no dialog, or no dialog can be shown.
    void ask(std::optional<Command> command, Chosen then) {
        if (!fileDialogsAllowed()) {
            then(FileChoice{});
            return;
        }
        if (up()) {
            then(FileChoice{{}, "A file dialog is already open."});
            return;
        }
        if (!command) {
            then(FileChoice{{}, "Choosing a file needs zenity, or kdialog on KDE: install one."});
            return;
        }
        if (!start(*command)) {
            then(FileChoice{{}, command->name + " could not be started."});
            return;
        }
        chosen = std::move(then);
        poll.start(slint::TimerMode::Repeated, kPollEvery, [this] {
            if (std::optional<FileChoice> choice = check()) {
                // Taken out before it is called, so a caller that asks again from inside finds
                // nothing pending.
                Chosen waiting = std::move(chosen);
                chosen = nullptr;
                if (waiting) {
                    waiting(*choice);
                }
            }
        });
    }

    /// Closes the dialog, if one is up, and forgets whoever was waiting for it.
    void stop() {
        poll.stop();
        if (child > 0) {
            // A dialog has nothing to save, and a blocking wait on one that ignored a polite
            // signal would hang the window closing.
            ::kill(child, SIGKILL);
            int status = 0;
            (void)::waitpid(child, &status, 0);
            child = -1;
        }
        if (output >= 0) {
            ::close(output);
            output = -1;
        }
        chosen = nullptr;
    }
};

FileDialogs::FileDialogs() : pending_(std::make_unique<Pending>()) {}
FileDialogs::~FileDialogs() = default;

bool FileDialogs::waiting() const {
    return pending_->up();
}

void FileDialogs::open(const std::string& title, FileKind kind, Chosen chosen) {
    pending_->ask(dialogCommand(false, title, "", kind), std::move(chosen));
}

void FileDialogs::save(const std::string& title, const std::string& suggested, Chosen chosen) {
    pending_->ask(dialogCommand(true, title, suggested, FileKind::Preset), std::move(chosen));
}

#endif

} // namespace takt4::ui
