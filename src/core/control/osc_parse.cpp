#include "core/control/osc_parse.hpp"

#include <array>
#include <bit>
#include <cstring>

namespace takt4::control {
namespace {

/// OSC pads every part to a multiple of four bytes *after* its terminator, so a
/// four-character address takes eight.
constexpr std::size_t kAlign = 4;

/// "#bundle" and its terminator, which every bundle begins with, and then its eight-byte time
/// tag: what comes before a bundle's first element.
constexpr std::array<char, 8> kBundleTag{'#', 'b', 'u', 'n', 'd', 'l', 'e', '\0'};
constexpr std::size_t kBundleHeader = 16;

std::size_t padded(std::size_t length) noexcept {
    return (length + kAlign) & ~(kAlign - 1);
}

/// A null-terminated string starting at `offset`, and where the next part begins. Nothing
/// when there is no terminator inside the packet — which is what a truncated datagram
/// looks like, and the reason this cannot use `strlen`.
struct Part {
    std::string_view text;
    std::size_t next = 0;
};

std::optional<Part> readString(std::span<const std::byte> packet, std::size_t offset) noexcept {
    for (std::size_t i = offset; i < packet.size(); ++i) {
        if (packet[i] != std::byte{0}) {
            continue;
        }
        const std::size_t length = i - offset;
        const std::size_t next = offset + padded(length);
        if (next > packet.size()) {
            return std::nullopt; // the padding itself runs off the end
        }
        return Part{std::string_view(reinterpret_cast<const char*>(packet.data() + offset), length),
                    next};
    }
    return std::nullopt;
}

std::optional<std::uint32_t> readBig32(std::span<const std::byte> packet,
                                       std::size_t offset) noexcept {
    if (offset + 4 > packet.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(packet[offset]) << 24 |
           static_cast<std::uint32_t>(packet[offset + 1]) << 16 |
           static_cast<std::uint32_t>(packet[offset + 2]) << 8 |
           static_cast<std::uint32_t>(packet[offset + 3]);
}

bool validAddress(std::string_view address) noexcept {
    if (address.empty() || address.front() != '/') {
        return false;
    }
    for (const char c : address) {
        if (c < ' ' || c > '~') {
            return false;
        }
    }
    return true;
}

std::size_t walk(std::span<const std::byte> packet,
                 const std::function<void(std::span<const std::byte>)>& visit, int depth) {
    if (!isOscBundle(packet)) {
        visit(packet);
        return 1;
    }
    if (depth >= kMaxBundleDepth) {
        return 0;
    }
    // Each element a size and that many bytes, a message or a bundle of its own. One whose size
    // is not a whole number of words, or runs past the bundle, is not an element at all: the walk
    // ends there, after the ones before it.
    std::size_t visited = 0;
    std::size_t offset = kBundleHeader;
    while (offset < packet.size()) {
        const std::optional<std::uint32_t> size = readBig32(packet, offset);
        if (!size || *size == 0 || *size % kAlign != 0 || *size > packet.size() - offset - 4) {
            break;
        }
        visited += walk(packet.subspan(offset + 4, *size), visit, depth + 1);
        offset += 4 + *size;
    }
    return visited;
}

} // namespace

std::optional<double> OscView::number(std::size_t index) const noexcept {
    if (index >= count_) {
        return std::nullopt;
    }
    switch (arguments_[index].type) {
    case OscArgument::Type::Int32:
        return static_cast<double>(arguments_[index].asInt);
    case OscArgument::Type::Float32:
        return static_cast<double>(arguments_[index].asFloat);
    case OscArgument::Type::True:
        return 1.0;
    case OscArgument::Type::False:
        return 0.0;
    case OscArgument::Type::String:
        break;
    }
    return std::nullopt;
}

std::optional<OscView> parseOsc(std::span<const std::byte> packet) noexcept {
    // A packet that is not a whole number of four-byte words cannot be OSC whatever else
    // is true of it, and checking first means nothing below has to handle a ragged end.
    if (packet.empty() || packet.size() % kAlign != 0) {
        return std::nullopt;
    }

    // "#bundle" is no address, and so no message: `forEachOscMessage` is what opens one.
    const std::optional<Part> address = readString(packet, 0);
    if (!address || !validAddress(address->text)) {
        return std::nullopt;
    }

    OscView view;
    view.address_ = address->text;
    if (address->next == packet.size()) {
        return view; // address only, no type tag string
    }

    const std::optional<Part> tags = readString(packet, address->next);
    if (!tags || tags->text.empty() || tags->text.front() != ',') {
        return std::nullopt;
    }

    std::size_t offset = tags->next;
    for (const char tag : tags->text.substr(1)) {
        if (view.count_ >= OscView::kMaxArguments) {
            return std::nullopt;
        }
        OscArgument& argument = view.arguments_[view.count_];
        switch (tag) {
        case 'i': {
            const std::optional<std::uint32_t> bits = readBig32(packet, offset);
            if (!bits) {
                return std::nullopt;
            }
            argument.type = OscArgument::Type::Int32;
            argument.asInt = std::bit_cast<std::int32_t>(*bits);
            offset += 4;
            break;
        }
        case 'f': {
            const std::optional<std::uint32_t> bits = readBig32(packet, offset);
            if (!bits) {
                return std::nullopt;
            }
            argument.type = OscArgument::Type::Float32;
            argument.asFloat = std::bit_cast<float>(*bits);
            offset += 4;
            break;
        }
        case 's': {
            const std::optional<Part> text = readString(packet, offset);
            if (!text) {
                return std::nullopt;
            }
            argument.type = OscArgument::Type::String;
            argument.asString = text->text;
            offset = text->next;
            break;
        }
        case 'T':
            argument.type = OscArgument::Type::True;
            break;
        case 'F':
            argument.type = OscArgument::Type::False;
            break;
        default:
            // Every other tag has a width this does not know, so the arguments after it
            // cannot be found. Guessing would mean acting on numbers nobody sent.
            return std::nullopt;
        }
        ++view.count_;
    }
    return view;
}

bool isOscBundle(std::span<const std::byte> packet) noexcept {
    return packet.size() >= kBundleHeader &&
           std::memcmp(packet.data(), kBundleTag.data(), kBundleTag.size()) == 0;
}

std::size_t forEachOscMessage(std::span<const std::byte> packet,
                              const std::function<void(std::span<const std::byte>)>& visit) {
    return walk(packet, visit, 0);
}

} // namespace takt4::control
