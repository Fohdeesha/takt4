#pragma once

#include "core/dmx/fixture.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace takt4::fixtures {

/// One DMX address of a mode as takt4 patches it: the role it is driven as, what the file calls
/// it, the level it sits at when nothing drives it, and anything worth saying about it.
struct ProfileChannel {
    dmx::Role role = dmx::Role::Unused;
    std::string label;
    std::uint8_t parked = 0;
    /// Empty, or why this channel is not what it might look like ("shares its range with strobe —
    /// takt4 would drive it into that", "no open value in the file — check this channel's parked
    /// level").
    std::string note;

    friend bool operator==(const ProfileChannel&, const ProfileChannel&) = default;
};

struct ProfileMode {
    std::string name;
    /// One part, or two for a mode that needs two start addresses. Empty when refused.
    std::vector<std::vector<ProfileChannel>> parts;
    std::vector<std::string> notes;
    /// Empty, or why the mode cannot be imported.
    std::string refused;

    bool importable() const noexcept { return refused.empty() && !parts.empty(); }
    friend bool operator==(const ProfileMode&, const ProfileMode&) = default;
};

/// **A fixture definition as the preset keeps it** — every mode of one file, converted, so that
/// fixtures made from it can be re-moded, a re-import can bring them up to date, and EXPORT
/// carries the definition with the rig. What `Preset::library` holds; the file itself is not
/// kept.
struct FixtureProfile {
    /// "p-…", generated when it joins a library, never shown. Fixtures link to it by this.
    std::string id;
    /// "gdtf" or "ofl".
    std::string format;
    /// The GDTF FixtureTypeID or the OFL "manufacturer/fixture" — what a re-import of the same
    /// fixture type is recognised by.
    std::string key;
    std::string manufacturer;
    std::string model;
    std::string revision;
    /// The file it was imported from, for the operator's reference.
    std::string file;
    /// What tells a changed definition from the same one again — `digestOf`. Recomputed from the
    /// content whenever it is read, so a hand-edited entry is never mistaken for the original.
    std::string digest;
    std::vector<std::string> notes;
    std::vector<ProfileMode> modes;

    friend bool operator==(const FixtureProfile&, const FixtureProfile&) = default;
};

} // namespace takt4::fixtures
