#pragma once

#include <charconv>
#include <concepts>
#include <system_error>

namespace takt4::io {

/// `std::from_chars`, for every system takt4 is built for.
///
/// **macOS has no `from_chars` for a `double` before macOS 26**, nor a `to_chars` for one before
/// 13.3: Apple's C++ library marks them unavailable below those, and takt4 runs on macOS 12 and
/// up, for the Intel Macs that stop at 12 and 13 (2026-10-08). There it is `strtod_l` in the C
/// locale, held to what `from_chars` takes — no leading space, no plus, no `0x` — so a box
/// accepts the same text on every system; elsewhere it is the standard one. Integers are the
/// standard one everywhere.
template <std::integral T>
std::from_chars_result fromChars(const char* first, const char* last, T& value) noexcept {
    return std::from_chars(first, last, value);
}
std::from_chars_result fromChars(const char* first, const char* last, double& value);

/// `std::to_chars` for a `double`: the shortest text that reads back as the same number; on
/// macOS, seventeen significant digits, which also read back as the same number.
std::to_chars_result toChars(char* first, char* last, double value) noexcept;

} // namespace takt4::io
