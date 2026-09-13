#include "ui/file_dialog.hpp"

#if defined(_WIN32)
// <windows.h> before <commdlg.h>, and NOMINMAX because this translation unit is compiled
// with the same warning flags as the rest and `max` as a macro breaks <algorithm> for
// anything that includes this later.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <commdlg.h>

#include <cstddef>
#endif

namespace takt4::ui {

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          needed);
    return out;
}

/// The filter is a list of null-separated label/pattern pairs, ended by a second null —
/// which is why it is built rather than written as a literal: a string literal stops at the
/// first embedded null and the dialog would see one truncated entry.
const std::wstring& filter() {
    static const std::wstring value = [] {
        std::wstring out;
        for (const wchar_t* part : {L"presets (*.json)", L"*.json", L"all files", L"*.*"}) {
            out.append(part);
            out.push_back(L'\0');
        }
        out.push_back(L'\0');
        return out;
    }();
    return value;
}

OPENFILENAMEW baseOf(std::wstring& buffer, const std::wstring& title) {
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ::GetActiveWindow();
    ofn.lpstrFilter = filter().c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = title.empty() ? nullptr : title.c_str();
    ofn.lpstrDefExt = L"json";
    return ofn;
}

std::wstring bufferWith(const std::string& suggested) {
    std::wstring buffer(1024, L'\0');
    const std::wstring wide = widen(suggested);
    // Leave room for the terminator the dialog writes.
    for (std::size_t i = 0; i < wide.size() && i + 1 < buffer.size(); ++i) {
        buffer[i] = wide[i];
    }
    return buffer;
}

} // namespace

std::filesystem::path askOpenFile(const std::string& title, const std::string& suggested) {
    std::wstring buffer = bufferWith(suggested);
    const std::wstring wideTitle = widen(title);
    OPENFILENAMEW ofn = baseOf(buffer, wideTitle);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetOpenFileNameW(&ofn) == 0) {
        return {}; // cancelled, or the dialog could not be shown; both mean "do nothing"
    }
    return std::filesystem::path(buffer.c_str());
}

std::filesystem::path askSaveFile(const std::string& title, const std::string& suggested) {
    std::wstring buffer = bufferWith(suggested);
    const std::wstring wideTitle = widen(title);
    OPENFILENAMEW ofn = baseOf(buffer, wideTitle);
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetSaveFileNameW(&ofn) == 0) {
        return {};
    }
    return std::filesystem::path(buffer.c_str());
}

#else

// Not yet implemented off Windows, and this is deliberately a *silent* empty rather than a
// stub that writes somewhere guessed: returning empty takes the caller's cancel path, so
// import and export are simply unavailable rather than wrong. The port (§1's box) will want
// GTK's chooser or a portal call here; until the UI compiles on those platforms at all,
// anything written now would be untested.
std::filesystem::path askOpenFile(const std::string&, const std::string&) { return {}; }
std::filesystem::path askSaveFile(const std::string&, const std::string&) { return {}; }

#endif

} // namespace takt4::ui
