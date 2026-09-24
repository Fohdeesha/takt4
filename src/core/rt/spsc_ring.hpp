#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace takt4::rt {

/// Lock-free single-producer/single-consumer ring: the one way data leaves the audio
/// thread (HANDOFF §4.2). Fixed capacity, no allocation after construction, no
/// blocking on either side. Exactly one thread may push and exactly one may pop.
template <class T, std::size_t Capacity>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "ring items are copied bitwise");
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "capacity must be a power of two");

public:
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Producer. Returns false, leaving the ring untouched, when it is full.
    bool tryPush(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail == Capacity) {
            return false;
        }
        slots_[head & kMask] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Consumer. Returns false, leaving `out` untouched, when the ring is empty.
    bool tryPop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = slots_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Items currently queued. Exact when called by either the producer or the
    /// consumer for its own purposes; a snapshot from anywhere else.
    ///
    /// **The tail first.** Neither index ever goes back and the tail never passes the head, so
    /// a head read after the tail is at least that tail. Read the other way round — as this
    /// did — a third thread could take the head, lose the core while the consumer popped past
    /// it, and subtract a larger tail from a smaller head: a count in the quintillions (the
    /// audit's Low items). A head read later can also have moved on by more than one ring's
    /// worth of pushes and pops, so it is held to the capacity.
    std::size_t size() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return std::min(head - tail, Capacity);
    }

    bool empty() const noexcept { return size() == 0; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Head and tail are monotonic counters; indices come from masking. Each lives on
    // its own cache line so the two threads do not bounce one between them. The
    // padding MSVC warns about (C4324) is the point.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    alignas(64) std::atomic<std::size_t> head_{0}; // producer writes
    alignas(64) std::atomic<std::size_t> tail_{0}; // consumer writes
    alignas(64) std::array<T, Capacity> slots_{};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
};

} // namespace takt4::rt
