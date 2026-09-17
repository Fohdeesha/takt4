#include "core/dmx/effect.hpp"

#include <algorithm>
#include <cmath>

namespace takt4::dmx {
namespace {

struct EffectName {
    EffectKind kind;
    std::string_view label;
    std::string_view name;
};

constexpr std::array<EffectName, 10> kEffectNames{{
    {EffectKind::Level, "level / fade", "level"},
    {EffectKind::Color, "color", "color"},
    {EffectKind::Flash, "flash", "flash"},
    {EffectKind::Pulse, "pulse", "pulse"},
    {EffectKind::Strobe, "strobe", "strobe"},
    {EffectKind::HueSweep, "hue sweep", "hue-sweep"},
    {EffectKind::Position, "position", "position"},
    {EffectKind::Path, "path", "path"},
    {EffectKind::Home, "home", "home"},
    {EffectKind::Blackout, "blackout", "blackout"},
}};

struct CurveName {
    Curve curve;
    std::string_view label;
    std::string_view name;
};

constexpr std::array<CurveName, 4> kCurveNames{{
    {Curve::Linear, "linear", "linear"},
    {Curve::EaseIn, "ease in", "ease-in"},
    {Curve::EaseOut, "ease out", "ease-out"},
    {Curve::EaseInOut, "ease in-out", "ease-in-out"},
}};

struct ShapeName {
    PathShape shape;
    std::string_view label;
    std::string_view name;
};

constexpr std::array<ShapeName, 4> kShapeNames{{
    {PathShape::Circle, "circle", "circle"},
    {PathShape::Figure8, "figure 8", "figure-8"},
    {PathShape::Sweep, "sweep", "sweep"},
    {PathShape::Square, "square", "square"},
}};

} // namespace

std::string_view labelOf(EffectKind kind) noexcept {
    for (const EffectName& entry : kEffectNames) {
        if (entry.kind == kind) {
            return entry.label;
        }
    }
    return "level / fade";
}

std::string_view nameOf(EffectKind kind) noexcept {
    for (const EffectName& entry : kEffectNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "level";
}

std::optional<EffectKind> effectKindOf(std::string_view name) noexcept {
    for (const EffectName& entry : kEffectNames) {
        if (entry.name == name) {
            return entry.kind;
        }
    }
    // **The spelling this used to write.** takt4 said "colour" everywhere until a rig asked for
    // the other spelling on 2026-09-16, and a preset saved before that holds `"effect":
    // "colour"`. An effect name that will not read back silently becomes the default — so a
    // colour rule an operator built would have come back as a fade, which is a rule that does
    // something else rather than a rule that fails. Read only; nothing writes this now.
    if (name == "colour") {
        return EffectKind::Color;
    }
    return std::nullopt;
}

std::string_view labelOf(Curve curve) noexcept {
    for (const CurveName& entry : kCurveNames) {
        if (entry.curve == curve) {
            return entry.label;
        }
    }
    return "linear";
}

std::string_view nameOf(Curve curve) noexcept {
    for (const CurveName& entry : kCurveNames) {
        if (entry.curve == curve) {
            return entry.name;
        }
    }
    return "linear";
}

std::optional<Curve> curveOf(std::string_view name) noexcept {
    for (const CurveName& entry : kCurveNames) {
        if (entry.name == name) {
            return entry.curve;
        }
    }
    return std::nullopt;
}

double curveAt(Curve curve, double progress) noexcept {
    const double t = std::clamp(progress, 0.0, 1.0);
    switch (curve) {
    case Curve::EaseIn:
        return t * t;
    case Curve::EaseOut:
        return 1.0 - (1.0 - t) * (1.0 - t);
    case Curve::EaseInOut:
        // Smoothstep. Zero slope at both ends, which is what stops a head jerking as it
        // arrives; a cosine would do the same and costs a transcendental per channel per
        // round, which this is called often enough to care about.
        return t * t * (3.0 - 2.0 * t);
    case Curve::Linear:
        break;
    }
    return t;
}

std::string_view labelOf(PathShape shape) noexcept {
    for (const ShapeName& entry : kShapeNames) {
        if (entry.shape == shape) {
            return entry.label;
        }
    }
    return "circle";
}

std::string_view nameOf(PathShape shape) noexcept {
    for (const ShapeName& entry : kShapeNames) {
        if (entry.shape == shape) {
            return entry.name;
        }
    }
    return "circle";
}

std::optional<PathShape> pathShapeOf(std::string_view name) noexcept {
    for (const ShapeName& entry : kShapeNames) {
        if (entry.name == name) {
            return entry.shape;
        }
    }
    return std::nullopt;
}

} // namespace takt4::dmx
