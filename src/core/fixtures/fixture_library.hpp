#pragma once

#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/fixture.hpp"
#include "core/fixtures/fixture_profile.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/// The preset's library of converted definitions, and the fixtures linked to it.
namespace takt4::fixtures {

const FixtureProfile* findProfile(const std::vector<FixtureProfile>& library,
                                  std::string_view id) noexcept;
const ProfileMode* findMode(const FixtureProfile& profile, std::string_view name) noexcept;

/// What a newly read definition is to the library.
enum class Arrival {
    /// No entry of that format and key: it is added.
    New,
    /// An entry with the same content (digest): that entry is used, nothing asked.
    Same,
    /// An entry of that format and key with other content: the operator is asked to replace it
    /// or keep both.
    Changed,
};

struct Comparison {
    Arrival arrival = Arrival::New;
    /// The entry it is the same as, or the first it differs from. Meaningless for `New`.
    std::size_t index = 0;
};

Comparison compareWithLibrary(const std::vector<FixtureProfile>& library,
                              const FixtureProfile& incoming);

/// Adds `incoming` with a fresh id and its digest: its index. "Keep both" is this too.
std::size_t addProfile(std::vector<FixtureProfile>& library, FixtureProfile incoming);

/// **Replace**: the entry's content becomes `incoming`'s and its id stays, so every fixture
/// linked to it stays linked. Each is re-mapped from its mode — channels, parked levels and
/// labels — keeping its name, group, universe, address, movement window and switch. A fixture
/// whose mode is gone, refused, or no longer the shape it was (one start address or two) keeps
/// its channels and loses its link instead. Their names, for the status line.
std::vector<std::string> replaceProfile(std::vector<FixtureProfile>& library, std::size_t index,
                                        FixtureProfile incoming, std::vector<dmx::Fixture>& patch);

/// The entry's model as a list shows it — with its revision in brackets where another entry
/// is the same fixture type (two kept side by side).
std::string displayName(const std::vector<FixtureProfile>& library, const FixtureProfile& profile);

/// How many fixtures of `patch` are linked to the entry.
std::size_t usersOf(const std::vector<dmx::Fixture>& patch, std::string_view id) noexcept;

/// Removes an entry **no fixture uses**; false (and nothing removed) when one does.
bool removeProfile(std::vector<FixtureProfile>& library, std::string_view id,
                   const std::vector<dmx::Fixture>& patch);

/// Gives `fixture` part `part` (1 or 2) of `mode`: channels, parked levels and labels, and
/// links it there. False, and the fixture untouched, when the mode cannot be imported or has no
/// such part.
bool applyMode(dmx::Fixture& fixture, const FixtureProfile& profile, const ProfileMode& mode,
               int part);

/// What IMPORT makes, or why it makes nothing.
struct NewFixtures {
    std::vector<dmx::Fixture> fixtures;
    /// Empty, or why nothing was made ("runs past 512: 140 channels from 400 — start at 373 or
    /// lower").
    std::string problem;
    /// The first and last channel the block takes on its universe.
    int firstChannel = 0;
    int lastChannel = 0;
};

/// `count` fixtures of `modeName`, laid end to end from `address` on `universe`, named
/// "<model> 1", "<model> 2"… with the smallest numbers no fixture of `patch` has, and grouped
/// as "<model>". A two-address mode makes two linked fixtures per copy — "<model> n" and
/// "<model> n · part 2", sharing a pair id — with part 2 patched right after part 1. Refused
/// when the block would run past channel 512. Every fixture has a fresh id.
NewFixtures makeFixtures(const FixtureProfile& profile, std::string_view modeName, int count,
                         dmx::PortAddress universe, int address,
                         const std::vector<dmx::Fixture>& patch);

/// Whether a linked fixture's roles or channel count differ from its mode's — shown as
/// "(edited)"; computed, never stored. False for an unlinked fixture.
bool isEdited(const dmx::Fixture& fixture, const FixtureProfile& profile);

/// The modes a linked fixture can be changed to: importable, and of its shape — two-part modes
/// for one of a pair, one-part modes otherwise.
std::vector<std::string> modesFor(const dmx::Fixture& fixture, const FixtureProfile& profile);

/// Changes the mode of `patch[index]` — and, for one of a pair, of its partner, each to its own
/// part. Restores roles, parked levels and labels from the definition. False, and nothing
/// changed, when the mode is not one `modesFor` offers.
bool remode(std::vector<dmx::Fixture>& patch, std::size_t index, const FixtureProfile& profile,
            std::string_view modeName);

/// What takt4 drives in a mode, every part of it, in the words the import sheet's list of modes
/// uses: "dimmer ×2 · RGBW ×7 · pan/tilt 16-bit · strobe ×2 · color wheel · zoom · speed" (the
/// operator approved the words and the order, 2026-10-05). A colour family that every cell has —
/// red, green and blue alike, white and amber with them — is one word and a count; a fixture with
/// several heads says how many ("pan/tilt ×2", `dmx::headsOf`). "nothing takt4 drives" when every
/// channel is unused.
std::string drivesOf(const ProfileMode& mode);

/// How many of a mode's channels have no role: what an import leaves at its parked level.
std::size_t unusedIn(const ProfileMode& mode) noexcept;

/// A mode's footprint as a list shows it: "25", or "18 + 28" for one that needs two start
/// addresses.
std::string channelCountOf(const ProfileMode& mode);

/// The library's entries in the order a list shows them: by manufacturer, then model, then
/// revision, each ignoring case, ties in library order — as indices into `library`.
std::vector<std::size_t> libraryOrder(const std::vector<FixtureProfile>& library);

} // namespace takt4::fixtures
