#include "core/trigger/value.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <system_error>

namespace takt4::trigger {

namespace {

/// A float as an address segment or a settings file should spell it, into `out`.
///
/// `snprintf` rather than `std::to_chars`, which for *floating point* is the one part of
/// <charconv> that is not reliably present everywhere this has to build — libc++ was years
/// behind the others on it, and `core.yml` compiles this file on macOS. The integer
/// overloads are universal and are used directly.
std::size_t formatFloat(char (&out)[32], float value) noexcept {
    // %g so a whole number comes out "4" rather than "4.000000", which is what an operator
    // typing a layer number would expect to see back.
    const int written = std::snprintf(out, sizeof(out), "%g", static_cast<double>(value));
    if (written <= 0) {
        return 0;
    }
    return std::min(static_cast<std::size_t>(written), sizeof(out) - 1);
}

} // namespace

Value Value::ofInt(std::int32_t value) noexcept {
    Value made;
    made.kind_ = Kind::Int;
    made.int_ = value;
    return made;
}

Value Value::ofFloat(float value) noexcept {
    Value made;
    made.kind_ = Kind::Float;
    made.float_ = value;
    return made;
}

Value Value::ofBool(bool value) noexcept {
    Value made;
    made.kind_ = Kind::Bool;
    made.int_ = value ? 1 : 0;
    return made;
}

Value Value::ofText(std::string_view value) noexcept {
    Value made;
    made.kind_ = Kind::Text;
    const std::size_t length = std::min(value.size(), kTextCapacity);
    made.truncated_ = length < value.size();
    if (length > 0) {
        std::memcpy(made.text_.data(), value.data(), length);
    }
    made.textLength_ = static_cast<std::uint8_t>(length);
    return made;
}

std::int32_t Value::asInt() const noexcept {
    switch (kind_) {
    case Kind::Int:
    case Kind::Bool:
        return int_;
    case Kind::Float: {
        if (!std::isfinite(float_)) {
            return 0;
        }
        const double rounded = std::round(static_cast<double>(float_));
        // Saturate rather than wrap: a MIDI note built from a wrapped negative is a note
        // nobody asked for, and clamping at least stays on the right side of the range.
        constexpr auto low = static_cast<double>(std::numeric_limits<std::int32_t>::min());
        constexpr auto high = static_cast<double>(std::numeric_limits<std::int32_t>::max());
        return static_cast<std::int32_t>(std::clamp(rounded, low, high));
    }
    case Kind::Text: {
        const char* begin = text_.data();
        const char* end = begin + textLength_;
        std::int32_t parsed = 0;
        const std::from_chars_result result = std::from_chars(begin, end, parsed);
        return result.ec == std::errc{} ? parsed : 0;
    }
    }
    return 0;
}

float Value::asFloat() const noexcept {
    switch (kind_) {
    case Kind::Int:
    case Kind::Bool:
        return static_cast<float>(int_);
    case Kind::Float:
        return float_;
    case Kind::Text:
        // Through the integer parse deliberately: a float parse would need
        // `std::from_chars` for floating point, which is the overload this file avoids.
        // Text holding "1.5" is a generator misconfigured, not a case to support.
        return static_cast<float>(asInt());
    }
    return 0.0f;
}

bool Value::asBool() const noexcept {
    switch (kind_) {
    case Kind::Int:
    case Kind::Bool:
        return int_ != 0;
    case Kind::Float:
        return float_ != 0.0f;
    case Kind::Text:
        return textLength_ != 0;
    }
    return false;
}

std::string_view Value::text() const noexcept {
    if (kind_ != Kind::Text) {
        return {};
    }
    return {text_.data(), textLength_};
}

void Value::appendTo(std::string& out) const {
    switch (kind_) {
    case Kind::Text:
        out.append(text_.data(), textLength_);
        return;
    case Kind::Bool:
        out.push_back(int_ != 0 ? '1' : '0');
        return;
    case Kind::Float: {
        char buffer[32];
        out.append(buffer, formatFloat(buffer, float_));
        return;
    }
    case Kind::Int: {
        char buffer[16];
        const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), int_);
        if (result.ec == std::errc{}) {
            out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
        }
        return;
    }
    }
}

bool Value::operator==(const Value& other) const noexcept {
    if (kind_ != other.kind_) {
        return false;
    }
    switch (kind_) {
    case Kind::Int:
    case Kind::Bool:
        return int_ == other.int_;
    case Kind::Float:
        return float_ == other.float_;
    case Kind::Text:
        return text() == other.text();
    }
    return false;
}

} // namespace takt4::trigger
