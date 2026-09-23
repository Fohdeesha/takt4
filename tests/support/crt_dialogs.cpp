// Two things every Windows test process needs before main(), done by a static initializer in
// the test executable:
//
// - On Windows, a Debug-CRT abort() or failed assert() pops a modal "Debug Error!" dialog
//   and waits for a click, which turns a failing test into a ctest hang. Route both to
//   stderr instead, so an abort — Eigen's alignment asserts, the [rt] allocation guard —
//   fails the test the way it does on every other platform.
// - No ASIO, unless the run is for the rig (below).
// - A settings folder of its own (below).

#ifdef _MSC_VER

#include <crtdbg.h>
#include <process.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

struct NoCrtDialogs {
    NoCrtDialogs() noexcept {
        // **No ASIO unless the run is for the rig.** Every window test builds a real tracker,
        // and a tracker enumerates every ASIO driver — which loads each driver into the test
        // process and initialises it, sixty times over, on the machine a show may be running
        // from (the audit's T2). The tests that need the interface itself are tagged
        // [hardware] and run with TAKT4_TEST_HARDWARE set; see the `-all` test presets.
        size_t length = 0;
        if (getenv_s(&length, nullptr, 0, "TAKT4_TEST_HARDWARE") != 0 || length == 0) {
            _putenv_s("TAKT4_NO_ASIO", "1");
        }
        // **A settings folder of its own.** These binaries are built into the same folder as
        // takt4.exe, and settings live beside the program — so on the rig, a test whose click
        // landed on SAVE would have written over the show's settings.json. Each test process
        // gets a folder under the temp directory instead; `settings::settingsDirectory`
        // honours it. A folder only appears if something is saved.
        std::size_t named = 0;
        if (_wgetenv_s(&named, nullptr, 0, L"TAKT4_SETTINGS_DIR") != 0 || named == 0) {
            std::error_code code;
            const std::filesystem::path temp = std::filesystem::temp_directory_path(code);
            if (!code) {
                const std::wstring folder =
                    (temp / L"takt4-tests" / std::to_wstring(_getpid())).wstring();
                _wputenv_s(L"TAKT4_SETTINGS_DIR", folder.c_str());
            }
        }
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _set_error_mode(_OUT_TO_STDERR);
#ifdef _DEBUG
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    }
};

const NoCrtDialogs noCrtDialogs;

} // namespace

#endif
