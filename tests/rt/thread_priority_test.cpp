#include "core/rt/thread_priority.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cfloat>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

using takt4::rt::DenormalsAsZero;
using takt4::rt::PriorityScope;
using takt4::rt::ThreadWork;

namespace {

/// Halved at run time, on the calling thread's FPU settings: `volatile` keeps the compiler
/// from working it out once, at build time, on its own.
float halved(float value) {
    volatile float in = value;
    return in * 0.5f;
}

/// A denormal plus zero, which is the denormal — unless denormal inputs are read as zero.
float plusZero(float value) {
    volatile float in = value;
    volatile float zero = 0.0f;
    return in + zero;
}

} // namespace

TEST_CASE("a thread asked to run ahead does, and is put back afterwards", "[rt]") {
    // The audit's M13. Asked on a thread of its own, as each of takt4's threads asks for
    // itself, and read by the thread itself: what another thread's priority is cannot be
    // asked through a pseudo-handle. Catch2's assertions are not for other threads, so the
    // readings come back here.
    for (const ThreadWork work : {ThreadWork::Output, ThreadWork::Compute}) {
        bool raised = false;
        bool multimedia = false;
        int before = 0;
        int during = 0;
        int after = 0;
        std::thread([&] {
#if defined(_WIN32)
            before = ::GetThreadPriority(::GetCurrentThread());
#endif
            {
                const PriorityScope scope(work);
                raised = scope.raised();
                multimedia = scope.multimedia();
#if defined(_WIN32)
                during = ::GetThreadPriority(::GetCurrentThread());
#endif
            }
#if defined(_WIN32)
            after = ::GetThreadPriority(::GetCurrentThread());
#endif
        }).join();
        INFO((work == ThreadWork::Output ? "output" : "compute")
             << ": priority " << before << " then " << during << " then " << after
             << (multimedia ? ", by MMCSS" : ", not by MMCSS"));
#if defined(_WIN32)
        CHECK(raised);
        CHECK(during > before);
        CHECK(after == before);
#else
        CHECK_FALSE(raised);
#endif
    }
}

TEST_CASE("a thread that asks reads denormals as zero, and only while it asks", "[rt]") {
    // The smallest float that is not a denormal, halved, is one.
    float before = 0.0f;
    float flushed = 1.0f;
    float readAsZero = 1.0f;
    float after = 0.0f;
    bool activeBefore = true;
    bool activeDuring = false;
    bool activeAfter = true;
    std::thread([&] {
        activeBefore = DenormalsAsZero::active();
        before = halved(FLT_MIN);
        const float denormal = before;
        {
            const DenormalsAsZero scope;
            activeDuring = DenormalsAsZero::active();
            flushed = halved(FLT_MIN);      // a result that would be denormal comes out zero
            readAsZero = plusZero(denormal); // and a denormal going in is read as zero
        }
        activeAfter = DenormalsAsZero::active();
        after = halved(FLT_MIN);
    }).join();
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    CHECK_FALSE(activeBefore);
    CHECK(before > 0.0f);
    CHECK(activeDuring);
    CHECK(flushed == 0.0f);
    CHECK(readAsZero == 0.0f);
    CHECK_FALSE(activeAfter);
    CHECK(after > 0.0f);
#else
    CHECK_FALSE(activeDuring);
    (void)activeBefore;
    (void)activeAfter;
    (void)flushed;
    (void)readAsZero;
    (void)after;
#endif
}
