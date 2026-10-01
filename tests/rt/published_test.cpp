#include "core/rt/published.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using takt4::rt::Published;

namespace {

/// Wide enough that a reader copying it while it is overwritten would see a mixture, and
/// self-describing enough to prove it did not: every field is derived from `seed`.
struct Wide {
    std::uint64_t seed = 0;
    std::uint64_t doubled = 0;
    double negated = 0.0;
    std::uint32_t low = 0;
    char text[32]{};

    static Wide of(std::uint64_t seed) {
        Wide w;
        w.seed = seed;
        w.doubled = seed * 2;
        w.negated = -static_cast<double>(seed);
        w.low = static_cast<std::uint32_t>(seed & 0xFFFFU);
        for (std::size_t i = 0; i < sizeof w.text; ++i) {
            w.text[i] = static_cast<char>('a' + (seed + i) % 26);
        }
        return w;
    }

    bool consistent() const {
        if (doubled != seed * 2 || negated != -static_cast<double>(seed) ||
            low != static_cast<std::uint32_t>(seed & 0xFFFFU)) {
            return false;
        }
        for (std::size_t i = 0; i < sizeof text; ++i) {
            if (text[i] != static_cast<char>('a' + (seed + i) % 26)) {
                return false;
            }
        }
        return true;
    }
};

} // namespace

TEST_CASE("a published value reads back, and counts its revisions", "[rt]") {
    Published<Wide> published;
    CHECK(published.revision() == 0);
    CHECK(published.load().seed == 0);

    published.publish(Wide::of(7));
    CHECK(published.revision() == 1);
    CHECK(published.load().seed == 7);
    CHECK(published.load().consistent());

    published.publish(Wide::of(9));
    CHECK(published.revision() == 2);
    CHECK(published.load().seed == 9);

    SECTION("an initial value is there before anything is published") {
        const Published<Wide> initial(Wide::of(42));
        CHECK(initial.revision() == 0);
        CHECK(initial.load().seed == 42);
    }
}

TEST_CASE("a reader never sees half of two values", "[rt]") {
    // The failure this guards against: a consumer drawing a tempo from one frame beside
    // a bar position from another. Without the sequence counter, a wide struct written
    // field by field while being copied gives exactly that, and it is the kind of bug
    // that shows up once an hour on someone else's machine.
    Published<Wide> published(Wide::of(0));
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> reads{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                if (!published.load().consistent()) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Publish until the readers have really been scheduled, rather than for a fixed count.
    // Three spinning threads on a contended two-core runner can get almost no time while
    // this one races through 200000 publishes in a couple of milliseconds, and then the
    // run proves nothing: it is the reads that catch a tear, not the writes. Measured on
    // CI 2026-09-05, where "the readers really ran" failed while `torn == 0` held — the
    // test reporting its own starvation as a fault in the code under test.
    //
    // The ceiling keeps a pathological scheduler to a failure rather than a hang.
    constexpr std::uint64_t kWrites = 200000;
    constexpr std::uint64_t kReadsWanted = 1000;
    constexpr std::uint64_t kWritesCeiling = 20000000;
    std::uint64_t writes = 0;
    while (writes < kWritesCeiling &&
           (writes < kWrites || reads.load(std::memory_order_relaxed) < kReadsWanted)) {
        ++writes;
        published.publish(Wide::of(writes));
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) {
        reader.join();
    }

    INFO(reads.load() << " reads across three threads against " << writes << " writes");
    CHECK(torn.load() == 0);             // the whole point: no reader ever saw half of two values
    CHECK(reads.load() >= kReadsWanted); // ...and enough of them ran for that to mean something
    CHECK(writes >= kWrites);
    CHECK(published.load().seed == writes);
    CHECK(published.revision() == writes);
}

#if defined(_WIN32)

TEST_CASE("a reader that interrupts a write lets the writer finish it", "[rt]") {
    // The audit of 2026-09-25, L25. The output thread (MMCSS "Pro Audio", High) reads what the
    // tracker (Audio, Medium) publishes, and on one core the reader can preempt the writer half
    // way through a write — and then spin on it, with the writer that would finish it unable to
    // run, until Windows' starvation boost comes round seconds later. Made to happen here: both
    // threads on one core, the reader above the writer and waking on a timer, so it lands
    // wherever the writer happens to be.
    DWORD_PTR processCores = 0;
    DWORD_PTR systemCores = 0;
    REQUIRE(GetProcessAffinityMask(GetCurrentProcess(), &processCores, &systemCores) != 0);
    REQUIRE(processCores != 0);
    DWORD_PTR core = 1;
    while ((processCores & core) == 0) {
        core <<= 1;
    }

    Published<Wide> published(Wide::of(0));
    const Wide values[2] = {Wide::of(1), Wide::of(2)};
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        SetThreadAffinityMask(GetCurrentThread(), core);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        // Nothing but writes, so a reader waking up finds one in flight as often as it can.
        for (std::size_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
            published.publish(values[i & 1U]);
        }
    });
    double worst = 0.0;
    std::size_t loads = 0;
    std::thread reader([&] {
        SetThreadAffinityMask(GetCurrentThread(), core);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            const auto from = std::chrono::steady_clock::now();
            const Wide got = published.load();
            const double took =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
            worst = std::max(worst, took);
            ++loads;
            if (!got.consistent()) {
                worst = 1e9; // torn: worse than any wait
            }
        }
    });
    reader.join();
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    INFO(loads << " loads; the slowest took " << worst * 1000.0 << " ms");
    CHECK(loads >= 50);
    // Half a second, not a tenth: hosted runners took 107 and 147 ms on 2026-10-01, and the
    // case this catches waits seconds for the starvation boost, so it is still told apart.
    CHECK(worst < 0.5);
}

#endif
