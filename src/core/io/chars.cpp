#include "core/io/chars.hpp"

#if defined(__APPLE__)
#include <xlocale.h>

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#endif

namespace takt4::io {

#if defined(__APPLE__)

namespace {

/// The C locale, so a decimal point is a point whatever the machine's language.
locale_t cLocale() {
    static const locale_t c = ::newlocale(LC_ALL_MASK, "C", nullptr);
    return c;
}

} // namespace

std::from_chars_result fromChars(const char* first, const char* last, double& value) {
    // What `from_chars` refuses and `strtod` would take: a leading space, a plus — after the
    // minus too — and a hexadecimal number, of which `from_chars` reads the "0" alone.
    const char* digits = first;
    if (digits != last && *digits == '-') {
        ++digits;
    }
    if (digits == last || *digits == '+' || std::isspace(static_cast<unsigned char>(*digits))) {
        return {first, std::errc::invalid_argument};
    }
    std::size_t length = static_cast<std::size_t>(last - first);
    if (last - digits >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        length = static_cast<std::size_t>(digits + 1 - first);
    }
    const std::string text(first, length); // strtod wants an end it can find
    char* end = nullptr;
    errno = 0;
    const double parsed = ::strtod_l(text.c_str(), &end, cLocale());
    if (end == text.c_str()) {
        return {first, std::errc::invalid_argument};
    }
    const char* const stop = first + (end - text.c_str());
    if (errno == ERANGE) {
        return {stop, std::errc::result_out_of_range}; // and `value` left alone, as from_chars does
    }
    value = parsed;
    return {stop, std::errc{}};
}

std::to_chars_result toChars(char* first, char* last, double value) noexcept {
    char buffer[40];
    const int written = ::snprintf_l(buffer, sizeof buffer, cLocale(), "%.17g", value);
    if (written < 0 || written > last - first) {
        return {last, std::errc::value_too_large};
    }
    std::memcpy(first, buffer, static_cast<std::size_t>(written));
    return {first + written, std::errc{}};
}

#else

std::from_chars_result fromChars(const char* first, const char* last, double& value) {
    return std::from_chars(first, last, value);
}

std::to_chars_result toChars(char* first, char* last, double value) noexcept {
    return std::to_chars(first, last, value);
}

#endif

} // namespace takt4::io
