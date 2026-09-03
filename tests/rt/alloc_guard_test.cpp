#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>
#include <vector>

// Catch2's own CHECK/REQUIRE allocate while building their report, so nothing below
// asserts inside a RealtimeScope: results are captured in locals and checked after the
// scope ends. Each expected count is exactly the heap operations the scope contains.

namespace {

// Turns aborting off for the duration of a test and restores it after, so a failing
// check does not take the whole test binary down with it.
struct CountOnly {
    CountOnly() { takt4::rt::setAbortOnViolation(false); }
    ~CountOnly() { takt4::rt::setAbortOnViolation(true); }
};

struct alignas(64) OverAligned {
    char payload[64];
};

// A compiler may drop a new/delete pair it can see both ends of ([expr.new]/10), which
// would turn the counts below into zeros. Storing the pointer to a volatile keeps every
// allocation real.
void* volatile g_sink = nullptr;

template <class T>
void keep(T* p) {
    g_sink = p;
}

} // namespace

TEST_CASE("the allocation guard is compiled into the test binary", "[rt]") {
    // If this fails, alloc_guard_new.cpp is not linked into takt4_tests and every other
    // real-time check in this binary is vacuous.
    REQUIRE(takt4::rt::allocationGuardEnabled());
    CHECK(takt4::rt::abortOnViolation());
    CHECK_FALSE(takt4::rt::inRealtimeScope());
}

TEST_CASE("heap use outside a real-time scope is not a violation", "[rt]") {
    const CountOnly countOnly;
    const std::uint64_t before = takt4::rt::violationCount();
    {
        std::vector<int> v(1000);
        auto p = std::make_unique<OverAligned>();
        auto q = std::unique_ptr<int>(new (std::nothrow) int(1));
        CHECK(p != nullptr);
        CHECK(q != nullptr);
    }
    CHECK(takt4::rt::violationCount() == before);
}

TEST_CASE("every operator new and delete inside a real-time scope is counted", "[rt]") {
    const CountOnly countOnly;

    SECTION("plain new/delete") {
        const std::uint64_t before = takt4::rt::violationCount();
        bool inScope = false;
        {
            const takt4::rt::RealtimeScope realtime;
            inScope = takt4::rt::inRealtimeScope();
            int* p = new int(7);
            keep(p);
            delete p;
        }
        CHECK(inScope);
        CHECK_FALSE(takt4::rt::inRealtimeScope());
        CHECK(takt4::rt::violationCount() == before + 2);
    }

    SECTION("array new/delete") {
        const std::uint64_t before = takt4::rt::violationCount();
        {
            const takt4::rt::RealtimeScope realtime;
            int* p = new int[16];
            keep(p);
            delete[] p;
        }
        CHECK(takt4::rt::violationCount() == before + 2);
    }

    SECTION("nothrow new") {
        const std::uint64_t before = takt4::rt::violationCount();
        bool allocated = false;
        {
            const takt4::rt::RealtimeScope realtime;
            int* p = new (std::nothrow) int(1);
            keep(p);
            allocated = p != nullptr;
            delete p;
        }
        CHECK(allocated);
        CHECK(takt4::rt::violationCount() == before + 2);
    }

    SECTION("over-aligned new/delete") {
        const std::uint64_t before = takt4::rt::violationCount();
        std::uintptr_t address = 1;
        {
            const takt4::rt::RealtimeScope realtime;
            auto* p = new OverAligned;
            keep(p);
            address = reinterpret_cast<std::uintptr_t>(p);
            delete p;
        }
        CHECK(address % 64 == 0);
        CHECK(takt4::rt::violationCount() == before + 2);
    }

    SECTION("a vector growing inside the scope") {
        const std::uint64_t before = takt4::rt::violationCount();
        {
            const takt4::rt::RealtimeScope realtime;
            std::vector<int> v;
            v.push_back(1);
            keep(v.data());
        }
        CHECK(takt4::rt::violationCount() >= before + 2);
    }

    SECTION("deleting nullptr is not heap use") {
        const std::uint64_t before = takt4::rt::violationCount();
        {
            const takt4::rt::RealtimeScope realtime;
            int* p = nullptr;
            delete p;
        }
        CHECK(takt4::rt::violationCount() == before);
    }
}

TEST_CASE("real-time scopes nest and are per thread", "[rt]") {
    const CountOnly countOnly;
    const std::uint64_t before = takt4::rt::violationCount();

    // The other thread is created before this one goes real-time (std::thread's
    // constructor allocates) and waits until it has, then allocates freely.
    std::atomic<int> stage{0};
    bool otherSawScope = true;
    std::thread other([&] {
        while (stage.load() < 1) {
            std::this_thread::yield();
        }
        otherSawScope = takt4::rt::inRealtimeScope();
        std::vector<int> v(64);
        stage.store(2);
    });

    bool innerSeen = false;
    bool outerSeenAfterInner = false;
    {
        const takt4::rt::RealtimeScope outer;
        {
            const takt4::rt::RealtimeScope inner;
            innerSeen = takt4::rt::inRealtimeScope();
        }
        outerSeenAfterInner = takt4::rt::inRealtimeScope();
        stage.store(1);
        while (stage.load() < 2) {
            std::this_thread::yield();
        }
    }
    other.join();

    CHECK(innerSeen);
    CHECK(outerSeenAfterInner);
    CHECK_FALSE(takt4::rt::inRealtimeScope());
    CHECK_FALSE(otherSawScope);
    CHECK(takt4::rt::violationCount() == before);
}
