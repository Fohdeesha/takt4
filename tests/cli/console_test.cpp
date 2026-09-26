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

TEST_CASE("a second Ctrl+C is caught as the first was", "[cli]") {
    // The audit of 2026-09-25, L47. Windows' runtime puts a signal back to its default before
    // it calls the handler, so the second Ctrl+C of an impatient operator ended the process —
    // before annotate wrote its taps, or `track --device` sent the MIDI clock its Stop. What is
    // in force after the first is looked at before a second is raised: with the default there,
    // the second would end this test binary.
    using takt4::cli::Interrupts;
    const auto before = std::signal(SIGINT, previousHandler);
    {
        const Interrupts interrupts;
        REQUIRE(std::raise(SIGINT) == 0);
        REQUIRE(Interrupts::requested());
        const auto inForce = std::signal(SIGINT, SIG_IGN);
        (void)std::signal(SIGINT, inForce);
        CHECK(inForce != SIG_DFL);
        CHECK(inForce != &previousHandler);
        if (inForce != SIG_DFL) {
            REQUIRE(std::raise(SIGINT) == 0);
            CHECK(Interrupts::requested());
        }
    }
    (void)std::signal(SIGINT, before);
}
