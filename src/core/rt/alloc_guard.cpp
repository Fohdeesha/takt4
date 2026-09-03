#include "core/rt/alloc_guard.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace takt4::rt {

namespace {

// Nesting depth of RealtimeScope on this thread. A plain int: the replacement operators
// read it from the same thread, and thread_local ints need no dynamic initialisation
// (which could itself allocate).
thread_local int g_realtimeDepth = 0;

std::atomic<bool> g_guardPresent{false};
std::atomic<bool> g_abortOnViolation{true};
std::atomic<std::uint64_t> g_violations{0};

} // namespace

RealtimeScope::RealtimeScope() noexcept {
    ++g_realtimeDepth;
}

RealtimeScope::~RealtimeScope() {
    --g_realtimeDepth;
}

bool inRealtimeScope() noexcept {
    return g_realtimeDepth > 0;
}

bool allocationGuardEnabled() noexcept {
    return g_guardPresent.load(std::memory_order_relaxed);
}

std::uint64_t violationCount() noexcept {
    return g_violations.load(std::memory_order_relaxed);
}

void setAbortOnViolation(bool abort) noexcept {
    g_abortOnViolation.store(abort, std::memory_order_relaxed);
}

bool abortOnViolation() noexcept {
    return g_abortOnViolation.load(std::memory_order_relaxed);
}

namespace detail {

void markGuardPresent() noexcept {
    g_guardPresent.store(true, std::memory_order_relaxed);
}

void checkHeapUse(const char* operation) noexcept {
    if (g_realtimeDepth <= 0) {
        return;
    }
    g_violations.fetch_add(1, std::memory_order_relaxed);
    if (g_abortOnViolation.load(std::memory_order_relaxed)) {
        // stderr is unbuffered, so this does not allocate on the way out.
        std::fputs("takt4: heap ", stderr);
        std::fputs(operation, stderr);
        std::fputs(" on a real-time thread (HANDOFF §4.2); aborting\n", stderr);
        std::abort();
    }
}

} // namespace detail

} // namespace takt4::rt
