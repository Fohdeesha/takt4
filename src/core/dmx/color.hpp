#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace takt4::dmx {

/// One color, as the three DMX channels a fixture actually has.
///
/// Eight bits per component and nothing wider, because that is what is on the wire: a DMX512
/// channel is a byte, and a color held to more precision than the cable can carry would be
/// precision an operator can see in the editor and never on stage. Fixtures with 16-bit
/// color exist; none of them are what this app is pointed at, and adding the fine channels
/// later is a `Role` each rather than a change here.
struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    // By reference: a defaulted comparison takes `const Color&` or `Color`, and GCC and Clang
    // refuse `const Color` by value, which MSVC let through (found by tools/lint_clang.ps1).
    friend bool operator==(const Color&, const Color&) = default;
};

inline constexpr Color kBlack{0, 0, 0};
inline constexpr Color kWhite{255, 255, 255};

/// A color from hue, saturation and value — hue in **degrees**, wrapped, the other two 0 to
/// 1 and clamped.
///
/// Degrees rather than a 0-1 hue because a hue sweep is described in turns of a wheel and an
/// operator saying "sweep from red round to red again" means 0 to 360. A fraction would make
/// the most common instruction read as 0 to 1, which is the same number as "no sweep at all"
/// once it has been rounded by anything.
Color fromHsv(double hueDegrees, double saturation, double value) noexcept;

/// The inverse. Hue comes back in 0 to 360 and is zero for a grey, where it means nothing.
void toHsv(Color color, double& hueDegrees, double& saturation, double& value) noexcept;

/// Straight-line interpolation in RGB, `t` clamped to 0-1.
///
/// **In RGB and not in HSV, deliberately.** A fade from red to green through HSV sweeps the
/// hue wheel and passes through yellow; through RGB it dims towards a dark olive and comes
/// back up. The second is what "fade this fixture from one color to another" means to
/// everyone who has done it on a desk, and the first is available on its own terms as
/// `Effect::HueSweep` — which is a different instruction and should not be what a plain fade
/// silently does.
Color mix(Color from, Color to, double t) noexcept;

/// Every component scaled by `level`, 0 to 1 — how a color is dimmed on a fixture that has
/// no dimmer channel of its own, which is most LED pars.
Color scale(Color color, double level) noexcept;

/// "#ff2040". The `#` is written because that is what a person pastes in from anywhere else.
std::string formatColor(Color color);

/// Reads what `formatColor` writes, and rather more: `#ff2040`, `ff2040`, `#f24` (the
/// three-digit short form every web tool offers), and `255, 32, 64`. Nothing for anything
/// else — this parses a settings file a person may have edited and a box they are half way
/// through typing, so it must not guess.
std::optional<Color> parseColor(std::string_view text) noexcept;

} // namespace takt4::dmx
