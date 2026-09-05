#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/hop_meter.hpp"
#include "core/audio/host_time.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/live_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>

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
// are not inputs in HANDOFF §5.1's sense.
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

TEST_CASE("LiveTracker feeds the engine and the meter from one stream", "[audio][hardware]") {
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
    while (levels < 50 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        drain();
    }
    tracker.stop();
    drain();

    INFO(levels << " hop levels, " << frames << " tracked frames");
    CHECK(levels >= 50);
    // The engine is two threads behind the meter, so it is a little short of the hops —
    // but it must have seen most of the same second, not a handful.
    CHECK(frames > levels / 2);
    CHECK(tracker.engine().activations().hopsDropped() == 0);

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
    // HANDOFF §4.3: the stamp is taken on the audio thread, so the clock has to be in
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
