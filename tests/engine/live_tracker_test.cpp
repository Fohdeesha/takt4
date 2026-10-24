#include "core/audio/audio_thread.hpp"
#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/hop_meter.hpp"
#include "core/audio/host_time.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/model/activation_engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <portaudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

using takt4::audio::ChannelSelection;
using takt4::audio::HopLevel;
using takt4::audio::InputDevice;
using takt4::engine::LiveTracker;

namespace {

const std::filesystem::path kWeights = std::filesystem::path(TAKT4_WEIGHTS_DIR) / "generic.bin";
const std::filesystem::path kStateSpace =
    std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin";

// The same ranking as the input stream's own test: a host API with native channel
// selection first, then the most inputs, then the platform's default. Loopback captures
// do not count as inputs.
std::optional<InputDevice> bestInputDevice(const LiveTracker& tracker) {
    std::optional<InputDevice> best;
    const auto rank = [](const InputDevice& d) {
        return std::make_tuple(takt4::audio::hasNativeChannelSelection(d.hostApi),
                               d.maxInputChannels, d.isDefaultInput, -d.index);
    };
    for (const InputDevice& device : tracker.devices()) {
        if (!device.isLoopback && (!best || rank(device) > rank(*best))) {
            best = device;
        }
    }
    return best;
}

} // namespace

TEST_CASE("LiveTracker comes up stopped, with the engine built and a device list", "[audio]") {
    LiveTracker tracker(kWeights, kStateSpace);

    CHECK_FALSE(tracker.running());
    CHECK_FALSE(tracker.current().has_value());
    CHECK(tracker.stream() == nullptr);
    CHECK_FALSE(tracker.engine().running());
    CHECK(tracker.weightsPath() == kWeights);
    // The state space was read, not merely remembered: 50 Hz is what every frame index in
    // the engine is counted in.
    CHECK(tracker.stateSpace().secondsPerFrame() == 0.02);

    // Enumerating twice is how a window refreshes its picker, and must stay harmless.
    // A machine with no inputs (the Windows and Linux CI runners) reports none.
    const auto first = tracker.devices();
    const auto second = tracker.devices();
    CHECK(first.size() == second.size());

    // Stopping something that never started is what a window does on the way out.
    tracker.stop();
    CHECK_FALSE(tracker.running());
}

TEST_CASE("LiveTracker names the asset it could not read", "[audio]") {
    // A missing weight set or state space is a startup failure with a filename in it,
    // rather than something the operator meets on their first click.
    CHECK_THROWS_AS(LiveTracker(kWeights.parent_path() / "no-such-set.bin", kStateSpace),
                    std::runtime_error);
    CHECK_THROWS_AS(LiveTracker(kWeights, kStateSpace.parent_path() / "no-such-blob.bin"),
                    std::runtime_error);
}

TEST_CASE("LiveTracker refuses a channel the device does not have", "[audio][hardware]") {
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    CHECK_THROWS_AS(tracker.start(*device, ChannelSelection::single(device->maxInputChannels)),
                    std::invalid_argument);
    // A refused start leaves nothing half-open behind it.
    CHECK_FALSE(tracker.running());
    CHECK_FALSE(tracker.current().has_value());
    CHECK_FALSE(tracker.engine().running());
}

TEST_CASE("LiveTracker feeds the engine and the meter from one stream",
          "[audio][hardware][realtime]") {
    // What the window is: a device chosen, the tracker running on it, and levels arriving
    // for the input meter beside the readouts. Both consumers are fed from the same
    // stream through HopFanout, so this is where that wiring is proven on real hardware.
    // Machines without an input device skip.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    const int channel = device->maxInputChannels - 1;
    INFO(device->hostApiName << " / " << device->name << ", input " << channel + 1 << " of "
                             << device->maxInputChannels);

    tracker.start(*device, ChannelSelection::single(channel));
    REQUIRE(tracker.running());
    REQUIRE(tracker.stream() != nullptr);
    REQUIRE(tracker.engine().running());
    REQUIRE(tracker.current().has_value());
    CHECK(tracker.current()->device.index == device->index);
    CHECK(tracker.current()->selection.channels[0] == channel);

    // A second of audio, drained the way a drawing timer would. The deadline only bounds
    // a device that never delivers.
    std::uint64_t levels = 0;
    std::uint64_t frames = 0;
    const auto drain = [&] {
        HopLevel level;
        while (tracker.popLevel(level)) {
            ++levels;
        }
        takt4::engine::EngineFrame frame;
        while (tracker.engine().popFrame(frame)) {
            ++frames;
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // The longest this thread went between looks: how long the machine held the process up, as
    // near as a thread at the default priority can tell.
    double heldUp = 0.0;
    auto looked = std::chrono::steady_clock::now();
    while (levels < 50 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto now = std::chrono::steady_clock::now();
        heldUp = std::max(heldUp, std::chrono::duration<double>(now - looked).count());
        looked = now;
        drain();
    }
    tracker.stop();
    drain();

    INFO(levels << " hop levels, " << frames << " tracked frames; this thread held up for as long "
                << "as " << heldUp << " s");
    CHECK(levels >= 50);
    // The engine is two threads behind the meter, so it is a little short of the hops —
    // but it must have seen most of the same second, not a handful.
    CHECK(frames > levels / 2);
    // Every hop reached the model — unless the machine held the whole process up for a good part
    // of what the model's queue holds (`kHopQueueCapacity` hops, 1.28 s): the hops that came
    // while it did could not all be kept, and that is said rather than failed. On a CI Mac this
    // loop once found 112 levels where it stops at 50 — the process held up about 1.2 s, by them
    // — and 8 hops were dropped: the model's thread was held longer than this one saw, so the
    // line is half the queue, not all of it. A quiet machine holds this thread up 10 to 20 ms.
    const double queueSeconds =
        static_cast<double>(takt4::model::ActivationEngine::kHopQueueCapacity) *
        tracker.stateSpace().secondsPerFrame();
    const std::uint64_t dropped = tracker.engine().activations().hopsDropped();
    if (dropped != 0 && heldUp >= queueSeconds / 2.0) {
        WARN(dropped << " hops dropped while the machine held this process up for " << heldUp
                     << " s");
    } else {
        CHECK(dropped == 0);
    }

    CHECK_FALSE(tracker.running());
    CHECK_FALSE(tracker.current().has_value());
    CHECK_FALSE(tracker.engine().running());

    // Starting again on the same device is the operator changing their mind and changing
    // it back. start() reseeds the filter, so the second run counts from nothing rather
    // than continuing the first — asserted as "far fewer" rather than "exactly zero",
    // because the second run's own first frames may land while this line is reading.
    const std::uint64_t firstRun = tracker.engine().framesTracked();
    CHECK(firstRun >= 25);
    tracker.start(*device, ChannelSelection::single(channel));
    CHECK(tracker.running());
    CHECK(tracker.engine().framesTracked() < firstRun);
    tracker.stop();
}

namespace {

/// A host time source with no Link behind it: a straight line, and counters for what the
/// tracker did with it. `audio::HostTimeSource` exists so that a test can be exactly this.
class FakeHostTime final : public takt4::audio::HostTimeSource {
public:
    std::int64_t hostMicrosForSample(double sampleTime) noexcept override {
        stamps_.fetch_add(1, std::memory_order_relaxed);
        return static_cast<std::int64_t>(sampleTime * 45.35);
    }
    void resetHostTimeFilter() noexcept override {
        resets_.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t stamps() const { return stamps_.load(std::memory_order_relaxed); }
    std::uint64_t resets() const { return resets_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> stamps_{0};
    std::atomic<std::uint64_t> resets_{0};
};

} // namespace

TEST_CASE("LiveTracker holds a host time source without installing it yet", "[audio]") {
    LiveTracker tracker(kWeights, kStateSpace);
    CHECK(tracker.hostTimeSource() == nullptr);

    FakeHostTime clock;
    tracker.setHostTimeSource(&clock);
    CHECK(tracker.hostTimeSource() == &clock);
    // Nothing is running, so nothing has asked it for anything.
    CHECK(clock.stamps() == 0);
    CHECK(clock.resets() == 0);
}

TEST_CASE("LiveTracker installs the host time source around a run", "[audio][hardware]") {
    // The stamp is taken on the audio thread, so the clock has to be in
    // place before the stream is opened — which is why the tracker does it rather than
    // leaving the ordering to a caller to remember. And a restarted stream counts samples
    // from zero again, so a regression fitted to the last run has to be forgotten with it.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    FakeHostTime clock;
    tracker.setHostTimeSource(&clock);

    tracker.start(*device, ChannelSelection::single(0));
    CHECK(clock.resets() == 1); // forgotten before the stream, not after
    std::this_thread::sleep_for(std::chrono::milliseconds{250});
    tracker.stop();

    INFO(clock.stamps() << " hops stamped");
    CHECK(clock.stamps() > 0); // the audio thread really used it

    // A second run forgets the first one's sample clock.
    tracker.start(*device, ChannelSelection::single(0));
    CHECK(clock.resets() == 2);
    tracker.stop();
}

TEST_CASE("LiveTracker looks for devices again only while stopped, and opens them after",
          "[audio][hardware]") {
    // RESCAN makes PortAudio enumerate the machine again, which renumbers every device — so it is
    // refused while a stream is open, doing nothing, and afterwards the interface has to be found
    // again and open as it did. That is `LiveTracker::rescan` and the `PortAudioSession::restart`
    // under it, on the real interface.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    INFO(device->hostApiName << " / " << device->name);
    const auto hopsWithin = [&tracker](std::chrono::seconds limit) {
        std::uint64_t levels = 0;
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (levels < 25 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            HopLevel level;
            while (tracker.popLevel(level)) {
                ++levels;
            }
        }
        return levels;
    };

    tracker.start(*device, ChannelSelection::single(0));
    REQUIRE(tracker.running());
    const std::size_t listed = tracker.devices().size();
    CHECK_FALSE(tracker.rescan());
    CHECK(tracker.running()); // and nothing was done to the stream
    CHECK(hopsWithin(std::chrono::seconds{10}) >= 25);
    tracker.stop();

    REQUIRE(tracker.rescan());
    const std::vector<InputDevice> again = tracker.devices();
    CHECK(again.size() == listed);
    std::optional<InputDevice> found;
    for (const InputDevice& candidate : again) {
        if (candidate.name == device->name && candidate.hostApi == device->hostApi) {
            found = candidate;
        }
    }
    REQUIRE(found.has_value());
    tracker.start(*found, ChannelSelection::single(0));
    REQUIRE(tracker.running());
    CHECK(hopsWithin(std::chrono::seconds{10}) >= 25);
    tracker.stop();
}

namespace {

/// Waits up to `limit` for `done`, looking every millisecond.
template <typename Done>
bool within(std::chrono::milliseconds limit, Done done) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// Limits a test can wait out: a real driver deserves seconds, a test's stand-in for one that
/// never answers does not.
LiveTracker::Limits shortLimits() {
    LiveTracker::Limits limits;
    limits.open = std::chrono::milliseconds(300);
    limits.stop = std::chrono::milliseconds(300);
    limits.rescan = std::chrono::milliseconds(300);
    limits.list = std::chrono::milliseconds(300);
    return limits;
}

} // namespace

TEST_CASE("a driver that does not answer costs the caller its limit, and is asked nothing more "
          "until it does",
          "[audio]") {
    // A driver wedged in PortAudio held whichever thread had asked, which was the window's. Here
    // the drivers are asked to look for devices again and do not answer — a hook on the audio
    // thread that waits until the test lets it go.
    LiveTracker tracker(kWeights, kStateSpace);
    tracker.setLimits(shortLimits());
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    tracker.setAudioJobHook([released](const char* what) {
        if (std::string_view(what) == "rescan") {
            released.wait();
        }
    });

    const auto asked = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(tracker.rescan(), takt4::audio::DriverNotAnswering);
    const auto waited = std::chrono::steady_clock::now() - asked;
    CHECK(waited >= std::chrono::milliseconds(290));
    CHECK(waited < std::chrono::seconds(2));
    CHECK(tracker.stuck());
    CHECK(tracker.stuckOn() == "rescan");

    // Nothing more is asked of it: an input to open is refused at once, and says why.
    InputDevice anything;
    anything.name = "takt4 test - an input";
    anything.maxInputChannels = 2;
    const auto opening = std::chrono::steady_clock::now();
    try {
        tracker.start(anything, ChannelSelection::single(0));
        FAIL("an input was opened while the audio thread was held");
    } catch (const takt4::audio::DriverNotAnswering& e) {
        INFO(e.what());
        CHECK(std::string(e.what()).find("since it was asked to rescan") != std::string::npos);
    }
    CHECK(std::chrono::steady_clock::now() - opening < std::chrono::milliseconds(100));
    CHECK_FALSE(tracker.running());
    CHECK_FALSE(tracker.engine().running());
    // And the device list is the last one it had, not a wait.
    const auto listing = std::chrono::steady_clock::now();
    (void)tracker.devices();
    CHECK(std::chrono::steady_clock::now() - listing < std::chrono::milliseconds(100));

    // The drivers answer after all: the tracker is itself again. Asked with the limits the window
    // uses: this rescan really lists the drivers, and with ASIO allowed (the `-all` presets) that
    // is a scan in a process of its own, which took more than the 300 ms above under
    // AddressSanitizer (2026-09-28).
    release.set_value();
    REQUIRE(within(std::chrono::milliseconds(2000), [&] { return !tracker.stuck(); }));
    tracker.setLimits(LiveTracker::Limits{});
    CHECK(tracker.rescan());
}

TEST_CASE("a rescan is begun and asked after, and nobody waits for the drivers meanwhile",
          "[audio]") {
    // An outage looked for the devices every third try with the window's thread waiting — up to
    // half a minute with ASIO, PANIC and the meters with it. Begun, it is asked after.
    LiveTracker tracker(kWeights, kStateSpace);
    tracker.setLimits(shortLimits());
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> hold{true};
    tracker.setAudioJobHook([released, &hold](const char* what) {
        if (std::string_view(what) == "rescan" && hold) {
            released.wait();
        }
    });
    const std::uint64_t table = tracker.deviceTable();
    const std::size_t listed = tracker.devices().size();

    const auto asked = std::chrono::steady_clock::now();
    REQUIRE(tracker.beginRescan());
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::milliseconds(100));
    std::string error;
    CHECK(tracker.pollRescan(error) == LiveTracker::Progress::Pending);
    // One at a time, and the list asked meanwhile is the one from before, at once — the thread
    // would take the question only after the rescan.
    CHECK_FALSE(tracker.beginRescan());
    const auto listing = std::chrono::steady_clock::now();
    CHECK(tracker.devices().size() == listed);
    CHECK(std::chrono::steady_clock::now() - listing < std::chrono::milliseconds(100));

    // Past its limit it is given up on, as `rescan` gives up — the driver holds the thread.
    REQUIRE(within(std::chrono::milliseconds(2000),
                   [&] { return tracker.pollRescan(error) != LiveTracker::Progress::Pending; }));
    CHECK(error.find("did not answer") != std::string::npos);
    CHECK(tracker.stuck());
    CHECK(tracker.deviceTable() == table);
    // And when the drivers do answer, a table is in place that nobody listed: said, so the
    // window lists it before opening anything by an index from the old one.
    release.set_value();
    REQUIRE(within(std::chrono::milliseconds(5000), [&] { return !tracker.stuck(); }));
    CHECK(tracker.deviceTable() == table + 1);

    // An answer in time is done, and moves the table on too.
    hold = false;
    tracker.setLimits(LiveTracker::Limits{});
    REQUIRE(tracker.beginRescan());
    LiveTracker::Progress answer = LiveTracker::Progress::Pending;
    error.clear();
    REQUIRE(within(std::chrono::milliseconds(30000), [&] {
        answer = tracker.pollRescan(error);
        return answer != LiveTracker::Progress::Pending;
    }));
    CHECK(answer == LiveTracker::Progress::Done);
    CHECK(error.empty());
    CHECK(tracker.deviceTable() == table + 2);
    CHECK(tracker.pollRescan(error) == LiveTracker::Progress::None);
    tracker.setAudioJobHook({}); // before `hold` goes: the tracker's way out runs jobs too
}

TEST_CASE("an open is begun and asked after, and fails as a start would", "[audio]") {
    LiveTracker tracker(kWeights, kStateSpace);
    tracker.setLimits(shortLimits());
    // An input no list has: refused by the open, on the audio thread, and said on the ask after.
    InputDevice gone;
    gone.index = 4096;
    gone.name = "takt4 test - an input that is not there";
    gone.hostApiName = "ASIO";
    gone.maxInputChannels = 2;
    gone.defaultSampleRate = 48000.0;
    tracker.beginStart(gone, ChannelSelection::single(0));
    CHECK(tracker.starting());
    std::string error;
    REQUIRE(within(std::chrono::milliseconds(2000),
                   [&] { return tracker.pollStart(error) != LiveTracker::Progress::Pending; }));
    INFO(error);
    CHECK(error.find("Press RESCAN") != std::string::npos);
    CHECK_FALSE(tracker.starting());
    CHECK_FALSE(tracker.running());
    CHECK_FALSE(tracker.engine().running());

    // One the driver does not answer: given up on at its limit, the thread left to the driver.
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    tracker.setAudioJobHook([released](const char* what) {
        if (std::string_view(what) == "open") {
            released.wait();
        }
    });
    const auto asked = std::chrono::steady_clock::now();
    tracker.beginStart(gone, ChannelSelection::single(0));
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::milliseconds(100));
    CHECK(tracker.pollStart(error) == LiveTracker::Progress::Pending);
    REQUIRE(within(std::chrono::milliseconds(2000),
                   [&] { return tracker.pollStart(error) != LiveTracker::Progress::Pending; }));
    CHECK(error.find("did not answer") != std::string::npos);
    CHECK(tracker.stuck());
    // Nothing more is begun while it holds the thread.
    CHECK_THROWS_AS(tracker.beginStart(gone, ChannelSelection::single(0)),
                    takt4::audio::DriverNotAnswering);
    CHECK_THROWS_AS(tracker.beginRescan(), takt4::audio::DriverNotAnswering);
    release.set_value();
    REQUIRE(within(std::chrono::milliseconds(2000), [&] { return !tracker.stuck(); }));
    tracker.setAudioJobHook({});
}

TEST_CASE("the device list after a rescan that could not bring PortAudio back up is empty, "
          "not a throw",
          "[audio]") {
    // PortAudio ended and not started again: no table to list. The list used to throw, into an
    // IMPORT or the outage's next look at the devices, and the window ended with it. Made to
    // happen on the audio thread, where PortAudio lives.
    LiveTracker tracker(kWeights, kStateSpace);
    tracker.setAudioJobHook([](const char* what) {
        if (std::string_view(what) == "rescan") {
            Pa_Terminate();
            throw std::runtime_error("Pa_Initialize failed: made to, for the test");
        }
    });
    REQUIRE(tracker.beginRescan());
    std::string error;
    REQUIRE(within(std::chrono::milliseconds(5000),
                   [&] { return tracker.pollRescan(error) != LiveTracker::Progress::Pending; }));
    CHECK(error.find("made to") != std::string::npos);
    std::vector<InputDevice> devices;
    CHECK_NOTHROW(devices = tracker.devices());
    CHECK(devices.empty());
}

TEST_CASE("an open given up on while its stream is starting sends the engine nothing after",
          "[audio][hardware]") {
    // A driver that calls back before it has returned from its start: an open given up on then
    // had its stream running with nothing able to shut it off from the engine — and the engine
    // could be the tracker's that was going. Held here just after the start, calling back.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    INFO(device->hostApiName << " / " << device->name);
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> started{false};
    tracker.setStartHook([released, &started] {
        started = true;
        released.wait();
    });
    tracker.beginStart(*device, ChannelSelection::single(0));
    REQUIRE(within(std::chrono::milliseconds(10000), [&] { return started.load(); }));
    // Calling back while it is held: what it sends goes where a running stream's goes.
    std::uint64_t levels = 0;
    CHECK(within(std::chrono::milliseconds(3000), [&] {
        HopLevel level;
        while (tracker.popLevel(level)) {
            ++levels;
        }
        return levels >= 5;
    }));

    tracker.stop(); // gives up on the open
    HopLevel level;
    while (tracker.popLevel(level)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::uint64_t after = 0;
    while (tracker.popLevel(level)) {
        ++after;
    }
    CHECK(after == 0);

    // The driver returns from its start: the stream is stopped and closed where it was opened,
    // and the interface opens again.
    release.set_value();
    tracker.setStartHook({});
    REQUIRE(within(std::chrono::milliseconds(3000), [&] { return !tracker.stuck(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tracker.start(*device, ChannelSelection::single(0));
    CHECK(tracker.running());
    tracker.stop();
}

TEST_CASE("a stream whose driver does not return from its stop is let go of, and sends the "
          "engine nothing more",
          "[audio][hardware]") {
    // The same on a real interface: the stop that never returns. `stop` detaches the stream on
    // this thread, hands it to the audio thread, and returns within its limit — and from the
    // moment it returns nothing the driver sends reaches the meter or the engine.
    LiveTracker tracker(kWeights, kStateSpace);
    const std::optional<InputDevice> device = bestInputDevice(tracker);
    if (!device) {
        SKIP("no input device on this machine");
    }
    INFO(device->hostApiName << " / " << device->name);
    tracker.setLimits(shortLimits());
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> holding{false};
    tracker.setAudioJobHook([released, &holding](const char* what) {
        if (std::string_view(what) == "stop" && holding) {
            released.wait();
        }
    });

    tracker.start(*device, ChannelSelection::single(0));
    REQUIRE(tracker.running());
    std::uint64_t levels = 0;
    REQUIRE(within(std::chrono::milliseconds(10000), [&] {
        HopLevel level;
        while (tracker.popLevel(level)) {
            ++levels;
        }
        return levels >= 25;
    }));

    holding = true;
    const auto asked = std::chrono::steady_clock::now();
    tracker.stop();
    const auto waited = std::chrono::steady_clock::now() - asked;
    CHECK(waited < std::chrono::seconds(2));
    CHECK(tracker.stuck());
    CHECK_FALSE(tracker.running());

    // Still open on the audio thread, still calling back — and none of it arrives.
    HopLevel level;
    while (tracker.popLevel(level)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::uint64_t after = 0;
    while (tracker.popLevel(level)) {
        ++after;
    }
    CHECK(after == 0);

    // The driver returns: the stream is closed where it was opened, and the interface opens again.
    holding = false;
    release.set_value();
    REQUIRE(within(std::chrono::milliseconds(3000), [&] { return !tracker.stuck(); }));
    tracker.start(*device, ChannelSelection::single(0));
    CHECK(tracker.running());
    tracker.stop();
}

TEST_CASE("LiveTracker opens a stereo pair on a real interface and the pair's sums arrive",
          "[audio][hardware][stereo]") {
    // 2026-09-28: stereo unless asked otherwise, so the pair is what opens on a rig — on ASIO
    // through its channel selectors, two of them. The engine gets hops, and the stereo check gets
    // both sides' sums, from the one stream.
    LiveTracker tracker(kWeights, kStateSpace);
    std::optional<InputDevice> device;
    for (const InputDevice& candidate : tracker.devices()) {
        if (!candidate.isLoopback && candidate.maxInputChannels >= 2 &&
            (!device || (takt4::audio::hasNativeChannelSelection(candidate.hostApi) &&
                         !takt4::audio::hasNativeChannelSelection(device->hostApi)))) {
            device = candidate;
        }
    }
    if (!device) {
        SKIP("no input device with two inputs on this machine");
    }
    INFO(device->hostApiName << " / " << device->name);
    tracker.start(*device, ChannelSelection::pair(0, 1));
    REQUIRE(tracker.running());
    REQUIRE(tracker.stream() != nullptr);
    std::uint64_t levels = 0;
    REQUIRE(within(std::chrono::milliseconds(10000), [&] {
        HopLevel level;
        while (tracker.popLevel(level)) {
            ++levels;
        }
        return levels >= 25;
    }));
    const takt4::audio::StereoSums sums = tracker.stream()->stereo();
    CHECK(sums.frames > 0);
    CHECK(tracker.stream()->picker().selection().count == 2);
    tracker.stop();
}

TEST_CASE("an ASIO stream opens and closes hundreds of times without its driver falling over",
          "[.stress][hardware]") {
    // Not run by any preset: `takt4_tests "[.stress]"` with the rig free. The MOTU's ASIO driver
    // was caught fast-failing inside ASIOExit about once in 840 releases, while PortAudio probed
    // drivers. Probing moved to a child process; what the app still does
    // in-process is open and close a stream — this, as many times as TAKT4_STRESS_CYCLES says
    // (default 300), through the audio thread as the window does. A fast fail ends the process,
    // which ctest or the shell reports; nothing here could catch it.
    LiveTracker tracker(kWeights, kStateSpace);
    std::optional<InputDevice> asio;
    for (const InputDevice& device : tracker.devices()) {
        if (device.hostApi == takt4::audio::HostApiKind::Asio && device.maxInputChannels >= 2) {
            asio = device;
            break;
        }
    }
    if (!asio) {
        SKIP("no ASIO input on this machine (TAKT4_TEST_HARDWARE=1 lets the test binary load ASIO)");
    }
    int cycles = 300;
#if defined(_MSC_VER)
    char* given = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&given, &length, "TAKT4_STRESS_CYCLES") == 0 && given != nullptr) {
        cycles = std::max(1, std::atoi(given));
    }
    std::free(given);
#else
    if (const char* given = std::getenv("TAKT4_STRESS_CYCLES"); given != nullptr) {
        cycles = std::max(1, std::atoi(given));
    }
#endif
    INFO(asio->name << ", " << cycles << " cycles");
    int opened = 0;
    for (int i = 0; i < cycles; ++i) {
        tracker.start(*asio, ChannelSelection::pair(0, 1));
        REQUIRE(tracker.running());
        std::this_thread::sleep_for(std::chrono::milliseconds(20 + (i % 7) * 10));
        tracker.stop();
        REQUIRE_FALSE(tracker.stuck());
        ++opened;
    }
    CHECK(opened == cycles);
}
