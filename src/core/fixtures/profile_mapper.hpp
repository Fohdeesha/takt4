#pragma once

#include "core/dmx/fixture.hpp"
#include "core/fixtures/definition.hpp"
#include "core/fixtures/fixture_profile.hpp"

namespace takt4::fixtures {

/// **What takt4 does with a definition — all of it, for both formats, here.** Each channel's
/// kind becomes a role, each channel gets its parked level, and what that loses is said on the
/// channel:
///
///  - **Roles.** Dimmer, the colours (warm and cool white as white, indirect RGB as RGB), CMY, pan
///    and tilt with their fine bytes, the shutter as strobe, the first colour wheel, gobo wheel,
///    zoom and focus, pan/tilt speed. Everything else is unused: a channel shared with another
///    function, a second wheel, a fine byte of anything but pan and tilt.
///  - **Several channels of one role all take it** — the cells of a pixel bar, the zones of a
///    shutter, a master and its cells alike — which is how `DmxEngine` drives a bar.
///  - **Parked levels**: every emitter and every dimmer at 0, so a fixture is dark until a rule
///    lights it (GDTF files default 85 dimmers and 8,069 colour channels to full); a shutter open;
///    pan and tilt at the file's default, else centred; the rest at the file's default.
///
/// The profile's id and digest are left empty: a library gives them.
FixtureProfile mapDefinition(const Definition& definition);

/// The role a channel of `kind` is driven as — see `mapDefinition`. `lowestOrdinal` is the
/// lowest ordinal of that kind in the mode: a mode whose only gobo wheel is "gobo 2" still has
/// a gobo.
dmx::Role roleFor(Kind kind, std::uint8_t byte, bool shared, std::uint32_t ordinal,
                  std::uint32_t lowestOrdinal) noexcept;

} // namespace takt4::fixtures
