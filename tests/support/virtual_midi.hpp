#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace takt4::testing {

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

} // namespace takt4::testing

#endif
