#include "core/audio/devices.hpp"
#include "core/audio/portaudio_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

using takt4::audio::HostApiKind;

TEST_CASE("Input devices enumerate consistently", "[audio]") {
    // Machines running this may have no audio hardware at all (CI runners); the list
    // being empty is fine. Whatever is listed must be internally consistent.
    const takt4::audio::PortAudioSession session;
    const auto devices = takt4::audio::listInputDevices(session);

    for (const auto& device : devices) {
        INFO("device " << device.index << ": " << device.hostApiName << " / " << device.name << " ("
                       << device.maxInputChannels << " in @ " << device.defaultSampleRate << " Hz)");
        CHECK(device.index >= 0);
        CHECK(device.hostApiIndex >= 0);
        CHECK_FALSE(device.hostApiName.empty());
        CHECK_FALSE(device.name.empty());
        CHECK(device.maxInputChannels > 0);
        CHECK(device.defaultSampleRate > 0.0);
        CHECK(device.defaultLowInputLatency >= 0.0);
        CHECK(device.defaultHighInputLatency >= device.defaultLowInputLatency);
        if (!device.channelNames.empty()) {
            CHECK(device.channelNames.size() == static_cast<std::size_t>(device.maxInputChannels));
        }
        if (device.isLoopback) {
            CHECK(device.hostApi == HostApiKind::Wasapi);
        }
        if (device.hostApi == HostApiKind::Asio) {
            // PortAudio reads the names from the driver when it loads it; a driver that
            // gave no names would be surprising enough to want to know about.
            CHECK_FALSE(device.channelNames.empty());
        }
    }
}

TEST_CASE("Host API kinds are named and classified", "[audio]") {
    CHECK(std::string(takt4::audio::toString(HostApiKind::Asio)) == "ASIO");
    CHECK(std::string(takt4::audio::toString(HostApiKind::Other)) == "other");
    CHECK(takt4::audio::hasNativeChannelSelection(HostApiKind::Asio));
    CHECK(takt4::audio::hasNativeChannelSelection(HostApiKind::CoreAudio));
    CHECK_FALSE(takt4::audio::hasNativeChannelSelection(HostApiKind::Wasapi));
    CHECK_FALSE(takt4::audio::hasNativeChannelSelection(HostApiKind::Alsa));
    CHECK_FALSE(takt4::audio::hasNativeChannelSelection(HostApiKind::Jack));
    CHECK_FALSE(takt4::audio::hasNativeChannelSelection(HostApiKind::Other));
}
