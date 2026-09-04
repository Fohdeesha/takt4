#include "core/audio/hop_fanout.hpp"

#include "core/audio/rates.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

using takt4::audio::HopFanout;
using takt4::audio::HopProcessor;
using takt4::audio::kHopSize;

namespace {

/// Records what it was handed and when, relative to its siblings. Allocates, so it is
/// for the behaviour tests and never for the [rt] one.
class Recorder final : public HopProcessor {
public:
    Recorder(std::vector<int>& order, int id) : order_(&order), id_(id) {}

    void processHop(const float* hop, std::uint64_t hopIndex) noexcept override {
        order_->push_back(id_);
        hops.push_back(hopIndex);
        firstSample = hop[0];
    }

    std::vector<std::uint64_t> hops;
    float firstSample = 0.0f;

private:
    std::vector<int>* order_;
    int id_;
};

/// The same, without a heap: what the [rt] test drives.
class Counter final : public HopProcessor {
public:
    void processHop(const float*, std::uint64_t) noexcept override { ++calls; }
    std::uint64_t calls = 0;
};

} // namespace

TEST_CASE("HopFanout gives every processor the same hop, in the order it was built with",
          "[audio]") {
    std::vector<int> order;
    Recorder first(order, 1);
    Recorder second(order, 2);
    HopFanout fanout{&first, &second};
    REQUIRE(fanout.size() == 2);

    std::vector<float> hop(kHopSize, 0.0f);
    hop[0] = 0.75f;
    fanout.processHop(hop.data(), 11);
    hop[0] = -0.25f;
    fanout.processHop(hop.data(), 12);

    // Both saw both hops, with the same indices and the same samples.
    CHECK(first.hops == std::vector<std::uint64_t>{11, 12});
    CHECK(second.hops == std::vector<std::uint64_t>{11, 12});
    CHECK(first.firstSample == -0.25f);
    CHECK(second.firstSample == -0.25f);
    // And in the order given, every time — the tracker is put first on purpose.
    CHECK(order == std::vector<int>{1, 2, 1, 2});
}

TEST_CASE("HopFanout is happy with one processor and with none", "[audio]") {
    std::vector<float> hop(kHopSize, 0.0f);

    std::vector<int> order;
    Recorder only(order, 1);
    HopFanout one{&only};
    CHECK(one.size() == 1);
    one.processHop(hop.data(), 3);
    CHECK(only.hops == std::vector<std::uint64_t>{3});

    // Forwarding to nobody is a state, not an error: a window with no device chosen yet
    // still has a stream shape to build.
    HopFanout none({});
    CHECK(none.size() == 0);
    none.processHop(hop.data(), 3);
}

TEST_CASE("HopFanout refuses what it cannot forward to", "[audio]") {
    Counter a;
    CHECK_THROWS_AS((HopFanout{&a, nullptr}), std::invalid_argument);

    static_assert(HopFanout::kMaxProcessors == 4, "the list below is one past the cap");
    CHECK_THROWS_AS((HopFanout{&a, &a, &a, &a, &a}), std::invalid_argument);
}

TEST_CASE("HopFanout::processHop() does not touch the heap", "[audio][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    Counter first;
    Counter second;
    HopFanout fanout{&first, &second};
    const std::vector<float> hop(kHopSize, 0.25f);

    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope realtime;
        for (std::uint64_t i = 0; i < 1000; ++i) {
            fanout.processHop(hop.data(), i);
        }
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(takt4::rt::violationCount() == before);
    CHECK(first.calls == 1000);
    CHECK(second.calls == 1000);
}
