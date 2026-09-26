#include "core/output/midi_ports.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("RtMidi was compiled with the platform's APIs", "[midi]") {
    const auto apis = takt4::output::compiledMidiApis();
    REQUIRE_FALSE(apis.empty());

    const auto has = [&](std::string_view name) {
        return std::any_of(apis.begin(), apis.end(), [&](const auto& api) { return api.name == name; });
    };

    for (const auto& api : apis) {
        INFO("MIDI API: " << api.name << " (" << api.displayName << ")");
        CHECK_FALSE(api.displayName.empty());
    }

#if defined(_WIN32)
    CHECK(has("winmm"));
#elif defined(__APPLE__)
    CHECK(has("core"));
#else
    CHECK(has("alsa"));
    // Compiled in whether or not a JACK server is running; see cmake/deps.cmake.
    CHECK(has("jack"));
#endif
    CHECK_FALSE(has("dummy"));
}

TEST_CASE("MIDI output ports can be listed without throwing", "[midi]") {
    CHECK_NOTHROW(takt4::output::listMidiOutputPorts());
}

TEST_CASE("a MIDI port is found again by name when it comes back at another number", "[midi]") {
    // The audit of 2026-09-25, C1. RtMidi's Windows backend ends every port's name with its place
    // in the list, so a device unplugged and plugged back in — or a machine that lists its ports
    // in another order — comes back under another name, and the one it was saved under matched
    // nothing, or matched "USB MIDI 10" by containing "USB MIDI 1".
    using takt4::output::findMidiPort;
    using takt4::output::midiPortBaseName;

    CHECK(midiPortBaseName("USB MIDI 3") == "USB MIDI");
    CHECK(midiPortBaseName("USB MIDI 10") == "USB MIDI");
    CHECK(midiPortBaseName("Launchpad Pro MK3 2") == "Launchpad Pro MK3");
    CHECK(midiPortBaseName("808") == "808"); // no space before it: the name itself
    CHECK(midiPortBaseName("Desk") == "Desk");
    CHECK(midiPortBaseName("") == "");

    const std::vector<std::string> ports = {"Microsoft GS Wavetable Synth 0", "loopMIDI Port 1",
                                            "USB MIDI 10", "USB MIDI 11"};
    // The exact name first.
    CHECK(findMidiPort(ports, "USB MIDI 11") == 3u);
    // The same device at another number, rather than the first name containing the old one.
    CHECK(findMidiPort(ports, "loopMIDI Port 4") == 1u);
    CHECK(findMidiPort(ports, "USB MIDI 1") == 2u);
    // A part of a name, which is what the console is typed: still the first that contains it.
    CHECK(findMidiPort(ports, "Wavetable") == 0u);
    CHECK(findMidiPort(ports, "loop") == 1u);
    // An index.
    CHECK(findMidiPort(ports, "2") == 2u);
    CHECK_FALSE(findMidiPort(ports, "4").has_value());
    CHECK_FALSE(findMidiPort(ports, "99999999999999999999").has_value());
    // Nothing.
    CHECK_FALSE(findMidiPort(ports, "Desk 1").has_value());
    CHECK_FALSE(findMidiPort(ports, "").has_value());
    CHECK_FALSE(findMidiPort({}, "USB MIDI 1").has_value());
}
