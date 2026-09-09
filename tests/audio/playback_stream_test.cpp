#include "core/audio/playback_stream.hpp"

#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <thread>
#include <vector>

using Catch::Approx;
using takt4::audio::kInternalSampleRate;

namespace {

/// Two tones well inside the band, so a shift of even a fraction of a millisecond shows.
std::vector<float> twoTones(double rate, std::size_t frames) {
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        out[i] = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * t)) +
                 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1234.0 * t));
    }
    return out;
}

} // namespace

TEST_CASE("Output devices enumerate consistently", "[audio]") {
    const takt4::audio::PortAudioSession session;
    const auto devices = takt4::audio::listOutputDevices(session);
    for (const auto& device : devices) {
        INFO("device " << device.index << ": " << device.hostApiName << " / " << device.name);
        CHECK(device.index >= 0);
        CHECK(device.hostApiIndex >= 0);
        CHECK_FALSE(device.hostApiName.empty());
        CHECK_FALSE(device.name.empty());
        CHECK(device.maxOutputChannels > 0);
        CHECK(device.defaultSampleRate > 0.0);
        CHECK(device.defaultLowOutputLatency >= 0.0);
    }
    const auto chosen = takt4::audio::defaultOutputDevice(session);
    if (devices.empty()) {
        CHECK_FALSE(chosen.has_value());
    } else {
        REQUIRE(chosen.has_value());
        CHECK(std::any_of(devices.begin(), devices.end(),
                          [&](const auto& d) { return d.index == chosen->index; }));
    }
}

TEST_CASE("Resampling for playback keeps the source's time", "[audio]") {
    const double outRate = 48000.0;
    const std::vector<float> source = twoTones(kInternalSampleRate, 22050);
    const std::vector<float> out = takt4::audio::resampleForPlayback(source, kInternalSampleRate, outRate);
    REQUIRE(out.size() == 48000);
    // Output sample k is the source at time k / outRate, so it matches the same tones
    // synthesised at the output rate directly — away from the ends, where the filter's
    // edge is.
    const std::vector<float> want = twoTones(outRate, 48000);
    double worst = 0.0;
    for (std::size_t i = 2400; i < 45600; ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(out[i] - want[i])));
    }
    CHECK(worst < 0.02);
}

TEST_CASE("Resampling to the same rate is a copy", "[audio]") {
    const std::vector<float> source = twoTones(kInternalSampleRate, 1000);
    CHECK(takt4::audio::resampleForPlayback(source, kInternalSampleRate, kInternalSampleRate) == source);
}

TEST_CASE("A silent buffer plays to its end and the clock reaches it", "[audio]") {
    const takt4::audio::PortAudioSession session;
    const auto device = takt4::audio::defaultOutputDevice(session);
    if (!device) {
        SUCCEED("no output device on this machine");
        return;
    }
    // Three tenths of a second of silence: nothing to hear, and a clock to read.
    const std::vector<float> silence(static_cast<std::size_t>(0.3 * kInternalSampleRate), 0.0f);
    takt4::audio::PlaybackStream player(session, *device, silence, kInternalSampleRate);
    CHECK(player.durationSeconds() == Approx(0.3).margin(0.001));
    CHECK(player.positionSeconds() == Approx(0.0));
    CHECK(player.channels() >= 1);
    CHECK(player.channels() <= 2);
    CHECK_FALSE(player.finished());

    player.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!player.finished() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(player.finished());
    CHECK(player.positionSeconds() == Approx(0.3).margin(0.05));
    player.stop();
    CHECK_FALSE(player.running());
}

TEST_CASE("Playback can start part-way in, with absolute positions", "[audio]") {
    const takt4::audio::PortAudioSession session;
    const auto device = takt4::audio::defaultOutputDevice(session);
    if (!device) {
        SUCCEED("no output device on this machine");
        return;
    }
    const std::vector<float> silence(static_cast<std::size_t>(2.0 * kInternalSampleRate), 0.0f);
    takt4::audio::PlaybackStream player(session, *device, silence, kInternalSampleRate, 1.8);
    CHECK(player.positionSeconds() == Approx(1.8).margin(0.001));
    player.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!player.finished() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(player.finished());
    CHECK(player.positionSeconds() == Approx(2.0).margin(0.05));
}
