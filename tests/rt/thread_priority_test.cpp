#include "core/rt/thread_priority.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
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

#if defined(__APPLE__)
/// The calling thread's quality of service.
int qosNow() {
    qos_class_t now = QOS_CLASS_UNSPECIFIED;
    int relative = 0;
    (void)pthread_get_qos_class_np(pthread_self(), &now, &relative);
    return static_cast<int>(now);
}

/// Whether the calling thread runs under Mach's time-constraint policy.
bool timeConstrainedNow() {
    thread_time_constraint_policy_data_t policy{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t isDefault = FALSE;
    return thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy), &count,
                             &isDefault) == KERN_SUCCESS &&
           !isDefault;
}
#endif

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
#elif defined(__APPLE__)
            // The output thread: whether it is time-constrained. The others: their QoS class.
            before = work == ThreadWork::Output ? int{timeConstrainedNow()} : qosNow();
#endif
            {
                const PriorityScope scope(work);
                raised = scope.raised();
                multimedia = scope.multimedia();
#if defined(_WIN32)
                // **The highest reading over a moment, not one reading.** MMCSS takes a
                // registered thread down for its share of each period when the machine is busy
                // — that is how it keeps time back for everything else — and a single reading
                // can land there: this read -7 once in a ctest run of four at a time under
                // AddressSanitizer (2026-09-26), and 15 on thirty runs alone.
                during = ::GetThreadPriority(::GetCurrentThread());
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
                while (during <= before && std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    during = (std::max)(during, ::GetThreadPriority(::GetCurrentThread()));
                }
#elif defined(__APPLE__)
                during = work == ThreadWork::Output ? int{timeConstrainedNow()} : qosNow();
#endif
            }
#if defined(_WIN32)
            after = ::GetThreadPriority(::GetCurrentThread());
#elif defined(__APPLE__)
            after = work == ThreadWork::Output ? int{timeConstrainedNow()} : qosNow();
#endif
        }).join();
        INFO((work == ThreadWork::Output ? "output" : "compute")
             << ": priority " << before << " then " << during << " then " << after
             << (multimedia ? ", by MMCSS" : ", not by MMCSS"));
#if defined(_WIN32)
        CHECK(raised);
        CHECK(during > before);
        CHECK(after == before);
#elif defined(__APPLE__)
        // macOS asks no privilege for either (2026-10-08).
        CHECK(raised);
        if (work == ThreadWork::Output) {
            CHECK(before == 0);
            CHECK(during == 1);
            CHECK(after == 0);
        } else {
            CHECK(during == static_cast<int>(QOS_CLASS_USER_INTERACTIVE));
            CHECK(after != static_cast<int>(QOS_CLASS_USER_INTERACTIVE));
        }
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
