#pragma once

#if defined(_WIN32)

#include <windows.h>

#include <cstdlib>
#include <string>

namespace takt4::test {

/// Sets an environment variable for one test and puts back what was there on the way out.
/// Null removes it.
///
/// **Both copies of the environment**, the process's block and the C runtime's. The block is
/// what a child process inherits and `GetEnvironmentVariable` reads; the CRT keeps its own copy
/// from startup, which `getenv_s` and `_wdupenv_s` read — and takt4 reads
/// `TAKT4_NO_FILE_DIALOGS`, `TAKT4_SETTINGS_DIR` and `TAKT4_TEST_SANDBOX` that way. Setting only
/// the block left those readers seeing the old value (the audit of 2026-09-25, T16).
/// `_putenv_s` writes both.
class ScopedVariable {
public:
    ScopedVariable(const char* name, const char* value) : name_(name) {
        char previous[4096] = {};
        const DWORD length = GetEnvironmentVariableA(name, previous, sizeof previous);
        had_ = length > 0 && length < sizeof previous;
        previous_ = had_ ? std::string(previous, length) : std::string();
        set(value);
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ~ScopedVariable() { set(had_ ? previous_.c_str() : nullptr); }

private:
    void set(const char* value) {
        // An empty value is how `_putenv_s` removes one; the block is told directly as well,
        // so a removal is a removal there too.
        (void)_putenv_s(name_, value != nullptr ? value : "");
        SetEnvironmentVariableA(name_, value);
    }

    const char* name_;
    bool had_ = false;
    std::string previous_;
};

} // namespace takt4::test

#endif
