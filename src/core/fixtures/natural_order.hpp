#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace takt4::fixtures::natural {

/// Two pixel keys in the Open Fixture Library's "natural" order — `a.localeCompare(b, undefined,
/// {numeric: true})`, which is ICU's root collation with numbers read as numbers — reproduced for
/// the keys fixture files use:
///
///  1. **Primary**: a run of digits is one number (leading zeros ignored), and sorts with the
///     digits; every other character by its place in ICU's order for printable ASCII, space first,
///     then punctuation, then digits, then letters, with a letter's case folded (`a` and `A` one
///     weight); anything outside ASCII after all of those, by code point.
///  2. Then the letters' cases in order, lower case first.
///  3. Otherwise equal ("9" and "09").
///
/// Negative, zero or positive, as `localeCompare` returns.
int compare(std::string_view a, std::string_view b);

/// Pixel keys in OFL's `eachPixelABC` order. `inStructureOrder` is every distinct key in the
/// order the matrix lists them (z, then y, then x). As JavaScript does before sorting, keys that
/// are array indices ("0", "7", "12" — not "07") come first, in numeric order, and the rest in the
/// order given; the sort is stable, so keys that compare equal keep that order.
std::vector<std::string> sorted(const std::vector<std::string>& inStructureOrder);

/// JavaScript's `Object.keys` order for keys inserted in this order: array indices first, in
/// numeric order, then the rest as inserted. What "the order of the file" means for an object's
/// keys in OFL's model.
std::vector<std::string> objectKeyOrder(const std::vector<std::string>& insertionOrder);

} // namespace takt4::fixtures::natural
