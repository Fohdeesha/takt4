#include "core/fixtures/profile_mapper.hpp"

#include "core/fixtures/text.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace takt4::fixtures {
namespace {

bool hasOrdinal(Kind kind) {
    return kind == Kind::ColorWheel || kind == Kind::Gobo || kind == Kind::Zoom ||
           kind == Kind::Focus;
}

/// The kinds parked dark: everything that puts light out, and the dimmer, and a CMY head's flags
/// (out of the beam, which is no colour — the head is dark through its dimmer).
bool parksAtZero(Kind kind) {
    switch (kind) {
    case Kind::Dimmer:
    case Kind::Red:
    case Kind::Green:
    case Kind::Blue:
    case Kind::White:
    case Kind::WarmWhite:
    case Kind::CoolWhite:
    case Kind::Amber:
    case Kind::Uv:
    case Kind::CyanSub:
    case Kind::MagentaSub:
    case Kind::YellowSub:
    case Kind::OtherEmitter:
    case Kind::IndirectRed:
    case Kind::IndirectGreen:
    case Kind::IndirectBlue:
    case Kind::ColorBrightness:
        return true;
    default:
        return false;
    }
}

std::string_view wordsFor(Kind kind) {
    switch (kind) {
    case Kind::ColorWheel:
        return "color wheel";
    case Kind::Gobo:
        return "gobo wheel";
    case Kind::Zoom:
        return "zoom";
    case Kind::Focus:
        return "focus";
    default:
        return "channel";
    }
}

std::string ordinalWord(std::size_t rank) {
    static constexpr std::array<std::string_view, 9> kWords{
        "first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth", "ninth"};
    return rank >= 1 && rank <= kWords.size() ? std::string(kWords[rank - 1]) : "another";
}

/// One byte of a value held at `bytes` bytes — byte 0 the most significant.
std::uint8_t byteOf(std::uint64_t value, unsigned bytes, unsigned byte) {
    if (bytes < 1 || bytes > 8 || byte >= bytes) {
        return 0;
    }
    return static_cast<std::uint8_t>((value >> (8 * (bytes - 1 - byte))) & 0xFFu);
}

} // namespace

dmx::Role roleFor(Kind kind, std::uint8_t byte, bool shared, std::uint32_t ordinal,
                  std::uint32_t lowestOrdinal) noexcept {
    if (shared) {
        return dmx::Role::Unused;
    }
    if (kind == Kind::Pan || kind == Kind::Tilt) {
        if (byte == 0) {
            return kind == Kind::Pan ? dmx::Role::Pan : dmx::Role::Tilt;
        }
        if (byte == 1) {
            return kind == Kind::Pan ? dmx::Role::PanFine : dmx::Role::TiltFine;
        }
        return dmx::Role::Unused;
    }
    if (byte != 0) {
        return dmx::Role::Unused;
    }
    if (hasOrdinal(kind) && ordinal != lowestOrdinal) {
        return dmx::Role::Unused;
    }
    switch (kind) {
    case Kind::Dimmer:
        return dmx::Role::Dimmer;
    case Kind::Red:
    case Kind::IndirectRed:
        return dmx::Role::Red;
    case Kind::Green:
    case Kind::IndirectGreen:
        return dmx::Role::Green;
    case Kind::Blue:
    case Kind::IndirectBlue:
        return dmx::Role::Blue;
    case Kind::White:
    case Kind::WarmWhite:
    case Kind::CoolWhite:
        return dmx::Role::White;
    case Kind::Amber:
        return dmx::Role::Amber;
    case Kind::Uv:
        return dmx::Role::Uv;
    case Kind::CyanSub:
        return dmx::Role::Cyan;
    case Kind::MagentaSub:
        return dmx::Role::Magenta;
    case Kind::YellowSub:
        return dmx::Role::Yellow;
    case Kind::Shutter:
        return dmx::Role::Strobe;
    case Kind::ColorWheel:
        return dmx::Role::ColorWheel;
    case Kind::Gobo:
        return dmx::Role::Gobo;
    case Kind::Zoom:
        return dmx::Role::Zoom;
    case Kind::Focus:
        return dmx::Role::Focus;
    case Kind::PanTiltSpeed:
        return dmx::Role::Speed;
    default:
        return dmx::Role::Unused;
    }
}

FixtureProfile mapDefinition(const Definition& definition) {
    FixtureProfile profile;
    profile.format = definition.format;
    profile.key = definition.key;
    profile.manufacturer = definition.manufacturer;
    profile.model = definition.model;
    profile.revision = definition.revision;
    profile.file = definition.fileName;
    profile.notes = definition.notes;
    for (const DefMode& mode : definition.modes) {
        ProfileMode out;
        out.name = mode.name;
        out.notes = mode.notes;
        out.refused = mode.refusal;
        if (!mode.refusal.empty()) {
            profile.modes.push_back(std::move(out));
            continue;
        }
        // "Ordinal 1" is the lowest present, over the whole mode.
        std::map<Kind, std::set<std::uint32_t>> ordinals;
        for (const std::vector<DefChannel>& part : mode.parts) {
            for (const DefChannel& channel : part) {
                if (hasOrdinal(channel.kind)) {
                    ordinals[channel.kind].insert(channel.ordinal);
                }
            }
        }
        for (const std::vector<DefChannel>& part : mode.parts) {
            std::vector<ProfileChannel>& mapped = out.parts.emplace_back();
            mapped.reserve(part.size());
            for (const DefChannel& channel : part) {
                ProfileChannel to;
                to.label = channel.label;
                const std::uint32_t lowest =
                    hasOrdinal(channel.kind) ? *ordinals[channel.kind].begin() : 0;
                to.role =
                    roleFor(channel.kind, channel.byte, channel.shared, channel.ordinal, lowest);

                // The parked level, from one value at the channel's full resolution.
                const unsigned bytes = std::clamp<unsigned>(channel.resolutionBytes, 1, 8);
                std::vector<std::string> notes = channel.notes;
                std::uint64_t value = 0;
                if (channel.kind == Kind::Nothing) {
                    value = channel.defaultValue.value_or(0);
                } else if (parksAtZero(channel.kind)) {
                    value = 0;
                } else if (channel.kind == Kind::Shutter) {
                    if (channel.openValue) {
                        value = *channel.openValue;
                    } else {
                        value = channel.defaultValue.value_or(channel.fallbackValue);
                        if (channel.byte == 0) {
                            notes.emplace_back("the file doesn't say which value opens the "
                                               "shutter — check its parked level");
                        }
                    }
                } else if (channel.kind == Kind::Pan || channel.kind == Kind::Tilt) {
                    // Centred: 128, then 0 — the middle of the travel at any resolution.
                    value = channel.defaultValue.value_or(std::uint64_t{0x80} << (8 * (bytes - 1)));
                } else {
                    value = channel.defaultValue.value_or(channel.fallbackValue);
                }
                to.parked = byteOf(value, bytes, channel.byte);

                // What the projection lost, said on the channel — in words for someone who has
                // never opened a fixture file (the operator, 2026-10-05).
                if (channel.shared) {
                    notes.push_back((channel.sharedWith.empty()
                                         ? std::string("part of its range does something else")
                                         : "part of its range is " + channel.sharedWith) +
                                    " — given no role, so takt4 never sends it there");
                }
                if (hasOrdinal(channel.kind) && channel.byte == 0 && channel.ordinal != lowest) {
                    const std::set<std::uint32_t>& present = ordinals[channel.kind];
                    const auto rank = static_cast<std::size_t>(
                        std::distance(present.begin(), present.find(channel.ordinal)) + 1);
                    notes.push_back(ordinalWord(rank) + " " + std::string(wordsFor(channel.kind)) +
                                    " — takt4 drives the first");
                }
                if (channel.byte >= 1 && !channel.shared) {
                    const dmx::Role coarse =
                        roleFor(channel.kind, 0, false, channel.ordinal, lowest);
                    const bool movement = channel.kind == Kind::Pan || channel.kind == Kind::Tilt;
                    if (coarse != dmx::Role::Unused && (!movement || channel.byte >= 2)) {
                        notes.emplace_back(movement ? "extra-fine adjustment — takt4 sets the "
                                                      "main and fine channels only"
                                                    : "fine adjustment — takt4 sets the main "
                                                      "channel only");
                    }
                }
                if (channel.kind == Kind::HsbOrCie) {
                    notes.emplace_back(
                        "sets color by hue, saturation or color coordinates — takt4 can't "
                        "drive that");
                }
                to.note = text::clean(text::join(notes, "; "), 400);
                mapped.push_back(std::move(to));
            }
        }
        profile.modes.push_back(std::move(out));
    }
    return profile;
}

} // namespace takt4::fixtures
