#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace takt4::ui {

/// What takt4 does when it falls over: a minidump beside the executable, a message saying
/// where it is, and the choice to start again.
///
/// **The process that fell over ends at once.** The message comes from a process of its own
/// — this executable again, started with `kAfterCrashOption` — which waits for the old one to
/// be gone before it says anything. Until 2026-09-24 the message was shown by the process that
/// had crashed, and every thread it had left went on running while the message sat unanswered:
/// the outputs kept sending, the window kept reopening the audio interface. The operator's call
/// was to end it at once.
///
/// **Why it exists.** takt4 crashed on a rig on 2026-09-16 and the only reason the cause was
/// ever found is that Windows Error Reporting happened to be configured to keep a dump. Nothing
/// in takt4 wrote one, so on any other machine a crash was a window that vanished and nothing
/// to read afterwards (the audit's H16). This writes `takt4-crash-<date>-<time>.dmp` for every
/// way a C++ program can die that can be caught at all:
///
///   * an unhandled SEH exception — an access violation, a stack overflow;
///   * `std::terminate` — an exception with nowhere to go, a `noexcept` that threw;
///   * `abort()` and the CRT's invalid-parameter and pure-virtual-call handlers.
///
/// **What it cannot catch, and what is done instead.** A panic in Slint's Rust code ends the
/// process with `__fastfail`, which goes straight to the kernel past every handler a process
/// can install. Rust prints the panic's message to stderr first, though, and a window
/// application's stderr goes nowhere — so this points it at `takt4.log` beside the executable.
/// A clean exit deletes that file; one still there at the next launch is a session that did not
/// end normally, and `takeLeftoverLog` hands it to the window to say so.
///
/// Windows only; a no-op elsewhere, where the platform's own core dumps do this job.
class CrashReport {
public:
    /// Installs the handlers and redirects stderr. Once, first thing, before any thread is
    /// started — a thread created earlier would not inherit the redirected handle's CRT stream.
    ///
    /// `directory` is where dumps and the log go: the executable's own folder in the app, a
    /// temporary one in a test. `notice` is what this executable is started with to tell the
    /// operator after a crash: `kAfterCrashOption` in the app, the name of a hidden test case in
    /// a test, and empty for none — a test that has nobody to answer the message.
    static void install(const std::filesystem::path& directory, std::string_view notice);

    /// The argument takt4.exe is started with to say that the process before it crashed.
    /// Not in the usage text: nobody types it.
    static constexpr std::string_view kAfterCrashOption = "--after-crash";

    /// Run by the process started to tell the operator: waits for the one that crashed to be
    /// gone, says so and where its report is, and asks whether to start takt4 again. True for
    /// yes. The crashed process and its dump are handed over in the environment, which carries
    /// a path in any language where a command line would have to be quoted and decoded.
    static bool tellAfterCrash();

    /// Undoes the stderr redirection and deletes `takt4.log`: the session ended as it should.
    /// Call on the way out of a clean run and nowhere else.
    static void cleanExit();

    /// A log left by a session that did not end cleanly, renamed out of the way so the next
    /// crash cannot overwrite it; empty when the last session ended normally. Read by the
    /// window to say "takt4 did not close normally last time" and where the log is.
    static std::filesystem::path takeLeftoverLog(const std::filesystem::path& directory);

    /// The exception codes a dump carries for the failures that are not SEH exceptions of
    /// their own, so a reader of the dump can tell which handler wrote it. In the range
    /// Microsoft reserves for applications (bit 29 set).
    static constexpr std::uint32_t kTerminateCode = 0xE0747401;
    static constexpr std::uint32_t kAbortCode = 0xE0747402;
    static constexpr std::uint32_t kInvalidParameterCode = 0xE0747403;
    static constexpr std::uint32_t kPureCallCode = 0xE0747404;

    /// Crashes the process on purpose, the way `how` names: "access-violation",
    /// "stack-overflow", "terminate", "abort", "invalid-parameter" or "pure-call". Anything else
    /// does nothing.
    ///
    /// Also covers "abort from another thread": "terminate" and "abort" are called on whichever
    /// thread calls this, and a test calls it from a worker to prove the global handlers reach
    /// threads that `std::set_terminate` — per thread in MSVC's runtime — does not.
    ///
    /// A bench switch that ships, like `TAKT4_TICK_PROBE`: `ui::run` calls it with the value of
    /// `TAKT4_TEST_CRASH`, which is how crash reporting is proved to work on a venue laptop
    /// with the real executable rather than trusted.
    static void crashOnPurpose(const std::string& how);
};

} // namespace takt4::ui
