// On Windows, a Debug-CRT abort() or failed assert() pops a modal "Debug Error!" dialog
// and waits for a click, which turns a failing test into a ctest hang. Route both to
// stderr instead, so an abort — Eigen's alignment asserts, the [rt] allocation guard —
// fails the test the way it does on every other platform. Runs before main() by being
// a static initializer in the test executable.

#ifdef _MSC_VER

#include <crtdbg.h>
#include <cstdlib>

namespace {

struct NoCrtDialogs {
    NoCrtDialogs() noexcept {
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
