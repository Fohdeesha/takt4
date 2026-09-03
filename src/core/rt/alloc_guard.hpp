#pragma once

#include <cstdint>

// The audio thread must never touch the heap (HANDOFF §4.2, §7.6). This is the guard
// that proves it: code running on the audio thread wraps itself in a RealtimeScope, and
// an executable built with the guard replaces the global operator new/delete with
// versions that count (or abort on) any allocation made inside such a scope.
//
// The replacement operators live in alloc_guard_new.cpp, which is compiled into each
// executable through the takt4::rt_guard target (a replacement in a static library
// might not be pulled in by the linker). TAKT4_RT_ALLOC_GUARD selects whether that
// file actually replaces anything; without it every scope is a no-op and
// allocationGuardEnabled() reports false.

namespace takt4::rt {

/// Marks the calling thread as real-time for its lifetime. Scopes nest.
class RealtimeScope {
public:
    RealtimeScope() noexcept;
    ~RealtimeScope();

    RealtimeScope(const RealtimeScope&) = delete;
    RealtimeScope& operator=(const RealtimeScope&) = delete;
};

/// Whether the current thread is inside a RealtimeScope.
bool inRealtimeScope() noexcept;

/// Whether this executable was built with the replacement operators, i.e. whether
/// violations can be detected at all.
bool allocationGuardEnabled() noexcept;

/// Heap operations seen inside a RealtimeScope on any thread since the process
/// started. Always 0 when the guard is not enabled.
std::uint64_t violationCount() noexcept;

/// By default a violation aborts the process after printing where it happened, so it
/// cannot go unnoticed in a debug run. Tests turn this off and read violationCount().
void setAbortOnViolation(bool abort) noexcept;
bool abortOnViolation() noexcept;

namespace detail {

// Called by the replacement operators; not for general use.
void markGuardPresent() noexcept;
void checkHeapUse(const char* operation) noexcept;

} // namespace detail

} // namespace takt4::rt
