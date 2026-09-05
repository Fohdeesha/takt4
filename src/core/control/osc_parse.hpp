#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace takt4::control {

/// HANDOFF §5.7's half of OSC: reading it.
///
/// `output::OscMessage` builds messages and this reads them, and they deliberately share
/// no code — building is "append and pad", reading is "check everything the sender might
/// have got wrong", and the two have almost nothing in common but the format.
///
/// **Everything arriving here came off a socket**, which is to say from anywhere on the
/// network. A malformed datagram, a truncated one, a hostile one: the only acceptable
/// answer is `std::nullopt`. Nothing here throws, allocates, or reads outside the span it
/// was given.

/// One argument, as the type tag said it was. No conversions are invented: a receiver
/// asking for an int from a string gets nothing.
struct OscArgument {
    enum class Type : char {
        Int32 = 'i',
        Float32 = 'f',
        String = 's',
        True = 'T',
        False = 'F',
    };

    Type type = Type::Int32;
    std::int32_t asInt = 0;
    float asFloat = 0.0f;
    /// Borrowed from the datagram, valid only while it is.
    std::string_view asString;
};

/// A parsed OSC 1.0 message. Borrows the buffer it was parsed from: the address and any
/// string arguments point into the datagram, which has to outlive this.
class OscView {
public:
    /// As many as `output::OscMessage` will build. A message with more is refused rather
    /// than truncated — silently dropping arguments is how a control surface ends up
    /// doing something other than what it said.
    static constexpr std::size_t kMaxArguments = 16;

    std::string_view address() const noexcept { return address_; }
    std::size_t argumentCount() const noexcept { return count_; }
    const OscArgument& argument(std::size_t index) const noexcept { return arguments_[index]; }

    /// The argument at `index` as a number, when it is one. `T` and `F` count: a Stream
    /// Deck sending a boolean means the same as one sending 1 or 0, and refusing it would
    /// be pedantry the operator pays for.
    std::optional<double> number(std::size_t index) const noexcept;

    /// §5.7's `<0|1>` arguments. True for a non-zero number or `T`; false for zero or
    /// `F`; nothing for a message that carried no argument at all, which callers treat as
    /// "the sender did not say" rather than as false.
    std::optional<bool> flag(std::size_t index) const noexcept;

private:
    friend std::optional<OscView> parseOsc(std::span<const std::byte> packet) noexcept;

    std::string_view address_;
    OscArgument arguments_[kMaxArguments];
    std::size_t count_ = 0;
};

/// Parses one datagram, or returns nothing.
///
/// Refused: anything not a multiple of four bytes, an address that does not start with
/// '/' or is not terminated inside the packet, a type tag string that does not start with
/// ',', a tag this does not know (the argument's width would have to be guessed), an
/// argument that runs past the end, and more than `kMaxArguments`.
///
/// A message with nothing after its address is accepted as one with no arguments. OSC 1.0
/// requires the type tag string, but enough senders omit it for an address-only message
/// that refusing them would mean refusing real control surfaces.
///
/// **Bundles are not handled** — `#bundle` returns nothing. Nothing in §5.7 needs one, and
/// accepting a container of messages means deciding what their timestamps mean.
std::optional<OscView> parseOsc(std::span<const std::byte> packet) noexcept;

} // namespace takt4::control
