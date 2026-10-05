#include "core/fixtures/gdtf_attributes.hpp"

#include "core/fixtures/text.hpp"

#include <array>
#include <string>

namespace takt4::fixtures::gdtf {
namespace {

struct Words {
    /// Annex A's spelling, with "(n)" and "(m)" for a positive number.
    std::string_view pattern;
    /// What a channel with that attribute is labelled, the same numbers carried through.
    std::string_view label;
};

/// **Every attribute of GDTF 1.2's Annex A, in its order**, with the words a person reads it by.
/// Lower case, as the rest of the patch editor; the abbreviations a lighting person already
/// knows (CTO, CRI, LED, UV, DMX) kept as they are.
constexpr Words kWords[] = {
    {"Dimmer", "dimmer"},
    {"Pan", "pan"},
    {"Tilt", "tilt"},
    {"PanRotate", "pan spin"},
    {"TiltRotate", "tilt spin"},
    {"PositionEffect", "position effect"},
    {"PositionEffectRate", "position effect rate"},
    {"PositionEffectFade", "position effect fade"},
    {"XYZ_X", "X position"},
    {"XYZ_Y", "Y position"},
    {"XYZ_Z", "Z position"},
    {"Rot_X", "X rotation"},
    {"Rot_Y", "Y rotation"},
    {"Rot_Z", "Z rotation"},
    {"Scale_X", "X scale"},
    {"Scale_Y", "Y scale"},
    {"Scale_Z", "Z scale"},
    {"Scale_XYZ", "scale"},
    {"Gobo(n)", "gobo wheel (n)"},
    {"Gobo(n)SelectSpin", "gobo wheel (n) select + spin"},
    {"Gobo(n)SelectShake", "gobo wheel (n) select + shake"},
    {"Gobo(n)SelectEffects", "gobo wheel (n) effects"},
    {"Gobo(n)WheelIndex", "gobo wheel (n) index"},
    {"Gobo(n)WheelSpin", "gobo wheel (n) spin"},
    {"Gobo(n)WheelShake", "gobo wheel (n) shake"},
    {"Gobo(n)WheelRandom", "gobo wheel (n) random"},
    {"Gobo(n)WheelAudio", "gobo wheel (n) audio"},
    {"Gobo(n)Pos", "gobo (n) rotation"},
    {"Gobo(n)PosRotate", "gobo (n) spin"},
    {"Gobo(n)PosShake", "gobo (n) shake"},
    {"AnimationWheel(n)", "animation wheel (n)"},
    {"AnimationWheel(n)Audio", "animation wheel (n) audio"},
    {"AnimationWheel(n)Macro", "animation wheel (n) macro"},
    {"AnimationWheel(n)Random", "animation wheel (n) random"},
    {"AnimationWheel(n)SelectEffects", "animation wheel (n) effects"},
    {"AnimationWheel(n)SelectShake", "animation wheel (n) select + shake"},
    {"AnimationWheel(n)SelectSpin", "animation wheel (n) select + spin"},
    {"AnimationWheel(n)Pos", "animation (n) rotation"},
    {"AnimationWheel(n)PosRotate", "animation (n) spin"},
    {"AnimationWheel(n)PosShake", "animation (n) shake"},
    {"AnimationSystem(n)", "animation system (n)"},
    {"AnimationSystem(n)Ramp", "animation system (n) ramp"},
    {"AnimationSystem(n)Shake", "animation system (n) shake"},
    {"AnimationSystem(n)Audio", "animation system (n) audio"},
    {"AnimationSystem(n)Random", "animation system (n) random"},
    {"AnimationSystem(n)Pos", "animation system (n) rotation"},
    {"AnimationSystem(n)PosRotate", "animation system (n) spin"},
    {"AnimationSystem(n)PosShake", "animation system (n) rotation shake"},
    {"AnimationSystem(n)PosRandom", "animation system (n) rotation random"},
    {"AnimationSystem(n)PosAudio", "animation system (n) rotation audio"},
    {"AnimationSystem(n)Macro", "animation system (n) macro"},
    {"MediaFolder(n)", "media folder (n)"},
    {"MediaContent(n)", "media content (n)"},
    {"ModelFolder(n)", "model folder (n)"},
    {"ModelContent(n)", "model content (n)"},
    {"PlayMode", "play mode"},
    {"PlayBegin", "play begin"},
    {"PlayEnd", "play end"},
    {"PlaySpeed", "play speed"},
    {"ColorEffects(n)", "color effects (n)"},
    {"Color(n)", "color wheel (n)"},
    {"Color(n)WheelIndex", "color wheel (n) index"},
    {"Color(n)WheelSpin", "color wheel (n) spin"},
    {"Color(n)WheelRandom", "color wheel (n) random"},
    {"Color(n)WheelAudio", "color wheel (n) audio"},
    {"ColorAdd_R", "red"},
    {"ColorAdd_G", "green"},
    {"ColorAdd_B", "blue"},
    {"ColorAdd_C", "cyan (LED)"},
    {"ColorAdd_M", "magenta (LED)"},
    {"ColorAdd_Y", "yellow (LED)"},
    {"ColorAdd_RY", "amber"},
    {"ColorAdd_GY", "lime"},
    {"ColorAdd_GC", "blue-green"},
    {"ColorAdd_BC", "light blue"},
    {"ColorAdd_BM", "purple"},
    {"ColorAdd_RM", "pink"},
    {"ColorAdd_W", "white"},
    {"ColorAdd_WW", "warm white"},
    {"ColorAdd_CW", "cool white"},
    {"ColorAdd_UV", "UV"},
    {"ColorSub_R", "red filter"},
    {"ColorSub_G", "green filter"},
    {"ColorSub_B", "blue filter"},
    {"ColorSub_C", "cyan"},
    {"ColorSub_M", "magenta"},
    {"ColorSub_Y", "yellow"},
    {"ColorMacro(n)", "color macro (n)"},
    {"ColorMacro(n)Rate", "color macro (n) rate"},
    {"CTO", "CTO"},
    {"CTC", "CTC"},
    {"CTB", "CTB"},
    {"Tint", "tint"},
    {"HSB_Hue", "hue"},
    {"HSB_Saturation", "saturation"},
    {"HSB_Brightness", "brightness"},
    {"HSB_Quality", "color quality"},
    {"CIE_X", "CIE x"},
    {"CIE_Y", "CIE y"},
    {"CIE_Brightness", "CIE brightness"},
    {"ColorRGB_Red", "red (indirect)"},
    {"ColorRGB_Green", "green (indirect)"},
    {"ColorRGB_Blue", "blue (indirect)"},
    {"ColorRGB_Cyan", "cyan (indirect)"},
    {"ColorRGB_Magenta", "magenta (indirect)"},
    {"ColorRGB_Yellow", "yellow (indirect)"},
    {"ColorRGB_Quality", "color quality (indirect)"},
    {"VideoBoost_R", "video boost red"},
    {"VideoBoost_G", "video boost green"},
    {"VideoBoost_B", "video boost blue"},
    {"VideoHueShift", "video hue shift"},
    {"VideoSaturation", "video saturation"},
    {"VideoBrightness", "video brightness"},
    {"VideoContrast", "video contrast"},
    {"VideoKeyColor_R", "video key red"},
    {"VideoKeyColor_G", "video key green"},
    {"VideoKeyColor_B", "video key blue"},
    {"VideoKeyIntensity", "video key intensity"},
    {"VideoKeyTolerance", "video key tolerance"},
    {"StrobeDuration", "strobe duration"},
    {"StrobeRate", "strobe rate"},
    {"StrobeFrequency", "strobe frequency"},
    {"StrobeModeShutter", "strobe mode: shutter"},
    {"StrobeModeStrobe", "strobe mode: strobe"},
    {"StrobeModePulse", "strobe mode: pulse"},
    {"StrobeModePulseOpen", "strobe mode: opening pulse"},
    {"StrobeModePulseClose", "strobe mode: closing pulse"},
    {"StrobeModeRandom", "strobe mode: random"},
    {"StrobeModeRandomPulse", "strobe mode: random pulse"},
    {"StrobeModeRandomPulseOpen", "strobe mode: random opening pulse"},
    {"StrobeModeRandomPulseClose", "strobe mode: random closing pulse"},
    {"StrobeModeEffect", "strobe mode: effect"},
    {"Shutter(n)", "shutter (n)"},
    {"Shutter(n)Strobe", "strobe (n)"},
    {"Shutter(n)StrobePulse", "pulse strobe (n)"},
    {"Shutter(n)StrobePulseClose", "closing pulse strobe (n)"},
    {"Shutter(n)StrobePulseOpen", "opening pulse strobe (n)"},
    {"Shutter(n)StrobeRandom", "random strobe (n)"},
    {"Shutter(n)StrobeRandomPulse", "random pulse strobe (n)"},
    {"Shutter(n)StrobeRandomPulseClose", "random closing pulse strobe (n)"},
    {"Shutter(n)StrobeRandomPulseOpen", "random opening pulse strobe (n)"},
    {"Shutter(n)StrobeEffect", "strobe effect (n)"},
    {"Iris", "iris"},
    {"IrisStrobe", "iris strobe"},
    {"IrisStrobeRandom", "iris random strobe"},
    {"IrisPulseClose", "iris closing pulse"},
    {"IrisPulseOpen", "iris opening pulse"},
    {"IrisRandomPulseClose", "iris random closing pulse"},
    {"IrisRandomPulseOpen", "iris random opening pulse"},
    {"Frost(n)", "frost (n)"},
    {"Frost(n)PulseOpen", "frost (n) opening pulse"},
    {"Frost(n)PulseClose", "frost (n) closing pulse"},
    {"Frost(n)Ramp", "frost (n) ramp"},
    {"Prism(n)", "prism (n)"},
    {"Prism(n)SelectSpin", "prism (n) select + spin"},
    {"Prism(n)Macro", "prism (n) macro"},
    {"Prism(n)Pos", "prism (n) rotation"},
    {"Prism(n)PosRotate", "prism (n) spin"},
    {"Effects(n)", "effects (n)"},
    {"Effects(n)Rate", "effects (n) rate"},
    {"Effects(n)Fade", "effects (n) fade"},
    {"Effects(n)Adjust(m)", "effects (n) adjust (m)"},
    {"Effects(n)Pos", "effects (n) rotation"},
    {"Effects(n)PosRotate", "effects (n) spin"},
    {"EffectsSync", "effects sync"},
    {"BeamShaper", "beam shaper"},
    {"BeamShaperMacro", "beam shaper macro"},
    {"BeamShaperPos", "beam shaper rotation"},
    {"BeamShaperPosRotate", "beam shaper spin"},
    {"Zoom", "zoom"},
    {"ZoomModeSpot", "zoom spot mode"},
    {"ZoomModeBeam", "zoom beam mode"},
    {"DigitalZoom", "digital zoom"},
    {"Focus(n)", "focus (n)"},
    {"Focus(n)Adjust", "focus (n) adjust"},
    {"Focus(n)Distance", "focus (n) distance"},
    {"Control(n)", "control (n)"},
    {"DimmerMode", "dimmer mode"},
    {"DimmerCurve", "dimmer curve"},
    {"BlackoutMode", "blackout mode"},
    {"LEDFrequency", "LED frequency"},
    {"LEDZoneMode", "LED zone mode"},
    {"PixelMode", "pixel mode"},
    {"PanMode", "pan mode"},
    {"TiltMode", "tilt mode"},
    {"PanTiltMode", "pan/tilt mode"},
    {"PositionModes", "position mode"},
    {"Gobo(n)WheelMode", "gobo wheel (n) mode"},
    {"GoboWheelShortcutMode", "gobo wheel shortcut mode"},
    {"AnimationWheel(n)Mode", "animation wheel (n) mode"},
    {"AnimationWheelShortcutMode", "animation wheel shortcut mode"},
    {"Color(n)Mode", "color wheel (n) mode"},
    {"ColorWheelShortcutMode", "color wheel shortcut mode"},
    {"CyanMode", "cyan mode"},
    {"MagentaMode", "magenta mode"},
    {"YellowMode", "yellow mode"},
    {"ColorMixMode", "color mix mode"},
    {"ChromaticMode", "chromatic mode"},
    {"ColorCalibrationMode", "color calibration mode"},
    {"ColorConsistency", "color consistency"},
    {"ColorControl", "color control"},
    {"ColorModelMode", "color model mode"},
    {"ColorSettingsReset", "color settings reset"},
    {"ColorUniformity", "color uniformity"},
    {"CRIMode", "CRI mode"},
    {"CustomColor", "custom color"},
    {"UVStability", "UV stability"},
    {"WavelengthCorrection", "wavelength correction"},
    {"WhiteCount", "white count"},
    {"StrobeMode", "strobe mode"},
    {"ZoomMode", "zoom mode"},
    {"FocusMode", "focus mode"},
    {"IrisMode", "iris mode"},
    {"Fan(n)Mode", "fan (n) mode"},
    {"FollowSpotMode", "follow spot mode"},
    {"BeamEffectIndexRotateMode", "beam effect index/rotate mode"},
    {"IntensityMSpeed", "intensity speed"},
    {"PositionMSpeed", "pan/tilt speed"},
    {"ColorMixMSpeed", "color mix speed"},
    {"ColorWheelSelectMSpeed", "color wheel speed"},
    {"GoboWheel(n)MSpeed", "gobo wheel (n) speed"},
    {"IrisMSpeed", "iris speed"},
    {"Prism(n)MSpeed", "prism (n) speed"},
    {"FocusMSpeed", "focus speed"},
    {"Frost(n)MSpeed", "frost (n) speed"},
    {"ZoomMSpeed", "zoom speed"},
    {"FrameMSpeed", "shaper speed"},
    {"GlobalMSpeed", "global speed"},
    {"ReflectorAdjust", "reflector adjust"},
    {"FixtureGlobalReset", "fixture reset"},
    {"DimmerReset", "dimmer reset"},
    {"ShutterReset", "shutter reset"},
    {"BeamReset", "beam reset"},
    {"ColorMixReset", "color mix reset"},
    {"ColorWheelReset", "color wheel reset"},
    {"FocusReset", "focus reset"},
    {"FrameReset", "shaper reset"},
    {"GoboWheelReset", "gobo wheel reset"},
    {"IntensityReset", "intensity reset"},
    {"IrisReset", "iris reset"},
    {"PositionReset", "pan/tilt reset"},
    {"PanReset", "pan reset"},
    {"TiltReset", "tilt reset"},
    {"ZoomReset", "zoom reset"},
    {"CTBReset", "CTB reset"},
    {"CTOReset", "CTO reset"},
    {"CTCReset", "CTC reset"},
    {"AnimationSystemReset", "animation system reset"},
    {"FixtureCalibrationReset", "calibration reset"},
    {"Function", "function"},
    {"LampControl", "lamp control"},
    {"DisplayIntensity", "display intensity"},
    {"DMXInput", "DMX input"},
    {"NoFeature", "no function"},
    {"Blower(n)", "blower (n)"},
    {"Fan(n)", "fan (n)"},
    {"Fog(n)", "fog (n)"},
    {"Haze(n)", "haze (n)"},
    {"LampPowerMode", "lamp power mode"},
    {"Fans", "fans"},
    {"Blade(n)A", "blade (n) A"},
    {"Blade(n)B", "blade (n) B"},
    {"Blade(n)Rot", "blade (n) rotation"},
    {"ShaperRot", "shaper rotation"},
    {"ShaperMacros", "shaper macros"},
    {"ShaperMacrosSpeed", "shaper macros speed"},
    {"BladeSoft(n)A", "soft blade (n) A"},
    {"BladeSoft(n)B", "soft blade (n) B"},
    {"KeyStone(n)A", "keystone (n) A"},
    {"KeyStone(n)B", "keystone (n) B"},
    {"Video", "video"},
    {"VideoEffect(n)Type", "video effect (n)"},
    {"VideoEffect(n)Parameter(m)", "video effect (n) parameter (m)"},
    {"VideoCamera(n)", "video camera (n)"},
    {"VideoSoundVolume(n)", "video volume (n)"},
    {"VideoBlendMode", "video blend mode"},
    {"InputSource", "input source"},
    {"FieldOfView", "field of view"},
};

/// The largest number a "(n)" takes. A file numbering its gobo wheels in the millions is not
/// describing a fixture, and the cap keeps the arithmetic far from overflow.
constexpr std::uint32_t kLargestNumber = 999999;

bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

/// Whether `text` is `pattern` with each "(n)" and "(m)" standing for a positive number, ignoring
/// case — and the numbers it stood for.
bool matches(std::string_view pattern, std::string_view text, std::uint32_t& n, std::uint32_t& m) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < pattern.size()) {
        const std::string_view rest = pattern.substr(i);
        if (rest.starts_with("(n)") || rest.starts_with("(m)")) {
            std::size_t digits = 0;
            std::uint64_t value = 0;
            while (j + digits < text.size() && isDigit(text[j + digits])) {
                value = value * 10 + static_cast<std::uint64_t>(text[j + digits] - '0');
                ++digits;
                if (value > kLargestNumber) {
                    return false;
                }
            }
            if (digits == 0 || value == 0) {
                return false;
            }
            (rest[1] == 'n' ? n : m) = static_cast<std::uint32_t>(value);
            i += 3;
            j += digits;
            continue;
        }
        if (j >= text.size() || text::foldAscii(pattern[i]) != text::foldAscii(text[j])) {
            return false;
        }
        ++i;
        ++j;
    }
    return j == text.size();
}

const Words* lookUp(std::string_view attribute, std::uint32_t& n, std::uint32_t& m) {
    for (const Words& words : kWords) {
        n = 0;
        m = 0;
        if (matches(words.pattern, attribute, n, m)) {
            return &words;
        }
    }
    return nullptr;
}

/// "Shutter" and a positive number at the start of `attribute`, and where it ends; 0 if not.
std::size_t shutterPrefix(std::string_view attribute) {
    constexpr std::string_view stem = "Shutter";
    if (!text::startsWithIgnoreCase(attribute, stem)) {
        return 0;
    }
    std::size_t at = stem.size();
    std::uint64_t value = 0;
    std::size_t digits = 0;
    while (at < attribute.size() && isDigit(attribute[at]) && digits < 7) {
        value = value * 10 + static_cast<std::uint64_t>(attribute[at] - '0');
        ++at;
        ++digits;
    }
    if (digits == 0 || value == 0 || (at < attribute.size() && isDigit(attribute[at]))) {
        return 0;
    }
    return at;
}

/// `stem` and a positive number, and nothing after it — "Color2", "Gobo1", "Focus3".
bool numbered(std::string_view attribute, std::string_view stem, std::uint32_t& n) {
    std::uint32_t unused = 0;
    const std::string pattern = std::string(stem) + "(n)";
    return matches(pattern, attribute, n, unused);
}

} // namespace

std::string labelOfAttribute(std::string_view attribute) {
    attribute = text::trim(attribute);
    std::uint32_t n = 0;
    std::uint32_t m = 0;
    const Words* words = lookUp(attribute, n, m);
    if (words == nullptr) {
        return text::splitCamelCase(attribute);
    }
    std::string out;
    const std::string_view label = words->label;
    for (std::size_t i = 0; i < label.size(); ++i) {
        const std::string_view rest = label.substr(i);
        if (rest.starts_with("(n)") || rest.starts_with("(m)")) {
            out += std::to_string(rest[1] == 'n' ? n : m);
            i += 2;
            continue;
        }
        out += label[i];
    }
    return out;
}

bool isAnnexAttribute(std::string_view attribute) {
    std::uint32_t n = 0;
    std::uint32_t m = 0;
    return lookUp(text::trim(attribute), n, m) != nullptr;
}

bool isShutterAttribute(std::string_view attribute) {
    attribute = text::trim(attribute);
    const std::size_t end = shutterPrefix(attribute);
    return end != 0 && end == attribute.size();
}

bool isShutterStrobeAttribute(std::string_view attribute) {
    attribute = text::trim(attribute);
    const std::size_t end = shutterPrefix(attribute);
    return end != 0 && text::startsWithIgnoreCase(attribute.substr(end), "Strobe");
}

bool isNoFeature(std::string_view attribute) {
    return text::equalsIgnoreCase(text::trim(attribute), "NoFeature");
}

Kind kindOfAttribute(std::string_view attribute, bool shutterInMode, std::uint32_t& ordinal) {
    attribute = text::trim(attribute);
    ordinal = 0;
    struct Exact {
        std::string_view name;
        Kind kind;
    };
    static constexpr std::array<Exact, 35> kExact{{
        {"Dimmer", Kind::Dimmer},
        {"ColorAdd_R", Kind::Red},
        {"ColorAdd_G", Kind::Green},
        {"ColorAdd_B", Kind::Blue},
        {"ColorAdd_W", Kind::White},
        {"ColorAdd_WW", Kind::WarmWhite},
        {"ColorAdd_CW", Kind::CoolWhite},
        {"ColorAdd_RY", Kind::Amber},
        {"ColorAdd_UV", Kind::Uv},
        {"ColorAdd_C", Kind::OtherEmitter},
        {"ColorAdd_M", Kind::OtherEmitter},
        {"ColorAdd_Y", Kind::OtherEmitter},
        {"ColorAdd_GY", Kind::OtherEmitter},
        {"ColorAdd_GC", Kind::OtherEmitter},
        {"ColorAdd_BC", Kind::OtherEmitter},
        {"ColorAdd_BM", Kind::OtherEmitter},
        {"ColorAdd_RM", Kind::OtherEmitter},
        {"ColorSub_C", Kind::CyanSub},
        {"ColorSub_M", Kind::MagentaSub},
        {"ColorSub_Y", Kind::YellowSub},
        {"ColorRGB_Red", Kind::IndirectRed},
        {"ColorRGB_Green", Kind::IndirectGreen},
        {"ColorRGB_Blue", Kind::IndirectBlue},
        {"HSB_Hue", Kind::HsbOrCie},
        {"HSB_Saturation", Kind::HsbOrCie},
        {"HSB_Quality", Kind::HsbOrCie},
        {"CIE_X", Kind::HsbOrCie},
        {"CIE_Y", Kind::HsbOrCie},
        {"HSB_Brightness", Kind::ColorBrightness},
        {"CIE_Brightness", Kind::ColorBrightness},
        {"Pan", Kind::Pan},
        {"Tilt", Kind::Tilt},
        {"StrobeDuration", Kind::StrobeRate},
        {"StrobeRate", Kind::StrobeRate},
        {"StrobeFrequency", Kind::StrobeRate},
    }};
    for (const Exact& exact : kExact) {
        if (text::equalsIgnoreCase(attribute, exact.name)) {
            return exact.kind;
        }
    }
    if (text::equalsIgnoreCase(attribute, "PositionMSpeed")) {
        return Kind::PanTiltSpeed;
    }
    if (text::equalsIgnoreCase(attribute, "Zoom")) {
        ordinal = 1;
        return Kind::Zoom;
    }
    if (isShutterAttribute(attribute)) {
        return Kind::Shutter;
    }
    if (isShutterStrobeAttribute(attribute)) {
        // Two strobe roles would get one level: beside a plain shutter, a strobe channel is a
        // rate (Cameo Zenit's strobe-effect channel). On its own it is the shutter there is.
        return shutterInMode ? Kind::StrobeRate : Kind::Shutter;
    }
    std::uint32_t n = 0;
    if (numbered(attribute, "Color", n)) {
        ordinal = n;
        return Kind::ColorWheel;
    }
    if (numbered(attribute, "Gobo", n)) {
        ordinal = n;
        return Kind::Gobo;
    }
    if (numbered(attribute, "Focus", n)) {
        ordinal = n;
        return Kind::Focus;
    }
    return Kind::Other;
}

} // namespace takt4::fixtures::gdtf
