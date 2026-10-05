#include "core/fixtures/natural_order.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_set>

namespace takt4::fixtures::natural {
namespace {

/// ICU's root order of printable ASCII (taken from Node's `localeCompare`): space, punctuation and
/// symbols, digits, then letters with each small letter just before its capital.
constexpr std::string_view kIcuOrder =
    " _-,;:!?.'\"()[]{}@*/"
    "\\&#%`^+<=>|~$0123456789aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ";

constexpr int kNonAscii = 1000;
constexpr int kControl = 500;

constexpr std::array<int, 128> ranks() {
    std::array<int, 128> table{};
    for (int c = 0; c < 128; ++c) {
        table[static_cast<std::size_t>(c)] = kControl + c;
    }
    for (std::size_t i = 0; i < kIcuOrder.size(); ++i) {
        const char c = kIcuOrder[i];
        table[static_cast<std::size_t>(static_cast<unsigned char>(c))] = static_cast<int>(i);
    }
    // A capital weighs what its small letter does; the case is compared afterwards.
    for (char c = 'A'; c <= 'Z'; ++c) {
        table[static_cast<std::size_t>(c)] = table[static_cast<std::size_t>(c - 'A' + 'a')];
    }
    return table;
}

constexpr std::array<int, 128> kRank = ranks();

struct Element {
    int primary = 0;
    /// For a number: its digits without leading zeros ("" for zero).
    std::string_view digits;
    bool number = false;
};

/// One code point of well-formed UTF-8 at `at`, and how many bytes it took. A byte that does not
/// begin a sequence counts as its own value.
std::uint32_t decode(std::string_view text, std::size_t at, std::size_t& length) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t count = lead < 0x80           ? 1
                        : (lead >> 5) == 0x6  ? 2
                        : (lead >> 4) == 0xE  ? 3
                        : (lead >> 3) == 0x1E ? 4
                                              : 1;
    if (at + count > text.size()) {
        count = 1;
    }
    std::uint32_t point = count == 1   ? lead
                          : count == 2 ? (lead & 0x1FU)
                          : count == 3 ? (lead & 0x0FU)
                                       : (lead & 0x07U);
    for (std::size_t i = 1; i < count; ++i) {
        point = (point << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3FU);
    }
    length = count;
    return point;
}

std::vector<Element> elementsOf(std::string_view text) {
    std::vector<Element> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const char c = text[at];
        if (c >= '0' && c <= '9') {
            std::size_t end = at;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
                ++end;
            }
            std::size_t first = at;
            while (first < end && text[first] == '0') {
                ++first;
            }
            out.push_back(Element{kRank[static_cast<std::size_t>('0')],
                                  text.substr(first, end - first), true});
            at = end;
            continue;
        }
        std::size_t length = 1;
        const std::uint32_t point = decode(text, at, length);
        const int primary =
            point < 128 ? kRank[point]
                        : kNonAscii + static_cast<int>(std::min<std::uint32_t>(point, 0x10FFFF));
        out.push_back(Element{primary, {}, false});
        at += length;
    }
    return out;
}

int comparePrimary(const std::vector<Element>& a, const std::vector<Element>& b) {
    const std::size_t count = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (a[i].primary != b[i].primary) {
            return a[i].primary < b[i].primary ? -1 : 1;
        }
        if (a[i].number && b[i].number) {
            if (a[i].digits.size() != b[i].digits.size()) {
                return a[i].digits.size() < b[i].digits.size() ? -1 : 1;
            }
            if (const int order = a[i].digits.compare(b[i].digits); order != 0) {
                return order < 0 ? -1 : 1;
            }
        }
    }
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    return 0;
}

bool isArrayIndex(std::string_view key) {
    if (key.empty() || key.size() > 10 || (key.size() > 1 && key.front() == '0')) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : key) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value <= 4294967294ULL;
}

} // namespace

int compare(std::string_view a, std::string_view b) {
    if (const int primary = comparePrimary(elementsOf(a), elementsOf(b)); primary != 0) {
        return primary;
    }
    // The letters' cases, in order, small first.
    std::size_t i = 0;
    std::size_t j = 0;
    while (true) {
        while (i < a.size() && !((a[i] >= 'a' && a[i] <= 'z') || (a[i] >= 'A' && a[i] <= 'Z'))) {
            ++i;
        }
        while (j < b.size() && !((b[j] >= 'a' && b[j] <= 'z') || (b[j] >= 'A' && b[j] <= 'Z'))) {
            ++j;
        }
        if (i >= a.size() || j >= b.size()) {
            return 0;
        }
        const bool upperA = a[i] <= 'Z';
        const bool upperB = b[j] <= 'Z';
        if (upperA != upperB) {
            return upperA ? 1 : -1;
        }
        ++i;
        ++j;
    }
}

std::vector<std::string> objectKeyOrder(const std::vector<std::string>& insertionOrder) {
    std::vector<std::string> indices;
    std::vector<std::string> rest;
    std::unordered_set<std::string_view> seen;
    for (const std::string& key : insertionOrder) {
        if (!seen.insert(key).second) {
            continue; // a key inserted twice keeps its first place
        }
        (isArrayIndex(key) ? indices : rest).push_back(key);
    }
    std::sort(indices.begin(), indices.end(), [](const std::string& x, const std::string& y) {
        return x.size() != y.size() ? x.size() < y.size() : x < y;
    });
    indices.insert(indices.end(), rest.begin(), rest.end());
    return indices;
}

std::vector<std::string> sorted(const std::vector<std::string>& inStructureOrder) {
    std::vector<std::string> keys = objectKeyOrder(inStructureOrder);
    std::stable_sort(keys.begin(), keys.end(),
                     [](const std::string& x, const std::string& y) { return compare(x, y) < 0; });
    return keys;
}

} // namespace takt4::fixtures::natural
