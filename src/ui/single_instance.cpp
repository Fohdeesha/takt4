#include "ui/single_instance.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace takt4::ui {

namespace {

/// FNV-1a, 64 bits: the same in every process and every build, which `std::hash` does not
/// promise.
std::uint64_t fnv1a(std::string_view bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

#if defined(_WIN32)
/// The key is hex digits, so a byte-wise widening is the whole of the conversion.
std::wstring widen(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

std::wstring mutexName(const std::string& key) {
    return L"Local\\takt4-" + widen(key);
}

std::wstring windowName(const std::string& key) {
    return L"Local\\takt4-window-" + widen(key);
}

/// This thread's first visible top-level window whose title is "takt4", or "takt4 " and more:
/// the main window, which is the only one open when this is asked.
HWND ownMainWindow() {
    HWND found = nullptr;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND window, LPARAM out) -> BOOL {
            wchar_t title[32] = {};
            const int length = GetWindowTextW(window, title, 32);
            if (length >= 5 && std::wcsncmp(title, L"takt4", 5) == 0 &&
                (length == 5 || title[5] == L' ') && IsWindowVisible(window) != 0) {
                *reinterpret_cast<HWND*>(out) = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}
#endif

} // namespace

std::string SingleInstance::keyOf(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::path whole = std::filesystem::weakly_canonical(folder, error);
    if (error) {
        whole = folder;
    }
#if defined(_WIN32)
    // One folder however it is typed: Windows' paths are not case-sensitive.
    std::wstring text = whole.wstring();
    for (wchar_t& c : text) {
        c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
    }
    const std::string_view bytes(reinterpret_cast<const char*>(text.data()),
                                 text.size() * sizeof(wchar_t));
#else
    const std::string text = whole.string();
    const std::string_view bytes = text;
#endif
    char hex[17] = {};
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(bytes)));
    return hex;
}

SingleInstance::SingleInstance(const std::filesystem::path& folder) : key_(keyOf(folder)) {
#if defined(_WIN32)
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName(key_).c_str());
    if (mutex == nullptr) {
        // The system would not say: carried on as the only one, which is what takt4 always did.
        claimed_ = true;
        return;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return;
    }
    mutex_ = mutex;
    claimed_ = true;
#else
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const std::string path = (folder / ".takt4.lock").string();
    lockFile_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lockFile_ < 0) {
        claimed_ = true; // as above
        return;
    }
    if (::flock(lockFile_, LOCK_EX | LOCK_NB) != 0) {
        ::close(lockFile_);
        lockFile_ = -1;
        return;
    }
    claimed_ = true;
#endif
}

SingleInstance::~SingleInstance() {
#if defined(_WIN32)
    if (mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_));
    }
    if (mutex_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mutex_));
    }
#else
    if (lockFile_ >= 0) {
        ::close(lockFile_); // the lock with it
    }
#endif
}

bool SingleInstance::announceWindow(void* given) {
#if defined(_WIN32)
    if (!claimed_) {
        return true;
    }
    const HWND window = given != nullptr ? static_cast<HWND>(given) : ownMainWindow();
    if (window == nullptr) {
        return false;
    }
    if (mapping_ == nullptr) {
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(std::uint64_t), windowName(key_).c_str());
        if (mapping_ == nullptr) {
            return true; // nothing to tell a later launch by, now or later
        }
    }
    void* view =
        MapViewOfFile(static_cast<HANDLE>(mapping_), FILE_MAP_WRITE, 0, 0, sizeof(std::uint64_t));
    if (view == nullptr) {
        return true;
    }
    const auto handle = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window));
    std::memcpy(view, &handle, sizeof handle);
    UnmapViewOfFile(view);
    return true;
#else
    (void)given;
    return true;
#endif
}

bool SingleInstance::bringOtherToFront(const std::filesystem::path& folder) {
#if defined(_WIN32)
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, windowName(keyOf(folder)).c_str());
    if (mapping == nullptr) {
        return false;
    }
    std::uint64_t handle = 0;
    if (const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof handle)) {
        std::memcpy(&handle, view, sizeof handle);
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    if (handle == 0 || IsWindow(window) == 0) {
        return false;
    }
    if (IsIconic(window) != 0) {
        ShowWindow(window, SW_RESTORE);
    }
    // This process is the one the operator just started, so it has the foreground to hand on.
    SetForegroundWindow(window);
    return true;
#else
    (void)folder;
    return false;
#endif
}

} // namespace takt4::ui
