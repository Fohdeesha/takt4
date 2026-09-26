#include "core/audio/input_stream.hpp"

#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/hop_meter.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::audio::ChannelSelection;
using takt4::audio::HopLevel;
using takt4::audio::HopMeter;
using takt4::audio::InputDevice;
using takt4::audio::InputStream;
using takt4::audio::InputStreamOptions;
using takt4::audio::kHopSize;
using takt4::audio::kInternalSampleRate;
using takt4::audio::PickMode;

namespace {

// The most interesting input this machine has: a host API with native channel selection
// first (ASIO, CoreAudio), then the most inputs, then the platform's default input. On
// the macOS CI runner that is the "Apple Virtual Sound Device"; here it is the MOTU over
// ASIO. Loopback captures are not inputs in the HANDOFF §5.1 sense.
std::optional<InputDevice> bestInputDevice(const takt4::audio::PortAudioSession& session) {
    std::optional<InputDevice> best;
    const auto rank = [](const InputDevice& d) {
        return std::make_tuple(takt4::audio::hasNativeChannelSelection(d.hostApi), d.maxInputChannels,
                               d.isDefaultInput, -d.index);
    };
    for (const InputDevice& device : takt4::audio::listInputDevices(session)) {
        if (!device.isLoopback && (!best || rank(device) > rank(*best))) {
            best = device;
        }
    }
    return best;
}

} // namespace

TEST_CASE("the input device streams hops end to end", "[audio][hardware]") {
    // Phase 1's live path on whatever this machine has: open a real device, pick its
    // last input, and check that a second of audio reaches the meter as hops, in order,
    // with nothing dropped. Native selection is used where the host API has it, so on
    // macOS this is the CoreAudio channel map at work; the second run forces the
    // software slice on the same device. The allocation guard is on, so any heap use in
    // the callback aborts the run. Machines without an input device (the Windows and
    // Linux CI runners) skip.
    const takt4::audio::PortAudioSession session;
    const std::optional<InputDevice> device = bestInputDevice(session);
    if (!device) {
        SKIP("no input device on this machine");
    }
    const bool forceSoftware = GENERATE(false, true);
    const int channel = device->maxInputChannels - 1;
    INFO(device->hostApiName << " / " << device->name << ", input " << channel + 1 << " of "
                             << device->maxInputChannels << (forceSoftware ? ", software slice" : ""));

    HopMeter meter;
    InputStreamOptions options;
    options.forceSoftwareSlice = forceSoftware;
    InputStream stream(session, *device, ChannelSelection::single(channel), meter, options);
    const bool nativeExpected = !forceSoftware && takt4::audio::hasNativeChannelSelection(device->hostApi);
    CHECK(stream.picker().mode() == (nativeExpected ? PickMode::Native : PickMode::Software));
    INFO("opened at " << stream.reportedSampleRate() << " Hz, input latency "
                      << stream.inputLatencySeconds() * 1000.0 << " ms");

    // Run until a second of audio has arrived, reading the meter as its consumer would.
    // The deadline only bounds a device that never delivers.
    const auto targetFrames = static_cast<std::uint64_t>(stream.sampleRate());
    std::vector<HopLevel> levels;
    const auto drain = [&] {
        HopLevel level;
        while (meter.pop(level)) {
            levels.push_back(level);
        }
    };
    stream.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (stream.counters().framesIn < targetFrames && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        drain();
    }
    stream.stop();
    REQUIRE_FALSE(stream.running());
    drain();

    const auto counters = stream.counters();
    INFO(counters.callbacks << " callbacks, " << counters.framesIn << " frames, " << counters.hopsOut
                            << " hops, " << counters.inputOverflows << " overflows");
    CHECK(counters.callbacks > 0);
    CHECK(counters.framesIn >= targetFrames);
    if (counters.inputOverflows > 0) {
        WARN("the device reported " << counters.inputOverflows << " input overflows");
    }

    // The same arithmetic as the synthetic pipeline test: every device frame past the
    // resampler's delay becomes internal-rate samples, kHopSize to a hop.
    const std::uint64_t delay = stream.resamplerDelayFrames();
    REQUIRE(counters.framesIn > delay);
    const std::uint64_t expectedHops =
        static_cast<std::uint64_t>(static_cast<double>(counters.framesIn - delay) * kInternalSampleRate /
                                   stream.sampleRate()) /
        kHopSize;
    CHECK(counters.hopsOut + 1 >= expectedHops);
    CHECK(counters.hopsOut <= expectedHops + 1);

    // Every hop reached the reader, in order.
    CHECK(meter.dropped() == 0);
    REQUIRE(levels.size() == counters.hopsOut);
    std::size_t outOfOrder = 0;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (levels[i].hopIndex != i) {
            ++outOfOrder;
        }
    }
    CHECK(outOfOrder == 0);
}

TEST_CASE("an ASIO interface is opened at the rate it is running at, and never moved",
          "[audio][hardware]") {
    // The audit's C5, and coverage gap 16 of 2026-09-25: the reopen at the interface's own rate
    // had no test on an interface. The device list is read by a scan at startup; another program
    // re-clocking the interface afterwards leaves the list's rate stale, and opening at that rate
    // used to re-clock the interface under Live and the front-of-house mix. The patched ASIO host
    // refuses instead, and the stream opens again at the rate the interface is running at. A rate
    // asked for outright is refused, naming the rate it runs at, and never forced.
    const takt4::audio::PortAudioSession session;
    const std::optional<InputDevice> device = bestInputDevice(session);
    if (!device || device->hostApi != takt4::audio::HostApiKind::Asio) {
        SKIP("no ASIO interface on this machine");
    }
    INFO(device->name);
    HopMeter meter;
    double clocked = 0.0;
    {
        InputStream first(session, *device, ChannelSelection::single(0), meter);
        clocked = first.reportedSampleRate();
    }
    REQUIRE(clocked > 0.0);
    const double other = clocked == 48000.0 ? 44100.0 : 48000.0;
    INFO("running at " << clocked << " Hz; the list says " << other);

    InputDevice stale = *device;
    stale.defaultSampleRate = other;
    {
        InputStream second(session, stale, ChannelSelection::single(0), meter);
        CHECK(second.reportedSampleRate() == clocked);
        second.start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (second.counters().framesIn < 4800 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        second.stop();
        CHECK(second.counters().framesIn >= 4800);
    }

    InputStreamOptions asked;
    asked.sampleRate = other;
    CHECK_THROWS_WITH(InputStream(session, *device, ChannelSelection::single(0), meter, asked),
                      ContainsSubstring("is running at"));

    // And the interface is where it was.
    InputStream after(session, *device, ChannelSelection::single(0), meter);
    CHECK(after.reportedSampleRate() == clocked);
}
