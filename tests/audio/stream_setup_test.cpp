#include "core/audio/stream_setup.hpp"

#include "core/audio/portaudio_session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <span>
#include <stdexcept>

using Catch::Matchers::ContainsSubstring;
using takt4::audio::HostApiKind;
using takt4::audio::InputDevice;
using takt4::audio::PortAudioError;
namespace detail = takt4::audio::detail;

// What every input stream is opened with: the parameters PortAudio is handed, the rate, and the
// error it throws. Built from a device description alone, so none of it opens anything (the audit
// of 2026-09-25, coverage gap 16 — stream_setup.cpp had no test of its own).

namespace {

InputDevice deviceOn(HostApiKind api) {
    InputDevice device;
    device.index = 7;
    device.hostApi = api;
    device.name = "an interface";
    device.maxInputChannels = 8;
    device.defaultSampleRate = 48000.0;
    device.defaultLowInputLatency = 0.012;
    device.defaultHighInputLatency = 0.090;
    return device;
}

} // namespace

TEST_CASE("a stream's parameters name the device, the channels and float samples",
          "[audio]") {
    PaStreamParameters params;
    detail::HostStreamInfo info;
    const InputDevice device = deviceOn(HostApiKind::Wasapi);

    detail::setupInputParameters(params, info, device, 2, {}, -1.0);
    CHECK(params.device == 7);
    CHECK(params.channelCount == 2);
    CHECK(params.sampleFormat == paFloat32);
    // No latency asked for is the device's own low one — not zero, which PortAudio would take
    // as "as small as you can", and not its high one.
    CHECK(params.suggestedLatency == 0.012);
    CHECK(params.hostApiSpecificStreamInfo == nullptr);

    detail::setupInputParameters(params, info, device, 1, {}, 0.050);
    CHECK(params.suggestedLatency == 0.050);
    CHECK(params.channelCount == 1);
}

TEST_CASE("native channel selection is asked of the host that has it, and refused elsewhere",
          "[audio]") {
    PaStreamParameters params;
    detail::HostStreamInfo info;

#if defined(_WIN32)
    SECTION("ASIO opens only the chosen inputs, in the order chosen") {
        const InputDevice asio = deviceOn(HostApiKind::Asio);
        const std::array<int, 2> pair{5, 2};
        detail::setupInputParameters(params, info, asio, 2, pair, -1.0);
        REQUIRE(params.hostApiSpecificStreamInfo == &info.asio);
        CHECK(info.asio.size == sizeof(PaAsioStreamInfo));
        CHECK(info.asio.hostApiType == paASIO);
        CHECK(info.asio.version == 1);
        CHECK(info.asio.flags == paAsioUseChannelSelectors);
        REQUIRE(info.asio.channelSelectors == info.selectors.data());
        CHECK(info.asio.channelSelectors[0] == 5);
        CHECK(info.asio.channelSelectors[1] == 2);

        // And a later stream on another host is not handed the ASIO block left from this one.
        detail::setupInputParameters(params, info, deviceOn(HostApiKind::Wasapi), 1, {}, -1.0);
        CHECK(params.hostApiSpecificStreamInfo == nullptr);
        CHECK(info.pointer == nullptr);
    }

    SECTION("selectors that are not one per channel are a mistake, not a guess") {
        const InputDevice asio = deviceOn(HostApiKind::Asio);
        const std::array<int, 1> one{3};
        CHECK_THROWS_AS(detail::setupInputParameters(params, info, asio, 2, one, -1.0),
                        std::logic_error);
        const std::array<int, 3> three{1, 2, 3};
        CHECK_THROWS_AS(detail::setupInputParameters(params, info, asio, 3, three, -1.0),
                        std::logic_error);
    }
#endif

    SECTION("a host without native selection is never handed selectors") {
        const std::array<int, 1> one{3};
        CHECK_THROWS_AS(detail::setupInputParameters(params, info, deviceOn(HostApiKind::Wasapi),
                                                     1, one, -1.0),
                        std::logic_error);
        CHECK_THROWS_AS(detail::setupInputParameters(params, info, deviceOn(HostApiKind::Other), 1,
                                                     one, -1.0),
                        std::logic_error);
    }
}

TEST_CASE("a stream opens at the rate asked for, or else the device's own", "[audio]") {
    const InputDevice device = deviceOn(HostApiKind::Asio);
    CHECK(detail::resolveSampleRate(device, 0.0) == 48000.0);
    CHECK(detail::resolveSampleRate(device, -1.0) == 48000.0);
    CHECK(detail::resolveSampleRate(device, 44100.0) == 44100.0);
}

TEST_CASE("PortAudio's refusal is thrown with its own words and its code", "[audio]") {
    try {
        detail::throwPortAudioError("Pa_OpenStream failed", paInvalidSampleRate);
        FAIL("nothing was thrown");
    } catch (const PortAudioError& error) {
        CHECK(error.code() == paInvalidSampleRate);
        CHECK_THAT(error.what(), ContainsSubstring("Pa_OpenStream failed: "));
        CHECK_THAT(error.what(), ContainsSubstring(Pa_GetErrorText(paInvalidSampleRate)));
    }
}
