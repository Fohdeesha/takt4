#include "core/io/utf8.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using takt4::io::isValidUtf8;
using takt4::io::pathText;
using takt4::io::validUtf8;

namespace {

constexpr const char* kReplacement = "\xEF\xBF\xBD";

} // namespace

TEST_CASE("well-formed UTF-8 passes through untouched", "[io][utf8]") {
    for (const char* text : {"", "takt4", "MOTU Pro Audio", "Jon\xE2\x80\x99s set", // ’
                             "Shows \xE2\x80\x93 2026",                             // –
                             "caf\xC3\xA9", "\xF0\x9F\x8E\xB5"}) {                  // é, 🎵
        INFO(text);
        CHECK(isValidUtf8(text));
        CHECK(validUtf8(text) == text);
    }
}

TEST_CASE("every byte Slint would refuse is replaced, and only those", "[io][utf8]") {
    // What Rust's `str::from_utf8` rejects, one case each: the strictness has to be exactly
    // Slint's, because anything this lets through aborts the process.
    struct Case {
        const char* in;
        std::string out;
    };
    const Case cases[] = {
        // A Latin-1 byte on its own — an ASIO driver name read through the ANSI API.
        {"Focusrite \xB5 Interface", std::string("Focusrite ") + kReplacement + " Interface"},
        {"Bad \xFF Port", std::string("Bad ") + kReplacement + " Port"},
        // A lead byte with its continuation cut off: the end of a truncated name.
        {"caf\xC3", std::string("caf") + kReplacement},
        // Overlong forms of '/' and of NUL.
        {"\xC0\xAF", std::string(kReplacement) + kReplacement},
        {"\xE0\x80\xAF", std::string(kReplacement) + kReplacement + kReplacement},
        // A UTF-16 surrogate half encoded as if it were a character.
        {"\xED\xA0\x80", std::string(kReplacement) + kReplacement + kReplacement},
        // Past U+10FFFF.
        {"\xF4\x90\x80\x80",
         std::string(kReplacement) + kReplacement + kReplacement + kReplacement},
    };
    for (const Case& c : cases) {
        INFO(c.in);
        CHECK_FALSE(isValidUtf8(c.in));
        const std::string fixed = validUtf8(c.in);
        CHECK(fixed == c.out);
        CHECK(isValidUtf8(fixed));
    }
}

#if defined(_WIN32)
TEST_CASE("a path is shown as UTF-8 whatever the code page", "[io][utf8]") {
    // The audit's H13 names, built from UTF-16 so the test means the same on every machine.
    // The en dash and the curly apostrophe are both in code page 1252, so `path.string()`
    // turned them into single bytes that are not UTF-8; the Japanese name is in no Western code
    // page at all, and `path.string()` threw on it. Both paths went straight into a status line.
    CHECK(pathText(std::filesystem::path(L"Shows – 2026.json")) ==
          "Shows \xE2\x80\x93 2026.json");
    CHECK(pathText(std::filesystem::path(L"Jon’s set.json")) == "Jon\xE2\x80\x99s set.json");
    CHECK(pathText(std::filesystem::path(L"セット.json")) ==
          "\xE3\x82\xBB\xE3\x83\x83\xE3\x83\x88.json");
    // NTFS allows a name holding half a surrogate pair, which no UTF-8 can spell.
    const std::wstring lone = std::wstring(L"x") + static_cast<wchar_t>(0xD800) + L"y";
    CHECK(pathText(std::filesystem::path(lone)) == std::string("x") + kReplacement + "y");
    CHECK(pathText({}).empty());
}
#endif
