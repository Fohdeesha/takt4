#include "core/io/utf8.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstddef>

namespace takt4::io {
namespace {

/// How long the well-formed sequence starting at `text[at]` is, or 0 when the byte there does
/// not begin one. The table is RFC 3629's: each lead byte narrows the range its *second* byte
/// may take, which is what rules out overlong forms (E0, F0), surrogates (ED) and code points
/// past U+10FFFF (F4) without decoding anything.
std::size_t sequenceAt(std::string_view text, std::size_t at) noexcept {
    const auto byte = [&text](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    if (lead < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead == 0xE0) {
        length = 3;
        low = 0xA0;
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
        length = 3;
    } else if (lead == 0xED) {
        length = 3;
        high = 0x9F;
    } else if (lead == 0xF0) {
        length = 4;
        low = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        length = 4;
    } else if (lead == 0xF4) {
        length = 4;
        high = 0x8F;
    } else {
        return 0; // a continuation byte on its own, C0/C1, or F5 and above
    }
    if (at + length > text.size()) {
        return 0;
    }
    const unsigned char second = byte(at + 1);
    if (second < low || second > high) {
        return 0;
    }
    for (std::size_t i = 2; i < length; ++i) {
        if ((byte(at + i) & 0xC0) != 0x80) {
            return 0;
        }
    }
    return length;
}

} // namespace

bool isValidUtf8(std::string_view text) noexcept {
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t length = sequenceAt(text, at);
        if (length == 0) {
            return false;
        }
        at += length;
    }
    return true;
}

std::string validUtf8(std::string_view text) {
    if (isValidUtf8(text)) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size() + 8);
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t length = sequenceAt(text, at);
        if (length == 0) {
            // One replacement per offending byte, which is what Rust's `from_utf8_lossy` and
            // WideCharToMultiByte both do — so a name reads the same wherever it is shown.
            out += "\xEF\xBF\xBD";
            ++at;
            continue;
        }
        out.append(text.substr(at, length));
        at += length;
    }
    return out;
}

std::string pathText(const std::filesystem::path& path) noexcept {
    try {
#if defined(_WIN32)
        const std::wstring& wide = path.native();
        if (wide.empty()) {
            return {};
        }
        const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                               static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                               nullptr);
        if (length <= 0) {
            return {};
        }
        std::string out(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                            length, nullptr, nullptr);
        // Belt and braces: flags of 0 already turn an unpaired surrogate into U+FFFD.
        return validUtf8(out);
#else
        return validUtf8(path.native());
#endif
    } catch (...) {
        // Only an allocation can fail above. A path that cannot be shown is shown as nothing
        // rather than taking down the status line that wanted it.
        return {};
    }
}

} // namespace takt4::io
