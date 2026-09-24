#include "core/rt/spsc_ring.hpp"

#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

struct Item {
    std::uint64_t sequence;
    float value;
};

} // namespace

TEST_CASE("SpscRing holds exactly its capacity and keeps order", "[rt]") {
    takt4::rt::SpscRing<Item, 8> ring;
    CHECK(ring.empty());
    CHECK(ring.capacity() == 8);

    for (std::uint64_t i = 0; i < 8; ++i) {
        CHECK(ring.tryPush({i, static_cast<float>(i)}));
    }
    CHECK(ring.size() == 8);
    CHECK_FALSE(ring.tryPush({99, 99.0f}));

    Item item{};
    for (std::uint64_t i = 0; i < 8; ++i) {
        REQUIRE(ring.tryPop(item));
        CHECK(item.sequence == i);
        CHECK(item.value == static_cast<float>(i));
    }
    CHECK(ring.empty());
    CHECK_FALSE(ring.tryPop(item));
    CHECK(item.sequence == 7); // a failed pop leaves the output alone
}

TEST_CASE("SpscRing keeps working past index wrap-around", "[rt]") {
    takt4::rt::SpscRing<Item, 4> ring;
    Item item{};
    for (std::uint64_t i = 0; i < 1000; ++i) {
        REQUIRE(ring.tryPush({i, 0.0f}));
        if (i % 3 == 0) {
            REQUIRE(ring.tryPush({i + 1000000, 0.0f}));
            REQUIRE(ring.tryPop(item));
            CHECK(item.sequence == i);
            REQUIRE(ring.tryPop(item));
            CHECK(item.sequence == i + 1000000);
        } else {
            REQUIRE(ring.tryPop(item));
            CHECK(item.sequence == i);
        }
    }
    CHECK(ring.empty());
}

TEST_CASE("SpscRing carries every item across threads in order", "[rt]") {
    constexpr std::uint64_t kItems = 200000;
    takt4::rt::SpscRing<Item, 64> ring;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems;) {
            if (ring.tryPush({i, static_cast<float>(i % 7)})) {
                ++i;
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::uint64_t expected = 0;
    std::uint64_t misordered = 0;
    Item item{};
    while (expected < kItems) {
        if (ring.tryPop(item)) {
            if (item.sequence != expected || item.value != static_cast<float>(expected % 7)) {
                ++misordered;
            }
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();

    CHECK(misordered == 0);
    CHECK(ring.empty());
}

TEST_CASE("SpscRing's size read from a third thread is never more than it holds", "[rt]") {
    // The audit's Low items: read head-then-tail from a thread that is neither end, a consumer
    // popping in between made the tail pass the head that was read, and the count wrapped to
    // the quintillions. A producer and a consumer running flat out on a small ring, which is
    // the ring nearly empty and both indices moving — the state that race needs — and a third
    // thread asking the size as fast as it can.
    constexpr std::uint64_t kItems = 2000000;
    takt4::rt::SpscRing<Item, 8> ring;
    std::atomic<bool> done{false};
    std::uint64_t largest = 0;
    std::uint64_t asked = 0;

    std::thread watcher([&] {
        while (!done.load(std::memory_order_relaxed)) {
            largest = std::max<std::uint64_t>(largest, ring.size());
            ++asked;
        }
    });
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems;) {
            if (ring.tryPush({i, 0.0f})) {
                ++i;
            }
        }
    });
    Item item{};
    for (std::uint64_t popped = 0; popped < kItems;) {
        if (ring.tryPop(item)) {
            ++popped;
        }
    }
    producer.join();
    done.store(true, std::memory_order_relaxed);
    watcher.join();

    INFO(asked << " sizes read; the largest " << largest);
    CHECK(asked > 1000);
    CHECK(largest <= decltype(ring)::capacity());
}

TEST_CASE("SpscRing push and pop do not touch the heap", "[rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    takt4::rt::SpscRing<Item, 16> ring;
    takt4::rt::setAbortOnViolation(false);
    const std::uint64_t before = takt4::rt::violationCount();
    bool ok = true;
    {
        const takt4::rt::RealtimeScope realtime;
        Item item{};
        for (std::uint64_t i = 0; i < 100; ++i) {
            ok = ok && ring.tryPush({i, 0.0f}) && ring.tryPop(item) && item.sequence == i;
        }
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(ok);
    CHECK(takt4::rt::violationCount() == before);
}
