#include "core/output/midi_clock.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::output::MidiClock;
using takt4::output::MidiOutput;
using takt4::output::MidiSink;

namespace {

/// Keeps every byte, so a test can say exactly what went down the wire.
class Recorder final : public MidiSink {
public:
    void send(std::span<const unsigned char> message) noexcept override {
        for (const unsigned char byte : message) {
            bytes.push_back(byte);
        }
        ++messages;
    }

    std::size_t count(unsigned char status) const {
        std::size_t found = 0;
        for (const unsigned char byte : bytes) {
            if (byte == status) {
                ++found;
            }
        }
        return found;
    }

    std::vector<unsigned char> bytes;
    std::size_t messages = 0;
};

} // namespace

TEST_CASE("nothing goes out before the clock is started", "[output][midi]") {
    Recorder recorder;
    MidiClock clock(recorder, 120.0);
    CHECK_FALSE(clock.running());
    CHECK(clock.advance(10.0) == 0);
    CHECK(recorder.bytes.empty());

    CHECK_THROWS_AS(MidiClock(recorder, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(MidiClock(recorder, -120.0), std::invalid_argument);
}

TEST_CASE("the clock runs at 24 pulses per quarter note", "[output][midi]") {
    Recorder recorder;
    MidiClock clock(recorder, 120.0); // two quarter notes a second: 48 ticks
    clock.start(0.0);
    CHECK(recorder.bytes == std::vector<unsigned char>{MidiClock::kStart});
    CHECK(clock.running());

    // Ticks fall on k/48 seconds, starting with k = 0. Advancing to just under a second
    // covers k = 0 to 47 — a whole second's worth, without asking a floating-point
    // comparison to decide whether the tick exactly on 1.0 has come due yet.
    CHECK(clock.advance(0.99) == 48);
    CHECK(clock.ticksSent() == 48);
    CHECK(recorder.count(MidiClock::kTick) == 48);
    CHECK(clock.pulseInQuarter() == 0);

    SECTION("a second second brings the total to two quarter notes a second") {
        CHECK(clock.advance(1.99) == 48);
        CHECK(clock.ticksSent() == 96);
    }

    SECTION("advancing to the same time again emits nothing") {
        CHECK(clock.advance(0.99) == 0);
        CHECK(clock.ticksSent() == 48);
    }

    SECTION("stop sends Stop and silences it") {
        clock.stop();
        CHECK(recorder.bytes.back() == MidiClock::kStop);
        CHECK_FALSE(clock.running());
        CHECK(clock.advance(5.0) == 0);
        CHECK(clock.ticksSent() == 48);

        // A second stop is not a second message.
        const std::size_t was = recorder.bytes.size();
        clock.stop();
        CHECK(recorder.bytes.size() == was);
    }

    SECTION("continue rather than start, for a transport that was paused") {
        clock.stop();
        clock.start(2.0, true);
        CHECK(recorder.bytes.back() == MidiClock::kContinue);
    }
}

namespace {

/// Every message, and when it went, on the clock the test drives.
class Wire final : public MidiSink {
public:
    struct Message {
        double at = 0.0;
        std::vector<unsigned char> bytes;
    };
    void send(std::span<const unsigned char> message) noexcept override {
        messages.push_back(Message{now, std::vector<unsigned char>(message.begin(), message.end())});
    }
    /// Where the first message starting with `status` is, or `messages.size()`.
    std::size_t find(unsigned char status) const {
        for (std::size_t i = 0; i < messages.size(); ++i) {
            if (!messages[i].bytes.empty() && messages[i].bytes[0] == status) {
                return i;
            }
        }
        return messages.size();
    }
    double now = 0.0;
    std::vector<Message> messages;
};

/// Runs `clock` a millisecond at a time up to `until`, as the output thread does.
void runTo(MidiClock& clock, Wire& wire, double until) {
    while (wire.now + 0.001 <= until + 1e-12) {
        wire.now += 0.001;
        (void)clock.advance(wire.now);
    }
}

} // namespace

TEST_CASE("MIDI Start waits for a downbeat, and the downbeat's tick is the first after it",
          "[output][midi]") {
    // The audit's M19. A receiver starts on the first tick after Start and counts its bars from
    // there, and Start used to go out on the press — so a drum machine's bar 1 was wherever the
    // operator's finger fell. The clock ticks from the press now, so the receiver has a tempo,
    // and Start waits for the downbeat it is given.
    Wire wire;
    MidiClock clock(wire, 120.0); // a beat every half second, a tick every 1/48 s
    clock.startTicking(0.0);
    CHECK(clock.running());
    CHECK_FALSE(clock.started());
    CHECK(clock.waitingToStart());
    runTo(clock, wire, 0.7);
    CHECK(wire.find(MidiClock::kTick) < wire.messages.size());
    CHECK(wire.find(MidiClock::kStart) == wire.messages.size());

    SECTION("started just ahead of the downbeat it was given, with Song Position 0 first") {
        // Bars of four from 0.5: 0.5 has gone, so the next downbeat is 2.5.
        clock.startOnDownbeat(0.5, 2.0);
        // Up to the tick before the tick before: Start follows the one at 2.479.
        runTo(clock, wire, 2.47);
        CHECK(wire.find(MidiClock::kStart) == wire.messages.size());
        runTo(clock, wire, 2.6);
        const std::size_t start = wire.find(MidiClock::kStart);
        REQUIRE(start < wire.messages.size());
        REQUIRE(start >= 2);
        CHECK(wire.messages[start - 1].bytes ==
              std::vector<unsigned char>{MidiClock::kSongPosition, 0x00, 0x00});
        CHECK(wire.messages[start - 2].bytes[0] == MidiClock::kTick);
        REQUIRE(start + 1 < wire.messages.size());
        // The tick after Start — the receiver's bar 1 — is the downbeat's.
        CHECK(wire.messages[start + 1].bytes[0] == MidiClock::kTick);
        CHECK_THAT(wire.messages[start + 1].at, Catch::Matchers::WithinAbs(2.5, 0.0011));
        CHECK(clock.started());
        CHECK_FALSE(clock.waitingToStart());

        clock.stop();
        CHECK(wire.messages.back().bytes[0] == MidiClock::kStop);
    }

    SECTION("given the downbeat after the tick before it had gone, it still starts on it") {
        runTo(clock, wire, 0.99); // pulse 23 at 0.979 has gone; the pulse 0 at 1.0 is next
        REQUIRE(clock.pulseInQuarter() == 0);
        clock.startOnDownbeat(1.0, 2.0);
        const std::size_t start = wire.find(MidiClock::kStart);
        REQUIRE(start < wire.messages.size());
        runTo(clock, wire, 1.01);
        REQUIRE(start + 1 < wire.messages.size());
        CHECK_THAT(wire.messages[start + 1].at, Catch::Matchers::WithinAbs(1.0, 0.0011));
    }

    SECTION("a clock whose beat ticks are off the beats does not start a bar off the music") {
        // Pulse 0s at 0, 0.5, 1.0, ... and downbeats at 0.3 + 2k: nowhere near each other.
        clock.startOnDownbeat(0.3, 2.0);
        runTo(clock, wire, 10.0);
        CHECK(wire.find(MidiClock::kStart) == wire.messages.size());
        CHECK(clock.waitingToStart());
    }

    SECTION("a receiver that was never told to play is not told to stop") {
        clock.stop();
        CHECK(wire.find(MidiClock::kStop) == wire.messages.size());
        CHECK_FALSE(clock.running());
    }
}

namespace {

/// A clock started on a downbeat at 0 at 120 BPM, fed a locked beat every half second whose
/// place in the bar `inBar(k)` says, as the output thread feeds it: synced, then asked to follow.
template <typename InBar>
void playBeats(MidiClock& clock, Wire& wire, int from, int to, InBar inBar) {
    for (int k = from; k < to; ++k) {
        const double at = 0.5 * k;
        runTo(clock, wire, at + 0.01); // drained a round or so after the beat, as it is live
        clock.syncToBeat(at);
        clock.followBar(at, static_cast<std::uint32_t>(inBar(k)), 4);
    }
}

std::size_t count(const Wire& wire, unsigned char status) {
    return static_cast<std::size_t>(
        std::count_if(wire.messages.begin(), wire.messages.end(), [status](const auto& m) {
            return !m.bytes.empty() && m.bytes[0] == status;
        }));
}

} // namespace

TEST_CASE("a DOWNBEAT pressed after Start moves a receiver's bars with the tracker's",
          "[output][midi]") {
    // The 2026-09-22 audit's M19, the half it left open. A receiver counts its bars from the
    // first tick after Start, and Song Position only moves it while it is stopped — so a DOWNBEAT
    // press once the clock had started moved the lights and Link and left every drum machine a
    // beat off for the rest of the run. Now the clock sends Stop, Song Position 0 and Start ahead
    // of the tracker's next downbeat when two beats in a row say the receiver's bar is elsewhere.
    Wire wire;
    MidiClock clock(wire, 120.0);
    clock.start(0.0); // the receiver's bar 1 begins with the tick at 0
    REQUIRE(clock.started());
    const auto fourFour = [](int k) { return (k % 4) + 1; };
    playBeats(clock, wire, 0, 9, fourFour);
    CHECK(count(wire, MidiClock::kStop) == 0);
    CHECK(clock.barsRealigned() == 0);

    SECTION("a beat the tracker called once in the wrong place costs nothing") {
        playBeats(clock, wire, 9, 10, [](int) { return 3; }); // beat 9 is a 2, called a 3
        playBeats(clock, wire, 10, 24, fourFour);
        CHECK(count(wire, MidiClock::kStop) == 0);
        CHECK(clock.barsRealigned() == 0);
    }

    SECTION("pressed on beat 9, the receiver's bar 1 is beat 13 — the tracker's next downbeat") {
        // The operator presses DOWNBEAT on beat 9: from there the tracker calls beat 9 the one.
        const auto pressed = [](int k) { return ((k - 9) % 4 + 4) % 4 + 1; };
        playBeats(clock, wire, 9, 24, pressed);
        REQUIRE(count(wire, MidiClock::kStop) == 1);
        CHECK(clock.barsRealigned() == 1);
        const std::size_t stop = wire.find(MidiClock::kStop);
        REQUIRE(stop + 3 < wire.messages.size());
        // Stop, then Song Position 0, then Start, then the tick on the downbeat: beat 13, at 6.5.
        CHECK(wire.messages[stop + 1].bytes ==
              std::vector<unsigned char>{MidiClock::kSongPosition, 0x00, 0x00});
        CHECK(wire.messages[stop + 2].bytes[0] == MidiClock::kStart);
        CHECK(wire.messages[stop + 3].bytes[0] == MidiClock::kTick);
        CHECK_THAT(wire.messages[stop + 3].at, Catch::Matchers::WithinAbs(6.5, 0.0011));
        // And not one tick added or dropped for it: 24 a beat from the first to the last.
        const std::size_t ticks = count(wire, MidiClock::kTick);
        const double last = wire.messages.back().at;
        CHECK(static_cast<double>(ticks) ==
              Catch::Approx(std::floor(last * 48.0) + 1.0).margin(1.0));
        // After it the receiver's bars and the tracker's agree, so nothing more is sent.
        playBeats(clock, wire, 24, 40, pressed);
        CHECK(count(wire, MidiClock::kStop) == 1);
    }

    SECTION("a receiver stopped by the operator is not started again by the bar") {
        clock.stop();
        playBeats(clock, wire, 9, 20, [](int k) { return ((k - 9) % 4 + 4) % 4 + 1; });
        CHECK(count(wire, MidiClock::kStart) == 1); // the one at 0
    }
}

TEST_CASE("the tick rate follows the tempo", "[output][midi]") {
    Recorder recorder;
    MidiClock clock(recorder, 60.0); // one quarter note a second: 24 ticks
    clock.start(0.0);
    CHECK(clock.advance(0.99) == 24);

    clock.setTempo(120.0);
    CHECK(clock.tempo() == Catch::Approx(120.0));
    // The tick already scheduled is at 1.0; from there the spacing halves, so the second
    // second holds it plus 47 more.
    CHECK(clock.advance(1.99) == 48);

    SECTION("a nonsensical tempo is ignored rather than dividing by zero") {
        clock.setTempo(0.0);
        CHECK(clock.tempo() == Catch::Approx(120.0));
        clock.setTempo(-5.0);
        CHECK(clock.tempo() == Catch::Approx(120.0));
    }
}

TEST_CASE("syncing steers the next beat tick towards the beat and adds no tick",
          "[output][midi]") {
    Recorder recorder;
    MidiClock clock(recorder, 120.0); // a tick every 1/48 s, a beat tick every half second
    clock.start(0.0);
    (void)clock.advance(0.7);
    REQUIRE(clock.ticksSent() == 34); // k/48 for k = 0..33
    REQUIRE(clock.pulseInQuarter() == 10);

    // The tracker calls a beat at 1.02, twenty milliseconds after the clock's own beat tick
    // at 1.0. The clock does not jump there — that is what added a tick (the audit's C2) —
    // it respaces the fourteen ticks left so the beat tick lands half-way, at 1.01.
    clock.syncToBeat(1.02);
    CHECK(clock.pulseInQuarter() == 10);
    CHECK(clock.advance(1.0099) == 14); // pulses 10 to 23
    CHECK(clock.pulseInQuarter() == 0);
    CHECK_THAT(clock.nextTickAt(), Catch::Matchers::WithinAbs(1.01, 1e-9));
    CHECK(clock.advance(1.0101) == 1);
    // Exactly two quarter notes since the start: 48 intervals, so the third beat's tick is the
    // 49th — not one more, which is what re-anchoring on the beat used to make it.
    CHECK(clock.ticksSent() == 49);

    SECTION("and the spacing goes back to the tempo's own after it") {
        CHECK_THAT(clock.nextTickAt() - 1.01, Catch::Matchers::WithinAbs(1.0 / 48.0, 1e-9));
    }

    SECTION("a wild beat is steered towards within bounds, never with a burst or a gap") {
        // Half a beat off: the spacing is clamped, so nothing is sent at once and nothing waits.
        clock.syncToBeat(1.26);
        const double before = clock.nextTickAt();
        CHECK(clock.advance(before) == 1);
        const double gap = clock.nextTickAt() - before;
        CHECK(gap >= MidiClock::kSteerMin / 48.0 - 1e-12);
        CHECK(gap <= MidiClock::kSteerMax / 48.0 + 1e-12);
    }

    SECTION("syncing while stopped does nothing") {
        clock.stop();
        clock.syncToBeat(1.5);
        CHECK(clock.advance(2.0) == 0);
    }
}

namespace {

/// A recorder that also notes *when* each tick went, on the clock the loop drives.
class TimedRecorder final : public MidiSink {
public:
    void send(std::span<const unsigned char> message) noexcept override {
        if (!message.empty() && message[0] == MidiClock::kTick) {
            ticks.push_back(now);
        }
    }
    double now = 0.0;
    std::vector<double> ticks;
};

/// When each beat really was, and when each tick went.
struct ClockRun {
    std::vector<double> beats;
    std::vector<double> ticks;
    std::uint64_t skipped = 0;
};

/// What the output thread does, round by round: beats reach it a pipeline after they were in
/// the music, with their stamps jittered, and it syncs the clock to each one — offset by the
/// latency — then advances it.
ClockRun runClock(double bpm, double latency, std::size_t count, double changeTo = 0.0,
                  std::size_t changeAt = 0) {
    TimedRecorder recorder;
    MidiClock clock(recorder, bpm);
    ClockRun run;
    // The music's own grid, starting somewhere the clock's start has nothing to do with.
    double at = 0.137;
    for (std::size_t k = 0; k < count; ++k) {
        run.beats.push_back(at);
        at += 60.0 / (changeTo > 0.0 && k >= changeAt ? changeTo : bpm);
    }
    // A fixed generator, so the run is the same every time: stamps ±10 ms, which is more
    // than a real beat's jitter, and a pipeline of 60 ms.
    std::mt19937 random(20260923);
    std::vector<double> stamps;
    stamps.reserve(count);
    for (std::size_t k = 0; k < count; ++k) {
        stamps.push_back(run.beats[k] +
                         (static_cast<double>(random()) / 4294967296.0) * 0.020 - 0.010);
    }
    constexpr double kPipeline = 0.060;
    clock.start(0.0);
    std::size_t next = 0;
    const double end = run.beats.back() + kPipeline + 0.5;
    for (std::size_t round = 0;; ++round) {
        const double now = static_cast<double>(round) * 0.001;
        if (now >= end) {
            break;
        }
        while (next < count && run.beats[next] + kPipeline <= now) {
            clock.setTempo(changeTo > 0.0 && next >= changeAt ? changeTo : bpm);
            clock.syncToBeat(stamps[next] + latency);
            ++next;
        }
        recorder.now = now;
        (void)clock.advance(now);
    }
    run.ticks = recorder.ticks;
    run.skipped = clock.ticksSkipped();
    return run;
}

/// Ticks between two instants, the first included.
std::size_t ticksBetween(const std::vector<double>& ticks, double from, double to) {
    return static_cast<std::size_t>(std::lower_bound(ticks.begin(), ticks.end(), to) -
                                    std::lower_bound(ticks.begin(), ticks.end(), from));
}

/// Where a receiver counting ticks from Start puts its beats — every 24th tick — against the
/// music's: how far the worst lands from the nearest beat, and how many times the nearest beat
/// was not the one after the last. A slip is a tick added or dropped, which is exactly what a
/// drum machine counting 24 to a beat cannot survive. From the receiver's beat `settle` on,
/// and only while there is music: the clock runs on after the last beat.
struct Landing {
    double worst = 0.0;
    std::size_t slips = 0;
    std::size_t beats = 0;
};

Landing landing(const ClockRun& run, double latency, std::size_t settle) {
    Landing out;
    std::size_t last = 0;
    bool any = false;
    for (std::size_t n = 24 * settle; n < run.ticks.size(); n += 24) {
        const double t = run.ticks[n] - latency;
        if (t > run.beats.back() + 0.05) {
            break;
        }
        const auto after = std::lower_bound(run.beats.begin(), run.beats.end(), t);
        std::size_t nearest = static_cast<std::size_t>(after - run.beats.begin());
        if (after == run.beats.end() ||
            (after != run.beats.begin() && t - *(after - 1) < *after - t)) {
            nearest -= 1;
        }
        out.worst = std::max(out.worst, std::abs(run.beats[nearest] - t));
        if (any && nearest != last + 1) {
            ++out.slips;
        }
        last = nearest;
        any = true;
        ++out.beats;
    }
    return out;
}

} // namespace

TEST_CASE("the clock sends exactly 24 ticks a beat against jittered beats", "[output][midi]") {
    // The audit's C2, as it asked for it: beats with ±10 ms of jitter, thousands of them, and
    // exactly 24 ticks to every one. The clock this replaced sent 25 on 44 % of beats at 128
    // BPM with no offset, and 26 to 28 on every beat at −40 ms, in zero-gap bursts — a drum
    // machine counting ticks walked off the music by two beats a minute.
    for (const double latency : {0.0, -0.040, 0.030}) {
        for (const double bpm : {128.0, 174.0, 93.7}) {
            INFO("latency " << latency * 1000.0 << " ms at " << bpm << " BPM");
            const ClockRun run = runClock(bpm, latency, 2000);
            const double beat = 60.0 / bpm;
            const double tick = beat / 24.0;
            CHECK(run.skipped == 0);

            // Every beat once the clock has pulled in: the receiver's beat lands on the next
            // beat of the music, every time — no tick added, none dropped — and on it within the
            // jitter's reach, halved by the steering, plus a round of the loop.
            const Landing landed = landing(run, latency, 16);
            INFO("worst beat-tick error " << landed.worst * 1000.0 << " ms over " << landed.beats
                                          << " beats");
            CHECK(landed.beats > 1900);
            CHECK(landed.slips == 0);
            CHECK(landed.worst < 0.012);

            // And never a burst or a hole: every gap within the steering's bounds, less the
            // millisecond a round can add or take.
            double narrowest = 1.0;
            double widest = 0.0;
            for (std::size_t i = 1; i < run.ticks.size(); ++i) {
                narrowest = std::min(narrowest, run.ticks[i] - run.ticks[i - 1]);
                widest = std::max(widest, run.ticks[i] - run.ticks[i - 1]);
            }
            CHECK(narrowest >= tick * MidiClock::kSteerMin - 0.0011);
            CHECK(widest <= tick * MidiClock::kSteerMax + 0.0011);
        }
    }
}

TEST_CASE("the clock follows a tempo change without adding or dropping a tick",
          "[output][midi]") {
    // 128 to 140 BPM half-way through: a new record. Within a few beats of the change every
    // beat is 24 ticks again and the beat tick is on the beat.
    const ClockRun run = runClock(128.0, 0.0, 1000, 140.0, 500);
    CHECK(run.skipped == 0);
    // No slip anywhere after the clock first pulled in — the change included — and on the beat
    // again within a few beats of it.
    const Landing whole = landing(run, 0.0, 16);
    CHECK(whole.slips == 0);
    const Landing after = landing(run, 0.0, 520);
    CHECK(after.worst < 0.012);
    // Over the whole run the ticks are the beats times 24, give or take the half beat the
    // clock started out of phase by.
    const std::size_t total =
        ticksBetween(run.ticks, run.beats.front() - 0.2, run.beats.back() + 0.2);
    CHECK(total + 24 >= 24 * run.beats.size());
    CHECK(total <= 24 * run.beats.size() + 24);
}

TEST_CASE("a long stall is skipped rather than flooded", "[output][midi]") {
    Wire wire;
    MidiClock clock(wire, 120.0); // a tick every 1/48 s, a beat every half second
    clock.start(0.0);
    // A third of the way into a quarter note.
    runTo(clock, wire, 0.16);
    REQUIRE(clock.ticksSent() == 8);
    REQUIRE(clock.pulseInQuarter() == 8);

    // Nothing advanced the clock for a minute. Sending the 2880 ticks that were missed
    // would be worse for the receiver than skipping them.
    wire.now = 60.01;
    const std::size_t emitted = clock.advance(wire.now);
    CHECK(emitted == MidiClock::kMaxBurst);
    CHECK(clock.ticksSkipped() > 2000);
    // Nothing was lost: the ticks sent plus the ones skipped are the whole minute.
    CHECK(clock.ticksSent() + clock.ticksSkipped() >= 60 * 48);

    // **Skipped in whole quarter notes**, so the pulse this clock counts is still the one the
    // receiver does: it counts every tick it is sent. A part of a quarter skipped moved the
    // clock's pulse 0 — where it puts the beat, and Start — off the receiver's for the rest of
    // the run.
    CHECK(clock.ticksSkipped() % 24 == 0);
    CHECK(clock.pulseInQuarter() == clock.ticksSent() % 24);

    // ...and it carries on from where it gave up, at the right rate and on the grid its ticks
    // were on: every 24th tick the receiver counts on a beat of it.
    const std::size_t from = wire.messages.size();
    const std::uint64_t counted = clock.ticksSent();
    runTo(clock, wire, 62.0);
    std::size_t inSecond = 0;
    std::size_t beats = 0;
    std::uint64_t index = counted;
    for (std::size_t i = from; i < wire.messages.size(); ++i) {
        REQUIRE(wire.messages[i].bytes == std::vector<unsigned char>{MidiClock::kTick});
        const double at = wire.messages[i].at;
        if (at >= 60.6 && at < 61.6) {
            ++inSecond;
        }
        if (index % 24 == 0) {
            INFO("the receiver's beat at " << at);
            const double beat = 0.5 * std::round(at / 0.5);
            CHECK(at - beat >= -1e-9);
            CHECK(at - beat <= 0.0011); // the round the output thread drains it in
            ++beats;
        }
        ++index;
    }
    CHECK(inSecond == 48);
    CHECK(beats >= 3);
}

TEST_CASE("a clock started off the beat starts its receiver on the downbeat", "[output][midi]") {
    // START is pressed wherever the operator's finger falls, and the clock ticks from there so a
    // receiver has a tempo. Start then waits for a pulse 0 within a quarter of a beat of the
    // tracker's downbeat — and the pulse 0 used to be steered there half the way each beat, so
    // the receiver's bar 1 began up to a quarter of a beat off the music (117 ms at 128). Before
    // Start nobody is counting the pulses, so the clock puts its pulse 0 on the beat in one step.
    // From every phase of a beat, so the press that leaves the next tick a pulse 0 is in it.
    constexpr int kPhases = 96;
    for (int i = 0; i < kPhases; ++i) {
        const double pressed = 0.003 + 0.5 * static_cast<double>(i) / kPhases;
        INFO("START pressed at " << pressed);
        Wire wire;
        MidiClock clock(wire, 120.0); // a tick every 1/48 s
        wire.now = pressed;
        clock.startTicking(pressed);
        // Beats every half second from 1.0, bars of four from 0, each drained 10 ms after it, as
        // the output thread gives them to the clock.
        for (int k = 2; k < 24 && !clock.started(); ++k) {
            const double at = 0.5 * k;
            runTo(clock, wire, at + 0.01);
            clock.syncToBeat(at);
            const int inBar = k % 4 + 1;
            if (clock.waitingToStart()) {
                clock.startOnDownbeat(at - 0.5 * (inBar - 1), 2.0);
            }
        }
        runTo(clock, wire, wire.now + 0.1);
        const std::size_t start = wire.find(MidiClock::kStart);
        REQUIRE(start + 1 < wire.messages.size());
        // The receiver's bar 1, the tick after Start, on the first downbeat after the first beat
        // it was given — within the round the output thread sends it in.
        REQUIRE(wire.messages[start + 1].bytes == std::vector<unsigned char>{MidiClock::kTick});
        const double bar1 = wire.messages[start + 1].at;
        CHECK(bar1 - 2.0 >= -1e-9);
        CHECK(bar1 - 2.0 <= 0.0011);
        // And no tick before it closer than half the tempo's spacing, or further than half again:
        // nothing counted them, but a burst would read to a receiver as a tempo.
        double previous = -1.0;
        for (std::size_t m = 0; m < start; ++m) {
            if (wire.messages[m].bytes[0] != MidiClock::kTick) {
                continue;
            }
            if (previous >= 0.0) {
                const double gap = wire.messages[m].at - previous;
                CHECK(gap >= 0.5 / 48.0 - 0.0011);
                CHECK(gap <= 1.5 / 48.0 + 0.0011);
            }
            previous = wire.messages[m].at;
        }
    }
}

TEST_CASE("a MIDI port that is not there fails with something worth reading", "[output][midi]") {
    // No machine this runs on is guaranteed to have a MIDI device, so the negative case
    // is the one that can be asserted anywhere. Two things differ across machines and
    // both have to come out as a std::runtime_error rather than RtMidi's own exception
    // type: a working API with no matching port, and — a headless Linux CI runner, for
    // instance — no usable MIDI API at all, which RtMidi signals by throwing from its
    // own constructor.
    for (const char* spec : {"no such port, surely", "99999"}) {
        INFO("asked for \"" << spec << "\"");
        bool threw = false;
        try {
            const MidiOutput opened(spec);
            (void)opened;
        } catch (const std::runtime_error& error) {
            threw = true;
            const std::string message = error.what();
            INFO("message: " << message);
            CHECK_THAT(message, ContainsSubstring("MIDI output"));
            // Where there is an API the message lists the ports that were found, which
            // is what an operator with a differently-named interface needs.
            CHECK((message.find("no port matching") != std::string::npos ||
                   message.find("no usable MIDI API") != std::string::npos));
            // Either way it says which port was wanted: the status line is all the operator
            // sees, and "no usable MIDI API" alone does not say what was being opened.
            CHECK_THAT(message, ContainsSubstring(std::string("\"") + spec + "\""));
        }
        CHECK(threw);
    }
}
