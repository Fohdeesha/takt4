#pragma once

#if defined(_WIN32)

#include <windows.h>

#include <string>

namespace takt4::test {

/// Sets an environment variable for one test and puts back what was there on the way out.
/// Null removes it. The process's own block is what changes, because the variables this is for
/// are read by child processes — which inherit that block — and by `GetEnvironmentVariable`.
class ScopedVariable {
public:
    ScopedVariable(const char* name, const char* value) : name_(name) {
        char previous[4096] = {};
        const DWORD length = GetEnvironmentVariableA(name, previous, sizeof previous);
        had_ = length > 0 && length < sizeof previous;
        previous_ = had_ ? std::string(previous, length) : std::string();
        SetEnvironmentVariableA(name, value);
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ~ScopedVariable() { SetEnvironmentVariableA(name_, had_ ? previous_.c_str() : nullptr); }

private:
    const char* name_;
    bool had_ = false;
    std::string previous_;
};

} // namespace takt4::test

#endif
