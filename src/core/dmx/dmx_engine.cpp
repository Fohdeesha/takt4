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
    // Effects point at buffer indices and channel numbers that a re-patch can move under
    // them. Cancelling holds the levels; see the header.
    running_.clear();

    patch_ = std::move(patch);
    universes_ = universesOf(patch_);

    std::vector<Buffer> rebuilt;
    rebuilt.reserve(universes_.size());
    for (const PortAddress universe : universes_) {
        Buffer buffer;
        buffer.universe = universe;
        const std::size_t old = bufferOf(universe);
        if (old != static_cast<std::size_t>(-1)) {
            // Keep what this universe was already showing. Only channels the old patch did
            // not reach are given a parked level below, so adding a fixture does not reset
            // the ones already lit.
            buffer.levels = buffers_[old].levels;
            buffer.covered = buffers_[old].covered;
            buffer.revision = buffers_[old].revision;
        }
        rebuilt.push_back(buffer);
    }

    // Now lay the patch over it: every patched channel is marked covered, and one that was
    // not covered before takes its fixture's parked level.
    for (const Fixture& fixture : patch_) {
        if (!fixture.enabled || fixture.channels.empty()) {
            continue;
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
        Buffer& buffer = rebuilt[index];
        for (std::size_t i = 0; i < fixture.channels.size(); ++i) {
            const std::size_t channel = std::size_t{fixture.address} + i;
            if (channel < 1 || channel > kChannelsPerUniverse) {
                continue;
            }
            const std::size_t at = channel - 1;
            if (!buffer.covered[at]) {
                buffer.levels[at] = i < fixture.parked.size() ? fixture.parked[i] : 0;
                ++buffer.revision;
            }
            buffer.covered[at] = true;
        }
    }

    buffers_ = std::move(rebuilt);
}

void DmxEngine::cancelAll() noexcept {
    running_.clear();
}

void DmxEngine::reset() {
    running_.clear();
    for (Buffer& buffer : buffers_) {
        buffer.levels.fill(0);
        buffer.covered.fill(false);
        ++buffer.revision;
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

bool DmxEngine::buildFor(const Fixture& fixture, const Payload& payload, Running& running) {
    const std::size_t index = bufferOf(fixture.universe);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    const auto buffer = static_cast<std::uint32_t>(index);

    /// One channel of this fixture as a track, or nothing when it has not got it. `fine`
    /// picks up the 16-bit partner where there is one, so a `Level` aimed at pan is smooth on
    /// a head that can be and stepped on one that cannot, without the rule saying which.
    const auto addTrack = [&](Role role, double to, Role tag) {
        const std::uint16_t channel = channelOf(fixture, role);
        if (channel == 0) {
            return false;
        }
        Track track;
        track.buffer = buffer;
        track.channel = static_cast<std::uint16_t>(channel - 1);
        track.role = tag;
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

    /// **A level aimed at a dimmer the fixture has not got.**
    ///
    /// Most LED pars have no master intensity channel at all: their brightness *is* their
    /// color, scaled. `Role::Dimmer` has said so since it was written — *"a fixture without
    /// one has its color scaled instead"* — and nothing implemented it, so the commonest
    /// rig in the world (an RGB par, the default effect, the default channel) fired and the
    /// lamp stayed dark. Reported from a rig on 2026-09-16: *"Choosing the closest thing
    /// available right now, dimmer, does absolutely nothing — the light is not coming on at
    /// all."*
    ///
    /// **The weights are the color the fixture is showing**, so "fade to half" on a fixture
    /// lit red is half red rather than half white, and a color rule followed by a dimmer
    /// rule does what the two of them say. A fixture sitting dark has no color to scale and
    /// would stay dark however far the fade went, so it is taken as **white** — its white LED
    /// where it has one, its three color LEDs where it has not. That is what an operator
    /// aiming a dimmer at an unlit par means, and it is the only reading under which the
    /// lamp comes on.
    ///
    /// `from` is left to `addTrack`, which reads the channel: a fade starts where the
    /// fixture is. The peak/base kinds pass both ends and build their own tracks.
    const auto dimmerWeights = [&]() {
        std::array<std::pair<Role, double>, kEmitters.size()> weights{};
        std::size_t count = 0;
        double lit = 0.0;
        for (const Role role : kEmitters) {
            const std::uint16_t channel = channelOf(fixture, role);
            if (channel == 0) {
                continue;
            }
            const double level = readChannel(buffer, static_cast<std::uint16_t>(channel - 1),
                                             DmxEngine::kNoChannel);
            weights[count++] = {role, level};
            lit = std::max(lit, level);
        }
        if (lit <= 0.0) {
            // Dark: white. On a fixture with a white LED that is the white channel alone —
            // the same judgement `EffectKind::Color` makes, and for the same reason, since
            // three colored LEDs make a worse white than the one built for it.
            const bool white = has(fixture, Role::White);
            for (std::size_t i = 0; i < count; ++i) {
                const Role role = weights[i].first;
                weights[i].second = white ? (role == Role::White ? 1.0 : 0.0)
                                          : (isColor(role) ? 1.0 : 0.0);
            }
            return std::pair{weights, count};
        }
        // **Normalised against the brightest channel, which is what makes the dimmer a
        // dimmer.** Taken raw, a par already faded to half would read as weights of 0.5 and
        // a second rule asking for full would reach a quarter — each fade compounding the
        // last, so the lamp walks down to nothing over a set and nothing on screen says why.
        // Divided through, the weights are the *hue* the fixture is showing and the level is
        // the whole of the brightness.
        for (std::size_t i = 0; i < count; ++i) {
            weights[i].second /= lit;
        }
        return std::pair{weights, count};
    };

    /// One `Level` on a virtual dimmer: each emitter to its share of the target.
    const auto addVirtualLevel = [&](double toUnit) {
        const auto [weights, count] = dimmerWeights();
        bool any = false;
        for (std::size_t i = 0; i < count; ++i) {
            any |= addTrack(weights[i].first, toUnit * weights[i].second, Role::Unused);
        }
        return any;
    };

    /// The same for the kinds that sweep between two levels — a flash, a pulse, a strobe.
    /// Both ends are scaled, so a strobe between 0 and full on a red par strobes red.
    const auto addVirtualSweep = [&](double fromUnit, double toUnit) {
        const auto [weights, count] = dimmerWeights();
        bool any = false;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint16_t channel = channelOf(fixture, weights[i].first);
            if (channel == 0) {
                continue;
            }
            Track track;
            track.buffer = buffer;
            track.channel = static_cast<std::uint16_t>(channel - 1);
            track.from = std::clamp(fromUnit * weights[i].second, 0.0, 1.0);
            track.to = std::clamp(toUnit * weights[i].second, 0.0, 1.0);
            running.tracks.push_back(track);
            any = true;
        }
        return any;
    };

    /// Whether this payload's role has to be faked. Only the dimmer is, and only on a
    /// fixture that emits — a "level on pan" aimed at a wash still reaches nothing, which is
    /// `missed()`'s job to count.
    const bool virtualDimmer =
        payload.role == Role::Dimmer && !has(fixture, Role::Dimmer) && emits(fixture);

    /// The pan/tilt pair, with the fixture's own window. Nothing when the fixture cannot move.
    const auto addMove = [&](double toPanUnit, double toTiltUnit, bool windowRelative) {
        const std::uint16_t pan = channelOf(fixture, Role::Pan);
        const std::uint16_t tilt = channelOf(fixture, Role::Tilt);
        if (pan == 0 && tilt == 0) {
            return false;
        }
        Move move;
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
            return addVirtualLevel(unitOfByte(payload.level));
        }
        return addTrack(payload.role, unitOfByte(payload.level), Role::Unused);
    case EffectKind::Flash:
        // Starts *at* the peak: `from` is the peak and `to` is the base, so evaluating at
        // progress zero writes full immediately and the duration is the decay.
        if (virtualDimmer) {
            return addVirtualSweep(unitOfByte(payload.level), unitOfByte(payload.base));
        }
        if (const std::uint16_t channel = channelOf(fixture, payload.role); channel != 0) {
            Track track;
            track.buffer = buffer;
            track.channel = static_cast<std::uint16_t>(channel - 1);
            track.from = unitOfByte(payload.level);
            track.to = unitOfByte(payload.base);
            running.tracks.push_back(track);
            return true;
        }
        return false;
    case EffectKind::Pulse:
    case EffectKind::Strobe:
        if (virtualDimmer) {
            return addVirtualSweep(unitOfByte(payload.base), unitOfByte(payload.level));
        }
        if (const std::uint16_t channel = channelOf(fixture, payload.role); channel != 0) {
            Track track;
            track.buffer = buffer;
            track.channel = static_cast<std::uint16_t>(channel - 1);
            track.from = unitOfByte(payload.base);
            track.to = unitOfByte(payload.level);
            running.tracks.push_back(track);
            return true;
        }
        return false;
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
        bool any = false;
        for (const Role role : kBlackoutRoles) {
            any |= addTrack(role, 0.0, Role::Unused);
        }
        return any;
    }
    case EffectKind::Position:
        return addMove(payload.pan, payload.tilt, true);
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
    // Which channels the new effect is about to drive. Small and on the stack in every real
    // case — a color is three, a head is two, and the biggest is a blackout's seven.
    const auto collides = [&running](std::uint32_t buffer, std::uint16_t channel) {
        if (channel == kNoChannel) {
            return false;
        }
        for (const Track& track : running.tracks) {
            if (track.buffer == buffer && (track.channel == channel || track.fine == channel)) {
                return true;
            }
        }
        for (const Move& move : running.moves) {
            if (move.buffer == buffer && (move.pan == channel || move.panFine == channel ||
                                          move.tilt == channel || move.tiltFine == channel)) {
                return true;
            }
        }
        return false;
    };

    for (Running& older : running_) {
        std::erase_if(older.tracks,
                      [&](const Track& track) { return collides(track.buffer, track.channel); });
        std::erase_if(older.moves, [&](const Move& move) {
            return collides(move.buffer, move.pan) || collides(move.buffer, move.tilt);
        });
    }
    std::erase_if(running_, [](const Running& r) { return r.tracks.empty() && r.moves.empty(); });
}

void DmxEngine::start(std::uint64_t fixtures, const Payload& payload, double now) {
    staging_.tracks.clear();
    staging_.moves.clear();
    staging_.payload = payload;
    staging_.start = now;
    staging_.duration = payload.durationSeconds > 0.0f ? payload.durationSeconds : 0.0;

    const std::size_t count = std::min(patch_.size(), kMaxRoutableFixtures);
    for (std::size_t i = 0; i < count; ++i) {
        if ((fixtures & (std::uint64_t{1} << i)) == 0) {
            continue;
        }
        const Fixture& fixture = patch_[i];
        if (!fixture.enabled) {
            continue;
        }
        buildFor(fixture, payload, staging_);
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
}

void DmxEngine::evaluate(const Running& running, double now) {
    const Payload& payload = running.payload;
    const double progress = running.duration > 0.0
                                ? std::clamp((now - running.start) / running.duration, 0.0, 1.0)
                                : 1.0;

    switch (payload.kind) {
    case EffectKind::Pulse: {
        const double phase = cyclePhase(progress, payload.cycles);
        // A cosine rather than a sine so the effect starts at its low end: a pulse that began
        // half way up would flash on every fire.
        const double wave = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * phase);
        for (const Track& track : running.tracks) {
            writeChannel(track.buffer, track.channel, track.fine,
                         track.from + (track.to - track.from) * wave);
        }
        return;
    }
    case EffectKind::Strobe: {
        const double phase = cyclePhase(progress, payload.cycles);
        const double duty = std::clamp(static_cast<double>(payload.duty), 0.0, 1.0);
        // The last frame lands on the low end rather than mid-flash, so a strobe that has
        // finished leaves the fixture off rather than wherever the square wave happened to be.
        const bool on = progress < 1.0 && phase < duty;
        for (const Track& track : running.tracks) {
            writeChannel(track.buffer, track.channel, track.fine, on ? track.to : track.from);
        }
        return;
    }
    case EffectKind::HueSweep: {
        const double shaped = curveAt(payload.curve, progress);
        const double hue = payload.hueFrom + (payload.hueTo - payload.hueFrom) * shaped;
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
            writeChannel(track.buffer, track.channel, track.fine, unit);
        }
        return;
    }
    case EffectKind::Path: {
        const double turns = progress * (payload.cycles > 0.0f ? payload.cycles : 1.0f);
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
        writeChannel(track.buffer, track.channel, track.fine,
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
    // Finished effects leave their last value in the buffer and are dropped. Holding rather
    // than releasing is what makes a fade to 40% *stay* at 40% — a lighting level is a state,
    // and an effect that undid itself when it ended would be a fade that flickered back.
    std::erase_if(running_, [now](const Running& running) {
        return running.duration <= 0.0 || now >= running.start + running.duration;
    });
}

} // namespace takt4::dmx
