#include "core/sandbox.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>

namespace takt4::sandbox {
namespace {

/// -1 until the environment has been read.
std::atomic<int> state{-1};

/// Ports a test's receiver was given. Only ever added to, so a reader needs no lock: a slot is
/// written before the count that makes it visible. A test process makes a few dozen receivers;
/// past the end, a port is simply not let through, which fails a test rather than the rig.
constexpr std::size_t kMaxPorts = 1024;
std::array<std::atomic<std::uint16_t>, kMaxPorts> ports{};
std::atomic<std::size_t> portCount{0};

std::atomic<std::uint64_t> refusedCount{0};
std::array<std::atomic<std::uint64_t>, 6> refusedByKind{};
std::atomic<Refused> refusedLast{Refused::Nothing};
std::atomic<std::uint16_t> refusedPort{0};

bool fromEnvironment() noexcept {
#if defined(_MSC_VER)
    // The CRT's copy, which is the one `_putenv_s` in the test executables writes.
    std::size_t length = 0;
    return getenv_s(&length, nullptr, 0, "TAKT4_TEST_SANDBOX") == 0 && length > 1;
#else
    const char* const value = std::getenv("TAKT4_TEST_SANDBOX");
    return value != nullptr && *value != '\0';
#endif
}

} // namespace

bool active() noexcept {
    int on = state.load(std::memory_order_relaxed);
    if (on < 0) {
        const int read = fromEnvironment() ? 1 : 0;
        // Whoever reads first sets it; a `setActive` in between wins.
        if (!state.compare_exchange_strong(on, read, std::memory_order_relaxed)) {
            return on > 0;
        }
        return read > 0;
    }
    return on > 0;
}

void setActive(bool on) noexcept {
    state.store(on ? 1 : 0, std::memory_order_relaxed);
}

void allowPort(std::uint16_t port) noexcept {
    if (port == 0) {
        return;
    }
    const std::size_t count = portCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        if (ports[i].load(std::memory_order_relaxed) == port) {
            return;
        }
    }
    // Two receivers made at once on two threads can take two slots for one port, which costs a
    // slot and nothing else.
    const std::size_t slot = portCount.load(std::memory_order_relaxed);
    if (slot >= kMaxPorts) {
        return;
    }
    ports[slot].store(port, std::memory_order_relaxed);
    portCount.store(slot + 1, std::memory_order_release);
}

namespace {

bool portAllowed(std::uint16_t port) noexcept {
    const std::size_t count = portCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        if (ports[i].load(std::memory_order_relaxed) == port) {
            return true;
        }
    }
    return false;
}

} // namespace

bool allowsSend(std::uint32_t ipv4, std::uint16_t port) noexcept {
    if (!active()) {
        return true;
    }
    const std::uint32_t first = ipv4 >> 24;
    if (first == 0) {
        return true; // 0.0.0.0/8: refused by every system, which is what a test wants of it
    }
    return first == 127 && portAllowed(port);
}

bool allowsBind(std::uint16_t port) noexcept {
    return !active() || port == 0 || portAllowed(port);
}

void refuse(Refused what, std::uint16_t port) noexcept {
    refusedLast.store(what, std::memory_order_relaxed);
    refusedPort.store(port, std::memory_order_relaxed);
    refusedByKind[static_cast<std::size_t>(what) % refusedByKind.size()].fetch_add(
        1, std::memory_order_relaxed);
    refusedCount.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t refusals() noexcept {
    return refusedCount.load(std::memory_order_relaxed);
}

std::uint64_t refusals(Refused what) noexcept {
    return refusedByKind[static_cast<std::size_t>(what) % refusedByKind.size()].load(
        std::memory_order_relaxed);
}

Refused lastRefused() noexcept {
    return refusedLast.load(std::memory_order_relaxed);
}

std::uint16_t lastRefusedPort() noexcept {
    return refusedPort.load(std::memory_order_relaxed);
}

} // namespace takt4::sandbox
