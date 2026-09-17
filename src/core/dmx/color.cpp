#include "core/dmx/color.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace takt4::dmx {
namespace {

std::uint8_t byteOf(double unit) noexcept {
    const double scaled = std::round(std::clamp(unit, 0.0, 1.0) * 255.0);
    return static_cast<std::uint8_t>(scaled);
}

int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// "255, 32, 64" — the other spelling a person pastes in, usually out of a color picker
/// that reports decimals. Nothing unless all three are there and all three fit a byte.
std::optional<Color> parseTriple(std::string_view text) noexcept {
    std::array<int, 3> parts{};
    std::size_t at = 0;
    for (int& part : parts) {
        const std::size_t comma = text.find(',', at);
        const std::string_view field =
            trim(text.substr(at, comma == std::string_view::npos ? comma : comma - at));
        if (field.empty()) {
            return std::nullopt;
        }
        const char* const begin = field.data();
        const char* const end = begin + field.size();
        const std::from_chars_result result = std::from_chars(begin, end, part);
        if (result.ec != std::errc{} || result.ptr != end || part < 0 || part > 255) {
            return std::nullopt;
        }
        if (comma == std::string_view::npos) {
            // The last field must be the last field: "1,2,3,4" is not a color.
            if (&part != &parts.back()) {
                return std::nullopt;
            }
            at = text.size();
            break;
        }
        at = comma + 1;
    }
    if (at != text.size()) {
        return std::nullopt;
    }
    return Color{static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
                  static_cast<std::uint8_t>(parts[2])};
}

} // namespace

Color fromHsv(double hueDegrees, double saturation, double value) noexcept {
    saturation = std::clamp(saturation, 0.0, 1.0);
    value = std::clamp(value, 0.0, 1.0);
    // Wrapped rather than clamped: a hue sweep runs past 360 by design, and a sweep that
    // stuck at red for its last turn would be a bug nobody could see in the numbers.
    double hue = std::fmod(hueDegrees, 360.0);
    if (hue < 0.0) {
        hue += 360.0;
    }

    const double chroma = value * saturation;
    const double sector = hue / 60.0;
    const double second = chroma * (1.0 - std::fabs(std::fmod(sector, 2.0) - 1.0));
    const double base = value - chroma;

    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    switch (static_cast<int>(sector)) {
    case 0:
        r = chroma;
        g = second;
        break;
    case 1:
        r = second;
        g = chroma;
        break;
    case 2:
        g = chroma;
        b = second;
        break;
    case 3:
        g = second;
        b = chroma;
        break;
    case 4:
        r = second;
        b = chroma;
        break;
    default:
        r = chroma;
        b = second;
        break;
    }
    return Color{byteOf(r + base), byteOf(g + base), byteOf(b + base)};
}

void toHsv(Color color, double& hueDegrees, double& saturation, double& value) noexcept {
    const double r = color.r / 255.0;
    const double g = color.g / 255.0;
    const double b = color.b / 255.0;
    const double high = std::max({r, g, b});
    const double low = std::min({r, g, b});
    const double chroma = high - low;

    value = high;
    saturation = high <= 0.0 ? 0.0 : chroma / high;
    if (chroma <= 0.0) {
        // A grey has no hue. Zero rather than "the last one" because this is also what a UI
        // puts in its hue box, and a box that remembers a hue for a color that has none
        // shows a number the operator cannot change anything by moving.
        hueDegrees = 0.0;
        return;
    }
    double hue = 0.0;
    if (high == r) {
        hue = std::fmod((g - b) / chroma, 6.0);
    } else if (high == g) {
        hue = (b - r) / chroma + 2.0;
    } else {
        hue = (r - g) / chroma + 4.0;
    }
    hue *= 60.0;
    if (hue < 0.0) {
        hue += 360.0;
    }
    hueDegrees = hue;
}

Color mix(Color from, Color to, double t) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    const auto step = [t](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>(std::lround(a + (static_cast<double>(b) - a) * t));
    };
    return Color{step(from.r, to.r), step(from.g, to.g), step(from.b, to.b)};
}

Color scale(Color color, double level) noexcept {
    level = std::clamp(level, 0.0, 1.0);
    const auto step = [level](std::uint8_t a) {
        return static_cast<std::uint8_t>(std::lround(a * level));
    };
    return Color{step(color.r), step(color.g), step(color.b)};
}

std::string formatColor(Color color) {
    char buffer[8] = {};
    std::snprintf(buffer, sizeof buffer, "#%02x%02x%02x", color.r, color.g, color.b);
    return std::string(buffer);
}

std::optional<Color> parseColor(std::string_view text) noexcept {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == '#') {
        text.remove_prefix(1);
    }
    if (text.find(',') != std::string_view::npos) {
        return parseTriple(text);
    }

    if (text.size() == 3) {
        // "#f24" — each digit doubled, which is what every web tool means by it.
        std::array<int, 3> parts{};
        for (std::size_t i = 0; i < 3; ++i) {
            const int digit = hexDigit(text[i]);
            if (digit < 0) {
                return std::nullopt;
            }
            parts[i] = digit * 16 + digit;
        }
        return Color{static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
                      static_cast<std::uint8_t>(parts[2])};
    }
    if (text.size() != 6) {
        return std::nullopt;
    }
    std::array<int, 3> parts{};
    for (std::size_t i = 0; i < 3; ++i) {
        const int high = hexDigit(text[i * 2]);
        const int low = hexDigit(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        parts[i] = high * 16 + low;
    }
    return Color{static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
                  static_cast<std::uint8_t>(parts[2])};
}

} // namespace takt4::dmx
