#include "core/rt/published.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

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

    for (std::uint64_t seed = 1; seed <= 200000; ++seed) {
        published.publish(Wide::of(seed));
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) {
        reader.join();
    }

    INFO(reads.load() << " reads across three threads against 200000 writes");
    CHECK(reads.load() > 1000); // the readers really ran
    CHECK(torn.load() == 0);
    CHECK(published.load().seed == 200000);
    CHECK(published.revision() == 200000);
}
