#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace takt4::rt {

/// One value written by one thread and read by any number of others, consistently.
///
/// A sequence lock. The writer bumps an odd counter, writes, and bumps it even again; a
/// reader takes the counter, copies, and retries if the counter moved or was odd while
/// it was copying. So the writer never waits — which is the whole point, since it is on
/// the inference thread and behind the audio — and a reader either gets a value that was
/// whole at some instant or goes round again.
///
/// This exists because a struct of a dozen fields cannot be a lock-free
/// `std::atomic<T>`, and reading the fields as separate atomics would let a UI draw a
/// tempo from one frame beside a bar position from another. SpscRing is the right answer
/// when a consumer wants *every* value; this is the right answer when it wants the
/// latest one and there may be several consumers.
///
/// Requires `T` to be trivially copyable: a reader may copy a value that is being
/// overwritten and then throw it away, which is only safe for bytes.
template <class T>
class Published {
    static_assert(std::is_trivially_copyable_v<T>, "a published value is copied bitwise");

public:
    Published() = default;
    explicit Published(const T& initial) : value_(initial) {}

    /// **One writer only.** Never blocks and never fails.
    void publish(const T& value) noexcept {
        const std::uint64_t before = sequence_.load(std::memory_order_relaxed);
        sequence_.store(before + 1, std::memory_order_release); // now odd: writing
        std::atomic_thread_fence(std::memory_order_release);
        value_ = value;
        sequence_.store(before + 2, std::memory_order_release); // even again: settled
    }

    /// Any thread. Spins only while a write is actually in flight.
    T load() const noexcept {
        for (;;) {
            const std::uint64_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1U) != 0U) {
                continue; // a write is in progress; the value is not whole
            }
            T copy = value_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before) {
                return copy;
            }
        }
    }

    /// How many times anything has been published. Even means settled.
    std::uint64_t revision() const noexcept {
        return sequence_.load(std::memory_order_acquire) / 2;
    }

private:
    // The counter and the value on separate cache lines: a reader polling the counter
    // should not be invalidating the line the writer is filling in.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    alignas(64) std::atomic<std::uint64_t> sequence_{0};
    alignas(64) T value_{};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
};

} // namespace takt4::rt
