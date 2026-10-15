#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "core/output/midi_ports.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace takt4::testing {

/// **The one name every test's own MIDI port is made under.** Windows lists a port a program
/// makes some 50 ms after it is made, and the process that closes one goes on listing it — and a
/// port made under *another* name just after one was closed was not listed at all for more than
/// 30 s (measured 2026-10-15, winmm's own list, teVirtualMIDI ports). A port made again under the
/// same name was listed and opened every time, in the same process and in the next. So two tests
/// that each made a port of their own, run one after the other, failed; under one name they do
/// not.
inline constexpr wchar_t kTestPortWide[] = L"takt4 test port";
inline constexpr char kTestPort[] = "takt4 test port";

/// Waits until Windows lists a MIDI input whose name contains `name`, for up to `seconds`, and
/// says whether it did — a port a program has just made is listed a moment later. See
/// `kTestPort`.
inline bool midiInputListed(std::string_view name, double seconds = 5.0) {
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           std::chrono::duration<double>(seconds));
    for (;;) {
        const std::vector<std::string> inputs = output::listMidiInputPorts();
        if (std::any_of(inputs.begin(), inputs.end(), [name](const std::string& input) {
                return input.find(name) != std::string::npos;
            })) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

/// teVirtualMIDI's driver API, which rtpMIDI and loopMIDI install: a MIDI port a program can make
/// for itself. Another program opens it like any WinMM device — as an input, to hear what this one
/// sends with `send`, or as an output, whose messages arrive in the port's callback. Loaded when
/// present; a test that needs one says `SKIP` when `usable()` is false.
struct VirtualMidi {
    using Port = void*;
    using DataCallback = void(CALLBACK*)(Port, LPBYTE, DWORD, DWORD_PTR);
    using CreatePort = Port(CALLBACK*)(LPCWSTR, DataCallback, DWORD_PTR, DWORD, DWORD);
    using SendData = BOOL(CALLBACK*)(Port, LPBYTE, DWORD);
    using ClosePort = void(CALLBACK*)(Port);
    static constexpr DWORD kParseRx = 1; // TE_VM_FLAGS_PARSE_RX: one callback per whole message

    HMODULE library = ::LoadLibraryW(L"teVirtualMIDI64.dll");
    CreatePort create = nullptr;
    SendData send = nullptr;
    ClosePort close = nullptr;

    VirtualMidi() {
        if (library != nullptr) {
            create = find<CreatePort>("virtualMIDICreatePortEx2");
            send = find<SendData>("virtualMIDISendData");
            close = find<ClosePort>("virtualMIDIClosePort");
        }
    }

    /// `kTestPort`, made and listed. **The last one is waited out**: teVirtualMIDI lets go of a
    /// port in its own time, and the same name made again straight after a test had flooded and
    /// closed it was refused, and in the next process not listed (2026-10-15). So making it is
    /// tried for up to `seconds`, and then Windows is waited on to list it. Null, with `why`
    /// saying which, when it could not be had.
    Port makeTestPort(DataCallback callback, DWORD_PTR instance, DWORD maxSysex, DWORD flags,
                      std::string& why, double seconds = 20.0) {
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                               std::chrono::duration<double>(seconds));
        Port port = nullptr;
        for (;;) {
            port = create(kTestPortWide, callback, instance, maxSysex, flags);
            if (port != nullptr) {
                break;
            }
            const DWORD error = ::GetLastError();
            if (std::chrono::steady_clock::now() >= until) {
                why = "teVirtualMIDI would not make \"" + std::string(kTestPort) +
                      "\": Windows error " + std::to_string(error);
                return nullptr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const double left = std::max(
            5.0, std::chrono::duration<double>(until - std::chrono::steady_clock::now()).count());
        if (!midiInputListed(kTestPort, left)) {
            close(port);
            why = "\"" + std::string(kTestPort) + "\" was made, and Windows never listed it";
            return nullptr;
        }
        return port;
    }
    VirtualMidi(const VirtualMidi&) = delete;
    VirtualMidi& operator=(const VirtualMidi&) = delete;
    /// An export of the driver's as the type it is. Through `void (*)()`, the one function type a
    /// cast to or from is not a claim about the signature — `FARPROC` straight to another is, and
    /// clang says so.
    template <typename Fn>
    Fn find(const char* name) const {
        return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(::GetProcAddress(library, name)));
    }
    ~VirtualMidi() {
        if (library != nullptr) {
            ::FreeLibrary(library);
        }
    }
    bool usable() const { return create != nullptr && send != nullptr && close != nullptr; }
};

/// What the shared test port does with what is sent to it: nothing. The tests send *from* it.
inline void CALLBACK ignoreTestPortData(VirtualMidi::Port, LPBYTE, DWORD, DWORD_PTR) {}

/// `kTestPort`, as the tests that send through a port of their own share it.
struct TestPort {
    VirtualMidi midi;
    VirtualMidi::Port port = nullptr;
    /// Why there is none, when there is not.
    std::string why;

    TestPort() = default;
    TestPort(const TestPort&) = delete;
    TestPort& operator=(const TestPort&) = delete;
    ~TestPort() {
        if (port != nullptr) {
            midi.close(port); // before `midi` lets go of the driver
        }
    }
};

/// **`kTestPort`, made once in this process and kept until it ends**, for every test that sends
/// through a port of its own. Made, closed and made again within one process, it went unlisted for
/// tens of seconds, or was refused outright (2026-10-15); made once and left, it is there for each
/// test that wants it. Null `port` — with `why` — where teVirtualMIDI is not installed or the port
/// could not be had.
inline TestPort& sharedTestPort() {
    static TestPort shared;
    static const bool made = [] {
        if (!shared.midi.usable()) {
            shared.why = "teVirtualMIDI is not installed here (rtpMIDI and loopMIDI both bring it)";
            return false;
        }
        shared.port = shared.midi.makeTestPort(&ignoreTestPortData, 0, 65535, VirtualMidi::kParseRx,
                                               shared.why);
        return shared.port != nullptr;
    }();
    (void)made;
    return shared;
}

} // namespace takt4::testing

#endif
