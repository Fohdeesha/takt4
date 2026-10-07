#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/host_time.hpp"
#include "core/audio/host_time_fit.hpp"
#include "core/audio/input_pipeline.hpp"
#include "core/audio/rates.hpp"
#include "core/model/activation_engine.hpp"
#include "core/model/weights.hpp"
#include "core/output/link_session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <vector>

using takt4::audio::ChannelPicker;
using takt4::audio::ChannelSelection;
using takt4::audio::HostApiKind;
using takt4::audio::InputDevice;
using takt4::audio::InputPipeline;
using takt4::audio::kHopSize;
using takt4::audio::kInternalSampleRate;
using takt4::model::ActivationEngine;
using takt4::model::FrameActivation;

namespace {

const std::filesystem::path kWeightsDir{TAKT4_WEIGHTS_DIR};
constexpr double kDeviceRate = 48000.0;
/// One second of the device's frames.
constexpr std::size_t kSecond = 48000;
/// The interface's input latency: what PortAudio says of an ASIO buffer's first sample.
constexpr double kInputLatency = 0.0053;
/// Where the room's clock was when the stream started, in microseconds — a machine up a month.
constexpr double kOriginMicros = 2.6e12;

/// The host-time source `output::LinkSession` is, with Link taken out: the same line, fed with
/// the moments given, on the clock they are given on.
class LineSource final : public takt4::audio::HostTimeSource {
public:
    void observe(double sampleTime, std::int64_t steadyMicros) noexcept override {
        fit_.observe(sampleTime, static_cast<double>(steadyMicros));
    }
    std::int64_t hostMicrosForSample(double sampleTime) noexcept override {
        return static_cast<std::int64_t>(std::llround(fit_.at(sampleTime)));
    }

private:
    takt4::audio::HostTimeFit fit_{1e6 / kInternalSampleRate};
};

InputDevice monoDevice() {
    InputDevice d;
    d.index = 1;
    d.hostApi = HostApiKind::Asio;
    d.hostApiName = takt4::audio::toString(HostApiKind::Asio);
    d.name = "mono interface";
    d.maxInputChannels = 1;
    d.defaultSampleRate = kDeviceRate;
    return d;
}

/// A dropout: from device frame `at` on, the room's time is `seconds` further on than the frames
/// delivered say — that much audio was never delivered.
struct Gap {
    std::size_t at = 0;
    double seconds = 0.0;
};

/// How far each frame's stamp is from when the audio it is centred on was at the input: an
/// interface delivering `bufferFrames` at a time, its callbacks jittered by up to half a
/// millisecond, through the real pipeline and the real stamping, `buffers` buffers long.
/// The one frame centred less than a hop before a gap is left out: it is stamped from the hop
/// after it, which is after the gap, and no stamp can be right for both.
std::vector<double> stampErrors(std::size_t bufferFrames, std::size_t buffers, Gap gap = {}) {
    const takt4::model::ModelWeights weights =
        takt4::model::ModelWeights::fromFile(kWeightsDir / "generic.bin");
    const auto engine = std::make_unique<ActivationEngine>(weights);
    LineSource clock;
    engine->setHostTimeSource(&clock);
    const ChannelPicker picker(monoDevice(), ChannelSelection::single(0));
    InputPipeline pipeline(picker, kDeviceRate, *engine);

    // When device frame `frame` was at the input, on the room's clock, in microseconds.
    const auto heardMicros = [&gap](double frame) {
        const bool after = gap.seconds > 0.0 && frame >= static_cast<double>(gap.at);
        return kOriginMicros + (frame / kDeviceRate + (after ? gap.seconds : 0.0)) * 1e6;
    };
    std::mt19937 random(20261007);
    std::uniform_real_distribution<double> jitter(-500.0, 500.0);
    std::vector<float> buffer(bufferFrames);
    std::vector<double> errors;
    std::uint64_t delivered = 0;
    for (std::size_t b = 0; b < buffers; ++b) {
        for (std::size_t i = 0; i < bufferFrames; ++i) {
            buffer[i] = 0.1f * std::sin(static_cast<float>(delivered + i) * 0.01f);
        }
        // The callback comes when the buffer's first sample has been in the interface its input
        // latency — plus a scheduler's jitter.
        const double arrivedMicros =
            heardMicros(static_cast<double>(delivered)) + kInputLatency * 1e6 + jitter(random);
        pipeline.process(buffer.data(), bufferFrames,
                         InputPipeline::Arrival{static_cast<std::int64_t>(arrivedMicros * 1000.0),
                                                kInputLatency});
        delivered += bufferFrames;
        while (engine->step()) {
        }
        FrameActivation frame;
        while (engine->pop(frame)) {
            // Frame k is centred on internal sample k · hop, which is device frame k · hop · 48/22.05.
            const double centre = static_cast<double>(frame.frameIndex) *
                                  static_cast<double>(kHopSize) * kDeviceRate / kInternalSampleRate;
            const double hop = static_cast<double>(kHopSize) * kDeviceRate / kInternalSampleRate;
            if (gap.seconds > 0.0 && centre < static_cast<double>(gap.at) &&
                centre + hop >= static_cast<double>(gap.at)) {
                continue;
            }
            errors.push_back((static_cast<double>(frame.hostMicros) - heardMicros(centre)) / 1000.0);
        }
    }
    return errors;
}

double worst(const std::vector<double>& errors, std::size_t from = 0) {
    double out = 0.0;
    for (std::size_t i = from; i < errors.size(); ++i) {
        out = std::max(out, std::abs(errors[i]));
    }
    return out;
}

double mean(const std::vector<double>& errors) {
    double sum = 0.0;
    for (const double e : errors) {
        sum += e;
    }
    return errors.empty() ? 0.0 : sum / static_cast<double>(errors.size());
}

} // namespace

TEST_CASE("a frame is stamped with when its audio was at the input, whatever the buffer size",
          "[audio][timing]") {
    // Each hop used to be stamped at the moment the callback that finished it came, against its
    // first sample: the hop's own length late, plus the input latency, plus wherever in the
    // buffer it happened to end — and the hops one callback finished all shared that moment, so
    // the line through them sat later the more a buffer held (+10 ms at two a callback, +30 at
    // four, simulated 2026-10-07). A change of buffer size moved every output.
    const std::size_t frames = GENERATE(Catch::Generators::as<std::size_t>{}, 64, 256, 1024, 4096);
    INFO(frames << "-frame buffers");
    const std::vector<double> errors = stampErrors(frames, 12 * kSecond / frames);
    REQUIRE(errors.size() > 400);
    // Past the first second, which the line is still settling in.
    CHECK(worst(errors, 50) < 1.0);
    CHECK(std::abs(mean(errors)) < 0.5);
}

TEST_CASE("a dropout does not move the stamps after it", "[audio][timing]") {
    // A 40 ms dropout four seconds in. Uncounted, the sample clock stood still across it while the
    // room's went on, and the line fitted to the two put every beat after it 40 ms early, then
    // 13 ms late seven seconds on, for the ten seconds it took to forget it (simulated
    // 2026-10-07). Counted in, the line runs straight through.
    const std::size_t at = 4 * kSecond;
    const std::vector<double> errors = stampErrors(512, 16 * kSecond / 512, Gap{at, 0.040});
    REQUIRE(errors.size() > 600);
    CHECK(worst(errors, 50) < 1.5);
}

TEST_CASE("a loopback paused for half a minute stamps its next beats where they are",
          "[audio][timing]") {
    // A WASAPI loopback sends nothing at all while nothing plays, and the stream is kept open
    // through it rather than reopened. Thirty seconds of nothing used to stamp the first beats
    // after it thirty seconds early.
    const std::size_t at = 4 * kSecond;
    const std::vector<double> errors = stampErrors(480, 12 * kSecond / 480, Gap{at, 30.0});
    REQUIRE(errors.size() > 400);
    CHECK(worst(errors, 50) < 1.5);
}

TEST_CASE("Link stamps on its own clock, moved there from the steady clock", "[audio][timing][link]") {
    // The audio thread says when a buffer was heard on `std::chrono::steady_clock`; the outputs
    // measure a beat's moment on Link's clock. The same count from the same zero on Windows, and
    // two different clocks on Linux — either way a moment observed comes back on Link's.
    takt4::output::LinkSession link(120.0);
    const auto steadyMicros = [] {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    const std::int64_t offset = link.now().count() - steadyMicros();
    const std::int64_t heard = steadyMicros() - 5'000;
    link.observe(0.0, heard);
    const std::int64_t stamped = link.hostMicrosForSample(0.0);
    CHECK(std::abs(stamped - (heard + offset)) < 200);
    // And a sample a second on, on the nominal line until the line has points enough to fit.
    CHECK(std::abs(link.hostMicrosForSample(kInternalSampleRate) - (stamped + 1'000'000)) <= 1);
}
