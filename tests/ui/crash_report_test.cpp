#include "ui/crash_report.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)

#include <slint.h>

#include <windows.h>
// After windows.h, which it needs.
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using takt4::ui::CrashReport;

namespace {

/// The variables that turn this test binary into the child of the tests below: where to write,
/// how to fall over, and whether to start a notice afterwards, as takt4.exe does.
constexpr const char* kDirectoryVariable = "TAKT4_TEST_CRASH_DIR";
constexpr const char* kHowVariable = "TAKT4_TEST_CRASH_HOW";
constexpr const char* kNoticeVariable = "TAKT4_TEST_CRASH_NOTICE";
/// What the child is started with to tell the operator, in place of takt4.exe's
/// `--after-crash`: this binary again, running the hidden case below.
constexpr const char* kNoticeCase = "\"crash notice child\"";
/// How the crashed process hands itself to the notice (`ui/crash_report.cpp`).
constexpr const wchar_t* kCrashedPidVariable = L"TAKT4_CRASHED_PID";
constexpr const wchar_t* kCrashDumpVariable = L"TAKT4_CRASH_DUMP";
constexpr const wchar_t* kNoticeTitle = L"takt4 has stopped";

std::string environment(const char* name) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    const std::string out(value);
    std::free(value);
    return out;
}

std::vector<unsigned char> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>());
}

std::uint32_t u32(const std::vector<unsigned char>& bytes, std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

/// What a person reading the dump without a debugger checks first — the memory note on
/// reading one: the signature, the exception stream's code, and whether the module list names
/// the executable that fell over. Parsed by hand so the test depends on nothing but the format.
struct Dump {
    bool valid = false;
    std::optional<std::uint32_t> exceptionCode;
    bool namesThisExecutable = false;
};

Dump readDump(const std::filesystem::path& path) {
    Dump dump;
    const std::vector<unsigned char> bytes = readAll(path);
    if (bytes.size() < 32 || u32(bytes, 0) != 0x504D444D) { // "MDMP"
        return dump;
    }
    dump.valid = true;
    const std::uint32_t streams = u32(bytes, 8);
    const std::uint32_t directory = u32(bytes, 12);
    for (std::uint32_t i = 0; i < streams; ++i) {
        const std::size_t entry = directory + std::size_t{i} * 12;
        if (entry + 12 > bytes.size()) {
            break;
        }
        const std::uint32_t type = u32(bytes, entry);
        const std::uint32_t rva = u32(bytes, entry + 8);
        if (type == 6 && rva + 12 <= bytes.size()) { // ExceptionStream
            // ThreadId, alignment, then MINIDUMP_EXCEPTION::ExceptionCode.
            dump.exceptionCode = u32(bytes, rva + 8);
        }
        if (type == 4 && rva + 4 <= bytes.size()) { // ModuleListStream
            const std::uint32_t modules = u32(bytes, rva);
            // Each MINIDUMP_MODULE is 108 bytes; its name is an RVA at offset 20 to a
            // MINIDUMP_STRING (a byte length, then UTF-16).
            for (std::uint32_t m = 0; m < modules; ++m) {
                const std::size_t module = rva + 4 + std::size_t{m} * 108;
                if (module + 24 > bytes.size()) {
                    break;
                }
                const std::uint32_t nameRva = u32(bytes, module + 20);
                if (nameRva + 4 > bytes.size()) {
                    continue;
                }
                const std::uint32_t length = u32(bytes, nameRva);
                if (nameRva + 4 + length > bytes.size()) {
                    continue;
                }
                std::wstring name(length / 2, L'\0');
                std::memcpy(name.data(), bytes.data() + nameRva + 4, length);
                if (name.find(L"takt4_ui_tests.exe") != std::wstring::npos) {
                    dump.namesThisExecutable = true;
                }
            }
        }
    }
    return dump;
}

std::vector<std::filesystem::path> filesIn(const std::filesystem::path& dir,
                                           const std::wstring& extension) {
    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == extension) {
            found.push_back(entry.path());
        }
    }
    return found;
}

/// Starts this binary again as the child, falling over `how`, and waits for it. Detached, so
/// it has no console and no stderr of its own — exactly the situation takt4.exe is in when it
/// is double-clicked, and the one in which the log redirection has anything to do.
///
/// With `notice`, the child starts a notice when it falls over, as takt4.exe does, and nobody
/// answers it: the child has to end on its own. `pid` is the child's process id.
DWORD runChild(const std::filesystem::path& dir, const std::string& how, bool notice = false,
               DWORD* pid = nullptr) {
    wchar_t self[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(nullptr, self, MAX_PATH) > 0);
    std::wstring command =
        L"\"" + std::wstring(self) + L"\" \"crash report child\"";
    REQUIRE(SetEnvironmentVariableA(kDirectoryVariable, dir.string().c_str()) != 0);
    REQUIRE(SetEnvironmentVariableA(kHowVariable, how.c_str()) != 0);
    if (notice) {
        REQUIRE(SetEnvironmentVariableA(kNoticeVariable, "1") != 0);
    }
#if defined(__SANITIZE_ADDRESS__)
    // Where the sanitizer's own report goes, said outright. The child runs detached, with no
    // console: with MSVC 19.44 the report reached takt4's log through the stderr it redirects,
    // and with the newer toolchain on the CI runners it went nowhere at all (2026-09-23) —
    // the test found neither a report nor a dump. A file of its own in the test's folder
    // leaves nothing to chance.
    const DWORD asanLength = GetEnvironmentVariableA("ASAN_OPTIONS", nullptr, 0);
    const bool hadOptions = asanLength > 1;
    std::string previousAsan(hadOptions ? asanLength : 0, '\0');
    if (hadOptions) {
        previousAsan.resize(
            GetEnvironmentVariableA("ASAN_OPTIONS", previousAsan.data(), asanLength));
    }
    // Quoted: ASAN_OPTIONS separates its options with ':', which a drive letter has one of. And
    // added to whatever the run already set rather than put in its place, so the child runs
    // with the same checks as the test that started it (the audit of 2026-09-25, B1).
    const std::string asanLog = "log_path='" + (dir / "asan").string() + "'";
    const std::string options = hadOptions ? previousAsan + ":" + asanLog : asanLog;
    REQUIRE(SetEnvironmentVariableA("ASAN_OPTIONS", options.c_str()) != 0);
#endif
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                        DETACHED_PROCESS, nullptr, nullptr, &startup, &child);
    SetEnvironmentVariableA(kDirectoryVariable, nullptr);
    SetEnvironmentVariableA(kHowVariable, nullptr);
    SetEnvironmentVariableA(kNoticeVariable, nullptr);
#if defined(__SANITIZE_ADDRESS__)
    SetEnvironmentVariableA("ASAN_OPTIONS", hadOptions ? previousAsan.c_str() : nullptr);
#endif
    REQUIRE(started != 0);
    if (pid != nullptr) {
        *pid = child.dwProcessId;
    }
    // With a notice, promptly: writing the dump is the only thing it has to wait for, and a
    // crashed process kept alive for a minute — what an unanswered message did — is the bug.
    const DWORD waited = WaitForSingleObject(child.hProcess, notice ? 20000 : 60000);
    DWORD code = 0;
    GetExitCodeProcess(child.hProcess, &code);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 1);
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    REQUIRE(waited == WAIT_OBJECT_0);
    return code;
}

/// The "takt4 has stopped" message on screen from process `from`; null when there is none.
HWND findNotice(DWORD from) {
    struct Search {
        DWORD from;
        HWND found;
    } search{from, nullptr};
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(parameter);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            wchar_t title[64] = {};
            GetWindowTextW(window, title, 64);
            if (owner == s.from && std::wstring_view(title) == kNoticeTitle) {
                s.found = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

/// Waits up to `limit` for the message from `from`; null if it never came.
HWND waitForNotice(DWORD from, std::chrono::milliseconds limit) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (true) {
        if (const HWND found = findNotice(from)) {
            return found;
        }
        if (std::chrono::steady_clock::now() > until) {
            return nullptr;
        }
        Sleep(20);
    }
}

/// The processes `parent` started that are still running.
std::vector<DWORD> childrenOf(DWORD parent) {
    std::vector<DWORD> children;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return children;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE;
         more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID == parent) {
            children.push_back(entry.th32ProcessID);
        }
    }
    CloseHandle(snapshot);
    return children;
}

/// What the message says.
std::wstring noticeText(HWND box) {
    // A message box's text is the static control it gives the id 0xFFFF.
    wchar_t text[4096] = {};
    GetDlgItemTextW(box, 0xFFFF, text, 4096);
    return text;
}

/// Presses one of the message's buttons (IDYES, IDNO) the way a click does.
void press(HWND box, int button) {
    const HWND control = GetDlgItem(box, button);
    REQUIRE(control != nullptr);
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(control, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 5000, &ignored);
}

std::wstring environmentW(const wchar_t* name) {
    wchar_t value[4096] = {};
    const DWORD length = GetEnvironmentVariableW(name, value, 4096);
    return length > 0 && length < 4096 ? std::wstring(value, length) : std::wstring();
}

/// `CrashReport::tellAfterCrash` on a thread of its own, as the notice process runs it, with
/// whatever is left on screen answered and the thread joined on the way out — so a failing
/// check cannot leave a message up, or a thread std::thread's destructor would terminate on.
///
/// `crashed` is the process standing in for the one that fell over, ended first on the way
/// out, since the message waits for it.
class Teller {
public:
    explicit Teller(HANDLE crashed = nullptr)
        : crashed_(crashed),
          thread_([this] { answer_.store(CrashReport::tellAfterCrash() ? 1 : 0); }) {}
    Teller(const Teller&) = delete;
    Teller& operator=(const Teller&) = delete;
    ~Teller() {
        if (crashed_ != nullptr) {
            TerminateProcess(crashed_, 1);
        }
        while (answer_.load() < 0) {
            if (const HWND box = findNotice(GetCurrentProcessId())) {
                PostMessageW(box, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
            }
            Sleep(20);
        }
        thread_.join();
    }
    /// -1 while the question is open, then 1 for yes and 0 for no.
    int waitForAnswer(std::chrono::milliseconds limit) const {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (answer_.load() < 0 && std::chrono::steady_clock::now() < until) {
            Sleep(10);
        }
        return answer_.load();
    }

private:
    HANDLE crashed_;
    std::atomic<int> answer_{-1};
    std::thread thread_;
};

/// A process that is alive and does nothing: this binary, created suspended. Stands in for a
/// process that has crashed and is not yet gone.
class Suspended {
public:
    Suspended() {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(self) + L"\" --list-tests";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process_) == 0) {
            process_ = PROCESS_INFORMATION{};
        }
    }
    Suspended(const Suspended&) = delete;
    Suspended& operator=(const Suspended&) = delete;
    ~Suspended() {
        if (process_.hProcess != nullptr) {
            TerminateProcess(process_.hProcess, 1);
            CloseHandle(process_.hThread);
            CloseHandle(process_.hProcess);
        }
    }
    HANDLE handle() const { return process_.hProcess; }
    DWORD id() const { return process_.dwProcessId; }

private:
    PROCESS_INFORMATION process_{};
};

} // namespace

TEST_CASE("crash report child", "[.child]") {
    // Hidden: only ever run by the tests below, as a process of its own that falls over.
    const std::string dir = environment(kDirectoryVariable);
    const std::string how = environment(kHowVariable);
    if (dir.empty() || how.empty()) {
        SKIP("run only as the child of the crash report tests");
    }
    CrashReport::install(std::filesystem::path(dir),
                         environment(kNoticeVariable) == "1" ? kNoticeCase : "");
    if (how == "clean-exit") {
        // The way out of an ordinary session: something is printed, the log is let go, and a
        // write to stderr afterwards — a static destructor, a library's last warning — must be
        // discarded rather than fail.
        std::fputs("an ordinary session\n", stderr);
        CrashReport::cleanExit();
        std::fputs("after the log has gone\n", stderr);
        std::fflush(stderr);
        return;
    }
    if (how == "terminate-thread" || how == "abort-thread") {
        // From a thread that did not install anything: `std::set_terminate` is per thread in
        // MSVC's runtime, so this is what proves the global SIGABRT handler catches the rest.
        std::thread worker([how] {
            if (how == "terminate-thread") {
                std::terminate();
            }
            std::abort();
        });
        worker.join();
    } else if (how == "slint-panic") {
        // The crash class of 2026-09-16 and of `trigger::Value`'s note: a string Slint cannot
        // decode is a Rust panic across the C ABI, which ends the process with `__fastfail`
        // past every handler. What can be kept is the message Rust prints on the way.
        std::fputs("before the panic\n", stderr);
        const slint::SharedString bad(std::string_view("\xFF"));
        (void)bad;
    } else {
        CrashReport::crashOnPurpose(how);
    }
    FAIL("still running after crashing on purpose");
}

TEST_CASE("crash notice child", "[.child]") {
    // Hidden: started by a crash report child that fell over, in the place of takt4.exe
    // --after-crash, and running the same code. Its exit code says what was answered: 0 for
    // no, which is what the test presses.
    if (environmentW(kCrashedPidVariable).empty()) {
        SKIP("run only by a crash report child that fell over");
    }
    CHECK_FALSE(CrashReport::tellAfterCrash());
}

TEST_CASE("a crash leaves a minidump that names what happened", "[ui][crash]") {
    // The audit's H16: nothing in takt4 wrote a dump, so a crash on any machine without WER's
    // LocalDumps configured was a window that vanished and nothing to read afterwards. Each way
    // a C++ process can die that *can* be caught, run for real in a child process, and the dump
    // it leaves read back the way the memory note on minidumps says to read one.
    struct Case {
        const char* how;
        std::uint32_t code;
    };
    const Case cases[] = {
        {"access-violation", 0xC0000005},
        {"terminate", CrashReport::kTerminateCode},
        {"abort", CrashReport::kAbortCode},
        {"invalid-parameter", CrashReport::kInvalidParameterCode},
        {"pure-call", CrashReport::kPureCallCode},
        // Another thread's terminate reaches `abort`, and SIGABRT's handler is global.
        {"terminate-thread", CrashReport::kAbortCode},
        {"abort-thread", CrashReport::kAbortCode},
    };
    for (const Case& c : cases) {
        INFO("failure: " << c.how);
        const takt4::test::TempDir dir;
        const DWORD exit = runChild(dir.path(), c.how);
        CHECK(exit != 0);

#if defined(__SANITIZE_ADDRESS__)
        // Under AddressSanitizer a hardware fault can belong to the sanitizer: with MSVC 19.44
        // its own handler reports it — with far more than a minidump would say — and ends the
        // process before an unhandled-exception filter is ever consulted, so the report has to
        // be *kept*, which is the log's job. With the newer toolchain on the CI runners the log
        // came back empty (2026-09-23), which is the fault reaching takt4's own filter instead;
        // then it is the minidump below that has to be there. Either way the crash is recorded,
        // and a run with neither fails. The shipped build has no sanitizer.
        if (std::string_view(c.how) == "access-violation") {
            const std::vector<unsigned char> bytes = readAll(dir.path() / "takt4.log");
            std::string reports(bytes.begin(), bytes.end());
            // And the sanitizer's own file, `asan.<pid>`, which runChild points it at.
            for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
                if (entry.path().filename().string().rfind("asan", 0) == 0) {
                    const std::vector<unsigned char> more = readAll(entry.path());
                    reports.append(more.begin(), more.end());
                }
            }
            INFO("log and sanitizer report:\n" << reports);
            if (reports.find("AddressSanitizer") != std::string::npos) {
                continue;
            }
        }
#endif

        const std::vector<std::filesystem::path> dumps = filesIn(dir.path(), L".dmp");
        REQUIRE(dumps.size() == 1);
        CHECK(dumps.front().filename().string().rfind("takt4-crash-", 0) == 0);
        const Dump dump = readDump(dumps.front());
        CHECK(dump.valid);
        REQUIRE(dump.exceptionCode.has_value());
        CHECK(*dump.exceptionCode == c.code);
        CHECK(dump.namesThisExecutable);
        // The log stays behind too — the session did not end cleanly — for the next launch
        // to find and report.
        CHECK(std::filesystem::exists(dir.path() / "takt4.log"));
    }
}

TEST_CASE("a Slint panic message is kept even though nothing can catch the panic",
          "[ui][crash]") {
    const takt4::test::TempDir dir;
    const DWORD exit = runChild(dir.path(), "slint-panic");
    CHECK(exit != 0);
    const std::vector<unsigned char> bytes = readAll(dir.path() / "takt4.log");
    const std::string log(bytes.begin(), bytes.end());
    INFO("log:\n" << log);
    // Both halves reach the file: what C++ wrote through the CRT's stderr and what Rust wrote
    // through the process's — the latter being the only record a panic leaves.
    CHECK(log.find("before the panic") != std::string::npos);
    CHECK(log.find("panicked") != std::string::npos);
    CHECK(log.find("Utf8Error") != std::string::npos);
}

TEST_CASE("a clean exit leaves no log and no dump behind", "[ui][crash]") {
    // The other half of "did not close normally": a session that ended as it should must not
    // leave the file whose presence means it did not.
    const takt4::test::TempDir dir;
    const DWORD exit = runChild(dir.path(), "clean-exit");
    CHECK(exit == 0);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "takt4.log"));
    CHECK(filesIn(dir.path(), L".dmp").empty());
}

TEST_CASE("a crash ends the process at once and the message comes from a process of its own",
          "[ui][crash]") {
    // The operator's call on 2026-09-24. The message used to be shown by the process that had
    // crashed, and every thread it had left — the outputs, the window reopening the audio —
    // ran on until somebody answered it. Here nobody answers, and the child has to end anyway.
    const takt4::test::TempDir dir;
    DWORD crashed = 0;
    // A failure that reaches takt4's own handler under AddressSanitizer too; an access
    // violation can be the sanitizer's to report (see the test above).
    const DWORD exit = runChild(dir.path(), "terminate", /*notice=*/true, &crashed);
    CHECK(exit != 0);
    const std::vector<std::filesystem::path> dumps = filesIn(dir.path(), L".dmp");
    REQUIRE(dumps.size() == 1);

    // The message, from a process the crashed one started — found by that, since another test
    // process may have a message of its own on screen at the same moment.
    HWND box = nullptr;
    DWORD teller = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (box == nullptr && std::chrono::steady_clock::now() < until) {
        for (const DWORD child : childrenOf(crashed)) {
            if ((box = findNotice(child)) != nullptr) {
                teller = child;
                break;
            }
        }
        Sleep(20);
    }
    REQUIRE(box != nullptr);
    CHECK(teller != crashed);
    const HANDLE process =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE,
                    teller);
    REQUIRE(process != nullptr);
    struct Close {
        HANDLE process;
        ~Close() {
            TerminateProcess(process, 1); // a no-op once it has ended
            CloseHandle(process);
        }
    } const close{process};

    const std::wstring text = noticeText(box);
    INFO("the message: " << std::filesystem::path(text).string());
    CHECK(text.find(L"takt4 has stopped") != std::wstring::npos);
    CHECK(text.find(dumps.front().wstring()) != std::wstring::npos);
    CHECK(text.find(L"Start takt4 again now?") != std::wstring::npos);

    press(box, IDNO);
    REQUIRE(WaitForSingleObject(process, 10000) == WAIT_OBJECT_0);
    DWORD answered = 1;
    GetExitCodeProcess(process, &answered);
    CHECK(answered == 0); // the hidden case's CHECK_FALSE held: "no" starts nothing
}

TEST_CASE("the crash message waits for the crashed process to be gone and starts again on yes",
          "[ui][crash]") {
    const takt4::test::TempDir dir;
    const std::filesystem::path dump = dir.path() / "takt4-crash-20260924-120000.dmp";
    std::ofstream(dump) << "MDMP";
    const Suspended stillThere;
    REQUIRE(stillThere.handle() != nullptr);
    REQUIRE(SetEnvironmentVariableW(kCrashedPidVariable,
                                    std::to_wstring(stillThere.id()).c_str()) != 0);
    REQUIRE(SetEnvironmentVariableW(kCrashDumpVariable, dump.c_str()) != 0);
    // As a crash made on purpose with the bench switch leaves it: in the runtime's copy of the
    // environment, where a process starting up finds it, as well as Windows's.
    REQUIRE(_putenv_s("TAKT4_TEST_CRASH", "terminate") == 0);
    REQUIRE(environment("TAKT4_TEST_CRASH") == "terminate");

    const Teller teller(stillThere.handle());
    // Nothing is said while the process that crashed is still there: starting again then
    // would find the audio interface held by it.
    CHECK(waitForNotice(GetCurrentProcessId(), std::chrono::milliseconds(1500)) == nullptr);
    // And the handover is not left for a takt4 started from here to carry, nor the bench
    // switch, which would crash the takt4 that Yes starts.
    CHECK(environmentW(kCrashedPidVariable).empty());
    CHECK(environmentW(kCrashDumpVariable).empty());
    CHECK(environmentW(L"TAKT4_TEST_CRASH").empty());
    // Read as `ui::run` reads it, through the runtime's copy.
    CHECK(environment("TAKT4_TEST_CRASH").empty());

    TerminateProcess(stillThere.handle(), 1);
    const HWND box = waitForNotice(GetCurrentProcessId(), std::chrono::seconds(10));
    REQUIRE(box != nullptr);
    const std::wstring text = noticeText(box);
    INFO("the message: " << std::filesystem::path(text).string());
    CHECK(text.find(dump.wstring()) != std::wstring::npos);
    CHECK(text.find(L"no longer sending anything") != std::wstring::npos);

    press(box, IDYES);
    CHECK(teller.waitForAnswer(std::chrono::seconds(10)) == 1);
}

TEST_CASE("the crash message says so when no report could be written, and no starts nothing",
          "[ui][crash]") {
    // No process to wait for and no dump: what the notice is handed when the dump failed.
    SetEnvironmentVariableW(kCrashedPidVariable, nullptr);
    REQUIRE(environmentW(kCrashedPidVariable).empty());
    REQUIRE(SetEnvironmentVariableW(kCrashDumpVariable, L"") != 0);
    const Teller teller;
    const HWND box = waitForNotice(GetCurrentProcessId(), std::chrono::seconds(10));
    REQUIRE(box != nullptr);
    const std::wstring text = noticeText(box);
    INFO("the message: " << std::filesystem::path(text).string());
    CHECK(text.find(L"No crash report could be written.") != std::wstring::npos);
    CHECK(text.find(L"saved as") == std::wstring::npos);

    press(box, IDNO);
    CHECK(teller.waitForAnswer(std::chrono::seconds(10)) == 0);
}

TEST_CASE("a log left by an unclean session is found once and kept", "[ui][crash]") {
    const takt4::test::TempDir dir;
    // Nothing there: a clean last session.
    CHECK(CrashReport::takeLeftoverLog(dir.path()).empty());

    std::ofstream(dir.path() / "takt4.log") << "thread 'main' panicked at ...\n";
    const std::filesystem::path kept = CrashReport::takeLeftoverLog(dir.path());
    REQUIRE_FALSE(kept.empty());
    CHECK(kept.filename().string().rfind("takt4-unclean-", 0) == 0);
    CHECK(std::filesystem::exists(kept));
    CHECK_FALSE(std::filesystem::exists(dir.path() / "takt4.log"));
    // And not reported twice.
    CHECK(CrashReport::takeLeftoverLog(dir.path()).empty());
}

#endif
