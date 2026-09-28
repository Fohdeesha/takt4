#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/hop_processor.hpp"
#include "core/audio/input_pipeline.hpp"
#include "core/audio/stereo_check.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <functional>
#include <random>
#include <vector>

using Catch::Approx;
using takt4::audio::ChannelPicker;
using takt4::audio::ChannelSelection;
using takt4::audio::InputDevice;
using takt4::audio::StereoCheck;
using takt4::audio::StereoSums;
using Verdict = takt4::audio::StereoCheck::Verdict;

namespace {

InputDevice twoInputs() {
    InputDevice device;
    device.name = "takt4 test stereo";
    device.hostApi = takt4::audio::HostApiKind::Wasapi; // sliced in software
    device.maxInputChannels = 2;
    device.defaultSampleRate = 48000.0;
    return device;
}

/// `seconds` of a two-input device at 48 kHz, each frame made by `frame`, run through the picker
/// the stream uses and looked at by `check` every 33 ms, the way the window looks. The last look.
StereoCheck::Reading play(StereoCheck& check, double seconds,
                          const std::function<std::pair<float, float>(std::size_t)>& frame,
                          StereoSums& sums, double& now) {
    const InputDevice device = twoInputs();
    const ChannelPicker picker(device, ChannelSelection::pair(0, 1), false);
    const std::size_t block = 1584; // 33 ms
    std::vector<float> interleaved(block * 2);
    StereoCheck::Reading last;
    static std::size_t sample = 0;
    for (double t = 0.0; t < seconds; t += 0.033) {
        for (std::size_t i = 0; i < block; ++i) {
            const auto [left, right] = frame(sample++);
            interleaved[2 * i] = left;
            interleaved[2 * i + 1] = right;
        }
        picker.addPairSums(interleaved.data(), block, sums);
        now += 0.033;
        last = check.observe(sums, now);
    }
    return last;
}

} // namespace

TEST_CASE("a stereo pair is judged by how its two sides go together", "[audio][stereo]") {
    // 2026-09-28: takt4 hears a stereo feed as the average of its sides, and an average goes
    // wrong in ways a single side does not. Music here is noise shared by both sides with a
    // little of each side's own, at about -20 dBFS: correlation near 0.9, where measured music
    // sits (median 0.83 over GiantSteps' 664 tracks).
    std::mt19937 random(20260928);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    StereoCheck check;
    StereoSums sums;
    double now = 0.0;

    SECTION("a healthy pair is fine") {
        const auto music = [&](std::size_t) {
            const float shared = noise(random);
            return std::pair<float, float>{shared + 0.3f * noise(random),
                                           shared + 0.3f * noise(random)};
        };
        StereoCheck::Reading reading = play(check, 1.0, music, sums, now);
        CHECK(reading.verdict == Verdict::Quiet); // not a window's worth yet
        reading = play(check, 12.0, music, sums, now);
        CHECK(reading.verdict == Verdict::Fine);
        CHECK(reading.correlation > 0.8);
        CHECK(reading.leftDb == Approx(-19.6).margin(1.0));
    }

    SECTION("one leg wired backwards is out of phase") {
        // Measured on the 23 electronic tracks: beat F 0.84 to 0.70, downbeat F 0.66 to 0.43.
        const auto flipped = [&](std::size_t) {
            const float shared = noise(random);
            return std::pair<float, float>{shared + 0.3f * noise(random),
                                           -(shared + 0.3f * noise(random))};
        };
        const StereoCheck::Reading reading = play(check, 4.0, flipped, sums, now);
        CHECK(reading.verdict == Verdict::OutOfPhase);
        CHECK(reading.correlation < -0.8);
        CHECK(StereoCheck::describe(reading.verdict, "In 11", "In 12").find("out of phase") !=
              std::string::npos);
    }

    SECTION("a side with nothing on it is said, and which") {
        const auto rightDead = [&](std::size_t) {
            return std::pair<float, float>{noise(random), 0.0f};
        };
        CHECK(play(check, 4.0, rightDead, sums, now).verdict == Verdict::RightSilent);
        const auto leftDead = [&](std::size_t) {
            return std::pair<float, float>{0.0f, noise(random)};
        };
        CHECK(play(check, 4.0, leftDead, sums, now).verdict == Verdict::LeftSilent);
    }

    SECTION("two inputs that share nothing are unrelated, once it has lasted") {
        const auto strangers = [&](std::size_t) {
            return std::pair<float, float>{noise(random), noise(random)};
        };
        CHECK(play(check, 4.0, strangers, sums, now).verdict == Verdict::Fine); // not long enough
        CHECK(play(check, 6.0, strangers, sums, now).verdict == Verdict::Unrelated);
    }

    SECTION("nothing playing is not judged") {
        const auto silence = [](std::size_t) { return std::pair<float, float>{0.0f, 0.0f}; };
        CHECK(play(check, 5.0, silence, sums, now).verdict == Verdict::Quiet);
    }
}

namespace {

class NoHops final : public takt4::audio::HopProcessor {
public:
    void processHop(const float*, std::uint64_t) noexcept override {}
};

} // namespace

TEST_CASE("the input pipeline adds up a pair's two sides for the stereo check, and a single "
          "input's nothing",
          "[audio][stereo]") {
    const InputDevice device = twoInputs();
    NoHops hops;
    const ChannelPicker pair(device, ChannelSelection::pair(0, 1), false);
    takt4::audio::InputPipeline stereo(pair, 48000.0, hops);
    std::vector<float> block(480 * 2);
    for (std::size_t i = 0; i < 480; ++i) {
        block[2 * i] = 0.5f;
        block[2 * i + 1] = -0.25f;
    }
    stereo.process(block.data(), 480);
    stereo.process(block.data(), 480);
    const StereoSums sums = stereo.stereoSums();
    CHECK(sums.frames == 960);
    CHECK(sums.left == Approx(960 * 0.25));
    CHECK(sums.right == Approx(960 * 0.0625));
    CHECK(sums.both == Approx(960 * -0.125));

    const ChannelPicker one(device, ChannelSelection::single(0), false);
    takt4::audio::InputPipeline mono(one, 48000.0, hops);
    mono.process(block.data(), 480);
    CHECK(mono.stereoSums().frames == 0);
}
