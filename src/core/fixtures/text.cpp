#include "core/fixtures/text.hpp"

#include "core/io/chars.hpp"
#include "core/io/utf8.hpp"

#include <charconv>
#include <system_error>

namespace takt4::fixtures::text {
namespace {

bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isLower(char c) noexcept {
    return c >= 'a' && c <= 'z';
}

bool isUpper(char c) noexcept {
    return c >= 'A' && c <= 'Z';
}

bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool isLetter(char c) noexcept {
    return isLower(c) || isUpper(c);
}

} // namespace

char foldAscii(char c) noexcept {
    return isUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = foldAscii(c);
    }
    return out;
}

std::string upperAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = isLower(c) ? static_cast<char>(c - 'a' + 'A') : c;
    }
    return out;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (foldAscii(a[i]) != foldAscii(b[i])) {
            return false;
        }
    }
    return true;
}

bool startsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && equalsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::string clean(std::string_view text, std::size_t limit) {
    std::string valid = io::validUtf8(trim(text));
    // Count characters: a byte that is not a continuation byte (10xxxxxx) begins one. The text
    // is well-formed now, so a cut before such a byte is a cut between characters.
    std::size_t characters = 0;
    for (std::size_t i = 0; i < valid.size(); ++i) {
        const auto byte = static_cast<unsigned char>(valid[i]);
        if ((byte & 0xC0U) != 0x80U) {
            if (characters == limit) {
                valid.resize(i);
                break;
            }
            ++characters;
        }
    }
    // A cut can leave a space at the end.
    return std::string(trim(valid));
}

std::optional<std::int64_t> parseInteger(std::string_view text) noexcept {
    text = trim(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || stop != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parseNumber(std::string_view text) noexcept {
    text = trim(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    double value = 0.0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = io::fromChars(text.data(), end, value);
    if (error != std::errc{} || stop != end) {
        return std::nullopt;
    }
    return value;
}

std::string splitCamelCase(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i > 0 && isUpper(c)) {
            const char before = text[i - 1];
            const bool afterSmall = isLower(before) || isDigit(before);
            const bool endsRun = isUpper(before) && i + 1 < text.size() && isLower(text[i + 1]);
            if ((afterSmall || endsRun) && !out.empty() && out.back() != ' ') {
                out += ' ';
            }
        }
        out += c;
    }
    return out;
}

bool hasWordOpen(std::string_view text) noexcept {
    constexpr std::string_view word = "open";
    for (std::size_t at = 0; at + word.size() <= text.size(); ++at) {
        if (!equalsIgnoreCase(text.substr(at, word.size()), word)) {
            continue;
        }
        const bool startsWord = at == 0 || !isLetter(text[at - 1]);
        const std::size_t after = at + word.size();
        const bool endsWord = after == text.size() || !isLetter(text[after]);
        if (startsWord && endsWord) {
            return true;
        }
    }
    return false;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

} // namespace takt4::fixtures::text
