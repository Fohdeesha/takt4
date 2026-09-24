#include "core/dmx/dmx_engine.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace takt4::dmx {
namespace {

/// The roles a `Blackout` drives to zero: everything that emits light, and nothing that
/// configures the fixture. A shutter closed by a blackout would be a fixture that stays dark
/// when the next rule fires, which is the one thing worse than a blackout that does not work.
///
/// `kEmitters` without the dimmer, which a blackout does want; the dimmer is added here.
constexpr std::array<Role, 7> kBlackoutRoles{Role::Dimmer, Role::Red,   Role::Green, Role::Blue,
                                             Role::White,  Role::Amber, Role::Uv};

double unitOfByte(std::uint8_t value) noexcept {
    return value / 255.0;
}

/// Where in a repeating effect's cycle we are, 0 to 1. Kept as its own function because
/// `Pulse` and `Strobe` differ only in what they do with it.
double cyclePhase(double progress, double cycles) noexcept {
    const double turns = progress * (cycles > 0.0 ? cycles : 1.0);
    // At the very end a repeating effect should land on the *end* of its last cycle rather
    // than wrapping back to the start of a new one — otherwise a two-cycle pulse finishes at
    // its low point and jumps, which reads as a glitch on the last frame.
    if (progress >= 1.0) {
        return 1.0;
    }
    return turns - std::floor(turns);
}

} // namespace

std::size_t DmxEngine::bufferOf(PortAddress universe) const noexcept {
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        if (buffers_[i].universe == universe) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

void DmxEngine::setPatch(std::vector<Fixture> patch) {
    // Where each fixture of the old patch is in the new one — by id, which a rename does not
    // move, and by name only for fixtures that have no id (one built in code rather than by the
    // patch editor). Each new fixture is taken once, in order, so two fixtures of one name keep
    // their order rather than both landing on the first. This is what lets a running effect and
    // a virtual fixture's state carry on across an edit.
    std::vector<std::uint16_t> mapped(patch_.size(), kNoFixture);
    {
        std::vector<bool> taken(patch.size(), false);
        for (std::size_t i = 0; i < patch_.size(); ++i) {
            for (std::size_t j = 0; j < patch.size() && j < kNoFixture; ++j) {
                const bool same = !patch[j].id.empty() || !patch_[i].id.empty()
                                      ? patch[j].id == patch_[i].id
                                      : patch[j].name == patch_[i].name;
                if (!taken[j] && same) {
                    mapped[i] = static_cast<std::uint16_t>(j);
                    taken[j] = true;
                    break;
                }
            }
        }
    }

    // A channel TEST hold is let go, and its channel put back where the hold found it — before
    // the patch is laid over the levels, so a channel the new patch re-parks is re-parked from
    // what it really was rather than from the test's level.
    for (Running& running : running_) {
        for (const Track& track : running.tracks) {
            if (track.fixture == kNoFixture && !track.isVirtual && track.buffer < buffers_.size() &&
                track.channel < kChannelsPerUniverse) {
                buffers_[track.buffer].levels[track.channel] = static_cast<std::uint8_t>(
                    std::lround(std::clamp(track.from, 0.0, 1.0) * 255.0));
                ++buffers_[track.buffer].revision;
            }
        }
        std::erase_if(running.tracks,
                      [](const Track& track) { return track.fixture == kNoFixture; });
    }

    const std::vector<PortAddress> universes = universesOf(patch);
    std::vector<Buffer> rebuilt;
    rebuilt.reserve(universes.size());
    for (const PortAddress universe : universes) {
        Buffer buffer;
        buffer.universe = universe;
        const std::size_t old = bufferOf(universe);
        if (old != static_cast<std::size_t>(-1)) {
            buffer.levels = buffers_[old].levels;
            buffer.meaning = buffers_[old].meaning;
            buffer.revision = buffers_[old].revision;
        }
        rebuilt.push_back(buffer);
    }

    // What the new patch makes of every channel.
    std::vector<std::array<Meaning, kChannelsPerUniverse>> after(rebuilt.size());
    for (const Fixture& fixture : patch) {
        if (!fixture.enabled || fixture.channels.empty()) {
            continue; // switched off contributes no channels — see `Fixture::enabled`
        }
        std::size_t index = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < rebuilt.size(); ++i) {
            if (rebuilt[i].universe == fixture.universe) {
                index = i;
                break;
            }
        }
        if (index == static_cast<std::size_t>(-1)) {
            continue;
        }
        for (std::size_t i = 0; i < fixture.channels.size(); ++i) {
            const std::size_t channel = std::size_t{fixture.address} + i;
            if (channel < 1 || channel > kChannelsPerUniverse) {
                continue;
            }
            Meaning& meaning = after[index][channel - 1];
            meaning.covered = true;
            meaning.position = static_cast<std::uint16_t>(i);
            meaning.role = fixture.channels[i];
            meaning.parked = i < fixture.parked.size() ? fixture.parked[i] : 0;
        }
    }
    // And each channel put right against what it was. See the header.
    for (std::size_t b = 0; b < rebuilt.size(); ++b) {
        Buffer& buffer = rebuilt[b];
        for (std::size_t at = 0; at < kChannelsPerUniverse; ++at) {
            const Meaning& was = buffer.meaning[at];
            const Meaning& now = after[b][at];
            if (now.covered && now != was) {
                // Newly patched, or patched to mean something else: parked.
                if (buffer.levels[at] != now.parked) {
                    buffer.levels[at] = now.parked;
                    ++buffer.revision;
                }
            } else if (!now.covered && was.covered) {
                // Nobody's any more: released, which is zero.
                if (buffer.levels[at] != 0) {
                    buffer.levels[at] = 0;
                    ++buffer.revision;
                }
            }
            buffer.meaning[at] = now;
        }
    }

    // Each virtual fixture's state carried across, or read from its channels as they now stand.
    std::vector<Virtual> carried(patch.size());
    std::vector<bool> carries(patch.size(), false);
    for (std::size_t i = 0; i < mapped.size(); ++i) {
        if (mapped[i] != kNoFixture && i < virtuals_.size() && virtuals_[i].active) {
            carried[mapped[i]] = virtuals_[i];
            carries[mapped[i]] = true;
        }
    }

    patch_ = std::move(patch);
    universes_ = universes;
    buffers_ = std::move(rebuilt);
    std::vector<Virtual> virtuals(patch_.size());
    for (std::size_t j = 0; j < patch_.size(); ++j) {
        const Fixture& fixture = patch_[j];
        if (!fixture.enabled || has(fixture, Role::Dimmer) || !emits(fixture)) {
            continue;
        }
        virtuals[j] = carries[j] ? carried[j] : virtualFromLevels(fixture);
        virtuals[j].active = true;
        virtuals[j].dirty = carries[j];
    }
    virtuals_ = std::move(virtuals);

    retarget(mapped);
    composeVirtuals();
}

void DmxEngine::retarget(const std::vector<std::uint16_t>& mapped) {
    const auto target = [&](std::uint16_t fixture) -> const Fixture* {
        if (fixture >= mapped.size() || mapped[fixture] == kNoFixture) {
            return nullptr;
        }
        const Fixture& found = patch_[mapped[fixture]];
        return found.enabled ? &found : nullptr;
    };
    const auto isEmitter = [](Role role) {
        return std::find(kEmitters.begin(), kEmitters.end(), role) != kEmitters.end();
    };
    for (Running& running : running_) {
        std::erase_if(running.tracks, [&](Track& track) {
            const Fixture* fixture = target(track.fixture);
            if (fixture == nullptr) {
                return true; // the fixture has gone, or is switched off
            }
            const std::uint16_t moved = mapped[track.fixture];
            const bool nowVirtual = moved < virtuals_.size() && virtuals_[moved].active;
            if (track.isVirtual) {
                if (!nowVirtual) {
                    return true;
                }
                track.fixture = moved;
                return false;
            }
            const std::uint16_t channel = channelOf(*fixture, track.source);
            const std::size_t buffer = bufferOf(fixture->universe);
            if (channel == 0 || buffer == static_cast<std::size_t>(-1) ||
                (nowVirtual && isEmitter(track.source))) {
                // No such channel any more — or it has become a virtual fixture's emitter, whose
                // level is written from its color and intensity rather than driven raw.
                return true;
            }
            track.fixture = moved;
            track.buffer = static_cast<std::uint32_t>(buffer);
            track.channel = static_cast<std::uint16_t>(channel - 1);
            const Role partner = track.source == Role::Pan    ? Role::PanFine
                                 : track.source == Role::Tilt ? Role::TiltFine
                                                              : Role::Unused;
            if (partner != Role::Unused) {
                const std::uint16_t fine = channelOf(*fixture, partner);
                track.fine = fine == 0 ? kNoChannel : static_cast<std::uint16_t>(fine - 1);
            }
            return false;
        });
        std::erase_if(running.moves, [&](Move& move) {
            const Fixture* fixture = target(move.fixture);
            if (fixture == nullptr) {
                return true;
            }
            const std::uint16_t pan = channelOf(*fixture, Role::Pan);
            const std::uint16_t tilt = channelOf(*fixture, Role::Tilt);
            const std::size_t buffer = bufferOf(fixture->universe);
            if ((pan == 0 && tilt == 0) || buffer == static_cast<std::size_t>(-1)) {
                return true;
            }
            move.fixture = mapped[move.fixture];
            move.buffer = static_cast<std::uint32_t>(buffer);
            const std::uint16_t panFine = channelOf(*fixture, Role::PanFine);
            const std::uint16_t tiltFine = channelOf(*fixture, Role::TiltFine);
            move.pan = pan == 0 ? kNoChannel : static_cast<std::uint16_t>(pan - 1);
            move.tilt = tilt == 0 ? kNoChannel : static_cast<std::uint16_t>(tilt - 1);
            move.panFine = panFine == 0 ? kNoChannel : static_cast<std::uint16_t>(panFine - 1);
            move.tiltFine = tiltFine == 0 ? kNoChannel : static_cast<std::uint16_t>(tiltFine - 1);
            // The window as the new patch has it, so a limit just tightened holds at once.
            const double panA = std::clamp(fixture->panMin, 0.0, 1.0);
            const double panB = std::clamp(fixture->panMax, 0.0, 1.0);
            const double tiltA = std::clamp(fixture->tiltMin, 0.0, 1.0);
            const double tiltB = std::clamp(fixture->tiltMax, 0.0, 1.0);
            move.panLow = std::min(panA, panB);
            move.panHigh = std::max(panA, panB);
            move.tiltLow = std::min(tiltA, tiltB);
            move.tiltHigh = std::max(tiltA, tiltB);
            move.toPan = std::clamp(move.toPan, move.panLow, move.panHigh);
            move.toTilt = std::clamp(move.toTilt, move.tiltLow, move.tiltHigh);
            return false;
        });
    }
    std::erase_if(running_, [](const Running& r) { return r.tracks.empty() && r.moves.empty(); });
}

DmxEngine::Virtual DmxEngine::virtualFromLevels(const Fixture& fixture) const {
    // As the channels stand, at full intensity: color × intensity is then exactly what is being
    // sent, so taking a fixture over changes nothing a node can see.
    Virtual state;
    const std::size_t buffer = bufferOf(fixture.universe);
    for (std::size_t e = 0; e < kEmitters.size(); ++e) {
        const std::uint16_t channel = channelOf(fixture, kEmitters[e]);
        if (channel != 0 && buffer != static_cast<std::size_t>(-1)) {
            state.color[e] = readChannel(static_cast<std::uint32_t>(buffer),
                                         static_cast<std::uint16_t>(channel - 1), kNoChannel);
        }
    }
    return state;
}

void DmxEngine::composeVirtuals() noexcept {
    for (std::size_t index = 0; index < virtuals_.size() && index < patch_.size(); ++index) {
        Virtual& state = virtuals_[index];
        if (!state.active || !state.dirty) {
            continue;
        }
        state.dirty = false;
        const Fixture& fixture = patch_[index];
        const std::size_t buffer = bufferOf(fixture.universe);
        if (buffer == static_cast<std::size_t>(-1)) {
            continue;
        }
        for (std::size_t e = 0; e < kEmitters.size(); ++e) {
            const std::uint16_t channel = channelOf(fixture, kEmitters[e]);
            if (channel != 0) {
                writeChannel(static_cast<std::uint32_t>(buffer),
                             static_cast<std::uint16_t>(channel - 1), kNoChannel,
                             state.color[e] * state.intensity);
            }
        }
    }
}

void DmxEngine::writeTrack(const Track& track, double unit) noexcept {
    if (!track.isVirtual) {
        writeChannel(track.buffer, track.channel, track.fine, unit);
        return;
    }
    if (track.fixture >= virtuals_.size()) {
        return;
    }
    Virtual& state = virtuals_[track.fixture];
    const double clamped = std::clamp(unit, 0.0, 1.0);
    double& slot =
        track.component == kIntensity ? state.intensity : state.color[track.component - 1u];
    if (slot != clamped) {
        slot = clamped;
        state.dirty = true;
    }
}

void DmxEngine::cancelAll() noexcept {
    running_.clear();
}

void DmxEngine::blackout(double now) {
    running_.clear();
    Payload dark;
    dark.kind = EffectKind::Blackout;
    // Every fixture a mask can reach, which is all of them: the patch is capped at that.
    start(~std::uint64_t{0}, dark, now);
    // A snap: nothing to animate, so nothing left running to be advanced on the next tick.
    running_.clear();
    // And every virtual fixture back to neutral — dark, and at full intensity — so the rig
    // comes back on the rules alone after a Start: a color rule lights an LED par again, as it
    // did before the stop. Intensity left at a flash's zero would keep it dark until a dimmer
    // rule happened to fire.
    for (Virtual& state : virtuals_) {
        if (state.active) {
            state.intensity = 1.0;
            state.color.fill(0.0);
            state.dirty = true;
        }
    }
    composeVirtuals();
}

void DmxEngine::reset() {
    running_.clear();
    for (Buffer& buffer : buffers_) {
        buffer.levels.fill(0);
        buffer.meaning.fill(Meaning{});
        ++buffer.revision;
    }
    // Nothing carried over: a reset is the rig from nothing.
    for (Virtual& state : virtuals_) {
        state.active = false;
    }
    // Re-laying the patch puts every parked level back, which is what "reset" means: the rig
    // as it sits before anything has fired, shutters open and dimmers down.
    setPatch(std::move(patch_));
}

std::span<const std::uint8_t> DmxEngine::levels(PortAddress universe) const noexcept {
    const std::size_t index = bufferOf(universe);
    if (index == static_cast<std::size_t>(-1)) {
        return {};
    }
    return std::span<const std::uint8_t>(buffers_[index].levels.data(), kChannelsPerUniverse);
}

std::uint64_t DmxEngine::revision(PortAddress universe) const noexcept {
    const std::size_t index = bufferOf(universe);
    return index == static_cast<std::size_t>(-1) ? 0 : buffers_[index].revision;
}

void DmxEngine::writeChannel(std::uint32_t buffer, std::uint16_t channel, std::uint16_t fine,
                             double unit) noexcept {
    if (buffer >= buffers_.size()) {
        return;
    }
    Buffer& target = buffers_[buffer];
    const double clamped = std::clamp(unit, 0.0, 1.0);
    if (fine == kNoChannel) {
        const auto value = static_cast<std::uint8_t>(std::lround(clamped * 255.0));
        if (channel < kChannelsPerUniverse && target.levels[channel] != value) {
            target.levels[channel] = value;
            ++target.revision;
        }
        return;
    }
    const auto word = static_cast<std::uint32_t>(std::lround(clamped * 65535.0));
    const auto high = static_cast<std::uint8_t>((word >> 8) & 0xFF);
    const auto low = static_cast<std::uint8_t>(word & 0xFF);
    if (channel < kChannelsPerUniverse && target.levels[channel] != high) {
        target.levels[channel] = high;
        ++target.revision;
    }
    if (fine < kChannelsPerUniverse && target.levels[fine] != low) {
        target.levels[fine] = low;
        ++target.revision;
    }
}

double DmxEngine::readChannel(std::uint32_t buffer, std::uint16_t channel,
                              std::uint16_t fine) const noexcept {
    if (buffer >= buffers_.size() || channel >= kChannelsPerUniverse) {
        return 0.0;
    }
    const Buffer& target = buffers_[buffer];
    if (fine == kNoChannel || fine >= kChannelsPerUniverse) {
        return unitOfByte(target.levels[channel]);
    }
    const auto word = static_cast<std::uint32_t>(target.levels[channel]) << 8 | target.levels[fine];
    return word / 65535.0;
}

bool DmxEngine::buildFor(std::size_t index, const Payload& payload, Running& running) {
    const Fixture& fixture = patch_[index];
    const std::size_t found = bufferOf(fixture.universe);
    if (found == static_cast<std::size_t>(-1)) {
        return false;
    }
    const auto buffer = static_cast<std::uint32_t>(found);
    const auto fixtureIndex = static_cast<std::uint16_t>(index);
    /// Whether this fixture's brightness is kept apart from its color — see `Virtual`.
    const bool isVirtualFixture = index < virtuals_.size() && virtuals_[index].active;
    const auto emitterOf = [](Role role) -> std::size_t {
        const auto at = std::find(kEmitters.begin(), kEmitters.end(), role);
        return at == kEmitters.end() ? kEmitters.size()
                                     : static_cast<std::size_t>(at - kEmitters.begin());
    };

    /// One of this fixture's emitters as a track on its virtual color, or nothing when the
    /// fixture is not virtual or has not got that emitter.
    const auto addVirtualColor = [&](Role role, double from, double to, Role tag) {
        const std::size_t emitter = emitterOf(role);
        if (!isVirtualFixture || emitter == kEmitters.size() || !has(fixture, role)) {
            return false;
        }
        Track track;
        track.isVirtual = true;
        track.component = static_cast<std::uint8_t>(emitter + 1);
        track.fixture = fixtureIndex;
        track.source = role;
        track.role = tag;
        track.from = std::clamp(from, 0.0, 1.0);
        track.to = std::clamp(to, 0.0, 1.0);
        running.tracks.push_back(track);
        return true;
    };

    /// One channel of this fixture as a track, or nothing when it has not got it. `fine`
    /// picks up the 16-bit partner where there is one, so a `Level` aimed at pan is smooth on
    /// a head that can be and stepped on one that cannot, without the rule saying which.
    ///
    /// On a virtual fixture an emitter is its color, not its channel: see `Virtual`.
    const auto addTrack = [&](Role role, double to, Role tag) {
        if (isVirtualFixture && emitterOf(role) != kEmitters.size()) {
            const double from = virtuals_[index].color[emitterOf(role)];
            return addVirtualColor(role, from, to, tag);
        }
        const std::uint16_t channel = channelOf(fixture, role);
        if (channel == 0) {
            return false;
        }
        Track track;
        track.buffer = buffer;
        track.channel = static_cast<std::uint16_t>(channel - 1);
        track.role = tag;
        track.fixture = fixtureIndex;
        track.source = role;
        const Role partner = role == Role::Pan    ? Role::PanFine
                             : role == Role::Tilt ? Role::TiltFine
                                                  : Role::Unused;
        if (partner != Role::Unused) {
            const std::uint16_t fine = channelOf(fixture, partner);
            track.fine = fine == 0 ? kNoChannel : static_cast<std::uint16_t>(fine - 1);
        }
        track.from = readChannel(track.buffer, track.channel, track.fine);
        track.to = std::clamp(to, 0.0, 1.0);
        running.tracks.push_back(track);
        return true;
    };

    /// The same with both ends given — a flash, a pulse, a strobe.
    const auto addSweep = [&](Role role, double from, double to) {
        if (isVirtualFixture && emitterOf(role) != kEmitters.size()) {
            return addVirtualColor(role, from, to, Role::Unused);
        }
        const std::uint16_t channel = channelOf(fixture, role);
        if (channel == 0) {
            return false;
        }
        Track track;
        track.buffer = buffer;
        track.channel = static_cast<std::uint16_t>(channel - 1);
        track.fixture = fixtureIndex;
        track.source = role;
        track.from = std::clamp(from, 0.0, 1.0);
        track.to = std::clamp(to, 0.0, 1.0);
        running.tracks.push_back(track);
        return true;
    };

    /// **A dimmer aimed at a fixture that has not got one** drives its virtual intensity.
    ///
    /// Most LED pars have no master intensity channel at all: their brightness *is* their
    /// color, scaled. `Role::Dimmer` has said so since it was written — *"a fixture without
    /// one has its color scaled instead"* — and nothing implemented it, so the commonest rig
    /// in the world (an RGB par, the default effect, the default channel) fired and the lamp
    /// stayed dark. Reported from a rig on 2026-09-16: *"Choosing the closest thing available
    /// right now, dimmer, does absolutely nothing — the light is not coming on at all."*
    ///
    /// The color is kept apart and scaled by this (`Virtual`), so "fade to half" on a fixture
    /// lit red is half red, and a flash after a flash is still red. A fixture that has no color
    /// at all — dark, never given one — is taken as **white**: its white LED where it has one,
    /// its color LEDs where it has not, which is what an operator aiming a dimmer at an unlit
    /// par means and the only reading under which the lamp comes on.
    const auto addIntensity = [&](double from, double to) {
        Virtual& state = virtuals_[index];
        const bool colorless = std::all_of(state.color.begin(), state.color.end(),
                                           [](double component) { return component <= 0.0; });
        if (colorless) {
            const bool white = has(fixture, Role::White);
            for (std::size_t e = 0; e < kEmitters.size(); ++e) {
                const Role role = kEmitters[e];
                if (has(fixture, role)) {
                    state.color[e] = white ? (role == Role::White ? 1.0 : 0.0)
                                           : (isColor(role) ? 1.0 : 0.0);
                }
            }
            state.dirty = true;
        }
        Track track;
        track.isVirtual = true;
        track.component = kIntensity;
        track.fixture = fixtureIndex;
        track.source = Role::Dimmer;
        track.from = std::clamp(from, 0.0, 1.0);
        track.to = std::clamp(to, 0.0, 1.0);
        running.tracks.push_back(track);
        return true;
    };

    /// Whether this payload's role is the virtual dimmer. A "level on pan" aimed at a wash still
    /// reaches nothing, which is `missed()`'s job to count.
    const bool virtualDimmer = payload.role == Role::Dimmer && isVirtualFixture;

    /// The pan/tilt pair, with the fixture's own window. Nothing when the fixture cannot move.
    const auto addMove = [&](double toPanUnit, double toTiltUnit, bool windowRelative) {
        const std::uint16_t pan = channelOf(fixture, Role::Pan);
        const std::uint16_t tilt = channelOf(fixture, Role::Tilt);
        if (pan == 0 && tilt == 0) {
            return false;
        }
        Move move;
        move.fixture = fixtureIndex;
        move.buffer = buffer;
        move.pan = pan == 0 ? kNoChannel : static_cast<std::uint16_t>(pan - 1);
        move.tilt = tilt == 0 ? kNoChannel : static_cast<std::uint16_t>(tilt - 1);
        const std::uint16_t panFine = channelOf(fixture, Role::PanFine);
        const std::uint16_t tiltFine = channelOf(fixture, Role::TiltFine);
        move.panFine = panFine == 0 ? kNoChannel : static_cast<std::uint16_t>(panFine - 1);
        move.tiltFine = tiltFine == 0 ? kNoChannel : static_cast<std::uint16_t>(tiltFine - 1);

        move.panLow = std::clamp(fixture.panMin, 0.0, 1.0);
        move.panHigh = std::clamp(fixture.panMax, 0.0, 1.0);
        move.tiltLow = std::clamp(fixture.tiltMin, 0.0, 1.0);
        move.tiltHigh = std::clamp(fixture.tiltMax, 0.0, 1.0);
        if (move.panHigh < move.panLow) {
            std::swap(move.panLow, move.panHigh);
        }
        if (move.tiltHigh < move.tiltLow) {
            std::swap(move.tiltLow, move.tiltHigh);
        }

        move.fromPan = move.pan == kNoChannel ? 0.0 : readChannel(buffer, move.pan, move.panFine);
        move.fromTilt =
            move.tilt == kNoChannel ? 0.0 : readChannel(buffer, move.tilt, move.tiltFine);
        // A target given as a fraction *of the window* is what makes one rule mean the same
        // gesture on six differently-limited heads. See `Payload::pan`.
        move.toPan = windowRelative ? move.panLow + std::clamp(toPanUnit, 0.0, 1.0) *
                                                        (move.panHigh - move.panLow)
                                    : std::clamp(toPanUnit, 0.0, 1.0);
        move.toTilt = windowRelative ? move.tiltLow + std::clamp(toTiltUnit, 0.0, 1.0) *
                                                          (move.tiltHigh - move.tiltLow)
                                     : std::clamp(toTiltUnit, 0.0, 1.0);
        running.moves.push_back(move);
        return true;
    };

    switch (payload.kind) {
    case EffectKind::Level:
        if (virtualDimmer) {
            return addIntensity(virtuals_[index].intensity, unitOfByte(payload.level));
        }
        return addTrack(payload.role, unitOfByte(payload.level), Role::Unused);
    case EffectKind::Flash:
        // Starts *at* the peak: `from` is the peak and `to` is the base, so evaluating at
        // progress zero writes full immediately and the duration is the decay.
        if (virtualDimmer) {
            return addIntensity(unitOfByte(payload.level), unitOfByte(payload.base));
        }
        return addSweep(payload.role, unitOfByte(payload.level), unitOfByte(payload.base));
    case EffectKind::Pulse:
    case EffectKind::Strobe:
        if (virtualDimmer) {
            return addIntensity(unitOfByte(payload.base), unitOfByte(payload.level));
        }
        return addSweep(payload.role, unitOfByte(payload.base), unitOfByte(payload.level));
    case EffectKind::Color: {
        bool any = false;
        // An RGBW fixture makes a far better white from its white LED than from three
        // colored ones, and a far worse *color* if some white is left mixed into it. So a
        // neutral grey goes to White and nothing else, and every other color goes to RGB with
        // White driven to zero. Predictable in both directions, and the only case it treats
        // specially is the one where the two are unambiguously the same instruction.
        //
        // **Only where the fixture actually has a white channel.** Without that guard, asking
        // an ordinary RGB par for white would route the level to a channel it has not got and
        // drive its three real ones to zero — a fixture that goes dark on the one color an
        // operator is most likely to try first.
        const bool neutral = has(fixture, Role::White) && payload.color.r == payload.color.g &&
                             payload.color.g == payload.color.b && payload.color.r != 0;
        any |= addTrack(Role::Red, neutral ? 0.0 : unitOfByte(payload.color.r), Role::Red);
        any |= addTrack(Role::Green, neutral ? 0.0 : unitOfByte(payload.color.g), Role::Green);
        any |= addTrack(Role::Blue, neutral ? 0.0 : unitOfByte(payload.color.b), Role::Blue);
        any |= addTrack(Role::White, neutral ? unitOfByte(payload.color.r) : 0.0, Role::White);
        return any;
    }
    case EffectKind::HueSweep: {
        bool any = false;
        // The targets are recomputed every round from the swept hue; these are here for their
        // `from` and for the channels they name. `role` tags which component each carries.
        any |= addTrack(Role::Red, 0.0, Role::Red);
        any |= addTrack(Role::Green, 0.0, Role::Green);
        any |= addTrack(Role::Blue, 0.0, Role::Blue);
        addTrack(Role::White, 0.0, Role::White);
        return any;
    }
    case EffectKind::Blackout: {
        // Every emitter to zero — on a virtual fixture its color, so the next dimmer effect
        // finds it colorless and comes up white rather than in a hue from before the blackout.
        bool any = false;
        for (const Role role : kBlackoutRoles) {
            any |= addTrack(role, 0.0, Role::Unused);
        }
        return any;
    }
    case EffectKind::Position:
        return addMove(static_cast<double>(payload.pan), static_cast<double>(payload.tilt), true);
    case EffectKind::Home:
        return addMove(0.5, 0.5, true);
    case EffectKind::Path:
        // A path orbits the middle of the window; `size` scales its radius against the
        // window's half-width, so the figure stays inside the limits whatever they are.
        return addMove(0.5, 0.5, true);
    }
    return false;
}

void DmxEngine::preempt(const Running& running) {
    // Which channels — or which half of which virtual fixture — the new effect is about to
    // drive. Small and on the stack in every real case: a color is three, a head is two, and
    // the biggest is a blackout's seven.
    const auto collides = [&running](const Track& older) {
        for (const Track& track : running.tracks) {
            if (older.isVirtual || track.isVirtual) {
                if (older.isVirtual && track.isVirtual && older.fixture == track.fixture &&
                    older.component == track.component) {
                    return true;
                }
                continue;
            }
            if (track.buffer == older.buffer &&
                (track.channel == older.channel || track.fine == older.channel)) {
                return true;
            }
        }
        for (const Move& move : running.moves) {
            if (!older.isVirtual && move.buffer == older.buffer &&
                (move.pan == older.channel || move.panFine == older.channel ||
                 move.tilt == older.channel || move.tiltFine == older.channel)) {
                return true;
            }
        }
        return false;
    };
    const auto movesCollide = [&running](const Move& older) {
        const auto hits = [&](std::uint16_t channel) {
            if (channel == kNoChannel) {
                return false;
            }
            for (const Track& track : running.tracks) {
                if (!track.isVirtual && track.buffer == older.buffer &&
                    (track.channel == channel || track.fine == channel)) {
                    return true;
                }
            }
            for (const Move& move : running.moves) {
                if (move.buffer == older.buffer &&
                    (move.pan == channel || move.panFine == channel || move.tilt == channel ||
                     move.tiltFine == channel)) {
                    return true;
                }
            }
            return false;
        };
        return hits(older.pan) || hits(older.tilt);
    };

    for (Running& older : running_) {
        std::erase_if(older.tracks, collides);
        std::erase_if(older.moves, movesCollide);
    }
    std::erase_if(running_, [](const Running& r) { return r.tracks.empty() && r.moves.empty(); });
}

void DmxEngine::start(std::uint64_t fixtures, const Payload& payload, double now) {
    staging_.tracks.clear();
    staging_.moves.clear();
    staging_.payload = payload;
    staging_.start = now;
    staging_.duration = payload.durationSeconds > 0.0f ? static_cast<double>(payload.durationSeconds) : 0.0;

    const std::size_t count = std::min(patch_.size(), kMaxRoutableFixtures);
    for (std::size_t i = 0; i < count; ++i) {
        if ((fixtures & (std::uint64_t{1} << i)) == 0) {
            continue;
        }
        if (!patch_[i].enabled) {
            continue;
        }
        buildFor(i, payload, staging_);
    }

    ++started_;
    if (staging_.tracks.empty() && staging_.moves.empty()) {
        ++missed_;
        return;
    }

    preempt(staging_);
    running_.push_back(staging_);
    // Evaluated once here so that a snap lands on this round rather than the next, and so
    // that a flash is at full in the same frame the rule fired in. A one-round delay is a
    // millisecond and would not be visible; landing a *frame* late would be 23 ms and is.
    evaluate(running_.back(), now);
    composeVirtuals();
}

void DmxEngine::evaluate(const Running& running, double now) {
    const Payload& payload = running.payload;
    const double progress = running.duration > 0.0
                                ? std::clamp((now - running.start) / running.duration, 0.0, 1.0)
                                : 1.0;

    switch (payload.kind) {
    case EffectKind::Pulse: {
        const double phase = cyclePhase(progress, static_cast<double>(payload.cycles));
        // A cosine rather than a sine so the effect starts at its low end: a pulse that began
        // half way up would flash on every fire.
        const double wave = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * phase);
        for (const Track& track : running.tracks) {
            writeTrack(track,
                         track.from + (track.to - track.from) * wave);
        }
        return;
    }
    case EffectKind::Strobe: {
        const double phase = cyclePhase(progress, static_cast<double>(payload.cycles));
        const double duty = std::clamp(static_cast<double>(payload.duty), 0.0, 1.0);
        // The last frame lands on the low end rather than mid-flash, so a strobe that has
        // finished leaves the fixture off rather than wherever the square wave happened to be.
        const bool on = progress < 1.0 && phase < duty;
        for (const Track& track : running.tracks) {
            writeTrack(track, on ? track.to : track.from);
        }
        return;
    }
    case EffectKind::HueSweep: {
        const double shaped = curveAt(payload.curve, progress);
        const double hue = static_cast<double>(payload.hueFrom) +
                           static_cast<double>(payload.hueTo - payload.hueFrom) * shaped;
        double baseHue = 0.0;
        double saturation = 1.0;
        double value = 1.0;
        toHsv(payload.color, baseHue, saturation, value);
        const Color swept = fromHsv(hue, saturation, value);
        for (const Track& track : running.tracks) {
            const double unit = track.role == Role::Red     ? unitOfByte(swept.r)
                                : track.role == Role::Green ? unitOfByte(swept.g)
                                : track.role == Role::Blue  ? unitOfByte(swept.b)
                                                            : 0.0;
            writeTrack(track, unit);
        }
        return;
    }
    case EffectKind::Path: {
        const double turns =
            progress * static_cast<double>(payload.cycles > 0.0f ? payload.cycles : 1.0f);
        const double angle = 2.0 * std::numbers::pi * turns;
        const double size = std::clamp(static_cast<double>(payload.size), 0.0, 1.0);
        for (const Move& move : running.moves) {
            // The radius is half the window times `size`, so a full-size circle just touches
            // the operator's own limits and never crosses them.
            const double panRadius = (move.panHigh - move.panLow) * 0.5 * size;
            const double tiltRadius = (move.tiltHigh - move.tiltLow) * 0.5 * size;
            double panOffset = 0.0;
            double tiltOffset = 0.0;
            switch (payload.shape) {
            case PathShape::Circle:
                panOffset = std::cos(angle) * panRadius;
                tiltOffset = std::sin(angle) * tiltRadius;
                break;
            case PathShape::Figure8:
                // Tilt at twice pan's rate: a lissajous 8 lying on its side, which is the one
                // that reads as a head looking around rather than nodding.
                panOffset = std::sin(angle) * panRadius;
                tiltOffset = std::sin(2.0 * angle) * tiltRadius * 0.5;
                break;
            case PathShape::Sweep:
                panOffset = std::sin(angle) * panRadius;
                break;
            case PathShape::Square: {
                const double side = std::fmod(turns, 1.0) * 4.0;
                const int corner = static_cast<int>(side) & 3;
                panOffset = (corner == 0 || corner == 3) ? panRadius : -panRadius;
                tiltOffset = (corner < 2) ? tiltRadius : -tiltRadius;
                break;
            }
            }
            if (move.pan != kNoChannel) {
                writeChannel(move.buffer, move.pan, move.panFine,
                             std::clamp(move.toPan + panOffset, move.panLow, move.panHigh));
            }
            if (move.tilt != kNoChannel) {
                writeChannel(move.buffer, move.tilt, move.tiltFine,
                             std::clamp(move.toTilt + tiltOffset, move.tiltLow, move.tiltHigh));
            }
        }
        return;
    }
    case EffectKind::Position:
    case EffectKind::Home: {
        const double shaped = curveAt(payload.curve, progress);
        for (const Move& move : running.moves) {
            if (move.pan != kNoChannel) {
                writeChannel(move.buffer, move.pan, move.panFine,
                             move.fromPan + (move.toPan - move.fromPan) * shaped);
            }
            if (move.tilt != kNoChannel) {
                writeChannel(move.buffer, move.tilt, move.tiltFine,
                             move.fromTilt + (move.toTilt - move.fromTilt) * shaped);
            }
        }
        return;
    }
    case EffectKind::Level:
    case EffectKind::Color:
    case EffectKind::Flash:
    case EffectKind::Blackout:
        break;
    }

    const double shaped = curveAt(payload.curve, progress);
    for (const Track& track : running.tracks) {
        writeTrack(track,
                     track.from + (track.to - track.from) * shaped);
    }
}

void DmxEngine::holdChannel(PortAddress universe, std::uint16_t channel, std::uint8_t level,
                            double seconds, double now) {
    const std::size_t index = bufferOf(universe);
    if (index == static_cast<std::size_t>(-1) || channel < 1 || channel > kChannelsPerUniverse) {
        return;
    }

    // **A strobe of one cycle at full duty**, which is a hold that lets go: `evaluate` writes
    // `to` for every round the effect is running and `from` on the round it ends — see
    // `EffectKind::Strobe`, where landing on the low end at the finish is deliberate so a
    // strobe leaves the fixture off rather than mid-flash. That is exactly *hold, then put it
    // back*, so this needs no machinery of its own and cannot drift from the rest.
    staging_.tracks.clear();
    staging_.moves.clear();
    staging_.payload = Payload{};
    staging_.payload.kind = EffectKind::Strobe;
    staging_.payload.cycles = 1.0f;
    staging_.payload.duty = 1.0f;
    staging_.start = now;
    staging_.duration = seconds > 0.0 ? seconds : 0.0;

    Track track;
    track.buffer = static_cast<std::uint32_t>(index);
    track.channel = static_cast<std::uint16_t>(channel - 1);
    track.from = readChannel(track.buffer, track.channel, kNoChannel);
    track.to = unitOfByte(level);
    staging_.tracks.push_back(track);

    ++started_;
    preempt(staging_);
    running_.push_back(staging_);
    evaluate(running_.back(), now);
}

void DmxEngine::tick(double now) {
    for (const Running& running : running_) {
        evaluate(running, now);
    }
    // Every virtual fixture an effect moved this round, written once from its color and its
    // intensity — after every effect, so a color change and a flash in the same round compose
    // rather than the later one's write replacing the earlier one's.
    composeVirtuals();
    // Finished effects leave their last value in the buffer and are dropped. Holding rather
    // than releasing is what makes a fade to 40% *stay* at 40% — a lighting level is a state,
    // and an effect that undid itself when it ended would be a fade that flickered back.
    std::erase_if(running_, [now](const Running& running) {
        return running.duration <= 0.0 || now >= running.start + running.duration;
    });
}

} // namespace takt4::dmx
