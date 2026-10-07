#include "core/rt/thread_priority.hpp"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
// avrt.h after windows.h, which it assumes.
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
// Both from SDKs newer than some that build this; the values are Windows', not ours.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define TAKT4_X86 1
#endif

namespace takt4::rt {

namespace {

#if defined(TAKT4_X86)
/// MXCSR's flush-to-zero and denormals-are-zero bits: results that would be denormal come out
/// as zero, and denormal inputs are read as zero.
constexpr std::uint32_t kFlushToZero = 0x8000;
constexpr std::uint32_t kDenormalsAreZero = 0x0040;
#endif

} // namespace

PriorityScope::PriorityScope(ThreadWork work) noexcept {
#if defined(_WIN32)
    DWORD taskIndex = 0;
    const wchar_t* const task = work == ThreadWork::Output ? L"Pro Audio" : L"Audio";
    HANDLE handle = ::AvSetMmThreadCharacteristicsW(task, &taskIndex);
    if (handle != nullptr) {
        task_ = handle;
        raised_ = true;
        return;
    }
    // MMCSS is a service, and a machine can have it stopped. A raised ordinary priority is
    // most of the benefit: ahead of the window's thread, which is what this is for.
    const HANDLE self = ::GetCurrentThread();
    previousPriority_ = ::GetThreadPriority(self);
    const int wanted =
        work == ThreadWork::Output ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL;
    if (previousPriority_ != THREAD_PRIORITY_ERROR_RETURN && previousPriority_ < wanted &&
        ::SetThreadPriority(self, wanted) != 0) {
        fallback_ = true;
        raised_ = true;
    }
#else
    // Raising a thread's priority on Linux or macOS needs a privilege an application does not
    // have; the default is what it has always run at.
    (void)work;
#endif
}

PriorityScope::~PriorityScope() {
#if defined(_WIN32)
    if (task_ != nullptr) {
        ::AvRevertMmThreadCharacteristics(static_cast<HANDLE>(task_));
    } else if (fallback_) {
        ::SetThreadPriority(::GetCurrentThread(), previousPriority_);
    }
#endif
}

bool keepFullSpeed() noexcept {
#if defined(_WIN32)
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // The kinds named in the control mask, with nothing in the state mask: switched off.
    state.ControlMask =
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    if (::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &state,
                                sizeof state) != 0) {
        return true;
    }
    // A Windows from before the timer kind refuses the whole call for the flag it does not know.
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    (void)::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &state,
                                  sizeof state);
#endif
    return false;
}

RoundTimer::RoundTimer() noexcept {
#if defined(_WIN32)
    timer_ = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
#endif
}

RoundTimer::~RoundTimer() {
#if defined(_WIN32)
    if (timer_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(timer_));
    }
#endif
}

void RoundTimer::wait(std::chrono::microseconds period) noexcept {
    const auto now = std::chrono::steady_clock::now();
    // The first round, or one more than a period behind its time: the grid from here.
    if (!started_ || now - next_ > period) {
        next_ = now;
        started_ = true;
    }
    next_ += period;
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(next_ - now);
    if (left <= std::chrono::microseconds::zero()) {
        return; // behind, by less than a round: this one now
    }
#if defined(_WIN32)
    if (timer_ != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(left.count()) * 10; // relative, in 100 ns
        if (::SetWaitableTimer(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, FALSE) != 0 &&
            ::WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE) == WAIT_OBJECT_0) {
            return;
        }
    }
#endif
    std::this_thread::sleep_until(next_);
}

DenormalsAsZero::DenormalsAsZero() noexcept {
#if defined(TAKT4_X86)
    previous_ = _mm_getcsr();
    _mm_setcsr(previous_ | kFlushToZero | kDenormalsAreZero);
#endif
}

DenormalsAsZero::~DenormalsAsZero() {
#if defined(TAKT4_X86)
    _mm_setcsr(previous_);
#endif
}

bool DenormalsAsZero::active() noexcept {
#if defined(TAKT4_X86)
    const std::uint32_t both = kFlushToZero | kDenormalsAreZero;
    return (_mm_getcsr() & both) == both;
#else
    return false;
#endif
}

} // namespace takt4::rt
