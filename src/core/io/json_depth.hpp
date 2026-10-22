#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace takt4::io {

/// Deeper than anything takt4 reads or writes — a settings file, its rules and its fixture
/// library are a handful of levels each — and shallow enough that nothing that walks a document
/// runs out of stack: nlohmann's `dump`, and a copy of a node, go down a level per call.
inline constexpr int kMaxJsonDepth = 64;

/// `nlohmann::json::parse`, refusing a document nested deeper than `kMaxJsonDepth` — with a
/// `std::runtime_error` that says so — rather than parsing it for something to overflow the stack
/// on later. A file of twenty kilobytes of brackets did that to IMPORT, and, as the rig's own file,
/// to every launch. Throws what `parse` throws otherwise.
inline nlohmann::json parseJsonCapped(std::string_view text) {
    return nlohmann::json::parse(
        text, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > kMaxJsonDepth) {
                throw std::runtime_error("it is nested more than " + std::to_string(kMaxJsonDepth) +
                                         " levels deep, which nothing takt4 writes is");
            }
            return true;
        });
}

} // namespace takt4::io
