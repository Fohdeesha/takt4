#include "ui/crash_report.hpp"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// dbghelp.h must follow windows.h. Only its types are used: the function itself is looked up
// at install time, so nothing has to be loaded while the process is falling over.
#include <dbghelp.h>

#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>

namespace takt4::ui {
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

/// Everything the handlers need, set once at install and never allocated again: a crash may
/// be a corrupted heap, and a handler that allocates can fall over inside it.
constexpr std::size_t kPathCapacity = 1024;
MiniDumpWriteDumpFn g_writeDump = nullptr;
wchar_t g_directory[kPathCapacity] = {};
wchar_t g_executable[kPathCapacity] = {};
wchar_t g_logPath[kPathCapacity] = {};
bool g_dialog = true;
HANDLE g_log = INVALID_HANDLE_VALUE;
/// Set by the first failure. A second one — another thread falling over at the same moment —
/// waits for the first report instead of racing it.
std::atomic<bool> g_crashing{false};
/// The thread writing the report. If *it* fails, nothing more can be done and the process is
/// ended at once rather than left waiting on itself.
std::atomic<DWORD> g_reporter{0};

struct Job {
    EXCEPTION_POINTERS* info = nullptr;
    DWORD threadId = 0;
    wchar_t path[kPathCapacity] = {};
    bool written = false;
};

void restartAfter(DWORD pid) {
    // The new instance is told which process to wait for, so it does not try to open the
    // audio interface while this one — about to be terminated — still holds it.
    wchar_t command[kPathCapacity + 64] = {};
    swprintf_s(command, L"\"%ls\" --after-crash %lu", g_executable, static_cast<unsigned long>(pid));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                       &process) != 0) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

DWORD WINAPI writeAndTell(void* parameter) {
    Job& job = *static_cast<Job*>(parameter);
    if (g_writeDump != nullptr) {
        const HANDLE file = CreateFileW(job.path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{};
            exception.ThreadId = job.threadId;
            exception.ExceptionPointers = job.info;
            exception.ClientPointers = FALSE;
            // Every thread's stack and whatever the stacks point at, which is what turns
            // `takt4.exe+0xf80723` into a function and a line — without the whole heap, which
            // would make each dump hundreds of megabytes.
            const auto type = static_cast<MINIDUMP_TYPE>(
                MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
            job.written = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                      job.info != nullptr ? &exception : nullptr, nullptr,
                                      nullptr) != FALSE;
            CloseHandle(file);
            if (!job.written) {
                DeleteFileW(job.path);
            }
        }
    }
    if (!g_dialog) {
        return 0;
    }
    wchar_t text[kPathCapacity * 2 + 512] = {};
    swprintf_s(text,
               L"takt4 has stopped because of an error.\n\n%ls%ls%ls\n"
               L"Settings are saved automatically a few seconds after every change, so little "
               L"or nothing should be lost.\n\nStart takt4 again now?",
               job.written ? L"A crash report was saved as:\n" : L"No crash report could be written.",
               job.written ? job.path : L"", job.written ? L"\n" : L"");
    const int answer = MessageBoxW(nullptr, text, L"takt4 has stopped",
                                   MB_YESNO | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND);
    if (answer == IDYES) {
        restartAfter(GetCurrentProcessId());
    }
    return 0;
}

[[noreturn]] void endProcess(UINT code) {
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) {
        Sleep(INFINITE); // TerminateProcess does not return for the calling process
    }
}

[[noreturn]] void report(EXCEPTION_POINTERS* info, UINT exitCode) {
    if (g_crashing.exchange(true)) {
        if (g_reporter.load() == GetCurrentThreadId()) {
            endProcess(exitCode); // the report itself fell over
        }
        for (;;) {
            Sleep(INFINITE); // the first failure is being reported; it ends the process
        }
    }
    if (g_log != INVALID_HANDLE_VALUE) {
        std::fflush(stderr);
        FlushFileBuffers(g_log);
    }
    static Job job; // static: the crashing thread's stack may be the thing that is gone
    job.info = info;
    job.threadId = GetCurrentThreadId();
    SYSTEMTIME now{};
    GetLocalTime(&now);
    swprintf_s(job.path, L"%ls\\takt4-crash-%04u%02u%02u-%02u%02u%02u.dmp", g_directory,
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    // Written from a fresh thread: a stack overflow leaves no stack to write a dump from, and
    // the dump is better taken of a thread that is stopped than of the one doing the writing.
    DWORD reporter = 0;
    const HANDLE worker = CreateThread(nullptr, 512 * 1024, writeAndTell, &job, 0, &reporter);
    if (worker != nullptr) {
        g_reporter.store(reporter);
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
    } else {
        g_reporter.store(GetCurrentThreadId());
        writeAndTell(&job);
    }
    endProcess(exitCode);
}

/// A failure that is not an SEH exception, reported as if it were one, with a context captured
/// here — so the dump's stack is the stack of whatever called `terminate` or `abort`.
[[noreturn]] void reportHere(DWORD code) {
    static CONTEXT context;
    static EXCEPTION_RECORD record;
    RtlCaptureContext(&context);
    record = EXCEPTION_RECORD{};
    record.ExceptionCode = code;
#if defined(_M_X64)
    record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
#endif
    static EXCEPTION_POINTERS pointers;
    pointers.ExceptionRecord = &record;
    pointers.ContextRecord = &context;
    report(&pointers, 3);
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS* info) {
    report(info, info != nullptr && info->ExceptionRecord != nullptr
                     ? info->ExceptionRecord->ExceptionCode
                     : 3);
}

void onTerminate() {
    reportHere(CrashReport::kTerminateCode);
}

void onAbort(int) {
    reportHere(CrashReport::kAbortCode);
}

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    reportHere(CrashReport::kInvalidParameterCode);
}

void onPureCall() {
    reportHere(CrashReport::kPureCallCode);
}

/// Points stderr — the CRT's and the process's — at `takt4.log`, unless something is already
/// there to receive it: a console takt4 was started from, or a pipe.
void redirectStderr() {
    const HANDLE existing = GetStdHandle(STD_ERROR_HANDLE);
    if (existing != nullptr && existing != INVALID_HANDLE_VALUE) {
        return;
    }
    // **One open, by the CRT.** In a process with no console the CRT's `stderr` is attached to
    // no descriptor at all (`_fileno` says -2), so pointing descriptor 2 somewhere changes
    // nothing for it; `freopen` is the documented way to give it a file. The first version
    // opened the log itself as well and `freopen` then failed against that second handle,
    // leaving `stderr` closed — measured: the next write was an invalid-parameter failure
    // rather than a line in the log.
    FILE* stream = nullptr;
    if (_wfreopen_s(&stream, g_logPath, L"w", stderr) != 0) {
        // Give the stream *something*, so that a write to it is discarded rather than refused.
        (void)_wfreopen_s(&stream, L"NUL", L"w", stderr);
        return;
    }
    // Unbuffered, so a line printed just before a crash is on disk when the process goes.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    // And the same handle as the process's stderr, which is what Rust asks for on every write —
    // so a Slint panic message lands in the same file, at the same file position, in order.
    g_log = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr)));
    if (g_log == INVALID_HANDLE_VALUE || g_log == nullptr) {
        g_log = INVALID_HANDLE_VALUE;
        return;
    }
    SetStdHandle(STD_ERROR_HANDLE, g_log);
}

void copyInto(wchar_t (&out)[kPathCapacity], const std::wstring& text) {
    wcsncpy_s(out, text.c_str(), _TRUNCATE);
}

} // namespace

void CrashReport::install(const std::filesystem::path& directory, bool dialog) {
    g_dialog = dialog;
    copyInto(g_directory, directory.native());
    copyInto(g_logPath, (directory / "takt4.log").native());
    GetModuleFileNameW(nullptr, g_executable, kPathCapacity);

    // Looked up now, while the process is healthy: loading a DLL from inside a crash is how a
    // crash handler becomes a hang.
    if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
        g_writeDump =
            reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    }

    redirectStderr();

    SetUnhandledExceptionFilter(onUnhandled);
    // Per thread in MSVC's runtime, so this covers the UI thread. The others reach `abort` when
    // they terminate, which the signal handler below covers — SIGABRT's handler is global.
    std::set_terminate(onTerminate);
    std::signal(SIGABRT, onAbort);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_purecall_handler(onPureCall);
    // No "abort() has been called" box of the runtime's own, and no Windows Error Reporting
    // pass after this one: the handler has said everything, and one dialog is enough.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

void CrashReport::cleanExit() {
    if (g_log == INVALID_HANDLE_VALUE) {
        return;
    }
    // Anything still writing to stderr from here on writes to nowhere, which is what a window
    // application's stderr always was. The stream's descriptor is moved onto the null device
    // with `_dup2`, which closes the log's handle and — unlike reopening the stream — leaves
    // the stream exactly as it was if it fails: a closed `stderr` turns the next write into an
    // invalid-parameter crash, which is the last thing a clean exit should do.
    std::fflush(stderr);
    SetStdHandle(STD_ERROR_HANDLE, nullptr);
    int nowhere = -1;
    if (_wsopen_s(&nowhere, L"NUL", _O_WRONLY, _SH_DENYNO, 0) == 0 && nowhere >= 0) {
        if (_dup2(nowhere, _fileno(stderr)) == 0) {
            DeleteFileW(g_logPath);
        }
        _close(nowhere);
    }
    g_log = INVALID_HANDLE_VALUE;
}

std::filesystem::path CrashReport::takeLeftoverLog(const std::filesystem::path& directory) {
    const std::filesystem::path log = directory / "takt4.log";
    // Opened without sharing first: another takt4 running from the same folder holds its log
    // open, and renaming a live one would report a crash that has not happened.
    const HANDLE probe = CreateFileW(log.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        return {};
    }
    CloseHandle(probe);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[64] = {};
    swprintf_s(name, L"takt4-unclean-%04u%02u%02u-%02u%02u%02u.log", now.wYear, now.wMonth,
               now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::filesystem::path kept = directory / name;
    if (MoveFileExW(log.c_str(), kept.c_str(), 0) == 0) {
        return {};
    }
    return kept;
}

void CrashReport::crashOnPurpose(const std::string& how) {
    if (how == "access-violation") {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    } else if (how == "terminate") {
        std::terminate();
    } else if (how == "abort") {
        std::abort();
    } else if (how == "invalid-parameter") {
        // A CRT function handed an argument it refuses, which is what reaches the handler.
        char buffer[4] = {};
        (void)strcpy_s(buffer, sizeof(buffer), nullptr);
    } else if (how == "pure-call") {
        // The classic way to reach one: a base constructor calling a pure virtual through a
        // non-virtual helper, while the object is still only a base.
        struct Base {
            Base() { start(); }
            virtual ~Base() = default;
            Base(const Base&) = delete;
            Base& operator=(const Base&) = delete;
            void start() { run(); }
            virtual void run() = 0;
        };
        struct Derived final : Base {
            void run() override {}
        };
        const Derived derived;
        (void)derived;
    }
}

} // namespace takt4::ui

#else

namespace takt4::ui {

void CrashReport::install(const std::filesystem::path&, bool) {}
void CrashReport::cleanExit() {}
std::filesystem::path CrashReport::takeLeftoverLog(const std::filesystem::path&) {
    return {};
}
void CrashReport::crashOnPurpose(const std::string&) {}

} // namespace takt4::ui

#endif
