#include "core/output/osc_message.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace takt4::output {

namespace {

/// OSC pads every part out to a multiple of four with nulls, and every part is
/// null-terminated, so a name of exactly four characters takes eight bytes.
constexpr std::size_t paddedSize(std::size_t length) noexcept {
    return (length + 4) & ~std::size_t{3};
}

/// Writes `text`, its terminator, and the padding. Returns false if it would not fit.
bool writePadded(std::span<std::byte> out, std::size_t& at, std::string_view text) noexcept {
    const std::size_t size = paddedSize(text.size());
    if (at + size > out.size()) {
        return false;
    }
    std::memcpy(out.data() + at, text.data(), text.size());
    std::memset(reinterpret_cast<unsigned char*>(out.data()) + at + text.size(), 0,
                size - text.size());
    at += size;
    return true;
}

/// OSC is big-endian throughout, whatever the host is.
void writeBigEndian(std::byte* out, const void* value, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(value);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>(
            std::endian::native == std::endian::little ? bytes[size - 1 - i] : bytes[i]);
    }
}

} // namespace

bool addressIsLegal(std::string_view address) noexcept {
    if (address.empty() || address.front() != '/') {
        return false;
    }
    for (const char c : address) {
        if (c < '\x21' || c > '\x7E') { // no space, no control, no non-ASCII
            return false;
        }
        if (std::string_view("#*,?[]{}").find(c) != std::string_view::npos) {
            return false;
        }
    }
    return true;
}

OscMessage::OscMessage(std::string_view address) noexcept : address_(address) {
    tags_[0] = ',';
    valid_ = addressIsLegal(address) && paddedSize(address.size()) < kCapacity;
}

void OscMessage::appendArgument(const void* bytes, std::size_t size, char tag) noexcept {
    if (!valid_) {
        return;
    }
    if (tagCount_ >= kMaxArguments || argumentBytes_ + size > arguments_.size()) {
        valid_ = false;
        return;
    }
    if (size > 0) {
        writeBigEndian(arguments_.data() + argumentBytes_, bytes, size);
        argumentBytes_ += size;
    }
    tags_[1 + tagCount_] = tag;
    ++tagCount_;
    assembled_ = false;
}

OscMessage& OscMessage::addInt(std::int32_t value) noexcept {
    appendArgument(&value, sizeof value, 'i');
    return *this;
}

OscMessage& OscMessage::addFloat(float value) noexcept {
    appendArgument(&value, sizeof value, 'f');
    return *this;
}

OscMessage& OscMessage::addString(std::string_view value) noexcept {
    if (!valid_) {
        return *this;
    }
    if (tagCount_ >= kMaxArguments) {
        valid_ = false;
        return *this;
    }
    std::size_t at = argumentBytes_;
    if (!writePadded(arguments_, at, value)) {
        valid_ = false;
        return *this;
    }
    argumentBytes_ = at;
    tags_[1 + tagCount_] = 's';
    ++tagCount_;
    assembled_ = false;
    return *this;
}

std::span<const std::byte> OscMessage::packet() noexcept {
    if (assembled_) {
        return std::span<const std::byte>(packet_.data(), packetBytes_);
    }
    packetBytes_ = 0;
    if (!valid_) {
        return {};
    }
    std::size_t at = 0;
    const std::string_view tags(tags_.data(), tagCount_ + 1);
    if (!writePadded(packet_, at, address_) || !writePadded(packet_, at, tags) ||
        at + argumentBytes_ > packet_.size()) {
        valid_ = false;
        return {};
    }
    std::memcpy(packet_.data() + at, arguments_.data(), argumentBytes_);
    at += argumentBytes_;
    packetBytes_ = at;
    assembled_ = true;
    return std::span<const std::byte>(packet_.data(), packetBytes_);
}

} // namespace takt4::output
