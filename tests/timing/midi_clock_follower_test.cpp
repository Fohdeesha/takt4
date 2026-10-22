#include "core/midi/midi_input_hub.hpp"
#include "core/timing/beat_grid.hpp"
#include "core/timing/grid_source.hpp"
#include "core/timing/midi_clock_follower.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
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

    /// Sends `message` now. A Start makes the next tick the song's first.
    void send(std::vector<unsigned char> message) {
        if (!message.empty() && message[0] == kStart) {
            songZero_ = moments_.size();
        }
        hub_.deliver("Mixer", std::span<const unsigned char>(message.data(), message.size()));
    }

    /// Runs `seconds` on, a tick every gap while `ticking`, a reading every millisecond. Returns
    /// the beats sent and remembers every tick's moment.
    std::vector<EngineBeat> run(double seconds, bool ticking = true) {
        return carryOn(seconds, ticking ? Ticks::Sent : Ticks::None);
    }

    /// `seconds` of a delivery stall while the clock runs on: the ticks due in it are held, and
    /// handed over together as it ends, `spacing` microseconds apart — a stalled driver's backlog.
    std::vector<EngineBeat> runStalled(double seconds, double spacing) {
        std::vector<EngineBeat> beats = carryOn(seconds, Ticks::Held);
        for (std::size_t i = 0; i < held_; ++i) {
            if (i > 0) {
                now_ += static_cast<std::int64_t>(spacing);
            }
            send({kTick});
            ticks_.push_back(now_);
        }
        held_ = 0;
        return beats;
    }

    /// `seconds` with the cable out while the clock runs on: every tick due in it is lost.
    std::vector<EngineBeat> runLosing(double seconds) { return carryOn(seconds, Ticks::Lost); }

    /// Where the sender's beat `n` of the song since the last Start was, by its own ticks.
    double songBeatAt(std::size_t n) const { return moments_.at(songZero_ + 24 * n); }
    /// The highest tempo the follower said it was present at.
    double highestBpm() const { return highestBpm_; }

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
    /// What becomes of the ticks due while the clock runs on.
    enum class Ticks { Sent, None, Held, Lost };

    std::vector<EngineBeat> carryOn(double seconds, Ticks ticks) {
        std::vector<EngineBeat> beats;
        const std::int64_t until = now_ + static_cast<std::int64_t>(seconds * 1e6);
        while (now_ < until) {
            const std::int64_t next = now_ + 1000;
            while (ticks != Ticks::None && nextTick_ < static_cast<double>(next)) {
                moments_.push_back(nextTick_);
                if (ticks == Ticks::Sent) {
                    now_ = static_cast<std::int64_t>(std::llround(nextTick_));
                    send({kTick});
                    ticks_.push_back(now_);
                } else if (ticks == Ticks::Held) {
                    ++held_;
                }
                nextTick_ += gap_;
            }
            now_ = next;
            last_ = follower_.read();
            if (last_.present) {
                highestBpm_ = std::max(highestBpm_, last_.bpm);
            }
            if (const std::optional<EngineBeat> beat = grid_.advance(last_)) {
                beats.push_back(*beat);
            }
        }
        if (ticks == Ticks::None) {
            nextTick_ = static_cast<double>(now_) + 300.0;
        }
        return beats;
    }

    double gap_;
    std::int64_t now_ = 5'000'000'000LL;
    double nextTick_ = 0.0;
    MidiInputHub hub_;
    MidiClockFollower follower_;
    BeatGrid grid_;
    GridReading last_;
    std::vector<std::int64_t> ticks_;
    /// Every tick's moment as the clock sent it, delivered or not; and where the last Start's
    /// song began in it.
    std::vector<double> moments_;
    std::size_t songZero_ = 0;
    std::size_t held_ = 0;
    double highestBpm_ = 0.0;
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

namespace {

/// No two beats closer than `share` of a beat at `bpm`: no flood.
void noFlood(const std::vector<EngineBeat>& beats, double bpm, double share = 0.4) {
    for (std::size_t i = 1; i < beats.size(); ++i) {
        INFO("beats " << i - 1 << " and " << i);
        CHECK(static_cast<double>(beats[i].hostMicros - beats[i - 1].hostMicros) >=
              share * 60e6 / bpm);
    }
}

std::vector<EngineBeat> joined(std::vector<EngineBeat> first, const std::vector<EngineBeat>& then) {
    first.insert(first.end(), then.begin(), then.end());
    return first;
}

} // namespace

TEST_CASE("a MIDI clock's delivery stall and the burst after it are followed as the clock, not "
          "as thousands of BPM",
          "[timing][midi][follow]") {
    // A stall of more than 100 ms starts a new run of ticks, whose
    // first were taken unchecked: the ticks it held, handed over microseconds apart, made a line
    // of thousands of BPM, trusted once two beats of ticks had come — 42 beats a millisecond apart
    // from a stall of half a second at 128, 455 from one of a second. Nothing is followed at a
    // tempo no clock has, the burst starts the run's count again, and the beats coast through.
    for (const double stall : {0.15, 0.5, 1.0}) {
        for (const double spacing : {5.0, 200.0}) {
            INFO(stall << " s stall, its ticks " << spacing << " us apart");
            Rig rig(128.0);
            (void)rig.run(3.0);
            REQUIRE(rig.last().present);
            std::vector<EngineBeat> beats = rig.runStalled(stall, spacing);
            beats = joined(beats, rig.run(4.0));
            noFlood(beats, 128.0);
            CHECK(rig.highestBpm() < 130.0);
            CHECK(rig.highestBpm() > 126.0);
            // As many beats as the music had: coasted through the stall, and followed after it.
            const double seconds = stall + 4.0;
            CHECK(static_cast<double>(beats.size()) >= std::floor(seconds * 128.0 / 60.0) - 1.0);
            CHECK(static_cast<double>(beats.size()) <= std::ceil(seconds * 128.0 / 60.0) + 1.0);
            CHECK(rig.last().present);
        }
    }
}

TEST_CASE("a MIDI clock stalled at 174 BPM with its ticks handed over 3 ms apart is no tempo",
          "[timing][midi][follow]") {
    // Ticks 3 ms apart are 833 BPM, inside the 2.5x a line may move by — and 600 ms of them sent
    // 262 beats. Bunched as the run starts, they are no tempo either.
    Rig rig(174.0);
    (void)rig.run(3.0);
    std::vector<EngineBeat> beats = rig.runStalled(0.6, 3000.0);
    beats = joined(beats, rig.run(4.0));
    noFlood(beats, 174.0);
    CHECK(rig.highestBpm() < 177.0);
}

TEST_CASE("STOP in a MIDI clock's stall remembers the clock's tempo, not the burst's",
          "[timing][midi][follow]") {
    // STOP during the flood used to remember 5 us a tick, and the next song's Start sent 93 beats
    // in its first 200 ms.
    Rig rig(128.0);
    rig.send({kStart});
    (void)rig.run(3.0);
    (void)rig.runStalled(0.5, 5.0);
    (void)rig.run(0.2);
    rig.restart();
    rig.send({kStart});
    const std::vector<EngineBeat> beats = rig.run(2.0);
    REQUIRE_FALSE(beats.empty());
    noFlood(beats, 128.0);
    CHECK(rig.highestBpm() < 130.0);
    CHECK(rig.last().bpm == Approx(128.0).margin(0.05));
}

TEST_CASE("a Start whose first MIDI clock ticks come in a bunch starts at the tempo remembered",
          "[timing][midi][follow]") {
    // A song started at a tempo known from before is followed from its first tick: those first
    // ticks bunched by a 120 ms stall made a line of thousands of BPM at once, and 456 beats.
    Rig rig(128.0);
    rig.send({kStart});
    (void)rig.run(3.0);
    rig.send({kStop});
    (void)rig.run(0.5, false);
    rig.restart();
    rig.send({kStart});
    std::vector<EngineBeat> beats = rig.runStalled(0.12, 5.0);
    beats = joined(beats, rig.run(3.0));
    REQUIRE(beats.size() >= 5);
    noFlood(beats, 128.0);
    CHECK(rig.highestBpm() < 130.0);
}

TEST_CASE("ticks a MIDI clock lost in a gap are counted, and its beats stay on its own",
          "[timing][midi][follow]") {
    // A clock that had a Start, its cable out for two seconds while the mixer ran
    // on: the ticks lost were not counted, so every beat after landed a part of a beat off the
    // music — 0.29 of one in the reader's case — with the row saying "ticking · started".
    for (const double gap : {0.7, 2.0, 5.0}) {
        INFO(gap << " s with the cable out");
        Rig rig(128.0);
        rig.send({kStart});
        (void)rig.run(3.0);
        (void)rig.runLosing(gap);
        const std::vector<EngineBeat> after = rig.run(5.0);
        CHECK(rig.follower().status().placed);
        CHECK(rig.follower().status().ticksLost > 0);
        // Every beat once the clock is followed again on one of the clock's own beats, labelled as
        // the song counts its bars.
        std::size_t checked = 0;
        for (const EngineBeat& beat : after) {
            if (!beat.state.locked || beat.event.beatInBar == 0) {
                continue;
            }
            const double since = static_cast<double>(beat.hostMicros) - rig.songBeatAt(0);
            const auto n = static_cast<std::size_t>(std::llround(since / (60e6 / 128.0)));
            INFO("beat " << n << " of the song");
            CHECK(static_cast<double>(beat.hostMicros) == Approx(rig.songBeatAt(n)).margin(1500.0));
            CHECK(beat.event.beatInBar == static_cast<std::uint32_t>(n % 4 + 1));
            ++checked;
        }
        CHECK(checked >= 8);
    }
}

TEST_CASE("a MIDI clock back after a gap too long to count across leaves its beats for DOWNBEAT",
          "[timing][midi][follow]") {
    // Over sixteen beats a tempo a quarter of a percent out is a tick out, and nothing says the
    // clock held its tempo meanwhile: the clock is unplaced instead, and the window says to press
    // DOWNBEAT.
    Rig rig(128.0);
    rig.send({kStart});
    (void)rig.run(3.0);
    REQUIRE(rig.follower().status().placed);
    (void)rig.runLosing(10.0);
    (void)rig.run(3.0);
    CHECK_FALSE(rig.follower().status().placed);
    CHECK(rig.follower().status().gapsUncounted == 1);
    CHECK(rig.last().present);
    CHECK_FALSE(rig.last().anchored);
}

TEST_CASE("a run that starts on a burst is followed once two beats of ticks have come at their own "
          "pace",
          "[timing][midi][follow]") {
    // A second's stall: 51 ticks in the burst, and two beats of ticks counted before a few on-time
    // ones had come — followed on a line through a handful of ticks. The count starts again after
    // the burst, so the line it is followed on is two beats of ticks that came at their own pace.
    Rig rig(128.0);
    (void)rig.run(3.0);
    (void)rig.runStalled(1.0, 5.0);
    (void)rig.run(0.07);
    CHECK_FALSE(rig.last().present);
    CHECK(rig.follower().status().bunched >= 50);
    (void)rig.run(48.0 * 60.0 / (128.0 * 24.0));
    CHECK(rig.last().present);
}

TEST_CASE("a MIDI clock back from a gap far from the tempo it ran at waits a few beats to be "
          "believed",
          "[timing][midi][follow]") {
    // In a run's first beats a tempo more than 2.5x from the one the clock last ran at is far
    // likelier a stall's ticks than a DJ's new tempo; a few beats more of ticks say which.
    Rig rig(128.0);
    (void)rig.run(3.0);
    (void)rig.run(0.3, false); // gone, and back at 330
    rig.setTempo(330.0);
    (void)rig.run(60.0 / 330.0 * 3.0); // three beats: counted, not believed
    CHECK_FALSE(rig.last().present);
    CHECK(rig.follower().status().refused > 0);
    (void)rig.run(60.0 / 330.0 * 2.0); // past four beats of this run's ticks: believed
    REQUIRE(rig.last().present);
    CHECK(rig.last().bpm == Approx(330.0).margin(0.05));
}

TEST_CASE("a MIDI clock's tempo is remembered only from a line through a beat of its ticks",
          "[timing][midi][follow]") {
    // A song started, at a tempo known from before, and stopped again within a beat: its line is
    // a handful of ticks, which is not a tempo to start the next song at.
    Rig rig(128.0);
    rig.send({kStart});
    (void)rig.run(3.0);
    rig.restart();
    rig.setTempo(100.0);
    rig.send({kStart});
    (void)rig.run(10.0 * 60.0 / (100.0 * 24.0)); // ten ticks
    rig.restart();
    rig.send({kStart});
    (void)rig.run(0.03); // a tick or two: the tempo is the one remembered until six have come
    REQUIRE(rig.last().present);
    CHECK(rig.last().bpm == Approx(128.0).margin(0.01));
}

TEST_CASE("a MIDI clock is followed down to 20 BPM and up to 400, and no further",
          "[timing][midi][follow]") {
    // Under 25 BPM every tick came more than 100 ms after the one before, and began a run of its
    // own: such a clock was never followed.
    for (const double bpm : {20.5, 22.0, 399.0}) {
        INFO(bpm << " BPM");
        Rig rig(bpm);
        (void)rig.run(60.0 / bpm * 2.5); // two beats and a half
        REQUIRE(rig.last().present);
        CHECK(rig.last().bpm == Approx(bpm).margin(0.01));
    }
    for (const double bpm : {15.0, 450.0}) {
        INFO(bpm << " BPM");
        Rig rig(bpm);
        (void)rig.run(60.0 / bpm * 4.0);
        CHECK(rig.highestBpm() == 0.0);
        CHECK(rig.follower().status().refused > 0);
    }
}
