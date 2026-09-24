#include "cli/console.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <csignal>

namespace {

std::atomic<int> g_previousCalls{0};
void previousHandler(int) {
    g_previousCalls.fetch_add(1);
}

} // namespace

TEST_CASE("a Ctrl+C while the console is tapping along is a flag, not the end", "[cli]") {
    // The audit's Low items: `annotate` had no handler, so a Ctrl+C in the middle of tapping
    // killed it and every tap with it. Raised here as the console raises it, with a handler of
    // the caller's own already in place: the process goes on, the flag is up, and the caller's
    // handler is back once the session is over.
    using takt4::cli::Interrupts;
    const auto before = std::signal(SIGINT, previousHandler);
    {
        const Interrupts interrupts;
        CHECK_FALSE(Interrupts::requested());
        REQUIRE(std::raise(SIGINT) == 0);
        CHECK(Interrupts::requested());
        CHECK(g_previousCalls.load() == 0); // the session's, not the caller's
    }
    REQUIRE(std::raise(SIGINT) == 0);
    CHECK(g_previousCalls.load() == 1); // put back
    {
        // A fresh session starts with the flag down.
        const Interrupts again;
        CHECK_FALSE(Interrupts::requested());
    }
    (void)std::signal(SIGINT, before);
}
