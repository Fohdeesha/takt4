#include "core/fixtures/fixture_library.hpp"

#include "core/fixtures/profile_json.hpp"
#include "core/fixtures/text.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <tuple>
#include <utility>

namespace takt4::fixtures {
namespace {

/// The number of start addresses a linked fixture's mode has: two for one of a pair (whether or
/// not its partner is still patched), one otherwise.
std::size_t shapeOf(const dmx::Fixture& fixture) {
    return fixture.profile.pair.empty() ? 1 : 2;
}

std::string newPairId(const std::vector<dmx::Fixture>& patch) {
    static thread_local std::mt19937_64 random{std::random_device{}()};
    for (;;) {
        char text[24] = {};
        std::snprintf(text, sizeof text, "pair-%08x", static_cast<unsigned int>(random()));
        const std::string id(text);
        if (std::none_of(patch.begin(), patch.end(),
                         [&](const dmx::Fixture& fixture) { return fixture.profile.pair == id; })) {
            return id;
        }
    }
}

} // namespace

const FixtureProfile* findProfile(const std::vector<FixtureProfile>& library,
                                  std::string_view id) noexcept {
    if (id.empty()) {
        return nullptr;
    }
    for (const FixtureProfile& profile : library) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

const ProfileMode* findMode(const FixtureProfile& profile, std::string_view name) noexcept {
    for (const ProfileMode& mode : profile.modes) {
        if (mode.name == name) {
            return &mode;
        }
    }
    return nullptr;
}

Comparison compareWithLibrary(const std::vector<FixtureProfile>& library,
                              const FixtureProfile& incoming) {
    const std::string digest = digestOf(incoming);
    Comparison out;
    bool differs = false;
    for (std::size_t i = 0; i < library.size(); ++i) {
        const FixtureProfile& entry = library[i];
        if (entry.format != incoming.format || entry.key != incoming.key) {
            continue;
        }
        if (digestOf(entry) == digest) {
            return Comparison{Arrival::Same, i};
        }
        if (!differs) {
            differs = true;
            out = Comparison{Arrival::Changed, i};
        }
    }
    return out;
}

std::size_t addProfile(std::vector<FixtureProfile>& library, FixtureProfile incoming) {
    incoming.id = newProfileId(library);
    incoming.digest = digestOf(incoming);
    library.push_back(std::move(incoming));
    return library.size() - 1;
}

std::vector<std::string> replaceProfile(std::vector<FixtureProfile>& library, std::size_t index,
                                        FixtureProfile incoming, std::vector<dmx::Fixture>& patch) {
    std::vector<std::string> unlinked;
    if (index >= library.size()) {
        return unlinked;
    }
    incoming.id = library[index].id;
    incoming.digest = digestOf(incoming);
    library[index] = std::move(incoming);
    const FixtureProfile& profile = library[index];
    for (dmx::Fixture& fixture : patch) {
        if (fixture.profile.id != profile.id) {
            continue;
        }
        const ProfileMode* mode = findMode(profile, fixture.profile.mode);
        const std::size_t shape = shapeOf(fixture);
        const int part = fixture.profile.part;
        if (mode != nullptr && mode->importable() && mode->parts.size() == shape && part >= 1 &&
            static_cast<std::size_t>(part) <= shape) {
            applyMode(fixture, profile, *mode, part);
        } else {
            fixture.profile = dmx::Fixture::ProfileLink{};
            unlinked.push_back(fixture.name);
        }
    }
    return unlinked;
}

std::string displayName(const std::vector<FixtureProfile>& library, const FixtureProfile& profile) {
    const auto alike =
        std::count_if(library.begin(), library.end(), [&](const FixtureProfile& other) {
            return other.format == profile.format && other.key == profile.key;
        });
    if (alike > 1 && !profile.revision.empty()) {
        return profile.model + " (" + profile.revision + ")";
    }
    return profile.model;
}

std::size_t usersOf(const std::vector<dmx::Fixture>& patch, std::string_view id) noexcept {
    if (id.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(
        std::count_if(patch.begin(), patch.end(),
                      [&](const dmx::Fixture& fixture) { return fixture.profile.id == id; }));
}

bool removeProfile(std::vector<FixtureProfile>& library, std::string_view id,
                   const std::vector<dmx::Fixture>& patch) {
    if (usersOf(patch, id) > 0) {
        return false;
    }
    const auto before = library.size();
    std::erase_if(library, [&](const FixtureProfile& profile) { return profile.id == id; });
    return library.size() != before;
}

bool applyMode(dmx::Fixture& fixture, const FixtureProfile& profile, const ProfileMode& mode,
               int part) {
    if (!mode.importable() || part < 1 || static_cast<std::size_t>(part) > mode.parts.size()) {
        return false;
    }
    const std::vector<ProfileChannel>& channels = mode.parts[static_cast<std::size_t>(part - 1)];
    fixture.channels.clear();
    fixture.parked.clear();
    fixture.labels.clear();
    for (const ProfileChannel& channel : channels) {
        fixture.channels.push_back(channel.role);
        fixture.parked.push_back(channel.parked);
        fixture.labels.push_back(channel.label);
    }
    fixture.profile.id = profile.id;
    fixture.profile.mode = mode.name;
    fixture.profile.part = part;
    return true;
}

NewFixtures makeFixtures(const FixtureProfile& profile, std::string_view modeName, int count,
                         dmx::PortAddress universe, int address,
                         const std::vector<dmx::Fixture>& patch) {
    NewFixtures out;
    const ProfileMode* mode = findMode(profile, modeName);
    if (mode == nullptr) {
        out.problem = "pick a mode";
        return out;
    }
    if (!mode->importable()) {
        out.problem =
            mode->refused.empty() ? std::string("this mode cannot be imported") : mode->refused;
        return out;
    }
    if (count < 1) {
        out.problem = "how many must be at least 1";
        return out;
    }
    if (address < 1 || address > static_cast<int>(dmx::kChannelsPerUniverse)) {
        out.problem = "the start address must be 1 to 512";
        return out;
    }
    std::size_t footprint = 0;
    for (const std::vector<ProfileChannel>& part : mode->parts) {
        footprint += part.size();
    }
    const std::size_t total = footprint * static_cast<std::size_t>(count);
    const std::size_t last = static_cast<std::size_t>(address) + total - 1;
    out.firstChannel = address;
    out.lastChannel = static_cast<int>(std::min<std::size_t>(last, 1000000));
    if (last > dmx::kChannelsPerUniverse) {
        // And what would fit, so the box can be put right without arithmetic.
        out.problem = total > dmx::kChannelsPerUniverse
                          ? "runs past 512: " + std::to_string(total) +
                                " channels are more than one universe holds — import fewer"
                          : "runs past 512: " + std::to_string(total) + " channels from " +
                                std::to_string(address) + " — start at " +
                                std::to_string(dmx::kChannelsPerUniverse + 1 - total) + " or lower";
        return out;
    }

    const std::string base = profile.model.empty() ? std::string("fixture") : profile.model;
    std::set<std::string> taken;
    for (const dmx::Fixture& fixture : patch) {
        taken.insert(fixture.name);
    }
    const bool pair = mode->parts.size() == 2;
    std::vector<dmx::Fixture> everyone = patch; // for ids no fixture has, the new ones included
    std::size_t cursor = static_cast<std::size_t>(address);
    int number = 1;
    for (int copy = 0; copy < count; ++copy) {
        // The smallest number no fixture's name is using, for both parts of a pair.
        std::string name;
        for (;; ++number) {
            name = base + " " + std::to_string(number);
            if (!taken.contains(name) && (!pair || !taken.contains(name + " · part 2"))) {
                break;
            }
        }
        const std::string pairId = pair ? newPairId(everyone) : std::string();
        for (std::size_t part = 0; part < mode->parts.size(); ++part) {
            dmx::Fixture fixture;
            fixture.name = part == 0 ? name : name + " · part 2";
            fixture.group = base;
            fixture.universe = universe;
            fixture.address = static_cast<std::uint16_t>(cursor);
            applyMode(fixture, profile, *mode, static_cast<int>(part + 1));
            fixture.profile.pair = pairId;
            fixture.id = dmx::newFixtureId(everyone);
            cursor += mode->parts[part].size();
            taken.insert(fixture.name);
            everyone.push_back(fixture);
            out.fixtures.push_back(std::move(fixture));
        }
    }
    return out;
}

bool isEdited(const dmx::Fixture& fixture, const FixtureProfile& profile) {
    if (!fixture.profile.linked() || fixture.profile.id != profile.id) {
        return false;
    }
    const ProfileMode* mode = findMode(profile, fixture.profile.mode);
    const int part = fixture.profile.part;
    if (mode == nullptr || !mode->importable() || part < 1 ||
        static_cast<std::size_t>(part) > mode->parts.size()) {
        return true;
    }
    const std::vector<ProfileChannel>& channels = mode->parts[static_cast<std::size_t>(part - 1)];
    if (channels.size() != fixture.channels.size()) {
        return true;
    }
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].role != fixture.channels[i]) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> modesFor(const dmx::Fixture& fixture, const FixtureProfile& profile) {
    std::vector<std::string> out;
    if (!fixture.profile.linked() || fixture.profile.id != profile.id) {
        return out;
    }
    const std::size_t shape = shapeOf(fixture);
    for (const ProfileMode& mode : profile.modes) {
        if (mode.importable() && mode.parts.size() == shape) {
            out.push_back(mode.name);
        }
    }
    return out;
}

bool remode(std::vector<dmx::Fixture>& patch, std::size_t index, const FixtureProfile& profile,
            std::string_view modeName) {
    if (index >= patch.size()) {
        return false;
    }
    dmx::Fixture& fixture = patch[index];
    if (!fixture.profile.linked() || fixture.profile.id != profile.id) {
        return false;
    }
    const ProfileMode* mode = findMode(profile, modeName);
    const std::size_t shape = shapeOf(fixture);
    if (mode == nullptr || !mode->importable() || mode->parts.size() != shape) {
        return false;
    }
    const auto partOf = [&](const dmx::Fixture& one) {
        return std::clamp(one.profile.part, 1, static_cast<int>(shape));
    };
    applyMode(fixture, profile, *mode, partOf(fixture));
    if (!fixture.profile.pair.empty()) {
        for (std::size_t j = 0; j < patch.size(); ++j) {
            if (j != index && patch[j].profile.id == profile.id &&
                patch[j].profile.pair == patch[index].profile.pair) {
                applyMode(patch[j], profile, *mode, partOf(patch[j]));
            }
        }
    }
    return true;
}

namespace {

/// "zoom", or "strobe ×2".
std::string counted(std::string_view word, std::size_t count) {
    std::string text(word);
    if (count > 1) {
        text += " ×" + std::to_string(count);
    }
    return text;
}

} // namespace

std::string drivesOf(const ProfileMode& mode) {
    std::map<dmx::Role, std::size_t> count;
    for (const std::vector<ProfileChannel>& part : mode.parts) {
        for (const ProfileChannel& channel : part) {
            ++count[channel.role];
        }
    }
    const auto of = [&count](dmx::Role role) {
        const auto found = count.find(role);
        return found == count.end() ? std::size_t{0} : found->second;
    };
    std::vector<std::string> words;
    if (of(dmx::Role::Dimmer) > 0) {
        words.push_back(counted("dimmer", of(dmx::Role::Dimmer)));
    }

    // The emitters: one word for a family every cell has all of — "RGB", "RGBW", "RGBWA", with
    // "+UV" — and the rest one by one.
    const std::size_t red = of(dmx::Role::Red);
    const std::size_t green = of(dmx::Role::Green);
    const std::size_t blue = of(dmx::Role::Blue);
    std::size_t white = of(dmx::Role::White);
    std::size_t amber = of(dmx::Role::Amber);
    std::size_t uv = of(dmx::Role::Uv);
    if (red > 0 && red == green && green == blue) {
        std::string family = "RGB";
        if (white == red) {
            family += "W";
            white = 0;
        }
        if (amber == red) {
            family += "A";
            amber = 0;
        }
        if (uv == red) {
            family += "+UV";
            uv = 0;
        }
        words.push_back(counted(family, red));
    } else {
        for (const auto& [word, role] :
             {std::pair{"red", dmx::Role::Red}, std::pair{"green", dmx::Role::Green},
              std::pair{"blue", dmx::Role::Blue}}) {
            if (of(role) > 0) {
                words.push_back(counted(word, of(role)));
            }
        }
    }
    if (white > 0) {
        words.push_back(counted("white", white));
    }
    if (amber > 0) {
        words.push_back(counted("amber", amber));
    }
    if (uv > 0) {
        words.push_back(counted("UV", uv));
    }
    const std::size_t cyan = of(dmx::Role::Cyan);
    if (cyan > 0 && cyan == of(dmx::Role::Magenta) && cyan == of(dmx::Role::Yellow)) {
        words.push_back(counted("CMY", cyan));
    } else {
        for (const auto& [word, role] :
             {std::pair{"cyan", dmx::Role::Cyan}, std::pair{"magenta", dmx::Role::Magenta},
              std::pair{"yellow", dmx::Role::Yellow}}) {
            if (of(role) > 0) {
                words.push_back(counted(word, of(role)));
            }
        }
    }

    // Movement, by heads: each head's pan with its tilt (`dmx::headsOf`).
    const std::size_t pans = of(dmx::Role::Pan);
    const std::size_t tilts = of(dmx::Role::Tilt);
    const bool fine = of(dmx::Role::PanFine) > 0 || of(dmx::Role::TiltFine) > 0;
    if (pans > 0 || tilts > 0) {
        std::string movement = pans > 0 && tilts > 0 ? "pan/tilt" : pans > 0 ? "pan" : "tilt";
        if (fine) {
            movement += " 16-bit";
        }
        words.push_back(counted(movement, std::max(pans, tilts)));
    }
    for (const auto& [word, role] :
         {std::pair{"strobe", dmx::Role::Strobe}, std::pair{"color wheel", dmx::Role::ColorWheel},
          std::pair{"gobo", dmx::Role::Gobo}, std::pair{"zoom", dmx::Role::Zoom},
          std::pair{"focus", dmx::Role::Focus}, std::pair{"speed", dmx::Role::Speed}}) {
        if (of(role) > 0) {
            words.push_back(counted(word, of(role)));
        }
    }
    if (words.empty()) {
        return "nothing takt4 drives";
    }
    std::string text;
    for (const std::string& word : words) {
        text += text.empty() ? word : " · " + word;
    }
    return text;
}

std::size_t unusedIn(const ProfileMode& mode) noexcept {
    std::size_t unused = 0;
    for (const std::vector<ProfileChannel>& part : mode.parts) {
        for (const ProfileChannel& channel : part) {
            unused += channel.role == dmx::Role::Unused ? 1 : 0;
        }
    }
    return unused;
}

std::string channelCountOf(const ProfileMode& mode) {
    std::string text;
    for (const std::vector<ProfileChannel>& part : mode.parts) {
        text += (text.empty() ? "" : " + ") + std::to_string(part.size());
    }
    return text;
}

std::vector<std::size_t> libraryOrder(const std::vector<FixtureProfile>& library) {
    std::vector<std::size_t> order(library.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    const auto folded = [](const std::string& text) { return text::lowerAscii(text); };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const FixtureProfile& x = library[a];
        const FixtureProfile& y = library[b];
        return std::make_tuple(folded(x.manufacturer), folded(x.model), folded(x.revision)) <
               std::make_tuple(folded(y.manufacturer), folded(y.model), folded(y.revision));
    });
    return order;
}

} // namespace takt4::fixtures
