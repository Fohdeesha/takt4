#include "core/output/midi_ports.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

TEST_CASE("RtMidi was compiled with the platform's native API", "[midi]") {
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
#endif
    CHECK_FALSE(has("dummy"));
}

TEST_CASE("MIDI output ports can be listed without throwing", "[midi]") {
    CHECK_NOTHROW(takt4::output::listMidiOutputPorts());
}
