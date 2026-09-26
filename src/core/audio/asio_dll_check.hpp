#pragma once

// Whether the DLL an ASIO driver's registry entry names is missing — the check the ASIO SDK's
// driver list makes before it offers a driver, as takt4 makes it. Included by the patched
// host/pc/asiolist.cpp (cmake/asiosdk.cmake) and by its test; Windows only, and nothing else of
// takt4 in it, since it is compiled inside PortAudio's ASIO host.
//
// The SDK asks `OpenFile(path, ..., OF_EXIST)`, whose result it then read the wrong way round, so
// every driver was offered — including one whose DLL is gone, which can take a process down as
// it loads. Read the right way round, `OpenFile` hid drivers that work (the audit of 2026-09-25,
// M9): it refuses a path of 127 bytes or more (its `OFSTRUCT` holds 128) and does not expand the
// `%SystemRoot%\...` a `REG_EXPAND_SZ` value holds, and COM loads both kinds fine. So this
// expands the path, takes off quotes an installer may have put round it, and asks
// `GetFileAttributesA` — and calls a driver missing only when Windows says the file, or the
// folder it would be in, is not there. Anything else it cannot tell, it offers.

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <string>

namespace takt4::audio {

/// `registered` is the InprocServer32 value as read, `size` bytes of buffer that need not end
/// in a terminator. True unless the file is certainly not there.
inline bool asioDriverDllPresent(const char* registered, std::size_t size) {
    std::string path(registered, strnlen(registered, size));
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
        path = path.substr(1, path.size() - 2);
    }
    // The longest an expansion can be, and a string rather than an array: this runs once per
    // driver at startup, and a 32 KB frame is not something to put on a stack nobody sized.
    std::string expanded(32768, '\0');
    const DWORD written =
        ExpandEnvironmentStringsA(path.c_str(), &expanded[0], static_cast<DWORD>(expanded.size()));
    if (written > 0 && written <= expanded.size()) {
        expanded.resize(written - 1); // the count includes the terminator
        path = expanded;
    }
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    const DWORD error = GetLastError();
    return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
}

} // namespace takt4::audio
