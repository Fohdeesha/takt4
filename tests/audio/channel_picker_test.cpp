#include "core/audio/channel_picker.hpp"

#include "core/audio/devices.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::audio::ChannelPicker;
using takt4::audio::ChannelSelection;
using takt4::audio::HostApiKind;
using takt4::audio::InputDevice;
using takt4::audio::PickMode;

namespace {

InputDevice device(HostApiKind kind, int channels) {
    InputDevice d;
    d.index = 3;
    d.hostApiIndex = 0;
    d.hostApi = kind;
    d.hostApiName = takt4::audio::toString(kind);
    d.name = "test interface";
    d.maxInputChannels = channels;
    d.defaultSampleRate = 48000.0;
    return d;
}

// Interleaved test signal: channel c carries the constant value c + 1 on every frame,
// so a picked sample says exactly which channel it came from.
std::vector<float> labelledFrames(int channels, std::size_t frames) {
    std::vector<float> out(static_cast<std::size_t>(channels) * frames);
    for (std::size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            out[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] =
                static_cast<float>(c + 1);
        }
    }
    return out;
}

} // namespace

TEST_CASE("ChannelPicker picks natively where the host API allows it", "[audio]") {
    const InputDevice asio = device(HostApiKind::Asio, 18);

    SECTION("single channel") {
        const ChannelPicker picker(asio, ChannelSelection::single(6));
        CHECK(picker.mode() == PickMode::Native);
        CHECK(picker.streamChannelCount() == 1);
        REQUIRE(picker.nativeSelectors().size() == 1);
        CHECK(picker.nativeSelectors()[0] == 6);

        // The stream then carries only channel 7's samples; they pass straight through.
        const std::vector<float> stream = {0.25f, -0.5f, 1.0f};
        std::vector<float> mono(3);
        picker.pickMono(stream.data(), 3, mono.data());
        CHECK(mono == stream);
    }

    SECTION("pair") {
        const ChannelPicker picker(asio, ChannelSelection::pair(2, 9));
        CHECK(picker.mode() == PickMode::Native);
        CHECK(picker.streamChannelCount() == 2);
        REQUIRE(picker.nativeSelectors().size() == 2);
        CHECK(picker.nativeSelectors()[0] == 2);
        CHECK(picker.nativeSelectors()[1] == 9);

        const std::vector<float> stream = {1.0f, 0.0f, 0.5f, -0.5f, -1.0f, -1.0f};
        std::vector<float> mono(3);
        picker.pickMono(stream.data(), 3, mono.data());
        CHECK_THAT(mono[0], WithinAbs(0.5, 1e-7));
        CHECK_THAT(mono[1], WithinAbs(0.0, 1e-7));
        CHECK_THAT(mono[2], WithinAbs(-1.0, 1e-7));
    }

    SECTION("forced software slice") {
        const ChannelPicker picker(asio, ChannelSelection::single(6), /*allowNative=*/false);
        CHECK(picker.mode() == PickMode::Software);
        CHECK(picker.streamChannelCount() == 18);
        CHECK(picker.nativeSelectors().empty());
    }
}

TEST_CASE("ChannelPicker slices in software on every other host API", "[audio]") {
    for (const HostApiKind kind : {HostApiKind::Wasapi, HostApiKind::Alsa, HostApiKind::Jack,
                                   HostApiKind::Other}) {
        INFO(takt4::audio::toString(kind));
        const InputDevice dev = device(kind, 24);
        const ChannelPicker picker(dev, ChannelSelection::single(6));
        CHECK(picker.mode() == PickMode::Software);
        CHECK(picker.streamChannelCount() == 24);
        CHECK(picker.nativeSelectors().empty());
    }
}

TEST_CASE("software slice takes exactly the selected channel out of the interleave", "[audio]") {
    constexpr int kChannels = 18;
    constexpr std::size_t kFrames = 100;
    const InputDevice dev = device(HostApiKind::Wasapi, kChannels);
    const std::vector<float> block = labelledFrames(kChannels, kFrames);
    std::vector<float> mono(kFrames, -1.0f);

    SECTION("input 7") {
        const ChannelPicker picker(dev, ChannelSelection::single(6));
        picker.pickMono(block.data(), kFrames, mono.data());
        for (std::size_t i = 0; i < kFrames; ++i) {
            REQUIRE(mono[i] == 7.0f);
        }
    }

    SECTION("first and last channel") {
        for (const int channel : {0, kChannels - 1}) {
            const ChannelPicker picker(dev, ChannelSelection::single(channel));
            picker.pickMono(block.data(), kFrames, mono.data());
            for (std::size_t i = 0; i < kFrames; ++i) {
                REQUIRE(mono[i] == static_cast<float>(channel + 1));
            }
        }
    }

    SECTION("pair 3 + 12 averages") {
        const ChannelPicker picker(dev, ChannelSelection::pair(2, 11));
        picker.pickMono(block.data(), kFrames, mono.data());
        for (std::size_t i = 0; i < kFrames; ++i) {
            REQUIRE_THAT(mono[i], WithinAbs(7.5, 1e-6)); // (3 + 12) / 2
        }
    }

    SECTION("zero frames is a no-op") {
        const ChannelPicker picker(dev, ChannelSelection::single(6));
        picker.pickMono(block.data(), 0, mono.data());
        CHECK(mono[0] == -1.0f);
    }
}

TEST_CASE("ChannelPicker rejects selections outside the device", "[audio]") {
    const InputDevice dev = device(HostApiKind::Wasapi, 8);
    CHECK_THROWS_AS(ChannelPicker(dev, ChannelSelection::single(8)), std::invalid_argument);
    CHECK_THROWS_AS(ChannelPicker(dev, ChannelSelection::single(-1)), std::invalid_argument);
    CHECK_THROWS_AS(ChannelPicker(dev, ChannelSelection::pair(0, 8)), std::invalid_argument);
    CHECK_THROWS_AS(ChannelPicker(dev, ChannelSelection::pair(3, 3)), std::invalid_argument);
    ChannelSelection bad;
    bad.count = 3;
    CHECK_THROWS_AS(ChannelPicker(dev, bad), std::invalid_argument);

    // The message names the channel the way the interface labels it.
    try {
        [[maybe_unused]] const ChannelPicker picker(dev, ChannelSelection::single(8));
        FAIL("expected an exception");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("channel 9") != std::string::npos);
        CHECK(std::string(e.what()).find("8 inputs") != std::string::npos);
    }
}

TEST_CASE("pickMono does not touch the heap", "[audio][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    const InputDevice dev = device(HostApiKind::Wasapi, 18);
    const ChannelPicker single(dev, ChannelSelection::single(6));
    const ChannelPicker pair(dev, ChannelSelection::pair(6, 7));
    const std::vector<float> block = labelledFrames(18, 512);
    std::vector<float> mono(512);

    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope realtime;
        single.pickMono(block.data(), 512, mono.data());
        pair.pickMono(block.data(), 512, mono.data());
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(takt4::rt::violationCount() == before);
}
