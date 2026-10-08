#include "core/io/chars.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <system_error>

using takt4::io::fromChars;
using takt4::io::toChars;

namespace {

struct Read {
    std::errc error;
    std::size_t used; ///< characters read
    double value;
};

Read read(const std::string& text) {
    double value = -7.0; // what a refusal must leave alone
    const std::from_chars_result result = fromChars(text.data(), text.data() + text.size(), value);
    return {result.ec, static_cast<std::size_t>(result.ptr - text.data()), value};
}

} // namespace

TEST_CASE("a number is read from text the same way on every system", "[io]") {
    // On macOS this is takt4's own reading, since Apple's from_chars for a double needs macOS
    // 26; elsewhere it is the standard one. Either way, the same answers: what a box accepts must
    // not depend on the machine.
    SECTION("what from_chars reads") {
        CHECK(read("120.5").error == std::errc{});
        CHECK(read("120.5").value == 120.5);
        CHECK(read("120.5").used == 5);
        CHECK(read("-3").value == -3.0);
        CHECK(read("1e3").value == 1000.0);
        CHECK(read(".5").value == 0.5);
        // As far as it is a number, and no further.
        CHECK(read("12ms").used == 2);
        CHECK(read("12ms").value == 12.0);
        CHECK(std::isinf(read("inf").value));
        CHECK(std::isnan(read("nan").value));
    }
    SECTION("what from_chars refuses, which strtod would take") {
        for (const char* text : {" 1", "+1", "-+1", "- 1", "", "-", "abc"}) {
            INFO('"' << text << '"');
            const Read got = read(text);
            CHECK(got.error == std::errc::invalid_argument);
            CHECK(got.used == 0);
            CHECK(got.value == -7.0);
        }
        // Hexadecimal: the "0" and no more.
        const Read hex = read("0x10");
        CHECK(hex.error == std::errc{});
        CHECK(hex.value == 0.0);
        CHECK(hex.used == 1);
    }
    SECTION("too large is said") {
        // What is left in the value then is not the same everywhere — MSVC's from_chars does not
        // leave it alone, as this test first assumed (the first Windows run, 2026-10-08) — so
        // nothing reads it after one.
        const Read huge = read("1e400");
        CHECK(huge.error == std::errc::result_out_of_range);
        CHECK(huge.used == 5);
    }
}

TEST_CASE("a number written as text reads back as the same number", "[io]") {
    for (const double value : {0.0, 1.0, -2.5, 48000.0, 0.1, 1.0 / 3.0, 123456.789,
                               std::numeric_limits<double>::max()}) {
        char buffer[64];
        const std::to_chars_result written = toChars(buffer, buffer + sizeof buffer, value);
        REQUIRE(written.ec == std::errc{});
        double back = 0.0;
        const std::from_chars_result read = fromChars(buffer, written.ptr, back);
        INFO(std::string(buffer, written.ptr));
        CHECK(read.ec == std::errc{});
        CHECK(read.ptr == written.ptr);
        CHECK(back == value);
    }
    char tiny[2];
    CHECK(toChars(tiny, tiny + sizeof tiny, 123.25).ec == std::errc::value_too_large);
}
