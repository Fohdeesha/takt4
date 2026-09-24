#include "ui/native_window.hpp"

#include <algorithm>
#include <atomic>

namespace takt4::ui {
namespace {

/// Room left around a window fitted to the work area, in logical pixels: the title bar and
/// the frame above and below, which `set_size` does not count, and a little either side.
constexpr float kFrameAllowanceHeight = 48.0f;
constexpr float kFrameAllowanceWidth = 16.0f;

std::atomic<bool> g_fitToScreen{false};

} // namespace

LogicalExtent fitWithin(LogicalExtent wanted, LogicalExtent work) noexcept {
    LogicalExtent out = wanted;
    if (work.width > kFrameAllowanceWidth) {
        out.width = std::min(wanted.width, work.width - kFrameAllowanceWidth);
    }
    if (work.height > kFrameAllowanceHeight) {
        out.height = std::min(wanted.height, work.height - kFrameAllowanceHeight);
    }
    return out;
}

void fitWindowsToScreen(bool fit) noexcept {
    g_fitToScreen.store(fit, std::memory_order_relaxed);
}

} // namespace takt4::ui

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>
#include <vector>

namespace takt4::ui {
namespace {

/// Where each window's original procedure is kept while ours is in front of it.
///
/// **`SetWindowLongPtrW`, not comctl32's `SetWindowSubclass`.** The subclass API is the tidier
/// one and it cannot be used here: it is documented as comctl32 version 6, and the comctl32 in
/// system32 is 5.82, which exports those four functions **by ordinal only**. Linking against
/// them by name produces an executable that will not start at all — STATUS_ENTRYPOINT_NOT_FOUND
/// before `main`, measured on this machine. Reaching version 6 needs an application manifest,
/// which is a great deal of machinery to buy a helper that chains subclasses when takt4 is the
/// only thing subclassing its own windows.
///
/// A window property rather than a table: it is per window, the loader cleans it up, and there
/// is nothing to keep in step when a window opens or closes.
constexpr const wchar_t* kPreviousProcProp = L"takt4.previousWindowProc";
/// The timer that runs inside a modal drag. Arbitrary; only has to be unique per window.
constexpr UINT_PTR kPumpTimerId = 0x74616B75;
/// About 30 Hz, which is `WindowController::kRedrawInterval` — the point is that a drag looks
/// no different from not dragging, so it should not run at a different rate either.
constexpr UINT kPumpIntervalMs = 33;

/// The pump, as one process-wide pair. There is one `WindowController` and it owns both
/// windows, so a slot per window would be the same pointer twice.
DragPump g_pump = nullptr;
void* g_pumpUser = nullptr;

/// Every top-level window belonging to *this thread*, which is the UI thread and the only one
/// that has any. Collected first rather than acted on inside the callback: subclassing while
/// enumerating is asking for trouble, and the list is two or three long.
BOOL CALLBACK collectWindow(HWND window, LPARAM out) {
    reinterpret_cast<std::vector<HWND>*>(out)->push_back(window);
    return TRUE;
}

std::vector<HWND> ownWindows() {
    std::vector<HWND> windows;
    EnumThreadWindows(GetCurrentThreadId(), collectWindow, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

std::wstring titleOf(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int written = GetWindowTextW(window, text.data(), length + 1);
    text.resize(written < 0 ? 0 : static_cast<std::size_t>(written));
    return text;
}

/// UTF-8 in, UTF-16 out. Used for both the title needle and the message box, and the second
/// is why it is a real decode rather than a byte-wise cast: a settings path can hold anything
/// a user called a folder, and `reportFatal` names one.
std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

WNDPROC previousProcOf(HWND window) {
    return reinterpret_cast<WNDPROC>(GetPropW(window, kPreviousProcProp));
}

LRESULT CALLBACK dragPumpProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    const WNDPROC previous = previousProcOf(window);
    switch (message) {
    case WM_ENTERSIZEMOVE:
        // The modal loop is about to start. From here until WM_EXITSIZEMOVE, nothing but
        // messages dispatched by that loop runs — and a timer is one of them.
        SetTimer(window, kPumpTimerId, kPumpIntervalMs, nullptr);
        break;
    case WM_EXITSIZEMOVE:
        KillTimer(window, kPumpTimerId);
        break;
    case WM_TIMER:
        if (wparam == kPumpTimerId) {
            if (g_pump != nullptr) {
                // The pump reads the engine's rings and writes Slint properties. It also asks
                // Slint for a redraw, and **that is not enough on its own** — measured: the
                // round runs, the properties move, and nothing reaches the screen, because
                // Slint renders when its event loop dispatches a redraw and the event loop is
                // what this modal loop has displaced.
                g_pump(g_pumpUser);
            }
            // So the paint is forced. `RDW_UPDATENOW` sends WM_PAINT *synchronously* down the
            // chain to the window's own procedure, which is where the toolkit renders from —
            // no event loop involved. Every window of this thread, not just the one being
            // dragged: the whole thread is inside the loop, so the editor is as frozen as the
            // window somebody has hold of.
            for (const HWND other : ownWindows()) {
                RedrawWindow(other, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            return 0;
        }
        break;
    case WM_NCDESTROY:
        // Put back before the window goes, and the property with it — a procedure pointing
        // into this module must not outlive the window it was installed on.
        KillTimer(window, kPumpTimerId);
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
        RemovePropW(window, kPreviousProcProp);
        break;
    default:
        break;
    }
    // The W form throughout: winit registers Unicode windows, and mixing the A and W calls
    // makes Windows insert a translation thunk that mangles text messages.
    return previous == nullptr ? DefWindowProcW(window, message, wparam, lparam)
                               : CallWindowProcW(previous, window, message, wparam, lparam);
}

} // namespace

bool bringWindowToFront(std::string_view titleContains) {
    const std::wstring needle = widen(titleContains);
    for (const HWND window : ownWindows()) {
        if (titleOf(window).find(needle) == std::wstring::npos) {
            continue;
        }
        if (IsIconic(window) != 0) {
            ShowWindow(window, SW_RESTORE);
        }
        // Both: `BringWindowToTop` moves it in the Z order and `SetForegroundWindow` gives it
        // the focus. Windows may refuse the second when the calling process is not already in
        // the foreground — which it is here, since this runs from a click on our own window —
        // and the first still leaves the editor visible if it does.
        BringWindowToTop(window);
        SetForegroundWindow(window);
        return true;
    }
    return false;
}

void keepPaintingWhileDragged(DragPump pump, void* user) {
    g_pump = pump;
    g_pumpUser = user;
    for (const HWND window : ownWindows()) {
        if (previousProcOf(window) != nullptr) {
            continue; // already ours
        }
        const auto previous = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&dragPumpProc)));
        if (previous == nullptr) {
            continue; // it refused; the window keeps whatever procedure it had
        }
        SetPropW(window, kPreviousProcProp, reinterpret_cast<HANDLE>(previous));
    }
}

void reportFatal(std::string_view message) {
    const std::wstring text = widen(message);
    MessageBoxW(nullptr, text.c_str(), L"takt4", MB_OK | MB_ICONERROR);
}

LogicalExtent fitToScreen(LogicalExtent wanted) noexcept {
    if (!g_fitToScreen.load(std::memory_order_relaxed)) {
        return wanted;
    }
    // The primary monitor, which is where a window opens when nothing has placed it. Its work
    // area and the DPI come in the same units whatever this process's DPI awareness: physical
    // pixels and the real DPI once winit has declared per-monitor awareness, 96-DPI pixels and
    // 96 before — so the ratio is the logical size either way.
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof info;
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == 0) {
        return wanted;
    }
    const UINT dpi = GetDpiForSystem();
    const double scale = dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;
    const LogicalExtent work{
        static_cast<float>(static_cast<double>(info.rcWork.right - info.rcWork.left) / scale),
        static_cast<float>(static_cast<double>(info.rcWork.bottom - info.rcWork.top) / scale)};
    return fitWithin(wanted, work);
}

} // namespace takt4::ui

#else

#include <iostream>

namespace takt4::ui {

bool bringWindowToFront(std::string_view) {
    return false;
}

void keepPaintingWhileDragged(DragPump, void*) {}

void reportFatal(std::string_view message) {
    std::cerr << "takt4: " << message << '\n';
}

LogicalExtent fitToScreen(LogicalExtent wanted) noexcept {
    return wanted;
}

} // namespace takt4::ui

#endif
