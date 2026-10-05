#include "core/fixtures/definition.hpp"

namespace takt4::fixtures {

std::string_view nameOf(Kind kind) noexcept {
    switch (kind) {
    case Kind::Nothing:
        return "nothing";
    case Kind::Dimmer:
        return "dimmer";
    case Kind::Red:
        return "red";
    case Kind::Green:
        return "green";
    case Kind::Blue:
        return "blue";
    case Kind::White:
        return "white";
    case Kind::WarmWhite:
        return "warm white";
    case Kind::CoolWhite:
        return "cool white";
    case Kind::Amber:
        return "amber";
    case Kind::Uv:
        return "uv";
    case Kind::CyanSub:
        return "cyan (subtractive)";
    case Kind::MagentaSub:
        return "magenta (subtractive)";
    case Kind::YellowSub:
        return "yellow (subtractive)";
    case Kind::OtherEmitter:
        return "other emitter";
    case Kind::IndirectRed:
        return "red (indirect)";
    case Kind::IndirectGreen:
        return "green (indirect)";
    case Kind::IndirectBlue:
        return "blue (indirect)";
    case Kind::HsbOrCie:
        return "hue/saturation/CIE";
    case Kind::ColorBrightness:
        return "color brightness";
    case Kind::Pan:
        return "pan";
    case Kind::Tilt:
        return "tilt";
    case Kind::Shutter:
        return "shutter";
    case Kind::StrobeRate:
        return "strobe rate";
    case Kind::ColorWheel:
        return "color wheel";
    case Kind::Gobo:
        return "gobo";
    case Kind::Zoom:
        return "zoom";
    case Kind::Focus:
        return "focus";
    case Kind::PanTiltSpeed:
        return "pan/tilt speed";
    case Kind::Other:
        return "other";
    }
    return "other";
}

} // namespace takt4::fixtures
