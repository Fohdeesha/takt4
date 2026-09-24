#include "core/rt/thread_priority.hpp"

#if defined(_WIN32)
#include <windows.h>
// avrt.h after windows.h, which it assumes.
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
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
