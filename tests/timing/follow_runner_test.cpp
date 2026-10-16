#include "core/output/midi_clock.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/output_target.hpp"
#include "core/output/transports.hpp"
#include "core/timing/beat_grid.hpp"
#include "core/timing/external_beats.hpp"
#include "core/timing/prolink_follower.hpp"
#include "core/trigger/rule.hpp"

#include "support/fake_grid_source.hpp"
#include "support/link_network.hpp"
#include "support/loopback_receiver.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Catch::Approx;
using takt4::output::OutputCommand;
using takt4::output::OutputRunner;
using takt4::output::OutputTarget;
using takt4::output::Transports;
using takt4::testing::FakeGridSource;
using takt4::testing::LoopbackReceiver;
using takt4::timing::BeatGrid;
using takt4::timing::ExternalBeats;
using takt4::trigger::Rule;

// The output thread fed by a followed grid rather than by the tracker: the whole chain a beat
// from the house DJ's Link session or a mixer's MIDI clock takes, from the timing worker's ring to
// the wire. The grid here is a stand-in on Link's own clock, so what is under test is everything
// downstream of where a real source plugs in.

namespace {

/// An OSC int argument's value, from a datagram that carries exactly one.
int lastInt(const std::string& datagram) {
    if (datagram.size() < 4) {
        return -1;
    }
    const auto byte = [&datagram](std::size_t at) {
        return static_cast<int>(static_cast<std::uint8_t>(datagram[at]));
    };
    const std::size_t at = datagram.size() - 4;
    return (byte(at) << 24) | (byte(at + 1) << 16) | (byte(at + 2) << 8) | byte(at + 3);
}

using Bytes = std::vector<std::vector<unsigned char>>;

/// A MIDI port that keeps every message it is sent, whole, and when on `clock`.
class KeepingPort final : public takt4::output::MidiPort {
public:
    KeepingPort(std::shared_ptr<Bytes> sent, std::shared_ptr<std::mutex> mutex)
        : sent_(std::move(sent)), mutex_(std::move(mutex)) {}
    std::string open(std::string_view spec) override { return std::string(spec); }
    void close() noexcept override {}
    void send(std::span<const unsigned char> message) override {
        const std::lock_guard<std::mutex> lock(*mutex_);
        sent_->emplace_back(message.begin(), message.end());
    }

private:
    std::shared_ptr<Bytes> sent_;
    std::shared_ptr<std::mutex> mutex_;
};

/// Stops the timing worker when it goes out of scope. Declared after the source the worker
/// reads, so that however a test ends — a failed REQUIRE included — the worker has stopped
/// before the source it reads is destroyed. (The source cannot simply come first: its clock is
/// the runner's, and the runner is built on the worker's beats.)
struct StopsWhenDone {
    ExternalBeats& beats;
    ~StopsWhenDone() { beats.stop(); }
};

/// A rule on every beat sending its place in the bar to `/cue`.
Rule::Config cueOnEveryBeat() {
    Rule::Config rule;
    rule.id = "position";
    rule.trigger = takt4::trigger::Trigger::Beat;
    rule.address = "/cue";
    rule.value = takt4::trigger::Generator::Config{};
    rule.value.kind = takt4::trigger::GeneratorKind::Live;
    rule.value.source = takt4::trigger::LiveSource::BeatInBar;
    return rule;
}

} // namespace

TEST_CASE("following a grid, every beat's cue lands on that beat, early or late",
          "[output][timing][follow][slow]") {
    // A rule on every beat, its media server set 200 ms early or 200 ms late. The tracker's beats
    // are heard a pipeline after their moment and jitter by milliseconds; a followed grid's are
    // pushed as their moment passes and are exact. So every beat's own cue — its own place in the
    // bar — has to land on it, offset by the delay, to within the loop's rounds, and only once.
    const double delay = GENERATE(-0.200, 0.200);
    INFO("the media server " << delay * 1000.0 << " ms");
    LoopbackReceiver media;
    Transports::Config config;
    OutputTarget server;
    server.name = "media";
    server.host = "127.0.0.1";
    server.port = media.port();
    server.delaySeconds = delay;
    config.outputs = {server};

    ExternalBeats beats;
    OutputRunner runner(beats, config);
    runner.post(OutputCommand::rules({cueOnEveryBeat()}));
    REQUIRE(runner.triggers().rule(0).valid());
    const auto linkNow = [&runner] { return runner.transports().link().now().count(); };
    constexpr double kBpm = 128.0;
    const double period = 60.0 / kBpm;
    FakeGridSource source(linkNow, kBpm, static_cast<double>(linkNow()) + 300'000.0);
    const StopsWhenDone stops{beats};

    struct Heard {
        double moment = 0.0;
        std::uint32_t beatInBar = 0;
    };
    std::mutex mutex;
    std::vector<Heard> heard;
    runner.setBeatObserver([&](const takt4::engine::EngineBeat& beat) {
        const double moment =
            runner.elapsed() - static_cast<double>(linkNow() - beat.hostMicros) / 1e6;
        const std::lock_guard<std::mutex> lock(mutex);
        heard.push_back({moment, beat.event.beatInBar});
    });
    struct Cue {
        double at = 0.0;
        int value = 0;
    };
    std::vector<Cue> cues;
    std::atomic<bool> listening{true};
    std::thread listener([&] {
        while (listening.load(std::memory_order_acquire)) {
            const std::string datagram = media.receive();
            if (datagram.rfind("/cue", 0) == 0) {
                const double at = runner.elapsed();
                const std::lock_guard<std::mutex> lock(mutex);
                cues.push_back({at, lastInt(datagram)});
            }
        }
    });

    runner.start();
    beats.start(source, BeatGrid::Options{});
    std::this_thread::sleep_for(std::chrono::seconds{5});
    beats.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
    runner.stop();
    listening.store(false, std::memory_order_release);
    listener.join();

    REQUIRE(heard.size() >= 9);
    std::size_t onTheirBeat = 0;
    for (std::size_t i = 0; i < heard.size(); ++i) {
        INFO("beat " << i << " at " << heard[i].moment << ", " << heard[i].beatInBar << " of 4");
        // The first beat early has nothing before it to be predicted from, and goes as heard.
        if (delay < 0.0 && i == 0) {
            continue;
        }
        std::size_t nearby = 0;
        std::size_t own = 0;
        double nearest = 0.0;
        int nearestValue = 0;
        for (const Cue& cue : cues) {
            const double off = cue.at - heard[i].moment - delay;
            if (std::abs(off) < 0.45 * period) {
                ++nearby;
                nearest = off;
                nearestValue = cue.value;
                if (cue.value == static_cast<int>(heard[i].beatInBar) && std::abs(off) < 0.010) {
                    ++own;
                }
            }
        }
        // What a miss was by: the cue's place in the bar, and how far off its beat it landed.
        INFO("its cue said " << nearestValue << ", " << nearest * 1000.0 << " ms off");
        CHECK(nearby == 1); // one cue for each beat, and only one
        CHECK(own == 1);    // and it is this beat's, on time
        onTheirBeat += own;
    }
    INFO(onTheirBeat << " of " << heard.size() << " beats with their own cue on time; "
                     << runner.scheduler().heardFires() << " fired as heard, "
                     << runner.scheduler().predictedFires() << " predicted, "
                     << runner.scheduler().matched() << " matched");
    CHECK(runner.errors() == 0);
    CHECK(runner.scheduler().stale() == 0);
    if (delay < 0.0) {
        CHECK(runner.scheduler().predictedFires() + 1 >= heard.size());
    } else {
        CHECK(runner.scheduler().heardFires() == heard.size());
    }
}

TEST_CASE("following a grid, the rig is let go of when the source has been gone four seconds",
          "[output][timing][follow][release][slow]") {
    // The operator's call of 2026-10-15: a source that goes is coasted, then treated exactly as
    // silence — and silence lets go of every note a rule holds on, so a Liberation clip driven by
    // MIDI is not left playing to a rig nobody is following.
    auto cable = std::make_shared<Bytes>();
    auto cableMutex = std::make_shared<std::mutex>();
    Transports::Config config;
    OutputTarget desk;
    desk.id = "o-0000desk";
    desk.name = "laser desk";
    desk.kind = OutputTarget::Kind::Midi;
    desk.device = "Laser";
    config.outputs = {desk};
    config.openMidi = [cable, cableMutex](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(
            name, std::make_unique<KeepingPort>(cable, cableMutex));
    };
    ExternalBeats beats;
    OutputRunner runner(beats, config);
    Rule::Config note;
    note.id = "note";
    note.trigger = takt4::trigger::Trigger::Manual;
    note.sendKind = takt4::trigger::Message::Kind::MidiNote;
    note.channel = 1;
    note.number.kind = takt4::trigger::GeneratorKind::Fixed;
    note.number.fixed = takt4::trigger::Value::ofInt(60);
    note.value.kind = takt4::trigger::GeneratorKind::Fixed;
    note.value.fixed = takt4::trigger::Value::ofInt(100);
    runner.post(OutputCommand::rules({note}));
    runner.post(OutputCommand::testRule("note"));
    REQUIRE(runner.holds().notes == 1);

    const auto linkNow = [&runner] { return runner.transports().link().now().count(); };
    FakeGridSource source(linkNow, 120.0, static_cast<double>(linkNow()));
    const StopsWhenDone stops{beats};
    runner.start();
    beats.start(source, BeatGrid::Options{});
    const auto until = [](double seconds) {
        return std::chrono::steady_clock::now() +
               std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                   std::chrono::duration<double>(seconds));
    };
    const auto lockBy = until(2.0);
    while (!beats.state().locked && std::chrono::steady_clock::now() < lockBy) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    REQUIRE(beats.state().locked);

    source.setPresent(false);
    const auto lost = std::chrono::steady_clock::now();
    // Still held while it coasts.
    std::this_thread::sleep_for(std::chrono::seconds{2});
    CHECK(runner.holds().notes == 1);
    const auto quietBy = until(4.0);
    while (!beats.state().noSignal && std::chrono::steady_clock::now() < quietBy) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    REQUIRE(beats.state().noSignal);
    const double after =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - lost).count();
    CHECK(after == Approx(4.0).margin(0.1));
    REQUIRE(runner.sync());
    REQUIRE(runner.sync()); // a whole round after the state the edge is read from
    CHECK(runner.holds().notes == 0);
    beats.stop();
    runner.stop();
    const std::lock_guard<std::mutex> lock(*cableMutex);
    CHECK(*cable == Bytes{{0x90, 60, 100}, {0x80, 60, 0}});
}

TEST_CASE("following a grid, a MIDI clock output is ticked from it and started on its downbeat",
          "[output][timing][follow][midi][slow]") {
    // The bridge the operator asked for: a drum machine or a lighting desk on MIDI clock hears the
    // followed grid's tempo, 24 ticks to its beat, and is started on its downbeat — the clock is
    // the same one the tracker drives, fed by beats that are exact.
    auto cable = std::make_shared<Bytes>();
    auto cableMutex = std::make_shared<std::mutex>();
    Transports::Config config;
    OutputTarget clock;
    clock.id = "o-00c10c00";
    clock.name = "drum machine";
    clock.kind = OutputTarget::Kind::MidiClock;
    clock.device = "Drums";
    config.outputs = {clock};
    config.openMidi = [cable, cableMutex](const std::string& name) {
        return std::make_unique<takt4::output::MidiOutput>(
            name, std::make_unique<KeepingPort>(cable, cableMutex));
    };
    ExternalBeats beats;
    OutputRunner runner(beats, config);
    const auto linkNow = [&runner] { return runner.transports().link().now().count(); };
    constexpr double kBpm = 150.0;
    FakeGridSource source(linkNow, kBpm, static_cast<double>(linkNow()) + 200'000.0);
    const StopsWhenDone stops{beats};
    runner.start();
    runner.setTracking(true);
    beats.start(source, BeatGrid::Options{});
    std::this_thread::sleep_for(std::chrono::seconds{4});
    const std::uint64_t sent = beats.beatsSent();
    beats.stop();
    runner.setTracking(false);
    REQUIRE(runner.sync());
    runner.stop();

    const std::lock_guard<std::mutex> lock(*cableMutex);
    std::size_t ticks = 0;
    std::size_t ticksAfterStart = 0;
    bool started = false;
    for (const auto& message : *cable) {
        if (message.size() == 1 && message[0] == takt4::output::MidiClock::kStart) {
            started = true;
        }
        if (message.size() == 1 && message[0] == takt4::output::MidiClock::kTick) {
            ++ticks;
            ticksAfterStart += started ? 1 : 0;
        }
    }
    INFO(sent << " beats, " << ticks << " ticks, " << ticksAfterStart << " after Start");
    CHECK(started);
    // Four seconds of 150 BPM is ten beats, 240 ticks, ticked from the press.
    CHECK(ticks == Approx(240.0).margin(30.0));
    // Started on a downbeat: a whole number of bars before the end, give or take a part of one.
    CHECK(ticksAfterStart > 96);
}

TEST_CASE("following Link, the outputs commit nothing to the session and keep its switch",
          "[output][timing][follow][link]") {
    // A follower cannot also lead: while the beats are Link's own, the Link output sends
    // nothing back, whatever its switch says — and the switch is kept for when following stops.
    // The session is handed over switched off, so this test joins nothing.
    Transports::Config config;
    config.link = true;
    ExternalBeats beats;
    OutputRunner runner(beats, config);
    runner.post(OutputCommand::linkFollowing(true));
    const auto linkNow = [&runner] { return runner.transports().link().now().count(); };
    FakeGridSource source(linkNow, 126.5, static_cast<double>(linkNow()));
    const StopsWhenDone stops{beats};
    runner.start();
    beats.start(source, BeatGrid::Options{});
    std::this_thread::sleep_for(std::chrono::milliseconds{2500});
    const std::uint64_t sent = beats.beatsSent();
    beats.stop();
    runner.stop();
    CHECK(sent >= 4);
    CHECK(runner.transports().beats() >= 4); // the beats went everywhere else
    CHECK(runner.transports().link().tempoUpdates() == 0);
    CHECK(runner.transports().link().beatRequests() == 0);
    CHECK(runner.transports().linkEnabled()); // the switch, kept
    CHECK(runner.transports().linkFollowing());
    const OutputRunner::Snapshot snapshot = runner.snapshot();
    CHECK(snapshot.link);
    CHECK(snapshot.linkFollowing);
}

TEST_CASE("following a grid that is not Link, the Link output leads at its tempo",
          "[output][timing][follow][link][network]") {
    // The other half of the bridge: following a MIDI clock or a CDJ, Link is led as it is from
    // the tracker — snapped under the first locked beat and sent the followed tempo. And handed
    // to a follower switched off, and back.
    //
    // Never beside another session: leading would move its tempo.
    std::string problem;
    const bool other = takt4::testing::anyLinkHeard(2.0, problem);
    if (!problem.empty()) {
        SKIP("cannot listen for Link sessions on this network: " + problem);
    }
    if (other) {
        SKIP("a Link session is already on this network, and leading would move its tempo");
    }
    Transports::Config config;
    config.link = true;
    ExternalBeats beats;
    OutputRunner runner(beats, config);
    const auto linkNow = [&runner] { return runner.transports().link().now().count(); };
    FakeGridSource source(linkNow, 131.0, static_cast<double>(linkNow()) + 400'000.0);
    const StopsWhenDone stops{beats};
    runner.start();
    REQUIRE(runner.transports().link().enabled());
    std::this_thread::sleep_for(std::chrono::milliseconds{300}); // Link's own first reset
    beats.start(source, BeatGrid::Options{});
    std::this_thread::sleep_for(std::chrono::milliseconds{2500});
    beats.stop();
    REQUIRE(runner.sync());
    CHECK(runner.transports().link().beatRequests() >= 1); // the snap
    CHECK(runner.transports().link().tempoBpm() == Approx(131.0).margin(0.01));

    runner.post(OutputCommand::linkFollowing(true));
    REQUIRE(runner.sync());
    CHECK_FALSE(runner.transports().link().enabled());
    runner.post(OutputCommand::linkFollowing(false));
    REQUIRE(runner.sync());
    CHECK(runner.transports().link().enabled());
    runner.stop();
}

namespace {

/// The CDJs as the follower reads them, played by a test thread: each beat pushed when it comes,
/// stamped on Link's clock.
class PushedFeed final : public takt4::timing::CdjFeed {
public:
    explicit PushedFeed(std::function<std::int64_t()> clock) : clock_(std::move(clock)) {}
    std::int64_t now() const noexcept override { return clock_(); }
    bool popBeat(takt4::timing::prolink::VirtualPlayer::Heard& out) noexcept override {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (beats_.empty()) {
            return false;
        }
        out = beats_.front();
        beats_.pop_front();
        return true;
    }
    void join() noexcept override {}
    void leave() noexcept override {}
    /// A beat of player 1 at `bpm`, `beatInBar`, heard now.
    void push(double bpm, std::uint8_t beatInBar) {
        takt4::timing::prolink::VirtualPlayer::Heard beat;
        beat.at = clock_();
        beat.device = 1;
        beat.bpm = bpm;
        beat.beatInBar = beatInBar;
        beat.pitch = 0x100000;
        const std::lock_guard<std::mutex> lock(mutex_);
        beats_.push_back(beat);
    }

private:
    std::function<std::int64_t()> clock_;
    std::mutex mutex_;
    std::deque<takt4::timing::prolink::VirtualPlayer::Heard> beats_;
};

} // namespace

TEST_CASE("following the CDJs, a hot cue's downbeat that comes a moment late goes out as the "
          "bar's first, not as the beat the grid expected",
          "[output][timing][follow][prolink][slow]") {
    // A CDJ sends its beats on its own 3 ms step, so a beat can come a moment after the tempo
    // puts it. The tracker's beats come a pipeline late and are fired on a prediction; a followed
    // grid's come on time, and a prediction at no lead would race them — and win with the bar
    // the grid expected. A quantised hot cue keeps the time and restarts the bar: its downbeat,
    // two milliseconds late, has to reach the wire as 1.
    LoopbackReceiver media;
    Transports::Config config;
    OutputTarget server;
    server.name = "media";
    server.host = "127.0.0.1";
    server.port = media.port();
    config.outputs = {server};
    ExternalBeats beats;
    OutputRunner runner(beats, config);
    runner.post(OutputCommand::rules({cueOnEveryBeat()}));
    REQUIRE(runner.triggers().rule(0).valid());
    const auto linkNow = [&runner] { return runner.transports().link().now().count(); };
    PushedFeed feed(linkNow);
    takt4::timing::ProLinkFollower follower(feed);
    const StopsWhenDone stops{beats};

    struct Cue {
        std::int64_t at = 0;
        int value = 0;
    };
    std::mutex mutex;
    std::vector<Cue> cues;
    std::atomic<bool> listening{true};
    std::thread listener([&] {
        while (listening.load(std::memory_order_acquire)) {
            const std::string datagram = media.receive();
            if (datagram.rfind("/cue", 0) == 0) {
                const std::int64_t at = linkNow();
                const std::lock_guard<std::mutex> lock(mutex);
                cues.push_back({at, lastInt(datagram)});
            }
        }
    });

    runner.start();
    beats.start(follower, BeatGrid::Options{4, 4.0});
    constexpr double kBpm = 125.0;
    const auto period = static_cast<std::int64_t>(std::llround(60e6 / kBpm));
    const std::int64_t first = linkNow() + 200'000;
    // 1 2 3 4 1 2 3, then the hot cue: 1, two milliseconds late, and on from there.
    const std::vector<std::uint8_t> bars{1, 2, 3, 4, 1, 2, 3, 1, 2, 3, 4, 1};
    constexpr std::size_t kCue = 7;
    std::vector<std::int64_t> moments;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const std::int64_t due =
            first + static_cast<std::int64_t>(i) * period + (i == kCue ? 2'000 : 0);
        while (linkNow() < due - 2'000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (linkNow() < due) {
            std::this_thread::yield();
        }
        moments.push_back(linkNow());
        feed.push(kBpm, bars[i]);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    beats.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    runner.stop();
    listening.store(false, std::memory_order_release);
    listener.join();

    // One cue for each beat pushed, on it, with the bar it was pushed with.
    for (std::size_t i = 0; i < bars.size(); ++i) {
        INFO("beat " << i << ", " << int(bars[i]) << " of 4");
        std::size_t onIt = 0;
        int value = 0;
        for (const Cue& cue : cues) {
            const std::int64_t after = cue.at - moments[i];
            if (after > -3'000 && static_cast<double>(after) < 0.4 * static_cast<double>(period)) {
                ++onIt;
                value = cue.value;
            }
        }
        CHECK(onIt == 1);
        CHECK(value == static_cast<int>(bars[i]));
    }
    CHECK(runner.scheduler().predictedFires() == 0);
    CHECK(runner.errors() == 0);
}
