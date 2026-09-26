#include "core/audio/asio_scan.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/portaudio_session.hpp"

#include "support/scoped_env.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <tlhelp32.h>

#include <pa_asio.h>
#include <portaudio.h>
#endif

using takt4::audio::AsioScan;
using takt4::audio::AsioScannedDevice;

namespace {

AsioScan sample() {
    AsioScan scan;
    AsioScannedDevice motu;
    motu.name = "MOTU Pro Audio";
    motu.inputChannels = 2;
    motu.outputChannels = 1;
    motu.minBufferSize = 32;
    motu.maxBufferSize = 2048;
    motu.preferredBufferSize = 256;
    motu.bufferGranularity = -1; // "powers of two", which is how a driver says it
    motu.defaultSampleRate = 44100.0;
    motu.defaultLowInputLatency = 256.0 / 44100.0; // a double with no short decimal form
    motu.defaultLowOutputLatency = 0.1 + 0.2;
    motu.defaultHighInputLatency = 2048.0 / 44100.0;
    motu.defaultHighOutputLatency = 1e-300;
    // What the format uses for itself, inside names, and a name in another script.
    motu.channelNames = {"Mic\t2", "back\\slash\nand line", "Ausgang Ü"};
    scan.devices.push_back(motu);
    AsioScannedDevice empty;
    empty.name = "An output-only driver";
    scan.devices.push_back(empty);
    return scan;
}

} // namespace

TEST_CASE("an ASIO scan reads back exactly what was written and nothing cut short", "[audio]") {
    const AsioScan written = sample();
    const std::string text = takt4::audio::writeAsioScan(written);
    const AsioScan read = takt4::audio::readAsioScan(text);
    CHECK(read.problem.empty());
    CHECK(read.devices == written.devices);

    // A scanning process that falls over part way leaves a text cut short somewhere. Every
    // cut is a scan that did not finish — never a shorter list taken for the whole one.
    for (std::size_t length = 0; length < text.size(); ++length) {
        INFO("cut to " << length << " of " << text.size() << " bytes");
        const AsioScan cut = takt4::audio::readAsioScan(text.substr(0, length));
        CHECK_FALSE(cut.problem.empty());
        CHECK(cut.devices.empty());
    }
    // Nor anything after the end, nor a text that is not a scan at all.
    CHECK_FALSE(takt4::audio::readAsioScan(text + "device\tmore\n").problem.empty());
    CHECK_FALSE(takt4::audio::readAsioScan("not a scan\nend\n").problem.empty());

    // A problem the scanning process reported itself comes through, with no devices.
    AsioScan failed;
    failed.problem = "PortAudio would not start";
    const AsioScan reported = takt4::audio::readAsioScan(takt4::audio::writeAsioScan(failed));
    CHECK(reported.problem == "PortAudio would not start");
    CHECK(reported.devices.empty());
}

#if defined(_WIN32)

namespace {

using takt4::test::ScopedVariable;

/// The DLL of every ASIO driver installed, from where the ASIO SDK looks them up: each driver
/// under HKLM\SOFTWARE\ASIO names a COM class, whose InprocServer32 is the file.
std::vector<std::wstring> installedAsioDrivers() {
    std::vector<std::wstring> dlls;
    HKEY asio = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &asio) != ERROR_SUCCESS) {
        return dlls;
    }
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD nameLength = 256;
        if (RegEnumKeyExW(asio, i, name, &nameLength, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS) {
            break;
        }
        wchar_t clsid[64] = {};
        DWORD size = sizeof clsid;
        if (RegGetValueW(asio, name, L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &size) !=
            ERROR_SUCCESS) {
            continue;
        }
        const std::wstring server = L"CLSID\\" + std::wstring(clsid) + L"\\InprocServer32";
        wchar_t path[MAX_PATH * 2] = {};
        size = sizeof path;
        if (RegGetValueW(HKEY_CLASSES_ROOT, server.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path,
                         &size) == ERROR_SUCCESS) {
            dlls.emplace_back(path);
        }
    }
    RegCloseKey(asio);
    return dlls;
}

/// The installed ASIO drivers that are loaded into this process.
std::vector<std::wstring> asioDriversLoadedHere() {
    std::vector<std::wstring> loaded;
    for (const std::wstring& dll : installedAsioDrivers()) {
        if (GetModuleHandleW(dll.c_str()) != nullptr ||
            GetModuleHandleW(std::filesystem::path(dll).filename().c_str()) != nullptr) {
            loaded.push_back(dll);
        }
    }
    return loaded;
}

/// The processes this one started that are still running.
std::size_t childrenRunning() {
    std::size_t count = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE;
         more = Process32NextW(snapshot, &entry)) {
        count += entry.th32ParentProcessID == GetCurrentProcessId() ? 1 : 0;
    }
    CloseHandle(snapshot);
    return count;
}

std::size_t asioDevicesListed() {
    const PaHostApiIndex api = Pa_HostApiTypeIdToHostApiIndex(paASIO);
    const PaHostApiInfo* info = api >= 0 ? Pa_GetHostApiInfo(api) : nullptr;
    return info != nullptr ? static_cast<std::size_t>(info->deviceCount) : 0;
}

/// Forgets whatever a test's scan handed PortAudio, so the next test starts from none.
struct ForgetScan {
    ~ForgetScan() { takt4::audio::useAsioScan({}); }
};

} // namespace

TEST_CASE("an ASIO driver falling over while it is asked costs the ASIO devices and not takt4",
          "[audio]") {
    // The MOTU's fast fail, made on purpose in the scanning process before it loads anything —
    // so nothing here touches a driver, and the test can run on the rig. Until 2026-09-24 the
    // same fall in takt4's own process ended takt4.
    const ForgetScan forget;
    const ScopedVariable asio("TAKT4_NO_ASIO", nullptr);
    const ScopedVariable fall("TAKT4_TEST_ASIO_SCAN", "fall-over");
    const std::size_t childrenBefore = childrenRunning();
    {
        const takt4::audio::PortAudioSession session; // still here: that is the test
        CHECK(asioDevicesListed() == 0);
        for (const auto& device : takt4::audio::listInputDevices(session)) {
            CHECK(device.hostApi != takt4::audio::HostApiKind::Asio);
        }
        const std::string problem = takt4::audio::asioScanProblem();
        INFO("the problem: " << problem);
        CHECK(problem.find("fell over twice") != std::string::npos);
        CHECK(problem.find("0xC0000409") != std::string::npos); // what a fast fail exits with
        CHECK(problem.find("RESCAN") != std::string::npos);
        // And with no scan to go on, PortAudio loaded no driver to find out for itself.
        CHECK(asioDriversLoadedHere().empty());
    }
    CHECK(childrenRunning() == childrenBefore);
}

TEST_CASE("an ASIO driver that never answers is given up on in time", "[audio]") {
    const ForgetScan forget;
    const ScopedVariable asio("TAKT4_NO_ASIO", nullptr);
    const ScopedVariable hang("TAKT4_TEST_ASIO_SCAN", "hang");
    const std::size_t childrenBefore = childrenRunning();
    const auto start = std::chrono::steady_clock::now();
    const AsioScan scan = takt4::audio::scanAsio(std::chrono::milliseconds(2000));
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO("the problem: " << scan.problem << "; took " << took << " s");
    CHECK(scan.devices.empty());
    CHECK(scan.problem.find("did not answer within 2 seconds") != std::string::npos);
    // Asked once: a driver that hung will hang again, and each try costs the whole limit.
    CHECK(took >= 2.0);
    CHECK(took < 3.5);
    // The hung process was ended rather than left behind.
    CHECK(childrenRunning() == childrenBefore);
}

TEST_CASE("an ASIO scan is done when it has answered, though a process it started holds the pipe",
          "[audio]") {
    // The audit of 2026-09-25, L24. A driver that starts a helper process hands it the scan's
    // output pipe with everything else inheritable, and the pipe stays whole while the helper
    // runs — so the read loop found nothing to read, never found the pipe broken, and spun the
    // UI thread until the limit, then threw the answer it had away as a timeout.
    const ScopedVariable asio("TAKT4_NO_ASIO", nullptr);
    const ScopedVariable helper("TAKT4_TEST_ASIO_SCAN", "helper");
    const auto start = std::chrono::steady_clock::now();
    const AsioScan scan = takt4::audio::scanAsio(std::chrono::milliseconds(4000));
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO("the problem: " << scan.problem << "; took " << took << " s");
    CHECK(scan.problem.empty());
    CHECK(took < 2.0); // the helper holds the pipe for five
}

TEST_CASE("an ASIO scan that answered and then hung keeps its answer", "[audio]") {
    // L24's other half: the whole answer read, and then the limit reached because the process
    // never went — the answer was thrown away with the process.
    const ScopedVariable asio("TAKT4_NO_ASIO", nullptr);
    const ScopedVariable hang("TAKT4_TEST_ASIO_SCAN", "answer-then-hang");
    const std::size_t childrenBefore = childrenRunning();
    const AsioScan scan = takt4::audio::scanAsio(std::chrono::milliseconds(1500));
    INFO("the problem: " << scan.problem);
    CHECK(scan.problem.empty());
    CHECK(childrenRunning() == childrenBefore); // and the process was still ended
}

TEST_CASE("ASIO devices are listed as their drivers describe them with none loaded here",
          "[audio][hardware]") {
    // The rig's interface. Run by the -all presets only; close Resolume and Live first.
    const ForgetScan forget;
    const AsioScan scan = takt4::audio::scanAsio();
    INFO("the problem: " << scan.problem);
    REQUIRE(scan.problem.empty());
    REQUIRE_FALSE(scan.devices.empty());

    const takt4::audio::PortAudioSession session;
    CHECK(takt4::audio::asioScanProblem().empty());
    // No driver in this process: that is what the scan is for.
    CHECK(asioDriversLoadedHere().empty());

    // And PortAudio here says of each exactly what the driver said to PortAudio there.
    const PaHostApiIndex api = Pa_HostApiTypeIdToHostApiIndex(paASIO);
    REQUIRE(api >= 0);
    const PaHostApiInfo* info = Pa_GetHostApiInfo(api);
    REQUIRE(info != nullptr);
    CHECK(static_cast<std::size_t>(info->deviceCount) == scan.devices.size());
    for (int i = 0; i < info->deviceCount; ++i) {
        const PaDeviceIndex index = Pa_HostApiDeviceIndexToDeviceIndex(api, i);
        const PaDeviceInfo* here = Pa_GetDeviceInfo(index);
        REQUIRE(here != nullptr);
        INFO("device " << here->name);
        const AsioScannedDevice* there = nullptr;
        for (const AsioScannedDevice& device : scan.devices) {
            there = device.name == here->name ? &device : there;
        }
        REQUIRE(there != nullptr);
        CHECK(here->maxInputChannels == there->inputChannels);
        CHECK(here->maxOutputChannels == there->outputChannels);
        CHECK(here->defaultSampleRate == there->defaultSampleRate);
        CHECK(here->defaultLowInputLatency == there->defaultLowInputLatency);
        CHECK(here->defaultHighInputLatency == there->defaultHighInputLatency);
        CHECK(here->defaultLowOutputLatency == there->defaultLowOutputLatency);
        CHECK(here->defaultHighOutputLatency == there->defaultHighOutputLatency);
        long minimum = 0;
        long maximum = 0;
        long preferred = 0;
        long granularity = 0;
        REQUIRE(PaAsio_GetAvailableBufferSizes(index, &minimum, &maximum, &preferred,
                                               &granularity) == paNoError);
        CHECK(minimum == there->minBufferSize);
        CHECK(maximum == there->maxBufferSize);
        CHECK(preferred == there->preferredBufferSize);
        CHECK(granularity == there->bufferGranularity);
        std::vector<std::string> names;
        for (int c = 0; c < here->maxInputChannels; ++c) {
            const char* name = nullptr;
            CHECK(PaAsio_GetInputChannelName(index, c, &name) == paNoError);
            names.emplace_back(name != nullptr ? name : "");
        }
        for (int c = 0; c < here->maxOutputChannels; ++c) {
            const char* name = nullptr;
            CHECK(PaAsio_GetOutputChannelName(index, c, &name) == paNoError);
            names.emplace_back(name != nullptr ? name : "");
        }
        CHECK(names == there->channelNames);
    }
}

TEST_CASE("an ASIO scan that falls over once is asked again and lists the devices",
          "[audio][hardware]") {
    const ForgetScan forget;
    const takt4::test::TempDir dir;
    const std::filesystem::path marker = dir.path() / "fell-over";
    const std::string how = "fall-over-once:" + marker.string();
    const ScopedVariable fall("TAKT4_TEST_ASIO_SCAN", how.c_str());
    const AsioScan scan = takt4::audio::scanAsio();
    INFO("the problem: " << scan.problem);
    CHECK(std::filesystem::exists(marker)); // the first one did fall over
    CHECK(scan.problem.empty());
    CHECK_FALSE(scan.devices.empty());
}

#endif
