#pragma once

// The console's own reading of its arguments, shared by the commands that send to the outputs —
// `track` and `follow` — so a number and an OSC target are refused in the same words by both.

#include <cmath>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace takt4::cli {

inline int parseInt(std::string_view text, std::string_view what) {
    std::size_t consumed = 0;
    const std::string s(text);
    int value = 0;
    try {
        value = std::stoi(s, &consumed);
    } catch (const std::exception&) {
        consumed = 0;
    }
    // An empty value consumed all of its nothing, and used to come back as 0: `--device ""`
    // opened device 0.
    if (s.empty() || consumed != s.size()) {
        throw std::invalid_argument(std::string(what) + ": not a number: \"" + s + "\"");
    }
    return value;
}

inline double parseDouble(std::string_view text, std::string_view what) {
    std::size_t consumed = 0;
    const std::string s(text);
    double value = 0.0;
    try {
        value = std::stod(s, &consumed);
    } catch (const std::exception&) {
        consumed = 0;
    }
    // Empty, as for parseInt; and `stod` reads "nan" and "inf" as numbers, which no option
    // here can mean.
    if (s.empty() || consumed != s.size() || !std::isfinite(value)) {
        throw std::invalid_argument(std::string(what) + ": not a number: \"" + s + "\"");
    }
    return value;
}

/// `HOST:PORT`, for `--osc`. The last colon separates the port, so a bare IPv6 address is still
/// usable.
inline std::pair<std::string, std::uint16_t> parseOscTarget(std::string_view target,
                                                            std::string_view what) {
    const std::size_t colon = target.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        throw std::invalid_argument(std::string(what) + " expects HOST:PORT");
    }
    const int port = parseInt(target.substr(colon + 1), what);
    if (port < 1 || port > 65535) {
        throw std::invalid_argument(std::string(what) + ": the port must be 1 to 65535");
    }
    return {std::string(target.substr(0, colon)), static_cast<std::uint16_t>(port)};
}

} // namespace takt4::cli
