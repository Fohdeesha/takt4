#include "core/timing/beat_grid.hpp"
#include "core/timing/prolink/virtual_player.hpp"
#include "core/timing/prolink_follower.hpp"

#include "support/prolink_capture.hpp"
#include "support/prolink_sim.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

using takt4::engine::EngineBeat;
using takt4::testing::Captured;
using takt4::testing::loadCapture;
using takt4::timing::BeatGrid;
using takt4::timing::CdjFeed;
using takt4::timing::ProLinkFollower;
using takt4::timing::prolink::VirtualPlayer;

namespace {

constexpr std::int64_t kMs = 1000;

/// The network as the follower reads it, set by hand: a clock the test moves and the beats it
/// says were heard.
class FakeFeed final : public CdjFeed {
public:
    std::int64_t now() const noexcept override { return now_.load(std::memory_order_acquire); }
    bool popBeat(VirtualPlayer::Heard& out) noexcept override {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (beats_.empty()) {
            return false;
        }
        out = beats_.front();
        beats_.pop_front();
        return true;
    }
    void join() noexcept override { ++joins_; }
    void leave() noexcept override { ++leaves_; }

    void setNow(std::int64_t now) { now_.store(now, std::memory_order_release); }
    void push(const VirtualPlayer::Heard& beat) {
        const std::lock_guard<std::mutex> lock(mutex_);
        beats_.push_back(beat);
    }
    int joins() const { return joins_.load(); }
    int leaves() const { return leaves_.load(); }

private:
    std::atomic<std::int64_t> now_{0};
    std::mutex mutex_;
    std::deque<VirtualPlayer::Heard> beats_;
    std::atomic<int> joins_{0};
    std::atomic<int> leaves_{0};
};

VirtualPlayer::Heard heard(std::int64_t at, std::uint8_t device, double bpm,
                           std::uint8_t beatInBar) {
    VirtualPlayer::Heard h;
    h.at = at;
    h.device = device;
    h.bpm = bpm;
    h.beatInBar = beatInBar;
    h.pitch = 0x100000;
    h.nextBeatMs = bpm > 0.0 ? static_cast<std::uint32_t>(std::lround(60000.0 / bpm)) : 0xffffffffu;
    return h;
}

/// The follower read once a millisecond, as the timing worker reads it, into a beat grid.
struct Drive {
    FakeFeed feed;
    ProLinkFollower follower{feed};
    BeatGrid grid;
    std::vector<EngineBeat> sent;
    std::vector<std::int64_t> noSignalAt;

    Drive() {
        grid.reset(BeatGrid::Options{4, 4.0});
        follower.begin(4);
    }
    void to(std::int64_t until) {
        std::int64_t now = feed.now();
        while (now < until) {
            now += kMs;
            feed.setNow(now);
            const bool wasQuiet = grid.state().noSignal;
            if (const std::optional<EngineBeat> beat = grid.advance(follower.read())) {
                sent.push_back(*beat);
            }
            if (grid.state().noSignal && !wasQuiet) {
                noSignalAt.push_back(now);
            }
        }
    }
    /// A beat heard at `at`: time runs to it, and it is there for the next reading.
    void beatAt(std::int64_t at, std::uint8_t device, double bpm, std::uint8_t beatInBar) {
        to(at);
        feed.push(heard(at, device, bpm, beatInBar));
    }
};

std::uint8_t barPosition(int i, int offset = 0) {
    return static_cast<std::uint8_t>((i + offset) % 4 + 1);
}

/// The sawtooth a CDJ-2000nexus's beat packets were measured to sit on: up to 1.7 ms either side
/// of the line, stepping back every sixteen beats.
std::int64_t sawtooth(int i) {
    return static_cast<std::int64_t>(std::lround((static_cast<double>(i % 16) - 7.5) * 200.0));
}

} // namespace

TEST_CASE("following the CDJs, every beat a player sends is sent once, on its moment, with the "
          "bar its player gave it",
          "[prolink][follow]") {
    Drive d;
    CHECK(d.feed.joins() == 1);
    const double bpm = 128.0;
    const auto period = static_cast<std::int64_t>(std::llround(60e6 / bpm));
    std::vector<std::int64_t> arrivals;
    for (int i = 0; i < 64; ++i) {
        const std::int64_t at = 1'000'000 + i * period + sawtooth(i);
        arrivals.push_back(at);
        d.beatAt(at, 2, bpm, barPosition(i, 2));
    }
    d.to(arrivals.back() + 200 * kMs);
    REQUIRE(d.sent.size() == arrivals.size());
    for (std::size_t i = 0; i < arrivals.size(); ++i) {
        INFO("beat " << i);
        // On its packet, early or late on the sawtooth: a beat is waited for past its
        // prediction, so the packet is what sends it.
        CHECK(std::abs(d.sent[i].hostMicros - arrivals[i]) <= 2);
        CHECK(d.sent[i].event.beatInBar == barPosition(static_cast<int>(i), 2));
        CHECK(d.sent[i].event.downbeat == (barPosition(static_cast<int>(i), 2) == 1));
        CHECK(d.sent[i].event.bpm == bpm);
    }
    CHECK(d.follower.status().placements == 1); // the first beat, and nothing since
    CHECK(d.follower.status().device == 2);
    CHECK(d.grid.state().locked);
    CHECK(d.grid.state().beatsPerBar == 4);
}

TEST_CASE("the real CDJ-2000nexus capture, followed: a beat on every beat it played, the bar it "
          "counted, hot cues that restart the bar, nothing for cue presses, and the rig let go "
          "after each pause",
          "[prolink][follow]") {
    const std::vector<Captured> capture = loadCapture("load-and-play.txt");
    VirtualPlayer player;
    player.listen(0);
    Drive d;
    std::vector<std::pair<std::int64_t, std::uint8_t>> packets; // the beats heard, and their bar
    for (const Captured& c : capture) {
        d.to(c.micros());
        player.receive(c.channel(), c.bytes, c.from, c.micros());
        player.advance(c.micros());
        for (const VirtualPlayer::Heard& h : player.heard()) {
            d.feed.push(h);
            packets.emplace_back(h.at, h.beatInBar);
        }
        player.heard().clear();
    }
    d.to(capture.back().micros() + 6'000 * kMs);
    REQUIRE(packets.size() == 89);

    // Every beat the deck played has one beat sent for it, on its packet, with its own place in
    // the bar — the hot cues' restarted bars too.
    std::size_t matched = 0;
    for (const auto& [at, bar] : packets) {
        INFO("the beat heard at " << at / 1000 << " ms, " << int(bar) << " of 4");
        std::size_t onIt = 0;
        for (const EngineBeat& beat : d.sent) {
            if (std::abs(beat.hostMicros - at) <= 2) {
                ++onIt;
                CHECK(beat.event.beatInBar == bar);
                CHECK(beat.event.downbeat == (bar == 1));
            }
        }
        CHECK(onIt == 1);
        matched += onIt;
    }
    CHECK(matched == packets.size());
    // Between two beats heard less than a beat and a half apart — the deck playing — nothing
    // else is sent: no beat twice, none made up.
    for (std::size_t i = 1; i < packets.size(); ++i) {
        const std::int64_t from = packets[i - 1].first + 5 * kMs;
        const std::int64_t to = packets[i].first - 5 * kMs;
        if (packets[i].first - packets[i - 1].first > 700 * kMs) {
            continue;
        }
        for (const EngineBeat& beat : d.sent) {
            CHECK_FALSE((beat.hostMicros > from && beat.hostMicros < to));
        }
    }
    // Two long pauses: each coasted four seconds and then let go (noSignal), as silence is.
    CHECK(d.noSignalAt.size() == 3); // the two pauses, and the end of the capture
}

TEST_CASE("a beat packet lost on the way is sent all the same, on the tempo, and the next one "
          "carries on the grid",
          "[prolink][follow]") {
    Drive d;
    const auto period = static_cast<std::int64_t>(std::llround(60e6 / 125.0));
    for (int i = 0; i < 16; ++i) {
        if (i == 9) {
            continue; // lost
        }
        d.beatAt(1'000'000 + i * period, 1, 125.0, barPosition(i));
    }
    d.to(1'000'000 + 16 * period - 10 * kMs);
    REQUIRE(d.sent.size() == 16);
    // Stamped with its own moment, though it went out once its packet had been waited for.
    CHECK(std::abs(d.sent[9].hostMicros - (1'000'000 + 9 * period)) <= 2);
    CHECK(d.sent[9].event.beatInBar == barPosition(9));
    CHECK(d.follower.status().placements == 1);
}

TEST_CASE("the same beat heard twice, as a machine with two network cards on the DJ's network "
          "hears every broadcast, is one beat",
          "[prolink][follow]") {
    Drive d;
    const auto period = static_cast<std::int64_t>(std::llround(60e6 / 128.0));
    for (int i = 0; i < 12; ++i) {
        const std::int64_t at = 1'000'000 + i * period;
        d.beatAt(at, 1, 128.0, barPosition(i));
        d.feed.push(heard(at + 300, 1, 128.0, barPosition(i)));
    }
    d.to(1'000'000 + 12 * period - 10 * kMs);
    CHECK(d.sent.size() == 12);
    CHECK(d.follower.status().repeats == 12);
    CHECK(d.follower.status().placements == 1);
}

TEST_CASE("a hot cue off the beat moves the grid to the DJ's, and the beat it lands on is sent "
          "on its moment as the bar's first",
          "[prolink][follow]") {
    Drive d;
    const auto period = static_cast<std::int64_t>(std::llround(60e6 / 120.0));
    std::int64_t at = 1'000'000;
    for (int i = 0; i < 6; ++i, at += period) {
        d.beatAt(at, 1, 120.0, barPosition(i)); // ... 1 2 3 4 1 2
    }
    // Half a beat after the last, the DJ jumps to a hot cue on a downbeat.
    const std::int64_t cue = at - period / 2;
    d.beatAt(cue, 1, 120.0, 1);
    for (int i = 1; i < 8; ++i) {
        d.beatAt(cue + i * period, 1, 120.0, barPosition(i));
    }
    d.to(cue + 8 * period - 10 * kMs);
    CHECK(d.follower.status().placements == 2);
    const auto onCue = std::find_if(d.sent.begin(), d.sent.end(),
                                    [&](const EngineBeat& b) { return b.hostMicros == cue; });
    REQUIRE(onCue != d.sent.end());
    CHECK(onCue->event.downbeat);
    CHECK(onCue->event.snapped);
    // From the cue on, a beat a period apart, each on its packet, counting the DJ's bar.
    const auto index = static_cast<std::size_t>(onCue - d.sent.begin());
    REQUIRE(d.sent.size() == index + 8);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(d.sent[index + i].hostMicros == cue + static_cast<std::int64_t>(i) * period);
        CHECK(d.sent[index + i].event.beatInBar == barPosition(static_cast<int>(i)));
    }
}

TEST_CASE("a quantised hot cue keeps the time and restarts the bar, and no beat is lost to it",
          "[prolink][follow]") {
    // As the capture shows: Bb 2 3 1 2 3 1 2 3 4, the beats evenly spaced.
    Drive d;
    const auto period = static_cast<std::int64_t>(std::llround(60e6 / 127.984));
    const std::vector<std::uint8_t> bars{2, 3, 1, 2, 3, 1, 2, 3, 4, 1, 2};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        d.beatAt(1'000'000 + static_cast<std::int64_t>(i) * period, 1, 127.984, bars[i]);
    }
    d.to(1'000'000 + static_cast<std::int64_t>(bars.size()) * period - 10 * kMs);
    REQUIRE(d.sent.size() == bars.size());
    for (std::size_t i = 0; i < bars.size(); ++i) {
        CHECK(d.sent[i].event.beatInBar == bars[i]);
        CHECK(d.sent[i].event.downbeat == (bars[i] == 1));
        CHECK(std::abs(d.sent[i].hostMicros -
                       (1'000'000 + static_cast<std::int64_t>(i) * period)) <= 2);
    }
    CHECK(d.follower.status().placements == 3);
}

TEST_CASE("the master handed to another player: its beats are followed from its first, with "
          "its own bar, and no beat is sent twice",
          "[prolink][follow]") {
    // Synced, the new master's beats fall on the old one's, a little off; not synced, anywhere.
    for (const double offBeats : {0.004, 0.5}) {
        INFO("the new master's beats " << offBeats << " of a beat after the old one's");
        Drive d;
        const auto period = static_cast<std::int64_t>(std::llround(60e6 / 126.0));
        std::int64_t at = 1'000'000;
        for (int i = 0; i < 8; ++i, at += period) {
            d.beatAt(at, 1, 126.0, barPosition(i));
        }
        // Player 3 takes over on the beat after player 1's last, its bar two beats round.
        const std::int64_t first =
            at + static_cast<std::int64_t>(std::llround(offBeats * static_cast<double>(period)));
        for (int i = 0; i < 8; ++i) {
            d.beatAt(first + i * period, 3, 126.0, barPosition(i, 2));
        }
        d.to(first + 8 * period - 10 * kMs);
        CHECK(d.follower.status().device == 3);
        CHECK(d.follower.status().placements == 2);
        // Not synced, player 1's next beat was waited for and not heard, and went out on the
        // tempo's prediction before player 3's first came half a beat on.
        const std::size_t predicted = offBeats > 0.1 ? 1 : 0;
        REQUIRE(d.sent.size() == 16 + predicted);
        for (std::size_t i = 0; i < 8; ++i) {
            CHECK(d.sent[i].hostMicros == 1'000'000 + static_cast<std::int64_t>(i) * period);
            CHECK(d.sent[i].event.beatInBar == barPosition(static_cast<int>(i)));
            const EngineBeat& handed = d.sent[8 + predicted + i];
            CHECK(std::abs(handed.hostMicros - (first + static_cast<std::int64_t>(i) * period)) <=
                  2);
            CHECK(handed.event.beatInBar == barPosition(static_cast<int>(i), 2));
        }
        if (predicted == 1) {
            CHECK(std::abs(d.sent[8].hostMicros - at) <= 2);
            CHECK(d.sent[8].event.beatInBar == 1);
        }
        CHECK(d.sent[8 + predicted].event.snapped);
        for (std::size_t i = 1; i < d.sent.size(); ++i) {
            CHECK(static_cast<double>(d.sent[i].hostMicros - d.sent[i - 1].hostMicros) >=
                  0.4 * static_cast<double>(period));
        }
    }
}

TEST_CASE("halved, the CDJs' beats keep their downbeats; doubled, a beat is added half way",
          "[prolink][follow]") {
    for (const bool halve : {true, false}) {
        INFO((halve ? "halved" : "doubled"));
        Drive d;
        if (halve) {
            d.grid.halve();
        } else {
            d.grid.redouble();
        }
        const auto period = static_cast<std::int64_t>(std::llround(60e6 / 128.0));
        for (int i = 0; i < 16; ++i) {
            d.beatAt(1'000'000 + i * period, 1, 128.0, barPosition(i));
        }
        d.to(1'000'000 + 16 * period - 10 * kMs);
        if (halve) {
            REQUIRE(d.sent.size() == 8);
            for (std::size_t i = 0; i < d.sent.size(); ++i) {
                CHECK(d.sent[i].hostMicros ==
                      1'000'000 + static_cast<std::int64_t>(2 * i) * period);
                // The bar is counted on the halved grid, four of its beats: every second source
                // bar's downbeat, as the tracker divides one.
                CHECK(d.sent[i].event.downbeat == (i % 4 == 0));
                CHECK(d.sent[i].event.bpm == 64.0);
            }
        } else {
            REQUIRE(d.sent.size() == 32);
            for (std::size_t i = 0; i < d.sent.size(); ++i) {
                const double expected =
                    1e6 + static_cast<double>(i) * static_cast<double>(period) / 2.0;
                CHECK(std::abs(static_cast<double>(d.sent[i].hostMicros) - expected) <= 1000.0);
            }
        }
    }
}

TEST_CASE("beats that stop are coasted, then let go of after four seconds; when they come back "
          "the lock is had again and the first is sent",
          "[prolink][follow]") {
    Drive d;
    const auto period = static_cast<std::int64_t>(std::llround(60e6 / 128.0));
    std::int64_t at = 1'000'000;
    for (int i = 0; i < 8; ++i, at += period) {
        d.beatAt(at, 1, 128.0, barPosition(i));
    }
    const std::int64_t last = at - period;
    d.to(last + period + 50 * kMs);
    CHECK(d.grid.state().locked);
    CHECK_FALSE(d.grid.coasting());
    d.to(last + 2 * period + 150 * kMs);
    CHECK(d.grid.coasting()); // gone, and run on
    CHECK(d.grid.state().locked);
    const std::size_t beforeQuiet = d.sent.size();
    d.to(last + 2 * period + 100 * kMs + 4'000 * kMs + 10 * kMs);
    CHECK(d.grid.state().noSignal);
    CHECK_FALSE(d.grid.state().locked);
    CHECK(d.sent.size() > beforeQuiet); // coasted beats went out
    const std::size_t quiet = d.sent.size();
    d.to(d.feed.now() + 3'000 * kMs);
    CHECK(d.sent.size() == quiet); // and none after
    // Back, at another phase: locked again from its first beat, which is sent.
    const std::int64_t back = d.feed.now() + 123 * kMs;
    d.beatAt(back, 1, 128.0, 3);
    d.to(back + 5 * kMs);
    CHECK(d.grid.state().locked);
    CHECK(d.grid.state().acquired);
    REQUIRE(d.sent.size() == quiet + 1);
    CHECK(d.sent.back().hostMicros == back);
    CHECK(d.sent.back().event.beatInBar == 3);
}

TEST_CASE("a beat with no tempo to run on is not a beat to begin from", "[prolink][follow]") {
    Drive d;
    VirtualPlayer::Heard none = heard(1'000'000, 1, 0.0, 1);
    none.nextBeatMs = 0xffffffff;
    d.to(1'000'000);
    d.feed.push(none);
    d.to(1'500'000);
    CHECK(d.sent.empty());
    CHECK_FALSE(d.grid.state().locked);
    // A tempo from the time to the next beat and the pitch, where the BPM field has none.
    VirtualPlayer::Heard fromNext = heard(1'600'000, 1, 0.0, 1);
    fromNext.nextBeatMs = 500;
    fromNext.pitch = 0x100000;
    d.to(1'600'000);
    d.feed.push(fromNext);
    d.to(1'700'000);
    REQUIRE(d.sent.size() == 1);
    CHECK(d.sent[0].event.bpm == Catch::Approx(120.0));
}

TEST_CASE("the DJ's pitch fader moving the tempo moves the beats with it", "[prolink][follow]") {
    Drive d;
    std::int64_t at = 1'000'000;
    double bpm = 124.0;
    std::vector<std::int64_t> arrivals;
    for (int i = 0; i < 24; ++i) {
        d.beatAt(at, 1, bpm, barPosition(i));
        arrivals.push_back(at);
        at += static_cast<std::int64_t>(std::llround(60e6 / bpm));
        bpm += 0.25; // a slow push of the fader
    }
    d.to(at - 10 * kMs);
    REQUIRE(d.sent.size() == arrivals.size());
    for (std::size_t i = 0; i < arrivals.size(); ++i) {
        CHECK(std::abs(d.sent[i].hostMicros - arrivals[i]) <= kMs);
    }
    CHECK(d.follower.status().placements == 1);
}

TEST_CASE("starting and stopping the follower joins and leaves the network, and a beat heard "
          "before the start is not this run's",
          "[prolink][follow]") {
    FakeFeed feed;
    ProLinkFollower follower(feed);
    feed.setNow(500'000);
    feed.push(heard(400'000, 1, 128.0, 1));
    follower.begin(4);
    CHECK(feed.joins() == 1);
    feed.setNow(600'000);
    const auto reading = follower.read();
    CHECK_FALSE(reading.present);
    follower.end();
    CHECK(feed.leaves() == 1);
    CHECK_FALSE(follower.status().present);
}

TEST_CASE("beats back on the very grid they left, after the rig was let go of, still send their "
          "first",
          "[prolink][follow]") {
    // A deck paused on a beat and played again whole beats later: its first beat lands where the
    // old grid puts one. The grid had gone quiet, so that beat begins a run afresh — it is sent,
    // not taken for one the grid already counted.
    Drive d;
    const auto period = static_cast<std::int64_t>(std::llround(60e6 / 125.0));
    for (int i = 0; i < 8; ++i) {
        d.beatAt(1'000'000 + i * period, 1, 125.0, barPosition(i));
    }
    // Twenty beats on — nine and a half seconds, well past the coast — exactly on the grid.
    const std::int64_t back = 1'000'000 + 28 * period;
    d.to(back - 10 * kMs);
    REQUIRE(d.grid.state().noSignal);
    const std::size_t before = d.sent.size();
    for (int i = 28; i < 32; ++i) {
        d.beatAt(1'000'000 + i * period, 1, 125.0, barPosition(i));
    }
    d.to(1'000'000 + 32 * period - 10 * kMs);
    REQUIRE(d.sent.size() == before + 4);
    CHECK(d.sent[before].hostMicros == back);
    CHECK(d.sent[before].event.beatInBar == barPosition(28));
    CHECK(d.grid.state().locked);
}
