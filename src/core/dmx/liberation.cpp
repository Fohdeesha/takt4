#include "core/dmx/liberation.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace takt4::dmx::liberation {
namespace {

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<int> wholeNumber(std::string_view text) noexcept {
    text = trim(text);
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::string format(Clip clip) {
    return std::to_string(clip.column) + "-" + std::to_string(clip.row);
}

std::optional<Clip> parseClip(std::string_view text) {
    text = trim(text);
    // The column's digits, then one separator with any spaces around it, then the row's.
    std::size_t split = 0;
    while (split < text.size() && text[split] >= '0' && text[split] <= '9') {
        ++split;
    }
    std::size_t rest = split;
    while (rest < text.size() && text[rest] == ' ') {
        ++rest;
    }
    if (rest < text.size() && (text[rest] == '-' || text[rest] == '.')) {
        ++rest;
    } else if (rest == split) {
        return std::nullopt; // two numbers run together, or nothing after the first
    }
    const std::optional<int> column = wholeNumber(text.substr(0, split));
    const std::optional<int> row = wholeNumber(text.substr(rest));
    if (!column || !row || *column < 0 || *column >= kMaxColumns || *row < 0 || *row >= kRows) {
        return std::nullopt;
    }
    return Clip{*column, *row};
}

std::optional<std::pair<Clip, Clip>> parseRange(std::string_view text) {
    // The dash inside a clip's name is why a range cannot be split on one: "to" first, then the
    // separators that cannot be read as part of a clip.
    for (const std::string_view separator : {std::string_view("to"), std::string_view(".."),
                                             std::string_view("\xE2\x80\x93") /* en dash */}) {
        const std::size_t at = text.find(separator);
        if (at == std::string_view::npos) {
            continue;
        }
        const std::optional<Clip> from = parseClip(text.substr(0, at));
        const std::optional<Clip> to = parseClip(text.substr(at + separator.size()));
        if (from && to) {
            return std::pair{*from, *to};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::string formatRange(int low, int high) {
    return formatIndex(low) + " to " + formatIndex(high);
}

std::optional<Clip> decode(std::uint8_t bank, std::uint8_t select) noexcept {
    // Liberation's own decoding, as its document gives it:
    //   slot  = 1 + floor((goboSelect - 1) * 40 / 255), clamped to 1..40
    //   gridX = goboBank * 8 + floor((slot - 1) / 5)
    //   gridY = (slot - 1) % 5
    if (select == 0) {
        return std::nullopt;
    }
    const int slot = std::clamp(1 + (select - 1) * kSlotsPerBank / 255, 1, kSlotsPerBank);
    return Clip{bank * kColumnsPerBank + (slot - 1) / kRows, (slot - 1) % kRows};
}

Fixture zone(std::string name, PortAddress universe, std::uint16_t address) {
    Fixture fixture;
    fixture.name = std::move(name);
    fixture.universe = universe;
    fixture.address = address;
    fixture.channels.assign(kZoneRoles.begin(), kZoneRoles.end());
    fixture.parked.assign(kZoneParked.begin(), kZoneParked.end());
    for (const std::string_view label : kZoneLabels) {
        fixture.labels.emplace_back(label);
    }
    return fixture;
}

bool isZone(const Fixture& fixture) noexcept {
    return std::equal(fixture.channels.begin(), fixture.channels.end(), kZoneRoles.begin(),
                      kZoneRoles.end());
}

} // namespace takt4::dmx::liberation
