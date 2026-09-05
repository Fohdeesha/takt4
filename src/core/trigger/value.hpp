#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace takt4::trigger {

/// One value a rule sends — HANDOFF §5.8's *"Fixed | Literal int, float, string or bool"*,
/// and what every other generator produces too.
///
/// Four kinds, because that is what the two transports underneath can carry between them:
/// OSC 1.0 as `output::OscMessage` encodes it (int32, float32, string) and MIDI note and CC
/// numbers (int). A bool is carried as an int by both and kept separate here only so a UI
/// can offer a checkbox and a settings file can say `true` — §5.8 lists it, so it exists.
///
/// **Nothing here allocates**, which is why the text case is a fixed buffer rather than a
/// `std::string`. The output thread is allowed to allocate (§4.2 forbids it only on the
/// audio thread), so this is not a correctness requirement — but a value is copied for
/// every segment of every address of every rule that fires, and a rule firing on every beat
/// at 214 BPM is 3.6 of these a second per segment. Text longer than `kTextCapacity` is
/// truncated rather than rejected: an address segment or a clip name that long is a
/// mistake, and silently sending a shorter one is less bad than a rule that stops firing.
class Value {
public:
    enum class Kind : std::uint8_t { Int, Float, Text, Bool };

    /// Long enough for any address segment or short label; see the note above about what
    /// happens past it.
    static constexpr std::size_t kTextCapacity = 47;

    /// Integer zero — what an unconfigured generator produces, and what a conversion that
    /// cannot be made falls back to.
    Value() noexcept = default;

    static Value ofInt(std::int32_t value) noexcept;
    static Value ofFloat(float value) noexcept;
    static Value ofBool(bool value) noexcept;
    /// Truncated to `kTextCapacity`. `truncated()` says whether it was.
    static Value ofText(std::string_view value) noexcept;

    Kind kind() const noexcept { return kind_; }
    bool truncated() const noexcept { return truncated_; }

    /// Read as whichever type is wanted, converting where a conversion is meaningful.
    ///
    /// These are total: every kind gives an answer for every accessor, because the
    /// alternative is a rule that silently sends nothing when an operator picks a float
    /// generator for a MIDI note. A float becomes an int by rounding to nearest, a bool by
    /// being non-zero; text becomes a number by parsing its leading digits, or 0.
    std::int32_t asInt() const noexcept;
    float asFloat() const noexcept;
    bool asBool() const noexcept;

    /// The text of the value, for a Text one. Empty for the others — use `appendTo`, which
    /// spells all four.
    std::string_view text() const noexcept;

    /// Appends the value as an OSC address segment would spell it: digits for an int, `0`
    /// or `1` for a bool, the shortest round-tripping form for a float, the characters
    /// themselves for text. The one place a `std::string` is touched, and the only reason
    /// is that an address is assembled into one.
    void appendTo(std::string& out) const;

    /// Value equality, which for text means the same characters. Used by the no-repeat
    /// guard in `Generator`, so it has to mean "an operator would call this the same clip".
    bool operator==(const Value& other) const noexcept;
    bool operator!=(const Value& other) const noexcept { return !(*this == other); }

private:
    Kind kind_ = Kind::Int;
    bool truncated_ = false;
    std::uint8_t textLength_ = 0;
    std::int32_t int_ = 0;
    float float_ = 0.0f;
    std::array<char, kTextCapacity> text_{};
};

} // namespace takt4::trigger
