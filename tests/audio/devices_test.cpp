#include "core/audio/devices.hpp"
#include "core/audio/portaudio_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

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

TEST_CASE("an input Windows renamed when it came back on another port is found by its own name",
          "[audio]") {
    // Plugged back in elsewhere, a USB interface comes back as "Microphone (2- USB Audio CODEC)",
    // and the input remembered — and the outage reopening it — looked for it by its old name for
    // ever. Two of a kind cannot be told apart that way, and are not guessed between.
    using takt4::audio::baseInputName;
    using takt4::audio::findInput;
    using takt4::audio::InputDevice;
    CHECK(baseInputName("Microphone (2- USB Audio CODEC)") == "Microphone (USB Audio CODEC)");
    CHECK(baseInputName("Microphone (12- USB Audio CODEC)") == "Microphone (USB Audio CODEC)");
    CHECK(baseInputName("2- USB Audio CODEC") == "USB Audio CODEC");
    CHECK(baseInputName("Microphone (USB Audio CODEC)") == "Microphone (USB Audio CODEC)");
    // A number that is not Windows' doing is left alone.
    CHECK(baseInputName("In 1-2 (MOTU Pro Audio)") == "In 1-2 (MOTU Pro Audio)");
    CHECK(baseInputName("Line (2-channel input)") == "Line (2-channel input)");
    CHECK(baseInputName("") == "");

    const auto device = [](const char* name, const char* api) {
        InputDevice made;
        made.name = name;
        made.hostApiName = api;
        return made;
    };
    const std::vector<InputDevice> machine = {
        device("Microphone (Realtek Audio)", "Windows WASAPI"),
        device("Microphone (2- USB Audio CODEC)", "Windows WASAPI"),
        device("MOTU Pro Audio", "ASIO"),
    };
    CHECK(findInput(machine, "Microphone (2- USB Audio CODEC)", "Windows WASAPI") == 1u);
    CHECK(findInput(machine, "Microphone (USB Audio CODEC)", "Windows WASAPI") == 1u);
    CHECK(findInput(machine, "Microphone (3- USB Audio CODEC)", "Windows WASAPI") == 1u);
    CHECK(findInput(machine, "MOTU Pro Audio", "ASIO") == 2u);
    // Another host API is another input.
    CHECK_FALSE(findInput(machine, "Microphone (USB Audio CODEC)", "MME").has_value());
    CHECK_FALSE(findInput(machine, "A desk mic", "Windows WASAPI").has_value());

    // Two of them: its own name finds each, and no guess is made without it.
    const std::vector<InputDevice> pair = {
        device("Microphone (USB Audio CODEC)", "Windows WASAPI"),
        device("Microphone (2- USB Audio CODEC)", "Windows WASAPI"),
    };
    CHECK(findInput(pair, "Microphone (USB Audio CODEC)", "Windows WASAPI") == 0u);
    CHECK(findInput(pair, "Microphone (2- USB Audio CODEC)", "Windows WASAPI") == 1u);
    CHECK_FALSE(findInput(pair, "Microphone (3- USB Audio CODEC)", "Windows WASAPI").has_value());
}
