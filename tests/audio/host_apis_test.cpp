#include "core/audio/host_apis.hpp"
#include "core/audio/portaudio_session.hpp"

#include "core/audio/devices.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>

TEST_CASE("PortAudio initialises and enumerates host APIs", "[audio]") {
    const auto apis = takt4::audio::listHostApis();

    for (const auto& api : apis) {
        INFO("host API " << api.index << ": " << api.name << " (" << api.deviceCount << " devices)");
        CHECK(api.index >= 0);
        CHECK_FALSE(api.name.empty());
        CHECK(api.deviceCount >= 0);
    }
}

#if defined(_WIN32)

namespace {

bool testsWantTheRig() {
    std::size_t length = 0;
    return getenv_s(&length, nullptr, 0, "TAKT4_TEST_HARDWARE") == 0 && length > 0;
}

} // namespace

TEST_CASE("the ASIO host is built in", "[audio][hardware]") {
    // The whole point of vendoring the ASIO SDK. PortAudio registers the ASIO host API
    // even on a machine with no ASIO drivers installed; it simply has zero devices.
    //
    // [hardware] because asking loads and initialises every ASIO driver on the machine.
    if (!testsWantTheRig()) {
        SKIP("ASIO is left alone unless TAKT4_TEST_HARDWARE is set (the -all test presets)");
    }
    const auto apis = takt4::audio::listHostApis();
    const bool hasAsio =
        std::any_of(apis.begin(), apis.end(), [](const auto& api) { return api.isAsio; });
    CHECK(hasAsio);
}

TEST_CASE("a test process leaves ASIO alone unless it is run for the rig", "[audio]") {
    // The audit's T2. Listing ASIO devices loads every installed ASIO driver into the process
    // and initialises it — and every window test builds a tracker that lists devices, so a
    // plain `ctest` did that about sixty times over, on the machine a show may be running
    // from. The test binaries now start with TAKT4_NO_ASIO set (tests/support/
    // crt_dialogs.cpp), and PortAudio's ASIO host, patched (cmake/pa_asio_patch.cmake),
    // then declines to initialise at all. What is checked is the effect: no ASIO host, and
    // not one ASIO device in the list a tracker picks from.
    if (testsWantTheRig()) {
        SKIP("run for the rig: ASIO is wanted here");
    }
    const auto apis = takt4::audio::listHostApis();
    for (const auto& api : apis) {
        INFO("host API " << api.index << ": " << api.name);
        CHECK_FALSE(api.isAsio);
    }
    const takt4::audio::PortAudioSession session;
    for (const auto& device : takt4::audio::listInputDevices(session)) {
        INFO(device.hostApiName << ": " << device.name);
        CHECK(device.hostApi != takt4::audio::HostApiKind::Asio);
    }
}

#endif

TEST_CASE("PortAudio sessions nest", "[audio]") {
    // listHostApis() opens its own session inside this one; PortAudio's reference
    // counting must keep the outer session valid after the inner one terminates.
    const takt4::audio::PortAudioSession outer;
    const auto inner = takt4::audio::listHostApis();
    const auto again = takt4::audio::listHostApis();
    CHECK(inner.size() == again.size());
}
