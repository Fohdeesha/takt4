#include "core/audio/asio_scan.hpp"

#include "core/io/chars.hpp"

#include <charconv>
#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <portaudio.h>
#include <pa_asio.h>

#include "core/audio/asio_probe.h"

#include <cstdio>
#endif

namespace takt4::audio {

namespace {

constexpr std::string_view kHeader = "takt4-asio-scan 1";
constexpr std::string_view kEnd = "end";
/// `readAsioScan`'s problem for a text that stops before its end.
constexpr std::string_view kUnfinished = "the scan did not finish";

// ---- the text a scanning process writes -----------------------------------------------------

/// A field with the three characters the format uses for itself taken out of the way.
void appendField(std::string& out, std::string_view text) {
    out += '\t';
    for (const char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out += c;
        }
    }
}

void appendNumber(std::string& out, double value) {
    char buffer[64];
    const auto [end, error] = io::toChars(buffer, buffer + sizeof buffer, value);
    out += '\t';
    out.append(buffer, error == std::errc{} ? end : buffer);
}

void appendNumber(std::string& out, long value) {
    out += '\t';
    out += std::to_string(value);
}

std::string unescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 == text.size()) {
            out += text[i];
            continue;
        }
        switch (text[++i]) {
        case 't':
            out += '\t';
            break;
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        default:
            out += text[i];
        }
    }
    return out;
}

std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string_view::npos ? tab : tab - start));
        if (tab == std::string_view::npos) {
            return out;
        }
        start = tab + 1;
    }
}

template <typename T>
bool number(std::string_view text, T& value) {
    const auto [end, error] = io::fromChars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

AsioScan unfinished() {
    AsioScan scan;
    scan.problem = kUnfinished;
    return scan;
}

// ---- what the next Pa_Initialize is handed ---------------------------------------------------

struct Handed {
    std::mutex mutex;
    AsioScan scan;
    /// Each device's channel names as the C array the probe hands over, pointing into `scan`.
    std::vector<std::vector<const char*>> names;
};

Handed& handed() {
    static Handed state;
    return state;
}

#if defined(_WIN32)

int probe(const char* driverName, PaAsioTakt4Device* out) {
    Handed& state = handed();
    const std::lock_guard lock(state.mutex);
    for (std::size_t i = 0; i < state.scan.devices.size(); ++i) {
        const AsioScannedDevice& device = state.scan.devices[i];
        if (device.name != driverName) {
            continue;
        }
        out->inputChannels = device.inputChannels;
        out->outputChannels = device.outputChannels;
        out->minBufferSize = device.minBufferSize;
        out->maxBufferSize = device.maxBufferSize;
        out->preferredBufferSize = device.preferredBufferSize;
        out->bufferGranularity = device.bufferGranularity;
        out->defaultSampleRate = device.defaultSampleRate;
        out->defaultLowInputLatency = device.defaultLowInputLatency;
        out->defaultLowOutputLatency = device.defaultLowOutputLatency;
        out->defaultHighInputLatency = device.defaultHighInputLatency;
        out->defaultHighOutputLatency = device.defaultHighOutputLatency;
        // A scan that named fewer channels than it counted hands over none rather than a
        // short array PortAudio would read past.
        out->channelNames = state.names[i].size() ==
                                    static_cast<std::size_t>(device.inputChannels +
                                                             device.outputChannels)
                                ? state.names[i].data()
                                : nullptr;
        return 1;
    }
    return 0;
}

// ---- the scanning process --------------------------------------------------------------------

std::string environment(const char* name) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    const std::string out(value);
    std::free(value);
    return out;
}

/// Every ASIO device this process's PortAudio found, as it describes them.
std::vector<AsioScannedDevice> asioDevicesHere() {
    std::vector<AsioScannedDevice> devices;
    const PaHostApiIndex api = Pa_HostApiTypeIdToHostApiIndex(paASIO);
    const PaHostApiInfo* info = api >= 0 ? Pa_GetHostApiInfo(api) : nullptr;
    if (info == nullptr) {
        return devices;
    }
    for (int i = 0; i < info->deviceCount; ++i) {
        const PaDeviceIndex index = Pa_HostApiDeviceIndexToDeviceIndex(api, i);
        const PaDeviceInfo* found = Pa_GetDeviceInfo(index);
        if (found == nullptr) {
            continue;
        }
        AsioScannedDevice device;
        device.name = found->name;
        device.inputChannels = found->maxInputChannels;
        device.outputChannels = found->maxOutputChannels;
        device.defaultSampleRate = found->defaultSampleRate;
        device.defaultLowInputLatency = found->defaultLowInputLatency;
        device.defaultLowOutputLatency = found->defaultLowOutputLatency;
        device.defaultHighInputLatency = found->defaultHighInputLatency;
        device.defaultHighOutputLatency = found->defaultHighOutputLatency;
        (void)PaAsio_GetAvailableBufferSizes(index, &device.minBufferSize, &device.maxBufferSize,
                                             &device.preferredBufferSize,
                                             &device.bufferGranularity);
        for (int c = 0; c < found->maxInputChannels; ++c) {
            const char* name = nullptr;
            device.channelNames.emplace_back(
                PaAsio_GetInputChannelName(index, c, &name) == paNoError && name != nullptr
                    ? name
                    : "");
        }
        for (int c = 0; c < found->maxOutputChannels; ++c) {
            const char* name = nullptr;
            device.channelNames.emplace_back(
                PaAsio_GetOutputChannelName(index, c, &name) == paNoError && name != nullptr
                    ? name
                    : "");
        }
        devices.push_back(std::move(device));
    }
    return devices;
}

/// The scanning process's answer, written to the pipe the asking process reads.
void answer(const AsioScan& scan) {
    const std::string text = writeAsioScan(scan);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()),
              &written, nullptr);
}

/// A bench switch that ships, like TAKT4_TEST_CRASH: TAKT4_TEST_ASIO_SCAN makes the scanning
/// process do what a driver might — "fall-over" is the MOTU's fast fail, before any driver is
/// loaded; "fall-over-once:<file>" falls over only if the file is not there yet, and makes it;
/// "hang" never answers; "answer-then-hang" answers, finding nothing, and never exits;
/// "helper" answers and exits at once, leaving a process of its own holding the answer's
/// pipe for five seconds, as a driver that starts one would.
void benchSwitch() {
    const std::string how = environment("TAKT4_TEST_ASIO_SCAN");
    if (how.empty()) {
        return;
    }
    constexpr std::string_view once = "fall-over-once:";
    bool fallOver = how == "fall-over";
    if (how.rfind(once, 0) == 0) {
        const std::string marker = how.substr(once.size());
        const HANDLE file = CreateFileA(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        fallOver = file != INVALID_HANDLE_VALUE;
        if (fallOver) {
            CloseHandle(file);
        }
    }
    if (fallOver) {
        __fastfail(FAST_FAIL_FATAL_APP_EXIT); // what MOTUProAudioASIO.dll does
    }
    if (how == "hang") {
        Sleep(INFINITE);
    }
    if (how == "answer-then-hang") {
        answer(AsioScan{});
        Sleep(INFINITE);
    }
    if (how == "helper") {
        // Everything inheritable goes to the helper, the pipe's write end included, and cmd.exe
        // holds it while the ping it waits on runs — its own output thrown away.
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        PROCESS_INFORMATION helper{};
        std::wstring line = L"cmd.exe /d /c ping -n 6 127.0.0.1 > nul";
        if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &startup, &helper) != 0) {
            CloseHandle(helper.hThread);
            CloseHandle(helper.hProcess);
        }
        answer(AsioScan{});
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

[[noreturn]] void scanHereAndExit() {
    // No "has stopped working" box of Windows's own for a driver that falls over in here:
    // takt4 is the one that says what happened.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    benchSwitch();
    AsioScan scan;
    const PaError error = Pa_Initialize();
    if (error == paNoError) {
        scan.devices = asioDevicesHere();
    } else {
        scan.problem = std::string("PortAudio would not start: ") + Pa_GetErrorText(error);
    }
    answer(scan);
    // Ended here, with nothing let go: what was asked for is written, and releasing PortAudio
    // and the drivers is only another chance for one of them to fall over.
    TerminateProcess(GetCurrentProcess(), 0);
    for (;;) {
        Sleep(INFINITE);
    }
}

bool askedToScan() {
    // The last argument, exactly as `scanOnce` writes it — and never what anybody would type.
    const std::wstring_view line = GetCommandLineW();
    return line.size() > kAsioScanOption.size() &&
           line.substr(line.size() - kAsioScanOption.size()) == kAsioScanOption &&
           line[line.size() - kAsioScanOption.size() - 1] == L' ';
}

/// Before `main`: an executable started to scan does only that. A static initializer, so that
/// every executable which links PortAudio — takt4, takt4-cli, the test binaries — answers
/// without each of their `main`s having to know.
struct ScanIfAsked {
    ScanIfAsked() {
        if (askedToScan()) {
            scanHereAndExit();
        }
    }
};
const ScanIfAsked scanIfAsked;

// ---- the process that asks -------------------------------------------------------------------

enum class Outcome { Finished, FellOver, TimedOut, NotStarted };

struct Attempt {
    Outcome outcome = Outcome::NotStarted;
    AsioScan scan;
    DWORD exitCode = 0;
    DWORD error = 0;
};

Attempt scanOnce(std::chrono::milliseconds limit) {
    Attempt attempt;
    wchar_t self[4096] = {};
    const DWORD length = GetModuleFileNameW(nullptr, self, 4096);
    if (length == 0 || length >= 4096) {
        attempt.error = GetLastError();
        return attempt;
    }
    std::wstring command = L"\"" + std::wstring(self, length) + L"\" " +
                           std::wstring(kAsioScanOption);

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (CreatePipe(&readEnd, &writeEnd, &inherit, 1 << 16) == 0) {
        attempt.error = GetLastError();
        return attempt;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    // The pipe's write end and nothing else goes to the child: without the list, every
    // inheritable handle this process has would, and a scan that hung would hold them.
    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<unsigned char> attributes(attributesSize);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = writeEnd;
    PROCESS_INFORMATION child{};
    const bool listed =
        InitializeProcThreadAttributeList(list, 1, 0, &attributesSize) != 0 &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &writeEnd,
                                  sizeof(writeEnd), nullptr, nullptr) != 0;
    startup.lpAttributeList = listed ? list : nullptr;
    // No console window for a console executable (takt4-cli, a test binary) standing in; a
    // window application ignores the flag.
    const BOOL started =
        listed && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup.StartupInfo, &child) != 0;
    if (!started) {
        attempt.error = GetLastError();
    }
    if (listed) {
        DeleteProcThreadAttributeList(list);
    }
    CloseHandle(writeEnd); // the child's copy is the one left; its exit ends the pipe
    if (!started) {
        CloseHandle(readEnd);
        return attempt;
    }
    CloseHandle(child.hThread);

    std::string text;
    const auto until = std::chrono::steady_clock::now() + limit;
    char buffer[4096];
    bool gone = false;
    while (true) {
        if (std::chrono::steady_clock::now() > until) {
            TerminateProcess(child.hProcess, 1);
            attempt.outcome = Outcome::TimedOut;
            break;
        }
        DWORD available = 0;
        if (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) == 0) {
            break; // the child has gone and everything it wrote has been read
        }
        if (available > 0) {
            DWORD got = 0;
            if (ReadFile(readEnd, buffer, sizeof buffer, &got, nullptr) == 0) {
                break;
            }
            text.append(buffer, got);
            continue;
        }
        // **Gone, and everything it wrote read: done** — not waiting for the pipe to break. A
        // driver that starts a process of its own hands it the scan's output with everything
        // else it inherits, and while that process runs the pipe stays whole: the peek went on
        // finding nothing to read, and the UI thread waited out the whole limit (the audit of
        // 2026-09-25, L24).
        if (gone) {
            break;
        }
        // Sleeps while there is nothing to read; wakes at once when the child goes, and the
        // peek then takes whatever it left in the pipe.
        gone = WaitForSingleObject(child.hProcess, 10) == WAIT_OBJECT_0;
    }
    CloseHandle(readEnd);
    WaitForSingleObject(child.hProcess, 5000);
    GetExitCodeProcess(child.hProcess, &attempt.exitCode);
    CloseHandle(child.hProcess);
    attempt.scan = readAsioScan(text);
    // What was written decides, not how the process ended: a driver that falls over after the
    // scan is written has not cost the scan — and nor has one that hung after it, whose whole
    // answer a timeout used to throw away (the audit of 2026-09-25, L24).
    if (attempt.scan.problem != kUnfinished) {
        attempt.outcome = Outcome::Finished;
    } else if (attempt.outcome != Outcome::TimedOut) {
        attempt.outcome = Outcome::FellOver;
    }
    return attempt;
}

std::string hex(DWORD code) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(code));
    return text;
}

#endif

} // namespace

std::string writeAsioScan(const AsioScan& scan) {
    std::string out(kHeader);
    out += '\n';
    if (!scan.problem.empty()) {
        out += "problem";
        appendField(out, scan.problem);
        out += '\n';
    }
    for (const AsioScannedDevice& device : scan.devices) {
        out += "device";
        appendField(out, device.name);
        out += "\nchannels";
        appendNumber(out, device.inputChannels);
        appendNumber(out, device.outputChannels);
        out += "\nbuffers";
        appendNumber(out, device.minBufferSize);
        appendNumber(out, device.maxBufferSize);
        appendNumber(out, device.preferredBufferSize);
        appendNumber(out, device.bufferGranularity);
        out += "\nrate";
        appendNumber(out, device.defaultSampleRate);
        appendNumber(out, device.defaultLowInputLatency);
        appendNumber(out, device.defaultLowOutputLatency);
        appendNumber(out, device.defaultHighInputLatency);
        appendNumber(out, device.defaultHighOutputLatency);
        out += '\n';
        for (const std::string& name : device.channelNames) {
            out += "name";
            appendField(out, name);
            out += '\n';
        }
    }
    out += kEnd;
    out += '\n';
    return out;
}

AsioScan readAsioScan(std::string_view text) {
    AsioScan scan;
    bool headed = false;
    bool ended = false;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        if (newline == std::string_view::npos) {
            return unfinished(); // a last line cut short
        }
        const std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline + 1);
        if (ended) {
            return unfinished(); // nothing may follow the end
        }
        if (!headed) {
            if (line != kHeader) {
                return unfinished();
            }
            headed = true;
            continue;
        }
        if (line == kEnd) {
            ended = true;
            continue;
        }
        const std::vector<std::string_view> f = fields(line);
        const std::string_view kind = f.front();
        AsioScannedDevice* device = scan.devices.empty() ? nullptr : &scan.devices.back();
        bool ok = true;
        if (kind == "problem" && f.size() == 2) {
            scan.problem = unescape(f[1]);
        } else if (kind == "device" && f.size() == 2) {
            scan.devices.emplace_back().name = unescape(f[1]);
        } else if (kind == "channels" && f.size() == 3 && device != nullptr) {
            ok = number(f[1], device->inputChannels) && number(f[2], device->outputChannels);
        } else if (kind == "buffers" && f.size() == 5 && device != nullptr) {
            ok = number(f[1], device->minBufferSize) && number(f[2], device->maxBufferSize) &&
                 number(f[3], device->preferredBufferSize) &&
                 number(f[4], device->bufferGranularity);
        } else if (kind == "rate" && f.size() == 6 && device != nullptr) {
            ok = number(f[1], device->defaultSampleRate) &&
                 number(f[2], device->defaultLowInputLatency) &&
                 number(f[3], device->defaultLowOutputLatency) &&
                 number(f[4], device->defaultHighInputLatency) &&
                 number(f[5], device->defaultHighOutputLatency);
        } else if (kind == "name" && f.size() == 2 && device != nullptr) {
            device->channelNames.push_back(unescape(f[1]));
        } else {
            ok = false;
        }
        if (!ok) {
            return unfinished();
        }
    }
    if (!ended) {
        return unfinished();
    }
    // A problem the scanning process reported itself: it finished, and found nothing usable.
    if (!scan.problem.empty()) {
        scan.devices.clear();
    }
    return scan;
}

AsioScan scanAsio(std::chrono::milliseconds limit) {
    AsioScan scan;
#if defined(_WIN32)
    Attempt attempt = scanOnce(limit);
    // Once more after a fall: the MOTU's driver does it about one release in 840, so two in a
    // row is something else. Not after running out of time, which would only happen again.
    if (attempt.outcome == Outcome::FellOver || attempt.outcome == Outcome::NotStarted) {
        attempt = scanOnce(limit);
    }
    switch (attempt.outcome) {
    // Each short enough for the window's two-line status bar at its narrowest, the last
    // sentence — what to do — included.
    case Outcome::Finished:
        scan = std::move(attempt.scan);
        if (!scan.problem.empty()) {
            scan.problem =
                "ASIO devices are not listed: " + scan.problem + ". Press RESCAN to try again.";
        }
        break;
    case Outcome::FellOver:
        scan.problem = "An ASIO driver fell over twice while being asked what it is (" +
                       hex(attempt.exitCode) +
                       "), so ASIO devices are not listed; takt4 is unaffected. Press RESCAN "
                       "to try again.";
        break;
    case Outcome::TimedOut:
        scan.problem = "An ASIO driver did not answer within " +
                       std::to_string(limit.count() / 1000) +
                       " seconds, so ASIO devices are not listed. Press RESCAN to try again.";
        break;
    case Outcome::NotStarted:
        scan.problem = "takt4 could not start its ASIO scan (Windows error " +
                       std::to_string(attempt.error) +
                       "), so ASIO devices are not listed. Press RESCAN to try again.";
        break;
    }
#else
    (void)limit;
#endif
    return scan;
}

void useAsioScan(AsioScan scan) {
    Handed& state = handed();
    {
        const std::lock_guard lock(state.mutex);
        state.scan = std::move(scan);
        state.names.clear();
        for (const AsioScannedDevice& device : state.scan.devices) {
            std::vector<const char*>& names = state.names.emplace_back();
            for (const std::string& name : device.channelNames) {
                names.push_back(name.c_str());
            }
        }
    }
#if defined(_WIN32)
    PaAsio_Takt4_SetProbe(&probe);
#endif
}

std::string asioScanProblem() {
    Handed& state = handed();
    const std::lock_guard lock(state.mutex);
    return state.scan.problem;
}

bool asioWanted() {
#if defined(_WIN32)
    return GetEnvironmentVariableA("TAKT4_NO_ASIO", nullptr, 0) == 0;
#else
    return false;
#endif
}

} // namespace takt4::audio
