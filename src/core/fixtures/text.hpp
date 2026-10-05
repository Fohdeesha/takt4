#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The few text operations both readers need, all **locale-free**: a definition is read the same
/// on every machine, whatever its language settings, so nothing here touches `<locale>`, `tolower`
/// or `strtod`. Case is folded for ASCII letters only, which is what the formats mean by it.
namespace takt4::fixtures::text {

/// The longest label a channel keeps, in characters (code points, not bytes).
inline constexpr std::size_t kLabelLimit = 80;

char foldAscii(char c) noexcept;
std::string lowerAscii(std::string_view text);
std::string upperAscii(std::string_view text);
bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept;
bool startsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept;

/// Without leading or trailing spaces, tabs and line breaks.
std::string_view trim(std::string_view text) noexcept;

/// `text` made well-formed UTF-8 (`io::validUtf8`), trimmed, and cut to `limit` characters —
/// never in the middle of one. What every name and label from a file goes through.
std::string clean(std::string_view text, std::size_t limit = kLabelLimit);

/// A whole decimal integer, sign allowed, surrounding spaces allowed, nothing else.
std::optional<std::int64_t> parseInteger(std::string_view text) noexcept;
/// A decimal number with "." as the separator, whatever the machine's locale.
std::optional<double> parseNumber(std::string_view text) noexcept;

/// "ColorTemperatureFine" as "Color Temperature Fine": a space before a capital that follows a
/// small letter or a digit, and before the last capital of a run followed by a small letter
/// ("LEDFrequency" → "LED Frequency"). Anything else is left as written.
std::string splitCamelCase(std::string_view text);

/// Whether "open" appears as a word of its own, ignoring case: not inside "opener" or
/// "reopen", but in "Open", "Shutter Open", "Main_Shutter_Open" and "No Func - Shutter Open".
/// Letters are a to z; anything else — a digit, an underscore, a space — separates words.
bool hasWordOpen(std::string_view text) noexcept;

/// `parts` joined by `separator`.
std::string join(const std::vector<std::string>& parts, std::string_view separator);

} // namespace takt4::fixtures::text
