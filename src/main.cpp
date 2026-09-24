#include "core/build_info.hpp"
#include "ui/app.hpp"
#include "ui/crash_report.hpp"

#include <iostream>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#endif

namespace {

void printUsage(std::ostream& out) {
    out << "usage: takt4 [--version] [--help]\n"
           "\n"
           "  --version, -v   print version and library information, then exit\n"
           "  --help, -h      print this help, then exit\n";
}

#if defined(_WIN32)
/// Borrows the console takt4 was launched from, if it was launched from one.
///
/// takt4 is a **window** application on Windows (see src/CMakeLists.txt): a console-subsystem
/// binary opens a cmd box of its own next to the window, which is not something an operator at
/// a gig should have to look at or close. The cost is that `std::cout` then goes nowhere by
/// default — and `--version` is how the build in front of you is identified, so it has to keep
/// working from a shell.
///
/// `AttachConsole(ATTACH_PARENT_PROCESS)` gets the parent's console when there is one and
/// fails harmlessly when there is not — double-clicked from Explorer, nothing is attached and
/// no window appears, which is the whole point. Reopening the three standard streams onto it
/// is what makes the C++ ones write there.
void attachToLaunchingConsole() {
    // **Only when there is nothing there already.** A shell hands its child the standard
    // handles whatever subsystem the child is, so `takt4 --version` from cmd or PowerShell,
    // and `takt4 --version > file`, both already have somewhere to write — and reopening the
    // streams onto `CONOUT$` would send the output to the console *instead of* the pipe or
    // the file, which is a redirection that silently stops working. Measured: the first
    // version of this printed nothing at all through a pipe.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        return; // no console to borrow; nothing printed, and nothing opened either
    }
    FILE* stream = nullptr;
    (void)freopen_s(&stream, "CONOUT$", "w", stdout);
    (void)freopen_s(&stream, "CONOUT$", "w", stderr);
    std::ios::sync_with_stdio(true);
}
#endif

} // namespace

int main(int argc, char** argv) {
    // Started by an instance that fell over (`ui::CrashReport`), which ended as soon as it had
    // started this one: say so, and open the window only if the operator asks for it. Not in the
    // usage text: nobody types it.
    if (argc == 2 && std::string_view(argv[1]) == takt4::ui::CrashReport::kAfterCrashOption) {
        return takt4::ui::CrashReport::tellAfterCrash() ? takt4::ui::run() : 0;
    }

    // Any other argument is a question answered in text — `--version`, `--help`, or an option
    // nobody knows. No argument is "open the window", and only then is no console wanted: a
    // window application that borrowed its parent's console on every launch would print into
    // whatever shell happened to start it and hold that console open.
    //
    // `argv[1]` is answered and that is the end of it: every branch returns. This was a loop
    // over the arguments whose increment could never run, which is what MSVC's C4702 said.
    if (argc > 1) {
#if defined(_WIN32)
        attachToLaunchingConsole();
#endif
        const std::string_view arg = argv[1];
        if (arg == "--version" || arg == "-v") {
            std::cout << takt4::describe(takt4::buildInfo());
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            return 0;
        }
        std::cerr << "takt4: unknown option '" << arg << "'\n";
        printUsage(std::cerr);
        return 2;
    }

    return takt4::ui::run();
}
