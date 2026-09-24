#include "ui/crash_report.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)

#include <slint.h>

#include <windows.h>

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

/// The two variables that turn this test binary into the child of the tests below: where to
/// write, and how to fall over.
constexpr const char* kDirectoryVariable = "TAKT4_TEST_CRASH_DIR";
constexpr const char* kHowVariable = "TAKT4_TEST_CRASH_HOW";

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
DWORD runChild(const std::filesystem::path& dir, const std::string& how) {
    wchar_t self[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(nullptr, self, MAX_PATH) > 0);
    std::wstring command =
        L"\"" + std::wstring(self) + L"\" \"crash report child\"";
    REQUIRE(SetEnvironmentVariableA(kDirectoryVariable, dir.string().c_str()) != 0);
    REQUIRE(SetEnvironmentVariableA(kHowVariable, how.c_str()) != 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                        DETACHED_PROCESS, nullptr, nullptr, &startup, &child);
    SetEnvironmentVariableA(kDirectoryVariable, nullptr);
    SetEnvironmentVariableA(kHowVariable, nullptr);
    REQUIRE(started != 0);
    const DWORD waited = WaitForSingleObject(child.hProcess, 60000);
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

} // namespace

TEST_CASE("crash report child", "[.child]") {
    // Hidden: only ever run by the tests below, as a process of its own that falls over.
    const std::string dir = environment(kDirectoryVariable);
    const std::string how = environment(kHowVariable);
    if (dir.empty() || how.empty()) {
        SKIP("run only as the child of the crash report tests");
    }
    CrashReport::install(std::filesystem::path(dir), /*dialog=*/false);
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
        // came back empty (2026-09-24), which is the fault reaching takt4's own filter instead;
        // then it is the minidump below that has to be there. Either way the crash is recorded,
        // and a run with neither fails. The shipped build has no sanitizer.
        if (std::string_view(c.how) == "access-violation") {
            const std::vector<unsigned char> bytes = readAll(dir.path() / "takt4.log");
            const std::string log(bytes.begin(), bytes.end());
            INFO("log:\n" << log);
            if (log.find("AddressSanitizer") != std::string::npos) {
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
