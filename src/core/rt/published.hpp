#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

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
///
/// **The value is kept as atomic words, not as a `T`.** A reader's copy races with the
/// writer's by design — that is what the retry is for — and a plain copy racing with a plain
/// write is a data race under the C++ memory model, whatever the hardware does with it:
/// harmless on x64, reported by ThreadSanitizer every time (the audit's Low items). The words
/// are Boehm's answer ("Can seqlocks get along with programming language memory models?",
/// 2012): every access is atomic, so there is no race to report.
///
/// **Ordered by the words themselves, with no fences** — Boehm's other form: the writer
/// stores each word with release, so its odd counter cannot be seen after any of them; the
/// reader loads each with acquire, so its second look at the counter cannot happen before
/// any of them. On x64 that is the same instructions as relaxed. The fence form would do the
/// same, and GCC refuses to build `atomic_thread_fence` under ThreadSanitizer, which cannot
/// model a fence on its own (the first linux-tsan run, 2026-09-23).
template <class T>
class Published {
    static_assert(std::is_trivially_copyable_v<T>, "a published value is copied bitwise");

    static constexpr std::size_t kWords =
        (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
    using Words = std::array<std::uint64_t, kWords>;

    /// A spin that tells the core it is spinning, so a reader waiting out a write does not
    /// starve the writer sharing its core of the pipeline.
    static void relax() noexcept {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
    }

    static Words toWords(const T& value) noexcept {
        Words words{};
        std::memcpy(words.data(), &value, sizeof(T));
        return words;
    }

public:
    Published() : Published(T{}) {}
    explicit Published(const T& initial) {
        const Words words = toWords(initial);
        for (std::size_t i = 0; i < kWords; ++i) {
            words_[i].store(words[i], std::memory_order_relaxed);
        }
    }

    /// **One writer only.** Never blocks and never fails.
    void publish(const T& value) noexcept {
        const Words words = toWords(value);
        const std::uint64_t before = sequence_.load(std::memory_order_relaxed);
        sequence_.store(before + 1, std::memory_order_relaxed); // now odd: writing
        for (std::size_t i = 0; i < kWords; ++i) {
            words_[i].store(words[i], std::memory_order_release);
        }
        sequence_.store(before + 2, std::memory_order_release); // even again: settled
    }

    /// Any thread. Spins only while a write is actually in flight — and after
    /// `kSpinsBeforeYield` spins gives its core away. A reader that preempted the writer part
    /// way through a write (the output thread, MMCSS "Pro Audio" at High, over the tracker at
    /// Medium on one core) spun on it until Windows' starvation boost let the writer finish,
    /// seconds later (the audit of 2026-09-25, L25).
    T load() const noexcept {
        int spins = 0;
        const auto wait = [&spins] {
            if (++spins < kSpinsBeforeYield) {
                relax();
                return;
            }
            spins = 0;
            std::this_thread::yield();
        };
        for (;;) {
            const std::uint64_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1U) != 0U) {
                wait(); // a write is in progress; the value is not whole
                continue;
            }
            Words words;
            for (std::size_t i = 0; i < kWords; ++i) {
                words[i] = words_[i].load(std::memory_order_acquire);
            }
            if (sequence_.load(std::memory_order_relaxed) == before) {
                std::array<std::byte, sizeof(T)> bytes;
                std::memcpy(bytes.data(), words.data(), sizeof(T));
                return std::bit_cast<T>(bytes);
            }
            wait();
        }
    }

    /// How many spins a reader makes on a write in flight before it yields its core. A write
    /// is a few dozen stores, so a writer that is running finishes well within these.
    static constexpr int kSpinsBeforeYield = 64;

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
    alignas(64) std::array<std::atomic<std::uint64_t>, kWords> words_{};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
};

} // namespace takt4::rt
