// What every test process needs before main(), done by a static initializer in the test
// executable:
//
// - On Windows, a Debug-CRT abort() or failed assert() pops a modal "Debug Error!" dialog
//   and waits for a click, which turns a failing test into a ctest hang. Route both to
//   stderr instead, so an abort — Eigen's alignment asserts, the [rt] allocation guard —
//   fails the test the way it does on every other platform.
// - No ASIO, unless the run is for the rig (below).
// - A settings folder of its own (below).
// - No file dialogs (below).
// - The sandbox (below).
//
// Everything but the first is on every platform. It used to sit inside the MSVC block too, so
// a GCC or Clang build of the UI tests — linux-tsan builds them — opened file dialogs and saved
// beside the binary (the audit of 2026-09-25, T15).

#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _MSC_VER
#include <crtdbg.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

bool isSet(const char* name) {
#ifdef _MSC_VER
    std::size_t length = 0;
    return getenv_s(&length, nullptr, 0, name) == 0 && length > 1;
#else
    const char* const value = std::getenv(name);
    return value != nullptr && *value != '\0';
#endif
}

/// Into the CRT's copy of the environment and the system's both, so it reaches `getenv_s` here
/// and every process a test starts.
void set(const char* name, const std::string& value) {
#ifdef _MSC_VER
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

struct TestProcess {
    TestProcess() noexcept {
        // **No ASIO unless the run is for the rig.** Every window test builds a real tracker,
        // and a tracker enumerates every ASIO driver — which loads each driver into the test
        // process and initialises it, sixty times over, on the machine a show may be running
        // from (the audit's T2). The tests that need the interface itself are tagged
        // [hardware] and run with TAKT4_TEST_HARDWARE set; see the `-all` test presets.
        if (!isSet("TAKT4_TEST_HARDWARE")) {
            set("TAKT4_NO_ASIO", "1");
        }
        // **A settings folder of its own.** These binaries are built into the same folder as
        // takt4.exe, and settings live beside the program — so on the rig, a test whose click
        // landed on SAVE would have written over the show's settings.json. Each test process
        // gets a folder under the temp directory instead; `settings::settingsDirectory`
        // honours it. A folder only appears if something is saved.
        if (!isSet("TAKT4_SETTINGS_DIR")) {
            std::error_code code;
            const std::filesystem::path temp = std::filesystem::temp_directory_path(code);
            if (!code) {
#ifdef _MSC_VER
                const int pid = _getpid();
#else
                const int pid = static_cast<int>(::getpid());
#endif
                const std::filesystem::path folder =
                    temp / "takt4-tests" / std::to_string(pid);
#ifdef _MSC_VER
                _wputenv_s(L"TAKT4_SETTINGS_DIR", folder.wstring().c_str());
#else
                set("TAKT4_SETTINGS_DIR", folder.string());
#endif
                folder_ = folder;
            }
        }
        // **No file dialogs.** A window test drives real clicks, and one that lands on EXPORT or
        // IMPORT would open a real modal dialog on this desktop — the rig's — and leave it for
        // somebody to dismiss (2026-09-25, repeatedly). `ui::askSaveFile` and `askOpenFile`
        // return a cancel instead while this is set.
        set("TAKT4_NO_FILE_DIALOGS", "1");
        // **The sandbox: nothing a test does reaches the rig** (src/core/sandbox.hpp). On for
        // every test and for every process a test starts; tests/support/rig_sandbox.cpp lifts it
        // for the length of a test tagged [network] or [hardware].
        set("TAKT4_TEST_SANDBOX", "1");
#ifdef _MSC_VER
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _set_error_mode(_OUT_TO_STDERR);
#ifdef _DEBUG
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#endif
    }

    // **And that folder goes with the process** (the audit of 2026-09-25, T4): one was left in the
    // temp directory by every run of every test binary. Only the one made above — a folder named
    // by this process's id, under "takt4-tests" — and never one handed in from outside, which
    // could be anybody's.
    ~TestProcess() {
        if (folder_.empty()) {
            return;
        }
        std::error_code code;
        std::filesystem::remove_all(folder_, code);
        std::filesystem::remove(folder_.parent_path(), code); // only if no other run is using it
    }

    TestProcess(const TestProcess&) = delete;
    TestProcess& operator=(const TestProcess&) = delete;

private:
    std::filesystem::path folder_;
};

const TestProcess testProcess;

} // namespace
