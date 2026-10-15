#include "core/midi/midi_input_hub.hpp"
#include "core/output/midi_ports.hpp"
#include "core/timing/grid_source.hpp"
#include "core/timing/midi_clock_follower.hpp"

#include "support/virtual_midi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using takt4::midi::MidiInputHub;
using takt4::midi::MidiInputListener;

namespace {

using Bytes = std::vector<unsigned char>;

/// Keeps what it is handed, and when.
class Keeping final : public MidiInputListener {
public:
    explicit Keeping(bool clock = false) : clock_(clock) {}
    void onMidi(std::span<const unsigned char> message, std::int64_t atMicros) noexcept override {
        const std::lock_guard<std::mutex> lock(mutex_);
        heard_.emplace_back(message.begin(), message.end());
        at_.push_back(atMicros);
    }
    bool wantsClock() const noexcept override { return clock_; }
    std::vector<Bytes> heard() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return heard_;
    }
    std::vector<std::int64_t> at() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return at_;
    }

private:
    bool clock_;
    mutable std::mutex mutex_;
    std::vector<Bytes> heard_;
    std::vector<std::int64_t> at_;
};

void send(MidiInputHub& hub, const std::string& port, const Bytes& bytes) {
    hub.deliver(port, std::span<const unsigned char>(bytes.data(), bytes.size()));
}

std::int64_t steadyMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

TEST_CASE("two listeners on one MIDI input share it, and each is handed what it asked for",
          "[midi]") {
    // A mixer sending notes for the control input and a clock for the follower: one device, two
    // listeners. The control input is never handed a clock's messages; the follower is handed
    // both, each stamped on the hub's clock as it arrives.
    std::int64_t now = 1'000'000;
    MidiInputHub hub([&now] { return now; });
    Keeping control;
    Keeping clock(true);
    hub.attachWithoutPort("Mixer", control);
    hub.attachWithoutPort("Mixer", clock);
    CHECK(hub.portOf(control) == "Mixer");
    CHECK(hub.portOf(clock) == "Mixer");

    send(hub, "Mixer", {0x90, 36, 100});
    now += 20'833;
    for (const int status : {0xF8, 0xFA, 0xFB, 0xFC, 0xF1}) {
        send(hub, "Mixer", {static_cast<unsigned char>(status)});
    }
    send(hub, "Mixer", {0xF2, 0x10, 0x00});
    send(hub, "Mixer", {0xB0, 7, 64});
    // Another input's messages reach neither.
    send(hub, "Elsewhere", {0x90, 40, 100});

    CHECK(control.heard() == std::vector<Bytes>{{0x90, 36, 100}, {0xB0, 7, 64}});
    const std::vector<Bytes> clocked = clock.heard();
    REQUIRE(clocked.size() == 8);
    CHECK(clocked[1] == Bytes{0xF8});
    CHECK(clocked[6] == Bytes{0xF2, 0x10, 0x00});
    const std::vector<std::int64_t> at = clock.at();
    CHECK(at.front() == 1'000'000);
    CHECK(at[1] == 1'020'833);
}

TEST_CASE("a MIDI listener that stops is handed nothing more, and moving it lets go of the first",
          "[midi]") {
    MidiInputHub hub(steadyMicros);
    Keeping one;
    Keeping two;
    hub.attachWithoutPort("A", one);
    hub.attachWithoutPort("A", two);
    hub.stopListening(one);
    send(hub, "A", {0x90, 1, 1});
    CHECK(one.heard().empty());
    CHECK(two.heard().size() == 1);
    CHECK(hub.portOf(one).empty());

    // Listening elsewhere moves it.
    hub.attachWithoutPort("B", two);
    send(hub, "A", {0x90, 2, 2});
    send(hub, "B", {0x90, 3, 3});
    CHECK(two.heard() == std::vector<Bytes>{{0x90, 1, 1}, {0x90, 3, 3}});
    CHECK(hub.portOf(two) == "B");
    hub.stopListening(two);
    hub.stopListening(two); // safe twice
    CHECK(hub.openPorts() == 0);
}

TEST_CASE("a MIDI listener that stops is not let go of while a message is being handed to it",
          "[midi]") {
    // A control surface stopping as a PANIC arrives: once `stopListening` returns, the listener
    // may be destroyed, so nothing may still be inside it.
    class Slow final : public MidiInputListener {
    public:
        void onMidi(std::span<const unsigned char>, std::int64_t) noexcept override {
            inside = true;
            while (!release) {
                std::this_thread::yield();
            }
            ++handled;
        }
        std::atomic<bool> inside{false};
        std::atomic<bool> release{false};
        std::atomic<int> handled{0};
    };
    MidiInputHub hub(steadyMicros);
    Slow slow;
    hub.attachWithoutPort("Pad", slow);
    std::thread driver([&] { send(hub, "Pad", {0x90, 36, 127}); });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!slow.inside && std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
    REQUIRE(slow.inside);
    std::atomic<bool> stopped{false};
    std::thread stopping([&] {
        hub.stopListening(slow);
        stopped = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(stopped);
    slow.release = true;
    driver.join();
    stopping.join();
    CHECK(stopped);
    CHECK(slow.handled == 1);
    send(hub, "Pad", {0x90, 36, 127});
    CHECK(slow.handled == 1);
}

TEST_CASE("a MIDI input that is not there is said to be, and in the sandbox none is opened",
          "[midi]") {
    MidiInputHub hub(steadyMicros);
    Keeping listener;
    // Not on this machine — or no MIDI API at all here, which is as much an error to report.
    CHECK_THROWS_AS(
        hub.listen("takt4 test: no such input exists on any machine", listener, "MIDI test: "),
        std::runtime_error);
    CHECK(hub.portOf(listener).empty());
    const std::vector<std::string> inputs = takt4::output::listMidiInputPorts();
    if (!inputs.empty()) {
        // One that is here: the test binaries' sandbox opens no device of the rig's.
        try {
            (void)hub.listen(inputs.front(), listener, "MIDI test: ");
            FAIL("opened \"" << inputs.front() << "\" in the sandbox");
        } catch (const std::runtime_error& error) {
            CHECK(std::string(error.what()).find("sandbox") != std::string::npos);
        }
        CHECK(hub.openPorts() == 0);
        CHECK(hub.portOf(listener).empty());
    }
}

#if defined(_WIN32)

TEST_CASE("a real MIDI input is shared by two listeners, a clock on it is followed, and it closes "
          "every time even while SysEx pours into it",
          "[midi][hardware]") {
    // Through the driver, on the tests' own virtual port. **One test, and the port's only one**:
    // after the stress below, Windows' view of that port is not to be trusted for a while — it
    // drops out of one listing and back into the next, and a handle opened on it hears nothing
    // (measured 2026-10-15) — so what has to hear something goes first, on a fresh port.
    takt4::testing::TestPort& shared = takt4::testing::sharedTestPort();
    if (!shared.midi.usable()) {
        SKIP(shared.why);
    }
    INFO(shared.why);
    REQUIRE(shared.port != nullptr);
    takt4::testing::VirtualMidi& virtualMidi = shared.midi;
    const takt4::testing::VirtualMidi::Port port = shared.port;
    MidiInputHub hub(steadyMicros);

    {
        // What a mixer sends — a clock and a note — to the control input and a clock follower on
        // one device: opened once, each listener handed what it asked for, stamped on arrival.
        Keeping control;
        Keeping clock(true);
        const std::string name = hub.listen(takt4::testing::kTestPort, control, "MIDI test: ");
        INFO("opened: " << name);
        CHECK(hub.listen(takt4::testing::kTestPort, clock, "MIDI test: ") == name);
        CHECK(hub.openPorts() == 1);
        const std::int64_t before = steadyMicros();
        std::vector<BYTE> tick{0xF8};
        std::vector<BYTE> note{0x90, 36, 100};
        for (int i = 0; i < 24; ++i) {
            (void)virtualMidi.send(port, tick.data(), static_cast<DWORD>(tick.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        (void)virtualMidi.send(port, note.data(), static_cast<DWORD>(note.size()));
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((clock.heard().size() < 25 || control.heard().empty()) &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(control.heard() == std::vector<Bytes>{{0x90, 36, 100}});
        const std::vector<Bytes> clocked = clock.heard();
        REQUIRE(clocked.size() == 25);
        CHECK(std::count(clocked.begin(), clocked.end(), Bytes{0xF8}) == 24);
        const std::vector<std::int64_t> at = clock.at();
        CHECK(at.front() >= before);
        CHECK(std::is_sorted(at.begin(), at.end()));
        hub.stopListening(control);
        CHECK(hub.openPorts() == 1);
        hub.stopListening(clock);
        CHECK(hub.openPorts() == 0);
    }

    {
        // A MIDI clock through the driver, followed: each tick stamped as it arrives on the
        // driver's thread, handed to the follower's, and the tempo and the bar read from them.
        takt4::timing::MidiClockFollower follower(hub);
        follower.begin(4);
        CHECK(follower.listen(takt4::testing::kTestPort).find("takt4 test port") !=
              std::string::npos);
        std::atomic<bool> sent{false};
        std::thread sender([&] {
            std::vector<BYTE> start{0xFA};
            std::vector<BYTE> tick{0xF8};
            (void)virtualMidi.send(port, start.data(), static_cast<DWORD>(start.size()));
            const auto gap = std::chrono::nanoseconds(19'531'250); // 128 BPM
            auto next = std::chrono::steady_clock::now();
            for (int i = 0; i < 82; ++i) {
                while (std::chrono::steady_clock::now() < next) {
                    // Spun, not slept: a sleep here is rounded to the system's 15.6 ms timer.
                }
                (void)virtualMidi.send(port, tick.data(), static_cast<DWORD>(tick.size()));
                next += gap;
            }
            sent = true;
        });
        takt4::timing::GridReading reading;
        while (!sent) {
            reading = follower.read();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        sender.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        reading = follower.read();
        CHECK(follower.status().ticks == 82);
        CHECK(reading.present);
        CHECK(std::abs(reading.bpm - 128.0) < 0.2);
        CHECK(reading.anchored);
        // The last tick sent was song tick 81, three and three-eighths beats in.
        CHECK(std::abs(reading.beat - 81.0 / 24.0) < 0.05);
        follower.stopListening();
        CHECK(hub.openPorts() == 0);
    }

    // an earlier review. RtMidi 6.0.0's WinMM `closePort` takes its lock and, holding it,
    // calls `midiInReset`, which hands back every SysEx buffer being filled through the input
    // callback — and the callback took the same lock to queue the buffer again: a close that never
    // returned, on STOP, on a port change and on the way out. cmake/rtmidi_patch.cmake applies
    // upstream's fix. Its own test could not arrange SysEx arriving at the moment of a close, and
    // passed with or without the patch; flooding this port arranges exactly that. Every close of
    // an input is the hub's.
    std::atomic<bool> pouring{true};
    std::thread flood([&] {
        std::vector<BYTE> sysex(512, 0x11);
        sysex.front() = 0xF0;
        sysex[1] = 0x7D; // non-commercial / educational
        sysex.back() = 0xF7;
        while (pouring) {
            (void)virtualMidi.send(port, sysex.data(), static_cast<DWORD>(sysex.size()));
        }
    });
    // Stopped and joined however this test ends: a REQUIRE that fails part-way through would
    // otherwise destroy a thread still running, which ends the process.
    struct StopsPouring {
        std::atomic<bool>& pouring;
        std::thread& flood;
        ~StopsPouring() {
            pouring = false;
            if (flood.joinable()) {
                flood.join();
            }
        }
    } stopsPouring{pouring, flood};

    int closes = 0;
    for (int round = 0; round < 40; ++round) {
        Keeping listener;
        (void)hub.listen(takt4::testing::kTestPort, listener, "MIDI test: ");
        REQUIRE(hub.openPorts() == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(15 + (round % 5) * 7));
        std::atomic<bool> closed{false};
        std::thread closing([&] {
            hub.stopListening(listener);
            closed = true;
        });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!closed && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!closed) {
            // A close that does not return cannot be joined or unwound: say so, and end here
            // rather than hang the run.
            FAIL_CHECK("closing the input deadlocked on round " << round);
            std::cerr << "a MIDI input close deadlocked with SysEx arriving (round " << round
                      << "); ending the test process\n"
                      << std::flush;
            ::TerminateProcess(::GetCurrentProcess(), 3);
        }
        closing.join();
        CHECK(hub.openPorts() == 0);
        ++closes;
    }
    pouring = false;
    flood.join();
    CHECK(closes == 40);
}

#endif
