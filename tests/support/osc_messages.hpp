#pragma once

// What arrived, read back as OSC: the address and the arguments of every message in a datagram — a
// bundle's each in turn — copied out of it, so a test can keep them past the next receive.

#include "core/control/osc_parse.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takt4::testing {

struct OscArg {
    /// 'i', 'f', 's', 'T' or 'F', as the type tag says.
    char type = 'i';
    std::int32_t i = 0;
    float f = 0.0f;
    std::string s;
};

struct OscMsg {
    std::string address;
    std::vector<OscArg> args;
};

inline std::vector<OscMsg> oscMessagesIn(std::span<const std::byte> datagram) {
    std::vector<OscMsg> out;
    (void)control::forEachOscMessage(datagram, [&out](std::span<const std::byte> packet) {
        const std::optional<control::OscView> view = control::parseOsc(packet);
        if (!view) {
            return;
        }
        OscMsg message{std::string(view->address()), {}};
        for (std::size_t i = 0; i < view->argumentCount(); ++i) {
            const control::OscArgument& argument = view->argument(i);
            message.args.push_back(OscArg{static_cast<char>(argument.type), argument.asInt,
                                          argument.asFloat, std::string(argument.asString)});
        }
        out.push_back(std::move(message));
    });
    return out;
}

inline std::vector<OscMsg> oscMessagesIn(const std::string& datagram) {
    return oscMessagesIn(std::as_bytes(std::span<const char>(datagram.data(), datagram.size())));
}

/// The first message at `address`, or null.
inline const OscMsg* findOsc(const std::vector<OscMsg>& messages, std::string_view address) {
    for (const OscMsg& message : messages) {
        if (message.address == address) {
            return &message;
        }
    }
    return nullptr;
}

} // namespace takt4::testing
