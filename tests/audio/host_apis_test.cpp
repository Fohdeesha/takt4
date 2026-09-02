#include "core/audio/host_apis.hpp"
#include "core/audio/portaudio_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_CASE("PortAudio initialises and enumerates host APIs", "[audio]") {
    const auto apis = takt4::audio::listHostApis();

    for (const auto& api : apis) {
        INFO("host API " << api.index << ": " << api.name << " (" << api.deviceCount << " devices)");
        CHECK(api.index >= 0);
        CHECK_FALSE(api.name.empty());
        CHECK(api.deviceCount >= 0);
    }

#if defined(_WIN32)
    // The whole point of vendoring the ASIO SDK. PortAudio registers the ASIO host API
    // even on a machine with no ASIO drivers installed; it simply has zero devices.
    const bool hasAsio =
        std::any_of(apis.begin(), apis.end(), [](const auto& api) { return api.isAsio; });
    CHECK(hasAsio);
#endif
}

TEST_CASE("PortAudio sessions nest", "[audio]") {
    // listHostApis() opens its own session inside this one; PortAudio's reference
    // counting must keep the outer session valid after the inner one terminates.
    const takt4::audio::PortAudioSession outer;
    const auto inner = takt4::audio::listHostApis();
    const auto again = takt4::audio::listHostApis();
    CHECK(inner.size() == again.size());
}
