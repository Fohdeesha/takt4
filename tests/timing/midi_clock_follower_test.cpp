#include "core/midi/midi_input_hub.hpp"
#include "core/timing/beat_grid.hpp"
#include "core/timing/grid_source.hpp"
#include "core/timing/midi_clock_follower.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

using Catch::Approx;
using takt4::engine::EngineBeat;
using takt4::midi::MidiInputHub;
using takt4::timing::BeatGrid;
using takt4::timing::GridReading;
using takt4::timing::MidiClockFollower;

namespace {

constexpr unsigned char kTick = 0xF8;
constexpr unsigned char kStart = 0xFA;
constexpr unsigned char kContinue = 0xFB;
constexpr unsigned char kStop = 0xFC;

/// A mixer on the end of a MIDI cable, a hand-set clock, and the follower listening to it, read
/// once a millisecond as `ExternalBeats` reads it, its readings turned into beats by a `BeatGrid`.
class Rig {
public:
    explicit Rig(double bpm)
        : gap_(60e6 / (bpm * 24.0)), hub_([this] { return now_; }), follower_(hub_) {
        hub_.attachWithoutPort("Mixer", follower_);
        follower_.begin(4);
        nextTick_ = static_cast<double>(now_) + 300.0;
    }

    /// Sends `message` now.
    void send(std::vector<unsigned char> message) {
        hub_.deliver("Mixer", std::span<const unsigned char>(message.data(), message.size()));
    }

    /// Runs `seconds` on, a tick every gap while `ticking`, a reading every millisecond. Returns
    /// the beats sent and remembers every tick's moment.
    std::vector<EngineBeat> run(double seconds, bool ticking = true) {
        std::vector<EngineBeat> beats;
        const std::int64_t until = now_ + static_cast<std::int64_t>(seconds * 1e6);
        while (now_ < until) {
            const std::int64_t next = now_ + 1000;
            while (ticking && nextTick_ < static_cast<double>(next)) {
                now_ = static_cast<std::int64_t>(std::llround(nextTick_));
                send({kTick});
                ticks_.push_back(now_);
                nextTick_ += gap_;
            }
            now_ = next;
            last_ = follower_.read();
            if (const std::optional<EngineBeat> beat = grid_.advance(last_)) {
                beats.push_back(*beat);
            }
        }
        if (!ticking) {
            nextTick_ = static_cast<double>(now_) + 300.0;
        }
        return beats;
    }

    void setTempo(double bpm) { gap_ = 60e6 / (bpm * 24.0); }

    /// takt4's STOP and START: the run ends, and another begins.
    void restart() {
        follower_.end();
        follower_.begin(4);
        grid_.reset(BeatGrid::Options{});
    }

    MidiClockFollower& follower() { return follower_; }
    BeatGrid& grid() { return grid_; }
    const GridReading& last() const { return last_; }
    /// The moment of every tick sent, in order.
    const std::vector<std::int64_t>& ticks() const { return ticks_; }
    std::int64_t now() const { return now_; }

private:
    double gap_;
    std::int64_t now_ = 5'000'000'000LL;
    double nextTick_ = 0.0;
    MidiInputHub hub_;
    MidiClockFollower follower_;
    BeatGrid grid_;
    GridReading last_;
    std::vector<std::int64_t> ticks_;
};

} // namespace

TEST_CASE("a MIDI clock is followed once two beats of it have come, at its tempo",
          "[timing][midi][follow]") {
    for (const double bpm : {90.0, 128.0, 174.0}) {
        INFO(bpm << " BPM");
        Rig rig(bpm);
        // 47 ticks: not yet.
        (void)rig.run(46.5 * 60.0 / (bpm * 24.0)); // the first comes 0.3 ms in
        CHECK(rig.ticks().size() == 47);
        CHECK_FALSE(rig.last().present);
        CHECK(rig.follower().status().ticking);
        CHECK(rig.follower().status().bpm == 0.0);
        // The 48th, and it is.
        (void)rig.run(1.0 * 60.0 / (bpm * 24.0));
        REQUIRE(rig.last().present);
        CHECK(rig.last().bpm == Approx(bpm).margin(0.01));
        CHECK(rig.follower().status().bpm == Approx(bpm).margin(0.01));
        // No Start has said where the beats are.
        CHECK_FALSE(rig.last().anchored);
        CHECK_FALSE(rig.follower().status().placed);
        // The beat runs on with the ticks: 24 of them to a beat.
        const double before = rig.last().beat;
        (void)rig.run(24.0 * 60.0 / (bpm * 24.0));
        CHECK(rig.last().beat - before == Approx(1.0).margin(0.002));
    }
}

TEST_CASE("Start makes the next MIDI clock tick the downbeat, and every beat is on its tick",
          "[timing][midi][follow]") {
    Rig rig(128.0);
    (void)rig.run(1.3); // a mixer's clock running with no Start
    rig.send({kStart});
    const std::size_t first = rig.ticks().size(); // the next tick is the song's first
    const std::vector<EngineBeat> beats = rig.run(4.0);
    REQUIRE(beats.size() >= 7);
    CHECK(rig.last().anchored);
    CHECK(rig.follower().status().placed);
    // Beat 1 of bar 1 at the first tick after Start, and each beat after it on its 24th tick.
    std::size_t checked = 0;
    for (const EngineBeat& beat : beats) {
        if (beat.event.beatInBar == 0) {
            continue; // before Start: nobody's beats
        }
        const auto offset = static_cast<double>(beat.hostMicros - rig.ticks()[first]);
        const auto index = static_cast<std::int64_t>(std::llround(offset / (60e6 / 128.0)));
        INFO("beat " << index);
        CHECK(beat.hostMicros ==
              Approx(static_cast<double>(rig.ticks()[first + 24 * static_cast<std::size_t>(index)]))
                  .margin(1000.0));
        CHECK(beat.event.beatInBar == static_cast<std::uint32_t>(index % 4 + 1));
        if (index == 0) {
            CHECK(beat.event.downbeat);
        }
        ++checked;
    }
    CHECK(checked >= 7);
}

TEST_CASE("Continue goes on from where the MIDI song stopped, or from a Song Position",
          "[timing][midi][follow]") {
    // A tick every 20.8 ms. The clock runs on through the Stop, and so do the beats, on from where
    // the song stopped; how long it runs decides how far before the Continue the last of them is.
    const double gap = 60e6 / (120.0 * 24.0) / 1e6;
    Rig rig(120.0);
    (void)rig.run(1.0);
    rig.send({kStart});
    const std::size_t start = rig.ticks().size();
    (void)rig.run(30.5 * gap); // song ticks 0 to 30: a beat and a quarter
    REQUIRE(rig.ticks().size() - start == 31);
    rig.send({kStop});

    SECTION("Continue: the next tick is the one after the last before Stop") {
        (void)rig.run(1.03);
        rig.send({kContinue});
        const std::size_t resumed = rig.ticks().size();
        const std::vector<EngineBeat> beats = rig.run(2.0);
        // Song tick 31 next, so beat 2 is 17 ticks on: the third of the bar.
        bool found = false;
        for (const EngineBeat& beat : beats) {
            if (beat.hostMicros > rig.ticks()[resumed] - 5000) {
                CHECK(beat.hostMicros ==
                      Approx(static_cast<double>(rig.ticks()[resumed + 17])).margin(1000.0));
                CHECK(beat.event.beatInBar == 3);
                found = true;
                break;
            }
        }
        CHECK(found);
    }

    SECTION("a Song Position while stopped is where Continue goes on from") {
        // 35 ticks on through the Stop: the beats are at song tick 66, three quarters past one.
        (void)rig.run(34.5 * gap);
        rig.send({0xF2, 16, 0}); // sixteenth 16: beat 4, the downbeat of bar 2
        rig.send({kContinue});
        const std::size_t resumed = rig.ticks().size();
        const std::vector<EngineBeat> beats = rig.run(2.0);
        bool found = false;
        for (const EngineBeat& beat : beats) {
            if (beat.hostMicros > rig.ticks()[resumed] - 5000) {
                CHECK(beat.hostMicros ==
                      Approx(static_cast<double>(rig.ticks()[resumed])).margin(1000.0));
                CHECK(beat.event.beatInBar == 1);
                CHECK(beat.event.downbeat);
                CHECK(beat.event.snapped); // the bar moved: a bridging Link output forces it
                found = true;
                break;
            }
        }
        CHECK(found);
    }

    SECTION("a downbeat placed just after a beat went out is declared on its moment, not sent") {
        // 19 ticks on: the beats are at song tick 50, just past one, so the bar the Song Position
        // places begins a sixth of a beat after the last beat sent — too soon to send another.
        (void)rig.run(18.5 * gap);
        const std::uint64_t declaredBefore = rig.grid().state().barsDeclared;
        const std::uint64_t beatsBefore = rig.grid().state().beats;
        rig.send({0xF2, 16, 0});
        rig.send({kContinue});
        const std::size_t resumed = rig.ticks().size();
        int steps = 0;
        while (rig.grid().state().barsDeclared == declaredBefore && steps++ < 100) {
            (void)rig.run(0.001);
        }
        REQUIRE(rig.grid().state().barsDeclared == declaredBefore + 1);
        // Declared by the reading after the downbeat's tick, and no beat sent for it.
        REQUIRE(rig.ticks().size() > resumed);
        CHECK(rig.now() - rig.ticks()[resumed] >= 0);
        CHECK(rig.now() - rig.ticks()[resumed] <= 1000);
        CHECK(rig.grid().state().beatInBar == 1);
        CHECK(rig.grid().state().declaredBar == rig.grid().state().bars);
        CHECK(rig.grid().state().beats == beatsBefore);
        // The next beat is the bar's second, a beat after the downbeat, and carries the move.
        const std::vector<EngineBeat> beats = rig.run(1.0);
        REQUIRE_FALSE(beats.empty());
        CHECK(beats[0].hostMicros ==
              Approx(static_cast<double>(rig.ticks()[resumed + 24])).margin(1000.0));
        CHECK(beats[0].event.beatInBar == 2);
        CHECK(beats[0].event.snapped);
    }
}

TEST_CASE("a MIDI Stop halts the beats only when it is to be obeyed", "[timing][midi][follow]") {
    Rig rig(128.0);
    rig.send({kStart});
    REQUIRE_FALSE(rig.run(2.0).empty());

    SECTION("not obeyed: the clock runs on and so do the beats") {
        rig.send({kStop});
        const std::vector<EngineBeat> beats = rig.run(2.0);
        CHECK_FALSE(rig.last().stopped);
        CHECK(beats.size() >= 3);
        CHECK(rig.grid().state().locked);
        CHECK(rig.follower().status().stopped);
    }

    SECTION("obeyed: nothing from the Stop on, and no coast, until Start") {
        rig.follower().setObeyStartStop(true);
        rig.send({kStop});
        const std::vector<EngineBeat> beats = rig.run(2.0);
        CHECK(rig.last().stopped);
        CHECK(beats.empty());
        CHECK(rig.grid().state().noSignal);
        rig.send({kStart});
        CHECK_FALSE(rig.run(2.0).empty());
        CHECK_FALSE(rig.last().stopped);
    }

    SECTION("obeying switched on while stopped halts them at once") {
        rig.send({kStop});
        (void)rig.run(0.5);
        CHECK_FALSE(rig.last().stopped);
        rig.follower().setObeyStartStop(true);
        CHECK(rig.run(1.0).empty());
        CHECK(rig.last().stopped);
    }
}

TEST_CASE("a MIDI clock that stops is gone, and comes back measured afresh",
          "[timing][midi][follow]") {
    Rig rig(128.0);
    (void)rig.run(2.0);
    REQUIRE(rig.last().present);
    // Four tick gaps, 78 ms at 128 BPM — but 100 ms at the least.
    (void)rig.run(0.09, false);
    CHECK(rig.last().present);
    (void)rig.run(0.02, false);
    CHECK_FALSE(rig.last().present);
    CHECK_FALSE(rig.follower().status().ticking);
    // Back at another tempo, from another phase, with no Start: two beats of it before it is
    // believed, though a tempo is known from before.
    rig.setTempo(140.0);
    (void)rig.run(46.5 * 60.0 / (140.0 * 24.0));
    CHECK_FALSE(rig.last().present);
    (void)rig.run(1.0 * 60.0 / (140.0 * 24.0));
    REQUIRE(rig.last().present);
    CHECK(rig.last().bpm == Approx(140.0).margin(0.01));
}

TEST_CASE("a MIDI clock says where in its beat the last tick was, counted from Start once there "
          "is one",
          "[timing][midi][follow]") {
    // What the window's strip lights: a beat's 24 ticks, as far as they have come.
    std::int64_t now = 1'000'000;
    MidiInputHub hub([&now] { return now; });
    MidiClockFollower follower(hub);
    hub.attachWithoutPort("Mixer", follower);
    const auto send = [&](std::vector<unsigned char> message) {
        hub.deliver("Mixer", std::span<const unsigned char>(message.data(), message.size()));
    };
    const auto ticks = [&](int count) {
        for (int i = 0; i < count; ++i) {
            now += 20'833;
            send({kTick});
        }
        (void)follower.read();
    };
    follower.begin(4);
    CHECK(follower.status().tickInBeat == 0);
    // Counted from the first tick heard: the 50th is the second of a beat.
    ticks(50);
    CHECK(follower.status().tickInBeat == 1);
    // Start: the next tick is the song's first, and 30 ticks in is a quarter past its second beat.
    send({kStart});
    ticks(30);
    CHECK(follower.status().tickInBeat == 5);
    // A Song Position while playing: sixteenth 3 is tick 18 of the song's first beat.
    send({0xF2, 3, 0});
    ticks(1);
    CHECK(follower.status().tickInBeat == 18);
}

TEST_CASE("a MIDI clock follower takes only a clock's messages, and none from before its run",
          "[timing][midi][follow]") {
    std::int64_t now = 1'000'000;
    MidiInputHub hub([&now] { return now; });
    MidiClockFollower follower(hub);
    hub.attachWithoutPort("Pads", follower);
    const auto send = [&](std::vector<unsigned char> message) {
        hub.deliver("Pads", std::span<const unsigned char>(message.data(), message.size()));
    };
    for (int i = 0; i < 10; ++i) {
        send({kTick});
    }
    follower.begin(4);
    CHECK(follower.status().ticks == 0); // what came before the run is not the run's
    send({0x90, 36, 100});
    send({0xB0, 7, 64});
    send({0xF1, 0x20});
    (void)follower.read();
    CHECK(follower.status().ticks == 0);
    send({kTick});
    (void)follower.read();
    CHECK(follower.status().ticks == 1);
    CHECK(follower.port() == "Pads");
    follower.stopListening();
    CHECK(follower.port().empty());
}

TEST_CASE(
    "a MIDI clock that ticks only while its song plays is followed from the song's first tick",
    "[timing][midi][follow]") {
    // A DAW's clock: Start and the first tick together, ticks while the song plays, then Stop and
    // nothing. Once a song has been followed its tempo is known, and the next song is followed
    // from its first tick — bar 1's downbeat on it — rather than two beats on.
    const double played = 128.0;
    const double period = 60e6 / played;
    Rig rig(played);
    rig.send({kStart});
    const std::vector<EngineBeat> first = rig.run(3.0);
    REQUIRE_FALSE(first.empty());
    rig.send({kStop});
    // Stopped half a beat after a beat — coasted still, or let go of — so the song's downbeat is a
    // beat of its own, not one declared just after a coasted one.
    const int beatsStopped = GENERATE(2, 12);
    const double resumeAt = static_cast<double>(first.back().hostMicros) +
                            (static_cast<double>(beatsStopped) + 0.5) * period;
    (void)rig.run((resumeAt - static_cast<double>(rig.now())) / 1e6, false);
    CHECK(rig.grid().state().noSignal == (beatsStopped > 10));
    // At the tempo it stopped at, or one set while it was stopped.
    const double bpm = GENERATE(128.0, 133.0);
    INFO(beatsStopped << " beats stopped, then " << bpm << " BPM");
    rig.setTempo(bpm);
    rig.send({kStart});
    const std::size_t song = rig.ticks().size();
    const std::vector<EngineBeat> beats = rig.run(2.5);
    std::vector<EngineBeat> after;
    for (const EngineBeat& beat : beats) {
        if (beat.hostMicros >= rig.ticks()[song] - 1000) {
            after.push_back(beat);
        }
    }
    REQUIRE(after.size() >= 4);
    for (std::size_t i = 0; i < after.size(); ++i) {
        INFO("beat " << i);
        CHECK(static_cast<double>(after[i].hostMicros) ==
              Approx(static_cast<double>(rig.ticks()[song + 24 * i])).margin(1000.0));
        CHECK(after[i].event.beatInBar == static_cast<std::uint32_t>(i % 4 + 1));
        CHECK(after[i].event.acquired);
    }
    CHECK(rig.last().bpm == Approx(bpm).margin(0.01));
}

TEST_CASE("a MIDI clock's tempo is known from one run of takt4 to the next, and not before",
          "[timing][midi][follow]") {
    Rig rig(120.0);
    // Nothing known: the first song waits two beats, and its first beat sent is the third.
    rig.send({kStart});
    std::size_t song = rig.ticks().size();
    std::vector<EngineBeat> beats = rig.run(3.0);
    REQUIRE_FALSE(beats.empty());
    CHECK(static_cast<double>(beats[0].hostMicros) ==
          Approx(static_cast<double>(rig.ticks()[song + 48])).margin(1000.0));
    CHECK(beats[0].event.beatInBar == 3);
    rig.send({kStop});
    (void)rig.run(1.0, false);
    // takt4 stopped and started again, then the next song: on its first tick.
    rig.restart();
    rig.send({kStart});
    song = rig.ticks().size();
    beats = rig.run(1.0);
    REQUIRE_FALSE(beats.empty());
    CHECK(static_cast<double>(beats[0].hostMicros) ==
          Approx(static_cast<double>(rig.ticks()[song])).margin(1000.0));
    CHECK(beats[0].event.beatInBar == 1);
}
