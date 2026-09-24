#pragma once

#include "core/dmx/color.hpp"
#include "core/dmx/fixture.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takt4::dmx {

/// What an effect does to the channels it is aimed at.
///
/// **Every one of these is bounded by a duration, and that is a design decision rather than
/// an omission.** A strobe that runs "until told otherwise" is the obvious thing to offer and
/// the wrong thing to build here: the failure mode is a fixture left strobing because the
/// rule that was going to stop it was disabled, edited or never fired, and it is the kind of
/// failure that happens in front of an audience. takt4's whole shape is *rules that fire on
/// the music*, so a strobe for two bars re-fired every two bars is both the natural gesture
/// and the one that cannot get stuck. A rule switched off fires no more, and whatever it had
/// already started runs out its duration — nothing stops it early, which is exactly why the
/// duration has to be there. PANIC stops every effect at once and freezes the levels where
/// they are (`DmxEngine::cancelAll`).
enum class EffectKind : std::uint8_t {
    /// One role to one level. With a duration it is a fade; with none it is a snap, and the
    /// two are the same instruction because *"fade this to full over two bars"* and *"put
    /// this at full"* differ by one number and should not be two entries in a dropdown.
    ///
    /// The editor offers "fade in" and "fade out" as what they are — this, with the target at
    /// full or at zero — because those are the words an operator uses.
    Level,
    /// The three color channels to one color, with the same snap-or-fade duration. A
    /// fixture with a White channel has it driven to zero, so that a color is the color
    /// asked for and not that color plus whatever white was left over.
    ///
    /// **A fade is in RGB and not in HSV, deliberately**: each channel runs in a straight
    /// line on its own, so red to green dims towards a dark olive and comes back up rather
    /// than passing through yellow. That is what "fade this fixture from one color to
    /// another" means to everyone who has done it on a desk; the wheel is `HueSweep`, a
    /// different instruction.
    Color,
    /// Straight to a peak and decay back to a base over the duration — the beat hit. Distinct
    /// from `Level` because it starts by *jumping*, which no ramp does, and because the decay
    /// is the part the duration describes.
    Flash,
    /// A cosine between two levels, `cycles` times over the duration. What "breathing" is.
    Pulse,
    /// On and off between two levels, `cycles` times over the duration, `duty` of each cycle
    /// spent on. A strobe on takt4's own dimmer channel rather than the fixture's, so it is
    /// locked to the music; a fixture's own strobe channel is faster and is reachable as
    /// `Level` on `Role::Strobe`.
    Strobe,
    /// The hue wheel from one angle to another over the duration, at the saturation and value
    /// of the rule's color — full saturation for a color that has none, such as the white a
    /// rule starts with, which would otherwise sweep nothing. The one color move that a
    /// straight `Color` fade deliberately does not do — see `Color`.
    HueSweep,
    /// Pan and tilt to one position, over the duration. The position is drawn when the rule
    /// fires (so a "random position" is a random *number*, resolved once, and the engine that
    /// executes it is deterministic); the duration is the move time.
    Position,
    /// Pan and tilt around a centre, tracing `shape`, `cycles` times over the duration.
    Path,
    /// Pan and tilt to the middle of the fixture's own movement window, over the duration.
    /// Its own kind rather than a `Position` at 0.5/0.5 because "home" should stay home when
    /// the operator changes the window.
    Home,
    /// Every light-emitting channel this fixture has to zero, over the duration: dimmer,
    /// color, white, amber and UV. Not the shutter, not the movement, not the speed — those
    /// stay where they are, because a fixture that is *dark* should still be ready for the
    /// next rule, and a shutter closed by a blackout would stay dark when it fires.
    Blackout,
};

inline constexpr std::array<EffectKind, 10> kEffectKinds{
    EffectKind::Level,  EffectKind::Color,   EffectKind::Flash,    EffectKind::Pulse,
    EffectKind::Strobe, EffectKind::HueSweep, EffectKind::Position, EffectKind::Path,
    EffectKind::Home,   EffectKind::Blackout};

std::string_view labelOf(EffectKind kind) noexcept;
std::string_view nameOf(EffectKind kind) noexcept;
std::optional<EffectKind> effectKindOf(std::string_view name) noexcept;

/// Whether the kind is aimed at one nameable role (`Payload::role`), rather than at color or
/// at movement. What tells the editor whether to show its channel dropdown.
constexpr bool takesRole(EffectKind kind) noexcept {
    return kind == EffectKind::Level || kind == EffectKind::Flash || kind == EffectKind::Pulse ||
           kind == EffectKind::Strobe;
}

/// Whether the kind writes the color channels.
constexpr bool takesColor(EffectKind kind) noexcept {
    return kind == EffectKind::Color || kind == EffectKind::HueSweep;
}

/// Whether the kind moves a head — and so does nothing at all on a fixture with no pan.
constexpr bool takesMovement(EffectKind kind) noexcept {
    return kind == EffectKind::Position || kind == EffectKind::Path || kind == EffectKind::Home;
}

/// Whether the kind repeats within its duration, and so has a `cycles` count.
constexpr bool takesCycles(EffectKind kind) noexcept {
    return kind == EffectKind::Pulse || kind == EffectKind::Strobe || kind == EffectKind::Path;
}

/// Whether the kind sweeps between two levels, and so has a `base` as well as a `level`.
constexpr bool takesBase(EffectKind kind) noexcept {
    return kind == EffectKind::Flash || kind == EffectKind::Pulse || kind == EffectKind::Strobe;
}

/// How a value gets from where it was to where it is going.
///
/// A fade is not usually wanted linear: a linear fade up reads as slow then sudden, because
/// the eye's response to light is not linear either. `EaseOut` is the one most fades want and
/// `Linear` is the one most *moves* want, which is why the default depends on the effect
/// rather than being one constant.
enum class Curve : std::uint8_t {
    Linear,
    /// Slow to start.
    EaseIn,
    /// Quick to start, settling at the end — what a fade up and a flash decay both want.
    EaseOut,
    /// Both, which is what a move between two positions wants so the head does not jerk at
    /// either end.
    EaseInOut,
};

inline constexpr std::array<Curve, 4> kCurves{Curve::Linear, Curve::EaseIn, Curve::EaseOut,
                                              Curve::EaseInOut};

std::string_view labelOf(Curve curve) noexcept;
std::string_view nameOf(Curve curve) noexcept;
std::optional<Curve> curveOf(std::string_view name) noexcept;

/// `progress` (0 to 1) shaped by `curve`, and still 0 to 1.
double curveAt(Curve curve, double progress) noexcept;

/// The figure a `Path` traces around its centre.
enum class PathShape : std::uint8_t {
    /// Pan and tilt in quadrature — the classic slow orbit.
    Circle,
    /// Tilt at twice pan's rate: a lissajous figure-of-eight, which reads as a head looking
    /// around rather than sweeping.
    Figure8,
    /// Pan only, tilt held at the centre. A fan across the room, and the one that reads
    /// cleanest on a row of heads.
    Sweep,
    /// Four corners, stepped rather than swept. The movement equivalent of a square wave, for
    /// when the music is not smooth either.
    Square,
};

inline constexpr std::array<PathShape, 4> kPathShapes{PathShape::Circle, PathShape::Figure8,
                                                      PathShape::Sweep, PathShape::Square};

std::string_view labelOf(PathShape shape) noexcept;
std::string_view nameOf(PathShape shape) noexcept;
std::optional<PathShape> pathShapeOf(std::string_view name) noexcept;

/// One effect, resolved — what a fired rule hands `DmxEngine::start`.
///
/// **Plain data with no allocation in it**, for the reason `trigger::Message` gives: this
/// travels inside a `Message`, a message is copied for every fire and held in the follow-up
/// queue after the rule that made it has been replaced, and an owning member would be an
/// allocation per fire and a dangling pointer per preset load.
///
/// Everything a generator could have decided is already decided here. The duration is in
/// **seconds**, converted from whatever unit the rule spelled it in against the tempo that
/// was playing when it fired — the same rule `trigger::Rule::followUpDelay` follows, and for
/// the same reason: *"two beats after a press means two beats of the tempo that was playing,
/// not of whatever the tracker says a second later"*. A random position is already a number.
/// A color drawn from a palette is already a color.
struct Payload {
    EffectKind kind = EffectKind::Level;
    /// Which channel, for the kinds `takesRole` names.
    Role role = Role::Dimmer;
    Curve curve = Curve::EaseOut;
    PathShape shape = PathShape::Circle;

    /// The target, the peak, or the high end of a sweep.
    std::uint8_t level = 255;
    /// The low end, for the kinds `takesBase` names.
    std::uint8_t base = 0;
    /// The color, for the kinds `takesColor` names. `HueSweep` takes its saturation and
    /// brightness from this color and replaces its hue.
    Color color = kWhite;

    /// How long it runs. Zero is a snap — legal for every kind, and for the repeating ones it
    /// means the effect lands on its own last value and is done.
    float durationSeconds = 0.0f;
    /// How many times a repeating kind repeats within the duration.
    float cycles = 1.0f;
    /// A strobe's on-fraction, 0 to 1.
    float duty = 0.5f;

    /// The hue sweep, in degrees. May run past 360 or backwards; `fromHsv` wraps.
    float hueFrom = 0.0f;
    float hueTo = 360.0f;

    /// Where a `Position` is going, as a fraction **of the fixture's own movement window** —
    /// so 0.5 is the middle of whatever the operator allowed, on every fixture, whatever each
    /// one's limits are. That is what makes one rule aimed at six differently-rigged heads
    /// mean something.
    float pan = 0.5f;
    float tilt = 0.5f;
    /// A `Path`'s radius, as a fraction of the window's half-width.
    float size = 0.5f;

    friend bool operator==(const Payload&, const Payload&) = default;
};

} // namespace takt4::dmx
