#include "core/audio/audio_thread.hpp"
#include "core/audio/callback_clock.hpp"
#include "core/audio/callback_gate.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#endif

using namespace std::chrono_literals;
using takt4::audio::AudioThread;
using takt4::audio::CallbackClock;
using takt4::audio::CallbackGate;

namespace {

/// Waits up to `limit` for `done`, looking every millisecond.
template <typename Done>
bool within(std::chrono::milliseconds limit, Done done) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/// Has `thread` take one job and come back, so it is up and waiting for the next.
///
/// A test that times a job against a limit of a few hundred milliseconds is timing the job, not
/// the thread's start: the first job waits for the thread to be created and join its apartment,
/// which is the process's first COM call. Measured on 2026-09-28: about 11 ms in Release and
/// 390 to 430 ms under AddressSanitizer, so a job given 200 ms there had not started when its
/// caller gave up, and the thread was not yet stuck on it.
void settle(AudioThread& thread) {
    REQUIRE(thread.run("settle", [] {}, 10s));
}

} // namespace

TEST_CASE("every job runs on the one audio thread, never on the caller's", "[audio][thread]") {
    // An ASIO driver is created in the apartment of the thread that opens it and has to be
    // started, stopped and closed from there — so every call is one thread's, and not the
    // window's.
    AudioThread thread;
    std::thread::id first;
    std::thread::id second;
    REQUIRE(thread.run("first", [&] { first = std::this_thread::get_id(); }, 5s));
    REQUIRE(thread.handOver("second", [&] { second = std::this_thread::get_id(); }, 5s));
    CHECK(first == second);
    CHECK(first == thread.id());
    CHECK(first != std::this_thread::get_id());
    CHECK_FALSE(thread.stuck());
}

TEST_CASE("what a job owns is let go of on the audio thread, not on its caller's",
          "[audio][thread]") {
    // A stream handed over to be stopped is closed by the job letting go of it, and closing it
    // anywhere but where it was opened is the call this thread exists to keep off the window's.
    struct Owned {
        explicit Owned(std::atomic<std::thread::id>* at) : where(at) {}
        Owned(const Owned&) = delete;
        Owned& operator=(const Owned&) = delete;
        ~Owned() { *where = std::this_thread::get_id(); }
        std::atomic<std::thread::id>* where;
    };
    AudioThread thread;
    std::atomic<std::thread::id> where{};
    {
        auto owned = std::make_shared<Owned>(&where);
        REQUIRE(thread.handOver("close", [owned = std::move(owned)]() mutable { (void)owned; }, 5s));
    }
    REQUIRE(within(2000ms, [&] { return where.load() != std::thread::id{}; }));
    CHECK(where.load() == thread.id());
}

TEST_CASE("what a job on the audio thread throws reaches the caller that waited for it",
          "[audio][thread]") {
    AudioThread thread;
    CHECK_THROWS_WITH(thread.run("open", [] { throw std::runtime_error("the driver said no"); }, 5s),
                      "the driver said no");
    // And the thread goes on taking jobs.
    bool ran = false;
    CHECK(thread.run("again", [&] { ran = true; }, 5s));
    CHECK(ran);
}

TEST_CASE("a job that does not come back costs its caller its limit and no more",
          "[audio][thread]") {
    // The audit of 2026-09-25, L23: PortAudio's ASIO and WASAPI hosts stop a stream in code that
    // waits with no limit, and a driver wedged in there held the window's thread — STOP, a device
    // change, and the outage's own reopen froze the whole window. Here the driver is a job that
    // waits on a promise nobody keeps until the test says so.
    AudioThread thread;
    settle(thread);
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> stopFinished{false};

    const auto asked = std::chrono::steady_clock::now();
    CHECK_FALSE(thread.handOver(
        "stop",
        [released, &stopFinished] {
            released.wait();
            stopFinished = true;
        },
        200ms));
    const auto waited = std::chrono::steady_clock::now() - asked;
    CHECK(waited >= 190ms);
    CHECK(waited < 1s);
    CHECK(thread.stuck());
    CHECK(thread.stuckOn() == "stop");

    // A caller wanting an answer is refused at once, and its job is never run — it would never
    // start behind a driver that does not return, and whoever asked has moved on.
    std::atomic<bool> opened{false};
    const auto refusedAt = std::chrono::steady_clock::now();
    CHECK_FALSE(thread.run("open", [&] { opened = true; }, 5s));
    CHECK(std::chrono::steady_clock::now() - refusedAt < 100ms);

    // A job that has to happen on the thread whenever it can — closing what it opened — waits its
    // turn behind it rather than being refused.
    std::atomic<bool> closed{false};
    CHECK_FALSE(thread.handOver("close", [&] { closed = true; }, 50ms));
    CHECK_FALSE(closed);

    // The driver comes back: the stuck job finishes, the queued one after it, and the thread
    // takes jobs again.
    release.set_value();
    REQUIRE(within(2000ms, [&] { return !thread.stuck() && closed; }));
    CHECK(stopFinished);
    CHECK_FALSE(opened); // refused, and never run late
    CHECK(thread.run("open", [&] { opened = true; }, 5s));
    CHECK(opened);
}

TEST_CASE("a job its caller gave up on before it started is not run late", "[audio][thread]") {
    // Waiting behind a slow job that is not stuck — one that has a caller still waiting on it —
    // a job can time out before it begins. Its answer is to nobody by then, so it is not run.
    AudioThread thread;
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> slowStarted{false};
    std::thread other([&] {
        (void)thread.handOver(
            "slow",
            [released, &slowStarted] {
                slowStarted = true;
                released.wait();
            },
            10s);
    });
    REQUIRE(within(2000ms, [&] { return slowStarted.load(); }));
    std::atomic<bool> ran{false};
    CHECK_FALSE(thread.run("late", [&] { ran = true; }, 50ms));
    release.set_value();
    other.join();
    // Something run after the slow one has been, which the late one would have been before it.
    bool after = false;
    CHECK(thread.run("after", [&] { after = true; }, 5s));
    CHECK(after);
    CHECK_FALSE(ran);
}

TEST_CASE("the audio thread's destructor does not wait on a driver holding it", "[audio][thread]") {
    // A window closed while a driver holds the audio thread must still close. The thread is left
    // to the driver with everything it uses, and finishes whenever the driver lets it.
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::chrono::steady_clock::time_point start;
    {
        AudioThread thread;
        settle(thread);
        start = std::chrono::steady_clock::now();
        CHECK_FALSE(thread.handOver(
            "stop",
            [released, finished] {
                released.wait();
                *finished = true;
            },
            50ms));
        REQUIRE(thread.stuck());
    }
    CHECK(std::chrono::steady_clock::now() - start < 1s);
    release.set_value();
    CHECK(within(2000ms, [&] { return finished->load(); }));
}

#if defined(_WIN32)

namespace {

std::atomic<int> timerFired{0};

void CALLBACK onTimer(HWND, UINT, UINT_PTR id, DWORD) {
    ::KillTimer(nullptr, id);
    ++timerFired;
}

} // namespace

TEST_CASE("the audio thread is a single-threaded apartment that dispatches messages while idle",
          "[audio][thread]") {
    // What an ASIO driver had from the window's thread without anybody deciding it should: COM's
    // single-threaded apartment, and a message loop for any window or timer it keeps there.
    AudioThread thread;
    APTTYPE type = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    HRESULT asked = E_FAIL;
    REQUIRE(thread.run("apartment", [&] { asked = ::CoGetApartmentType(&type, &qualifier); }, 5s));
    REQUIRE(SUCCEEDED(asked));
    CHECK((type == APTTYPE_STA || type == APTTYPE_MAINSTA));

    // A timer set by one job fires while the thread waits for the next: only a thread that
    // dispatches its messages ever calls a TIMERPROC.
    timerFired = 0;
    UINT_PTR timer = 0;
    REQUIRE(thread.run("timer", [&] { timer = ::SetTimer(nullptr, 0, 10, &onTimer); }, 5s));
    REQUIRE(timer != 0);
    CHECK(within(2000ms, [] { return timerFired.load() > 0; }));
}

#endif

TEST_CASE("a shut callback gate turns callbacks away and waits for the one inside",
          "[audio][thread]") {
    // Shut before a stream is handed to the audio thread to be stopped, so whatever the driver
    // does next, nothing reaches the engine the window is about to restart.
    CallbackGate gate;
    std::atomic<bool> inside{false};
    std::atomic<bool> leave{false};
    std::atomic<bool> writtenAfterClose{false};
    std::atomic<bool> closed{false};
    std::atomic<bool> admitted{false};
    std::atomic<bool> emptied{false};
    // Catch's assertions are the main thread's; the two threads only record what they saw.
    std::thread callback([&] {
        const CallbackGate::Pass pass(gate);
        admitted = pass.admitted();
        inside = true;
        while (!leave) {
            std::this_thread::yield();
        }
        // Still inside when the gate is shut: this write must land before close() returns.
        writtenAfterClose = closed.load();
    });
    REQUIRE(within(2000ms, [&] { return inside.load(); }));
    CHECK(admitted);
    std::thread closer([&] {
        emptied = gate.close(5000ms);
        closed = true;
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(closed); // still waiting on the callback inside
    leave = true;
    callback.join();
    closer.join();
    CHECK(closed);
    CHECK(emptied);
    CHECK_FALSE(writtenAfterClose);
    // And from now on every callback turns straight round.
    const CallbackGate::Pass late(gate);
    CHECK_FALSE(late.admitted());
    CHECK_FALSE(gate.isOpen());
}

TEST_CASE("the callback clock counts the time and frames of normal callbacks only",
          "[audio][watchdog]") {
    // 256 frames at 48 kHz: a callback every 5.33 ms. Gaps up to two buffers and 5 ms are normal;
    // a dropout's longer gap is left out with the audio lost in it.
    const double rate = 48000.0;
    CallbackClock clock(rate);
    const std::int64_t period = static_cast<std::int64_t>(256.0 / rate * 1e9);
    std::int64_t now = 1'000'000'000;
    clock.onCallback(256, now); // the first: nothing before it to measure from
    CHECK(clock.read().normalFrames == 0);
    for (int i = 0; i < 100; ++i) {
        now += period;
        clock.onCallback(256, now);
    }
    CallbackClock::Reading reading = clock.read();
    CHECK(reading.callbacks == 101);
    CHECK(reading.normalFrames == 100U * 256U);
    CHECK(static_cast<double>(reading.normalFrames) / (static_cast<double>(reading.normalNanos) / 1e9) ==
          Catch::Approx(rate).epsilon(1e-6));

    // A 200 ms dropout: the callback after it brings a buffer, not 200 ms of audio, and is left
    // out — so the rate over the totals is still the device's.
    now += 200'000'000;
    clock.onCallback(256, now);
    for (int i = 0; i < 100; ++i) {
        now += period;
        clock.onCallback(256, now);
    }
    reading = clock.read();
    CHECK(reading.normalFrames == 200U * 256U);
    CHECK(static_cast<double>(reading.normalFrames) / (static_cast<double>(reading.normalNanos) / 1e9) ==
          Catch::Approx(rate).epsilon(1e-6));
    CHECK(reading.lastNanos == now);
}
