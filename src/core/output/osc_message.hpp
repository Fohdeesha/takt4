#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace takt4::output {

/// One OSC 1.0 message, built into a fixed buffer.
///
/// HANDOFF §6 lists the OSC encoder as ours rather than a dependency, and this is why it
/// can be: a message is an address pattern, a type tag string and the arguments, each
/// null-terminated and padded with nulls to a multiple of four bytes, integers and
/// floats big-endian. There is nothing else to it, and no library is worth the
/// dependency.
///
/// Nothing here allocates. A message that would not fit, or that was given an address
/// OSC does not allow, goes invalid and stays that way rather than sending nonsense.
class OscMessage {
public:
    /// Enough for every address in HANDOFF §5.6 and §5.7 with room to spare; a
    /// Resolume clip address with both indices filled in is about 45 bytes.
    static constexpr std::size_t kCapacity = 256;
    static constexpr std::size_t kMaxArguments = 16;

    /// An address must begin with '/' and hold none of OSC 1.0's reserved characters
    /// (space, '#', '*', ',', '?', '[', ']', '{', '}') nor anything outside printable
    /// ASCII. One that does leaves the message invalid.
    explicit OscMessage(std::string_view address) noexcept;

    OscMessage& addInt(std::int32_t value) noexcept;
    OscMessage& addFloat(float value) noexcept;
    OscMessage& addString(std::string_view value) noexcept;

    /// False if the address was rejected or anything since would not fit.
    bool valid() const noexcept { return valid_; }

    /// The finished packet. Empty when the message is invalid. Assembling is idempotent;
    /// adding another argument after this is called starts it over.
    std::span<const std::byte> packet() noexcept;

    std::string_view address() const noexcept { return address_; }
    std::size_t argumentCount() const noexcept { return tagCount_; }

private:
    void appendArgument(const void* bytes, std::size_t size, char tag) noexcept;

    std::string_view address_;
    std::array<char, kMaxArguments + 2> tags_{}; // ',' then one per argument
    std::size_t tagCount_ = 0;
    std::array<std::byte, kCapacity> arguments_{};
    std::size_t argumentBytes_ = 0;
    std::array<std::byte, kCapacity> packet_{};
    std::size_t packetBytes_ = 0;
    bool valid_ = true;
    bool assembled_ = false;
};

} // namespace takt4::output
