#include "core/timing/grid_source.hpp"
#include "core/timing/link_follower.hpp"
#include "core/timing/link_reader.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using Catch::Approx;
using takt4::timing::GridReading;
using takt4::timing::LinkFollower;
using takt4::timing::LinkListener;
using takt4::timing::LinkReader;

namespace {

/// Link as the follower sees it, on a clock the test moves: a session at `bpm` whose beat 0 is
/// at `zero`, and as many peers in it as the test says.
class FakeLink final : public LinkReader {
public:
    std::chrono::microseconds now() const override { return std::chrono::microseconds{clock}; }
    void enable(bool on) override {
        enabledNow = on;
        switches.push_back(on);
    }
    bool enabled() const override { return enabledNow; }
    std::size_t numPeers() const override { return peers; }
    Timeline timelineAt(std::chrono::microseconds at, double quantum) const override {
        quantumAsked = quantum;
        return Timeline{bpm, (static_cast<double>(at.count()) - zero) * bpm / 60e6};
    }

    std::int64_t clock = 50'000'000;
    bool enabledNow = false;
    std::vector<bool> switches;
    std::size_t peers = 0;
    double bpm = 126.5;
    double zero = 0.0;
    mutable double quantumAsked = 0.0;
};

/// Link's discovery traffic as the follower hears it. Closing forgets every peer, as the real
/// one does.
class FakeListener final : public LinkListener {
public:
    bool open(std::string& problem) override {
        ++opens;
        if (!opens_) {
            problem = "Link's port is taken";
            return false;
        }
        isOpen = true;
        return true;
    }
    void close() noexcept override {
        ++closes;
        isOpen = false;
        peers.clear();
    }
    void poll(double now) override { polledAt = now; }
    std::vector<Heard> heard() const override { return isOpen ? peers : std::vector<Heard>{}; }

    bool opens_ = true;
    bool isOpen = false;
    int opens = 0;
    int closes = 0;
    double polledAt = -1.0;
    std::vector<Heard> peers;
};

constexpr std::int64_t kMillisecond = 1000;

double seconds(const FakeLink& link) {
    return static_cast<double>(link.clock) / 1e6;
}

/// A round of the timing worker every millisecond for `ms`, a peer announcing four times a
/// second for as long as `announcing` — Link's own rate.
std::vector<GridReading> rounds(LinkFollower& follower, FakeLink& link, FakeListener& listener,
                                int ms, bool announcing = true) {
    std::vector<GridReading> readings;
    for (int i = 0; i < ms; ++i) {
        link.clock += kMillisecond;
        if (announcing && listener.isOpen && link.clock % 250'000 == 0) {
            for (LinkListener::Heard& peer : listener.peers) {
                peer.lastHeard = seconds(link);
            }
        }
        readings.push_back(follower.read());
    }
    return readings;
}

} // namespace

TEST_CASE("following Link, it stays off until a session has been heard for a second and still is",
          "[timing][follow][link]") {
    // Link's older session wins when two meet, and switching on founds a new one: switched on
    // alone and left waiting, takt4 would be the older session when the DJ's app arrived, and
    // the DJ's app would take on takt4's tempo. So it listens, and switches on only once a
    // session has been out there long enough to be older than the one switching on makes.
    FakeLink link;
    FakeListener listener;
    LinkFollower follower(link, listener);
    follower.begin(4);
    CHECK(link.switches == std::vector<bool>{false}); // whatever it was left at
    CHECK(listener.isOpen);
    CHECK(follower.status().phase == LinkFollower::Phase::Listening);

    // Nobody about: nothing switched on, however long.
    for (const GridReading& reading : rounds(follower, link, listener, 5000)) {
        CHECK_FALSE(reading.present);
        CHECK(reading.at != 0);
    }
    CHECK_FALSE(link.enabledNow);

    // A peer is heard.
    const double heardAt = seconds(link);
    listener.peers.push_back({heardAt, heardAt, 126.5});
    (void)rounds(follower, link, listener, 900);
    CHECK_FALSE(link.enabledNow);
    CHECK(follower.status().peers == 1);
    CHECK(follower.status().bpm == Approx(126.5));
    (void)rounds(follower, link, listener, 200);
    REQUIRE(link.enabledNow);
    // Within a listen of the second, not before it.
    CHECK(link.switches.size() == 2);
    CHECK(follower.status().phase == LinkFollower::Phase::Joining);
    CHECK_FALSE(listener.isOpen); // not listened to while joined
}

TEST_CASE("following Link, a peer that has stopped announcing is not joined",
          "[timing][follow][link]") {
    // An app quit or a cable pulled goes on being listed until Link's time to live runs out; to
    // join it then is to be alone in a session for as long as the join is waited for.
    FakeLink link;
    FakeListener listener;
    LinkFollower follower(link, listener);
    follower.begin(4);
    const double heardAt = seconds(link);
    listener.peers.push_back({heardAt, heardAt + 0.25, 128.0});
    (void)rounds(follower, link, listener, 3000, false);
    CHECK_FALSE(link.enabledNow);
    CHECK(follower.status().phase == LinkFollower::Phase::Listening);
}

TEST_CASE("following Link, the session joined is read at the bar's quantum, anchored",
          "[timing][follow][link]") {
    const std::uint32_t meter = GENERATE(3u, 4u);
    FakeLink link;
    FakeListener listener;
    LinkFollower follower(link, listener);
    follower.begin(meter);
    listener.peers.push_back({seconds(link), seconds(link), 126.5});
    (void)rounds(follower, link, listener, 1100);
    REQUIRE(follower.status().phase == LinkFollower::Phase::Joining);
    // Joining: not a source yet.
    CHECK_FALSE(follower.read().present);

    link.peers = 2;
    const GridReading reading = follower.read();
    CHECK(follower.status().phase == LinkFollower::Phase::Following);
    CHECK(reading.present);
    CHECK(reading.anchored);
    CHECK(reading.bpm == 126.5);
    CHECK(reading.beat == Approx(static_cast<double>(link.clock) * 126.5 / 60e6));
    CHECK(link.quantumAsked == static_cast<double>(meter));
    CHECK(follower.status().peers == 2);
    CHECK(follower.status().bpm == 126.5);
}

TEST_CASE("following Link, it switches off the moment the last peer leaves and listens afresh",
          "[timing][follow][link]") {
    FakeLink link;
    FakeListener listener;
    LinkFollower follower(link, listener);
    follower.begin(4);
    listener.peers.push_back({seconds(link), seconds(link), 126.5});
    (void)rounds(follower, link, listener, 1100);
    link.peers = 1;
    REQUIRE(follower.read().present);
    const int opens = listener.opens;

    // The DJ's app leaves. Off at once — left on, alone, this session would be the older one when
    // it came back — and listening again from nothing: what was heard before is forgotten, so the
    // app that has gone is not joined again on the strength of it.
    link.peers = 0;
    CHECK_FALSE(follower.read().present);
    CHECK_FALSE(link.enabledNow);
    CHECK(follower.status().phase == LinkFollower::Phase::Listening);
    CHECK(listener.opens == opens + 1);
    CHECK(listener.isOpen);
    CHECK(listener.peers.empty());
    (void)rounds(follower, link, listener, 3000);
    CHECK_FALSE(link.enabledNow);

    // And when it is heard again, joined again after a second.
    listener.peers.push_back({seconds(link), seconds(link), 130.0});
    (void)rounds(follower, link, listener, 1100);
    CHECK(link.enabledNow);
}

TEST_CASE("following Link, a join nobody answers is given up before its session is old",
          "[timing][follow][link]") {
    // A peer that can be heard and not reached. Switched on, takt4 is alone in a session of its
    // own; switched off again within a few seconds, before that session is old enough to win.
    FakeLink link;
    FakeListener listener;
    LinkFollower follower(link, listener);
    follower.begin(4);
    listener.peers.push_back({seconds(link), seconds(link), 126.5});
    while (!link.enabledNow && seconds(link) < 60.0) {
        (void)rounds(follower, link, listener, 1);
    }
    REQUIRE(link.enabledNow);
    const double joined = seconds(link);
    while (link.enabledNow && seconds(link) - joined < 10.0) {
        (void)rounds(follower, link, listener, 1);
    }
    CHECK_FALSE(link.enabledNow);
    CHECK(seconds(link) - joined == Approx(LinkFollower::Options{}.joinFor).margin(0.002));
    CHECK(follower.status().phase == LinkFollower::Phase::Listening);
    CHECK(listener.isOpen);
}

TEST_CASE("following Link, the end of a run switches Link off and stops listening",
          "[timing][follow][link]") {
    FakeLink link;
    FakeListener listener;
    LinkFollower follower(link, listener);
    follower.begin(4);
    listener.peers.push_back({seconds(link), seconds(link), 126.5});
    (void)rounds(follower, link, listener, 1100);
    link.peers = 1;
    REQUIRE(follower.read().present);
    follower.end();
    CHECK_FALSE(link.enabledNow);
    CHECK_FALSE(listener.isOpen);
    CHECK(follower.status().phase == LinkFollower::Phase::Listening);
}

TEST_CASE("following Link with nothing to listen on says why and never switches Link on",
          "[timing][follow][link]") {
    FakeLink link;
    FakeListener listener;
    listener.opens_ = false;
    LinkFollower follower(link, listener);
    follower.begin(4);
    CHECK_FALSE(follower.status().canListen);
    CHECK(follower.problem() == "Link's port is taken");
    (void)rounds(follower, link, listener, 3000);
    CHECK_FALSE(link.enabledNow);
}

TEST_CASE("following Link, a listener that would not open is tried again until it does",
          "[timing][follow][link]") {
    // A follower that could not listen at START never tried again,
    // and stayed deaf for the run.
    FakeLink link;
    FakeListener listener;
    listener.opens_ = false;
    LinkFollower follower(link, listener);
    follower.begin(4);
    REQUIRE(listener.opens == 1);
    (void)rounds(follower, link, listener, 4900);
    CHECK(listener.opens == 1);
    (void)rounds(follower, link, listener, 200);
    CHECK(listener.opens == 2);
    // Free now: the next try opens it, and says so.
    listener.opens_ = true;
    (void)rounds(follower, link, listener, 5100);
    CHECK(listener.opens == 3);
    CHECK(follower.status().canListen);
    CHECK(follower.problem().empty());
    // And a session heard from then on is joined.
    listener.peers.push_back({seconds(link), seconds(link), 126.5});
    (void)rounds(follower, link, listener, 1100);
    CHECK(link.enabledNow);
}

TEST_CASE("following Link, a join nobody answers is tried again later each time, up to half a "
          "minute apart",
          "[timing][follow][link]") {
    // an earlier review. A peer heard and not reached left takt4 alone in a session of its own for
    // three seconds of every four — past half a second alone it is the older session, and a DJ
    // app joining then takes its leftover tempo. Each join that finds nobody waits longer before
    // the next, up to half a minute; one that works starts the waiting from nothing.
    FakeLink link;
    FakeListener listener;
    LinkFollower follower(link, listener);
    follower.begin(4);
    const LinkFollower::Options options;
    // The peer goes on announcing: heard again each time the listener is opened afresh.
    const auto keepHeard = [&] {
        if (listener.isOpen && listener.peers.empty()) {
            listener.peers.push_back({seconds(link), seconds(link), 126.5});
        }
    };
    std::vector<double> waits;
    double offAt = -1.0;
    for (int ms = 0; ms < 200'000 && waits.size() < 7; ++ms) {
        keepHeard();
        const bool was = link.enabledNow;
        (void)rounds(follower, link, listener, 1);
        if (was && !link.enabledNow) {
            offAt = seconds(link);
        } else if (!was && link.enabledNow && offAt >= 0.0) {
            waits.push_back(seconds(link) - offAt);
        }
    }
    REQUIRE(waits.size() == 7);
    // Off, then on again no sooner than the wait — 2, 4, 8, 16, then 30 s and no more — and not
    // much later: the peer heard a second and still announcing is all else it needs.
    const std::vector<double> expected{2.0, 4.0, 8.0, 16.0, 30.0, 30.0, 30.0};
    for (std::size_t i = 0; i < waits.size(); ++i) {
        INFO("wait " << i);
        CHECK(waits[i] >= expected[i]);
        CHECK(waits[i] <= expected[i] + options.listenEvery + 0.002);
    }
    CHECK(follower.status().failedJoins == 7);
    // A join that works starts the waiting from nothing.
    link.peers = 1;
    (void)rounds(follower, link, listener, 1);
    REQUIRE(follower.status().phase == LinkFollower::Phase::Following);
    CHECK(follower.status().failedJoins == 0);
    CHECK(follower.status().retryIn == 0.0);
}
