#include "core/audio/input_pipeline.hpp"

#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/hop_processor.hpp"
#include "core/audio/rates.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::audio::ChannelPicker;
using takt4::audio::ChannelSelection;
using takt4::audio::HostApiKind;
using takt4::audio::InputDevice;
using takt4::audio::InputPipeline;
using takt4::audio::kHopSize;
using takt4::audio::kInternalSampleRate;

namespace {

constexpr int kChannels = 18;
constexpr double kDeviceRate = 48000.0;
constexpr double kToneHz = 440.0;
constexpr double kToneAmplitude = 0.5;
constexpr int kToneChannel = 6; // "input 7"

InputDevice device(HostApiKind kind) {
    InputDevice d;
    d.index = 1;
    d.hostApi = kind;
    d.hostApiName = takt4::audio::toString(kind);
    d.name = "18-input interface";
    d.maxInputChannels = kChannels;
    d.defaultSampleRate = kDeviceRate;
    return d;
}

// The synthetic interface: input 7 carries a quiet tone; both neighbours are loud
// (full-scale noise on 6, a full-scale tone on 8); everything else is silent.
std::vector<float> interfaceBlock(std::size_t frames) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> out(frames * kChannels, 0.0f);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kDeviceRate;
        float* frame = out.data() + i * kChannels;
        frame[kToneChannel - 1] = dist(rng);
        frame[kToneChannel] = static_cast<float>(kToneAmplitude * std::sin(2.0 * std::numbers::pi * kToneHz * t));
        frame[kToneChannel + 1] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 3000.0 * t));
    }
    return out;
}

// Keeps every hop it is given, as a test double for the tracker.
class RecordingProcessor final : public takt4::audio::HopProcessor {
public:
    void processHop(const float* hop, std::uint64_t hopIndex) noexcept override {
        if (hopIndex != hops.size()) {
            ++indexGaps;
        }
        hops.emplace_back(hop, hop + kHopSize);
    }

    std::vector<std::vector<float>> hops;
    int indexGaps = 0;
};

// Feeds the block through in slices of `blockFrames` device frames.
void feed(InputPipeline& pipeline, const std::vector<float>& interleaved, int streamChannels,
          std::size_t blockFrames) {
    const std::size_t stride = static_cast<std::size_t>(streamChannels);
    const std::size_t frames = interleaved.size() / stride;
    for (std::size_t pos = 0; pos < frames; pos += blockFrames) {
        const std::size_t n = std::min(blockFrames, frames - pos);
        pipeline.process(interleaved.data() + pos * stride, n);
    }
}

double hopRms(const std::vector<float>& hop) {
    double sum = 0.0;
    for (const float x : hop) {
        sum += static_cast<double>(x) * static_cast<double>(x);
    }
    return std::sqrt(sum / static_cast<double>(hop.size()));
}

} // namespace

TEST_CASE("picking input 7 meters only input 7", "[audio]") {
    // The Phase 1 exit criterion (HANDOFF §8) in synthetic form: with loud signal on
    // the neighbouring inputs, the hops carry input 7's tone and nothing else.
    constexpr std::size_t kFrames = 48000 * 2;
    const std::vector<float> block = interfaceBlock(kFrames);
    const InputDevice wasapi = device(HostApiKind::Wasapi);

    const std::size_t blockFrames = GENERATE_COPY(std::size_t{1}, std::size_t{479}, std::size_t{4096});
    INFO("device block size " << blockFrames);

    RecordingProcessor recorder;
    const ChannelPicker picker(wasapi, ChannelSelection::single(kToneChannel));
    REQUIRE(picker.mode() == takt4::audio::PickMode::Software);
    InputPipeline pipeline(picker, kDeviceRate, recorder);
    feed(pipeline, block, kChannels, blockFrames);

    CHECK(pipeline.framesIn() == kFrames);
    CHECK(recorder.indexGaps == 0);
    const std::size_t expectedHops =
        (static_cast<std::size_t>(static_cast<double>(kFrames - pipeline.inputDelayFrames()) *
                                  kInternalSampleRate / kDeviceRate)) / kHopSize;
    CHECK(recorder.hops.size() >= expectedHops - 1);
    CHECK(recorder.hops.size() <= expectedHops + 1);
    CHECK(pipeline.hopsOut() == recorder.hops.size());
    CHECK(pipeline.samplesOut() >= recorder.hops.size() * kHopSize);

    // Every hop after the switch-on transient is the tone at its level, sample-exact
    // against the ideal tone at internal-rate time: hop k, sample i is t = (k·441 + i) / 22050.
    const double expectedRms = kToneAmplitude / std::numbers::sqrt2;
    double worstSample = 0.0;
    for (std::size_t k = 10; k < recorder.hops.size(); ++k) {
        const auto& hop = recorder.hops[k];
        REQUIRE(hop.size() == kHopSize);
        CHECK_THAT(hopRms(hop), WithinAbs(expectedRms, 0.01));
        for (std::size_t i = 0; i < kHopSize; ++i) {
            const double t = static_cast<double>(k * kHopSize + i) / kInternalSampleRate;
            const double ideal = kToneAmplitude * std::sin(2.0 * std::numbers::pi * kToneHz * t);
            worstSample = std::max(worstSample, std::fabs(static_cast<double>(hop[i]) - ideal));
        }
    }
    INFO("worst sample deviation " << worstSample);
    CHECK(worstSample < 2e-3);
}

TEST_CASE("a silent input stays exactly silent beside loud ones", "[audio]") {
    const std::vector<float> block = interfaceBlock(48000);
    const InputDevice wasapi = device(HostApiKind::Wasapi);
    for (const int channel : {0, kToneChannel - 2, kToneChannel + 2, kChannels - 1}) {
        INFO("input " << channel + 1);
        RecordingProcessor recorder;
        const ChannelPicker picker(wasapi, ChannelSelection::single(channel));
        InputPipeline pipeline(picker, kDeviceRate, recorder);
        feed(pipeline, block, kChannels, 512);
        REQUIRE(recorder.hops.size() > 40);
        for (const auto& hop : recorder.hops) {
            REQUIRE(std::all_of(hop.begin(), hop.end(), [](float x) { return x == 0.0f; }));
        }
    }
}

TEST_CASE("a sample that is not a number reaches no hop as one", "[audio]") {
    // The audit's M2, at the front door. A loopback or a virtual cable can hand over a NaN or
    // an infinity, and the resampler's filter would spread one across every output sample it
    // touches — on the way to a network whose state it would never leave.
    std::vector<float> block = interfaceBlock(48000);
    const float junk[] = {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()};
    for (std::size_t n = 0; n < 3; ++n) {
        block[(10000 + n * 7000) * kChannels + kToneChannel] = junk[n];
    }
    RecordingProcessor recorder;
    const ChannelPicker picker(device(HostApiKind::Wasapi), ChannelSelection::single(kToneChannel));
    InputPipeline pipeline(picker, kDeviceRate, recorder);
    feed(pipeline, block, kChannels, 512);

    REQUIRE(recorder.hops.size() > 40);
    for (const auto& hop : recorder.hops) {
        REQUIRE(std::all_of(hop.begin(), hop.end(), [](float x) { return std::isfinite(x); }));
    }
    CHECK(pipeline.samplesRepaired() == 3);
    // And the tone either side of each is still the tone: one sample of silence in 48000 is
    // a click nobody hears, where a NaN was the end of the run.
    const double expectedRms = kToneAmplitude / std::numbers::sqrt2;
    CHECK_THAT(hopRms(recorder.hops[recorder.hops.size() / 2]), WithinAbs(expectedRms, 0.02));
    CHECK_THAT(hopRms(recorder.hops.back()), WithinAbs(expectedRms, 0.02));
}

TEST_CASE("native and software picks produce identical hops", "[audio]") {
    // Native mode opens only the selected channel, so its stream is the one-channel
    // slice of the interface; both paths must feed the resampler the same samples.
    constexpr std::size_t kFrames = 48000;
    const std::vector<float> full = interfaceBlock(kFrames);
    std::vector<float> nativeStream(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        nativeStream[i] = full[i * kChannels + kToneChannel];
    }

    RecordingProcessor viaSoftware;
    const ChannelPicker software(device(HostApiKind::Wasapi), ChannelSelection::single(kToneChannel));
    InputPipeline softwarePipeline(software, kDeviceRate, viaSoftware);
    feed(softwarePipeline, full, kChannels, 1000);

    RecordingProcessor viaNative;
    const ChannelPicker native(device(HostApiKind::Asio), ChannelSelection::single(kToneChannel));
    REQUIRE(native.mode() == takt4::audio::PickMode::Native);
    REQUIRE(native.streamChannelCount() == 1);
    InputPipeline nativePipeline(native, kDeviceRate, viaNative);
    feed(nativePipeline, nativeStream, 1, 1000);

    REQUIRE(viaNative.hops.size() == viaSoftware.hops.size());
    CHECK(viaNative.hops.size() > 40);
    CHECK(viaNative.hops == viaSoftware.hops);
}

TEST_CASE("a summed pair carries the average of both inputs", "[audio]") {
    // Inputs 7 and 6: the tone plus noise, averaged. Compare against a pipeline fed the
    // pre-averaged signal on one channel.
    constexpr std::size_t kFrames = 48000;
    const std::vector<float> full = interfaceBlock(kFrames);
    std::vector<float> averaged(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        averaged[i] = 0.5f * (full[i * kChannels + kToneChannel] + full[i * kChannels + kToneChannel - 1]);
    }

    RecordingProcessor viaPair;
    const ChannelPicker pair(device(HostApiKind::Wasapi), ChannelSelection::pair(kToneChannel, kToneChannel - 1));
    InputPipeline pairPipeline(pair, kDeviceRate, viaPair);
    feed(pairPipeline, full, kChannels, 600);

    RecordingProcessor viaMono;
    const ChannelPicker mono(device(HostApiKind::Asio), ChannelSelection::single(0));
    InputPipeline monoPipeline(mono, kDeviceRate, viaMono);
    feed(monoPipeline, averaged, 1, 600);

    REQUIRE(viaPair.hops.size() == viaMono.hops.size());
    CHECK(viaPair.hops == viaMono.hops);
}

TEST_CASE("InputPipeline::process() does not touch the heap", "[audio][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    const std::vector<float> block = interfaceBlock(20000);

    // A processor that records nothing, so the only heap use possible is the pipeline's.
    class NullProcessor final : public takt4::audio::HopProcessor {
    public:
        void processHop(const float*, std::uint64_t) noexcept override { ++hops; }
        std::uint64_t hops = 0;
    } sink;

    const ChannelPicker picker(device(HostApiKind::Wasapi), ChannelSelection::pair(3, 11));
    InputPipeline pipeline(picker, kDeviceRate, sink);

    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope realtime;
        for (const std::size_t blockFrames : {std::size_t{1}, std::size_t{479}, std::size_t{4096}}) {
            feed(pipeline, block, kChannels, blockFrames);
        }
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(takt4::rt::violationCount() == before);
    CHECK(sink.hops > 0);
    CHECK(pipeline.framesIn() == 60000);
}
