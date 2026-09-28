#pragma once

#include <atomic>
#include <chrono>
#include <thread>

namespace takt4::audio {

/// The door between a driver's callback and everything the callback writes into — the resampler,
/// the engine, the meter — which a caller can shut and then **know nothing is inside**, without
/// asking the driver anything.
///
/// Why it exists (the audit of 2026-09-25, L23): stopping a stream is the one way PortAudio
/// promises the callback has returned, and a wedged driver never returns from the stop. So the
/// window could not stop on another thread and go on — the callback might still be writing into
/// an engine the window was about to restart or destroy. Shut this first and it can: a callback
/// that begins afterwards turns straight round, and one already inside is waited for, which is a
/// callback's length — it is real-time code that never blocks.
///
/// **Sequentially consistent on both sides**, deliberately: the callback raises `inside_` and
/// then reads `open_`, the closer lowers `open_` and then reads `inside_`, and only with both
/// orders total does one of them always see the other. Weaker orders let both slip past.
class CallbackGate {
public:
    /// The callback's side, for the length of one callback: `admitted()` says whether it may go
    /// on. Never blocks and never allocates.
    class Pass {
    public:
        explicit Pass(CallbackGate& gate) noexcept : gate_(gate) {
            gate_.inside_.fetch_add(1, std::memory_order_seq_cst);
            admitted_ = gate_.open_.load(std::memory_order_seq_cst);
        }
        ~Pass() { gate_.inside_.fetch_sub(1, std::memory_order_release); }
        Pass(const Pass&) = delete;
        Pass& operator=(const Pass&) = delete;
        bool admitted() const noexcept { return admitted_; }

    private:
        CallbackGate& gate_;
        bool admitted_ = false;
    };

    /// Shuts it, and waits until no callback is inside. True when none is; false only if one was
    /// still inside after `limit`, which a callback that never blocks cannot be.
    bool close(std::chrono::milliseconds limit = std::chrono::milliseconds(1000)) noexcept {
        open_.store(false, std::memory_order_seq_cst);
        const auto until = std::chrono::steady_clock::now() + limit;
        while (inside_.load(std::memory_order_seq_cst) != 0) {
            if (std::chrono::steady_clock::now() >= until) {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

    bool isOpen() const noexcept { return open_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> open_{true};
    std::atomic<int> inside_{0};
};

} // namespace takt4::audio
