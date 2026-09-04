#include "core/engine/control.hpp"

#include "core/tracking/tempo_tracker.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

using Catch::Approx;
using takt4::engine::Command;
using takt4::engine::ControlQueue;
using takt4::tracking::TempoTracker;

namespace {

TempoTracker::Options windowed(double minBpm, double maxBpm) {
    TempoTracker::Options options;
    options.minBpm = minBpm;
    options.maxBpm = maxBpm;
    return options;
}

} // namespace

TEST_CASE("the queue hands the consumer everything in the order it was posted", "[engine]") {
    ControlQueue queue;
    CHECK(queue.pending() == 0);

    CHECK(queue.post(Command::halve()));
    CHECK(queue.post(Command::redouble()));
    CHECK(queue.post(Command::setTempoOptions(windowed(90.0, 180.0))));
    CHECK(queue.pending() == 3);

    std::vector<Command> drained;
    queue.drain(drained);
    REQUIRE(drained.size() == 3);
    CHECK(drained[0].kind == Command::Kind::Halve);
    CHECK(drained[1].kind == Command::Kind::Redouble);
    CHECK(drained[2].kind == Command::Kind::SetTempoOptions);
    CHECK(drained[2].tempo.minBpm == Approx(90.0));

    // Draining empties it, and draining an empty queue is not an error.
    CHECK(queue.pending() == 0);
    queue.drain(drained);
    CHECK(drained.empty());
    CHECK(queue.dropped() == 0);
}

TEST_CASE("newer settings supersede older ones without disturbing the order", "[engine]") {
    ControlQueue queue;
    queue.post(Command::setTempoOptions(windowed(60.0, 120.0)));
    queue.post(Command::halve());
    queue.post(Command::setTempoOptions(windowed(100.0, 200.0)));

    // Two settings in, one out: an operator dragging a slider generates a stream of these
    // and only the last one means anything.
    std::vector<Command> drained;
    queue.drain(drained);
    REQUIRE(drained.size() == 2);
    // ...and it lands where it was posted, not where the one it replaced was, because a
    // ÷2 pressed before it must still apply before it.
    CHECK(drained[0].kind == Command::Kind::Halve);
    CHECK(drained[1].kind == Command::Kind::SetTempoOptions);
    CHECK(drained[1].tempo.minBpm == Approx(100.0));
    CHECK(drained[1].tempo.maxBpm == Approx(200.0));
    CHECK(queue.dropped() == 0);
}

TEST_CASE("a full queue refuses and counts rather than growing", "[engine]") {
    ControlQueue queue;
    REQUIRE(queue.post(Command::setTempoOptions(windowed(60.0, 120.0))));
    for (std::size_t i = 1; i < ControlQueue::kCapacity; ++i) {
        INFO("post " << i);
        REQUIRE(queue.post(Command::halve()));
    }
    CHECK(queue.pending() == ControlQueue::kCapacity);

    CHECK_FALSE(queue.post(Command::halve()));
    CHECK_FALSE(queue.post(Command::redouble()));
    CHECK(queue.dropped() == 2);
    CHECK(queue.pending() == ControlQueue::kCapacity);

    // Settings are the exception: this one frees the slot the stale one held, so the
    // operator's newest window is never the thing that gets thrown away.
    CHECK(queue.post(Command::setTempoOptions(windowed(100.0, 200.0))));
    CHECK(queue.dropped() == 2);
    CHECK(queue.pending() == ControlQueue::kCapacity);

    std::vector<Command> drained;
    queue.drain(drained);
    REQUIRE(drained.size() == ControlQueue::kCapacity);
    CHECK(drained.back().kind == Command::Kind::SetTempoOptions);
    CHECK(drained.back().tempo.minBpm == Approx(100.0));
    CHECK(queue.post(Command::halve()));
}

TEST_CASE("several producers may post at once", "[engine]") {
    // A UI thread, an OSC receiver and a MIDI receiver all post here (§5.7), which is the
    // reason this is not an rt::SpscRing. Nothing may be lost or torn while under
    // capacity, and the consumer may drain in the middle of it all.
    ControlQueue queue;
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kEach = 200;

    std::atomic<bool> go{false};
    std::atomic<std::size_t> refused{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue, &go, &refused] {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (std::size_t i = 0; i < kEach; ++i) {
                if (!queue.post(Command::halve())) {
                    refused.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::size_t seen = 0;
    std::vector<Command> drained;
    go.store(true, std::memory_order_release);
    for (std::thread& producer : producers) {
        producer.join();
        queue.drain(drained);
        for (const Command& command : drained) {
            CHECK(command.kind == Command::Kind::Halve);
            ++seen;
        }
    }
    queue.drain(drained);
    seen += drained.size();

    // Everything posted arrived exactly once, and every refusal was counted.
    CHECK(seen + refused.load() == kProducers * kEach);
    CHECK(queue.dropped() == refused.load());
}
