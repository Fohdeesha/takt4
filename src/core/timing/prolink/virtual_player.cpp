#include "core/timing/prolink/virtual_player.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

namespace takt4::timing::prolink {

namespace {

/// A claim heard from another device keeps its number out of reach for this long, finished or
/// not: a player that claims a number holds it, and its first keep-alive may yet be lost.
constexpr std::int64_t kClaimHeldMicros = 10'000'000;

} // namespace

VirtualPlayer::VirtualPlayer() : VirtualPlayer(Options{}) {}

VirtualPlayer::VirtualPlayer(Options options) : options_(std::move(options)) {}

void VirtualPlayer::listen(std::int64_t now) {
    phase_ = Phase::Listening;
    listeningSince_ = now;
    number_ = 0;
    counter_ = 0;
    mixerAsked_ = false;
    led_ = 0;
    devices_.clear();
    claimed_.fill(false);
    refused_.fill(false);
    outbox_.clear();
    heard_.clear();
}

void VirtualPlayer::join(const Identity& identity, std::int64_t now) {
    if (phase_ != Phase::Listening) {
        return;
    }
    identity_ = identity;
    haveIdentity_ = true;
    refused_.fill(false);
    number_ = 0;
    mixerAsked_ = false;
    phase_ = Phase::Watching;
    joinAt_ = std::max(now, listeningSince_ + options_.watchMicros);
}

void VirtualPlayer::leave() noexcept {
    // Its address is kept: a keep-alive of its own still on its way back round is not a device.
    phase_ = Phase::Listening;
    number_ = 0;
    counter_ = 0;
}

VirtualPlayer::Device* VirtualPlayer::find(std::uint8_t number) noexcept {
    const auto at = std::find_if(devices_.begin(), devices_.end(),
                                 [number](const Device& d) { return d.number == number; });
    return at == devices_.end() ? nullptr : &*at;
}

const VirtualPlayer::Device* VirtualPlayer::find(std::uint8_t number) const noexcept {
    const auto at = std::find_if(devices_.begin(), devices_.end(),
                                 [number](const Device& d) { return d.number == number; });
    return at == devices_.end() ? nullptr : &*at;
}

VirtualPlayer::Device& VirtualPlayer::deviceFor(std::uint8_t number, std::int64_t at) {
    if (Device* known = find(number)) {
        return *known;
    }
    Device made;
    made.number = number;
    made.announcedAt = at;
    devices_.push_back(made);
    return devices_.back();
}

void VirtualPlayer::receive(Channel channel, std::span<const std::uint8_t> datagram,
                            std::uint32_t from, std::int64_t at, std::uint32_t via) {
    // Its own, back round: a broadcast is delivered to the sender's machine too.
    if (haveIdentity_ && from == identity_.ip) {
        return;
    }
    switch (channel) {
    case Channel::Announce: {
        AnnouncePacket packet;
        if (parseAnnounce(datagram, packet)) {
            announce(packet, from, at, via);
        }
        break;
    }
    case Channel::Beat: {
        BeatPacket packet;
        if (parseBeat(datagram, packet)) {
            beat(packet, at);
        }
        break;
    }
    case Channel::Status: {
        StatusPacket packet;
        if (parseStatus(datagram, packet)) {
            status(packet, at);
        }
        break;
    }
    }
}

void VirtualPlayer::announce(const AnnouncePacket& packet, std::uint32_t from, std::int64_t at,
                             std::uint32_t via) {
    const bool claiming = phase_ == Phase::Hello || phase_ == Phase::FirstClaim ||
                          phase_ == Phase::SecondClaim || phase_ == Phase::FinalClaim;
    const bool mine = number_ != 0 && packet.device == number_;
    switch (packet.type) {
    case Announce::KeepAlive: {
        if (packet.device == 0) {
            return;
        }
        Device& device = deviceFor(packet.device, at);
        device.name = packet.name;
        device.ip = packet.ip != 0 ? packet.ip : from;
        device.mac = packet.mac;
        if (via != 0) {
            device.via = via;
        }
        if (packet.kind != DeviceKind::Unknown) {
            device.kind = packet.kind;
        }
        device.announcedAt = at;
        if (!mine) {
            return;
        }
        if (phase_ == Phase::Joined && !otherKeeps(from)) {
            sendTo(from, buildConflict(options_.name, number_, identity_.ip));
        } else if (phase_ == Phase::Joined || phase_ == Phase::SecondClaim ||
                   phase_ == Phase::FinalClaim) {
            // Held by a device already on the network, or by one that keeps it: another number.
            giveUpNumber(at);
        }
        return;
    }
    case Announce::SecondClaim:
    case Announce::FinalClaim:
        if (packet.device == 0) {
            return;
        }
        claimed_[packet.device] = true;
        claimedAt_[packet.device] = at;
        if (!mine) {
            return;
        }
        if (phase_ == Phase::Joined) {
            // "That number is mine" — the defence a player already holding it sends.
            sendTo(from, buildConflict(options_.name, number_, identity_.ip));
        } else if ((phase_ == Phase::SecondClaim || phase_ == Phase::FinalClaim) &&
                   otherKeeps(from)) {
            giveUpNumber(at);
        }
        return;
    case Announce::Conflict:
        if (mine && (claiming || phase_ == Phase::Joined)) {
            giveUpNumber(at);
        }
        return;
    case Announce::AssignmentFinished:
        if (phase_ == Phase::FinalClaim) {
            becomeJoined(at);
        }
        return;
    case Announce::AssignmentIntention:
        // Answered once a join. An XDJ-XZ sends one for every number claimed on its network port,
        // and answering each would go back to asking it for a number for ever.
        if (!mixerAsked_ && (phase_ == Phase::Hello || phase_ == Phase::FirstClaim ||
                             phase_ == Phase::SecondClaim)) {
            mixerAsked_ = true;
            mixerIp_ = from;
            sendTo(from, buildNumberRequest(options_.name, identity_.ip, identity_.mac));
            phase_ = Phase::AwaitingMixer;
            mixerDeadline_ = at + options_.mixerWaitMicros;
        }
        return;
    case Announce::Assignment:
        if (phase_ == Phase::AwaitingMixer && from == mixerIp_) {
            if (assignable(packet.device, at)) {
                number_ = packet.device;
                startStage(Phase::FinalClaim, at);
            } else {
                // A deck's number from a channel's port, or nought: not one takt4 may hold, so it
                // is claimed as anywhere else, now rather than at the deadline.
                claimNext(at);
            }
        }
        return;
    case Announce::Hello:
    case Announce::FirstClaim:
        return;
    }
}

void VirtualPlayer::status(const StatusPacket& packet, std::int64_t at) {
    if (packet.device == 0) {
        return;
    }
    Device& device = deviceFor(packet.device, at);
    if (device.kind == DeviceKind::Unknown) {
        device.kind = packet.mixer ? DeviceKind::Mixer : DeviceKind::Player;
        device.name = packet.name;
    }
    const bool wasMaster = device.hasStatus && at - device.statusAt <= options_.statusFreshMicros &&
                           device.status.master();
    if (packet.master() && !wasMaster) {
        device.masterSince = at;
    }
    device.status = packet;
    device.hasStatus = true;
    device.statusAt = at;
}

void VirtualPlayer::beat(const BeatPacket& packet, std::int64_t at) {
    if (packet.device == 0) {
        return;
    }
    Device& device = deviceFor(packet.device, at);
    device.beatAt = at;
    device.heardBeat = true;
    if (packet.device != followed(at)) {
        return;
    }
    if (device.hasStatus && at - device.statusAt <= options_.statusFreshMicros &&
        device.status.cueing()) {
        ++cueDropped_;
        return;
    }
    Heard heard;
    heard.at = at;
    heard.device = packet.device;
    heard.bpm = packet.bpm();
    heard.nextBeatMs = packet.nextBeatMs;
    heard.pitch = packet.pitch;
    heard.beatInBar =
        packet.beatInBar >= 1 && packet.beatInBar <= 4 ? packet.beatInBar : std::uint8_t{0};
    heard_.push_back(heard);
}

std::uint8_t VirtualPlayer::master(std::int64_t now) const noexcept {
    const Device* best = nullptr;
    for (const Device& device : devices_) {
        if (!device.hasStatus || now - device.statusAt > options_.statusFreshMicros ||
            !device.status.master()) {
            continue;
        }
        // During a hand-off the old master and the new both say they are, for a moment: the new
        // one is the one that took the role most recently.
        if (best == nullptr || device.masterSince > best->masterSince ||
            (device.masterSince == best->masterSince && device.number < best->number)) {
            best = &device;
        }
    }
    return best == nullptr ? std::uint8_t{0} : best->number;
}

bool VirtualPlayer::onAirTold(std::int64_t now) const noexcept {
    return std::any_of(devices_.begin(), devices_.end(), [&](const Device& device) {
        return fresh(device, now) && !device.status.mixer && device.status.onAir();
    });
}

bool VirtualPlayer::canLead(const Device& device, std::int64_t now, bool onAir) const noexcept {
    if (!fresh(device, now)) {
        return false;
    }
    const StatusPacket& status = device.status;
    if (status.mixer) {
        // It says it is playing whatever happens, and is never on the air: as the master, it is
        // the one the DJ chose.
        return status.master();
    }
    // Playing, by its status or by a beat newer than its status: a deck's first beat after a
    // pause comes some 60 ms before its status says it is playing (S06, resuming at 46.2 s). A cue
    // press's beat is not mistaken for that — its status says it is cueing about 12 ms first.
    const bool playing = status.playing() || (device.heardBeat && device.beatAt > device.statusAt);
    return playing && !status.cueing() && status.beatGrid() && status.masterMeaningful != 2 &&
           (!onAir || status.onAir());
}

std::uint8_t VirtualPlayer::leader(std::int64_t now) const noexcept {
    const bool onAir = onAirTold(now);
    if (const std::uint8_t number = master(now); number != 0) {
        if (const Device* device = find(number);
            device != nullptr && canLead(*device, now, onAir)) {
            return number;
        }
    }
    // No master that leads: paused, faded out, playing a track it cannot lead with — or nobody
    // holds the role yet, which a player that starts playing takes only some 50 ms after its first
    // beat (measured on a CDJ-2000nexus). The one player that can lead does; of more than one, the
    // one already followed while it still can, so a deck mixed in by ear does not take the beats
    // from the one playing to the room — and otherwise nobody, as there is no telling which.
    std::uint8_t only = 0;
    std::size_t count = 0;
    for (const Device& device : devices_) {
        if (device.status.mixer || !canLead(device, now, onAir)) {
            continue;
        }
        if (device.number == led_) {
            return led_;
        }
        only = device.number;
        ++count;
    }
    return count == 1 ? only : std::uint8_t{0};
}

std::uint8_t VirtualPlayer::followed(std::int64_t now) const noexcept {
    return follow_ == kFollowMaster ? leader(now) : follow_;
}

bool VirtualPlayer::numberUsed(std::uint8_t number, std::int64_t now) const noexcept {
    if (refused_[number] || (claimed_[number] && now - claimedAt_[number] < kClaimHeldMicros)) {
        return true;
    }
    return find(number) != nullptr;
}

bool VirtualPlayer::assignable(std::uint8_t number, std::int64_t now) const noexcept {
    return number >= options_.firstNumber && number <= options_.lastNumber &&
           !numberUsed(number, now);
}

void VirtualPlayer::claimNext(std::int64_t now) {
    number_ = 0;
    for (int n = options_.firstNumber; n <= options_.lastNumber; ++n) {
        const auto candidate = static_cast<std::uint8_t>(n);
        if (!numberUsed(candidate, now)) {
            number_ = candidate;
            break;
        }
    }
    if (number_ == 0) {
        phase_ = Phase::NoNumber;
        nextStepAt_ = now + options_.keepAliveMicros;
        return;
    }
    startStage(Phase::SecondClaim, now);
}

void VirtualPlayer::giveUpNumber(std::int64_t now) noexcept {
    refused_[number_] = true;
    ++numbersLost_;
    number_ = 0;
    startStage(Phase::FirstClaim, now);
}

void VirtualPlayer::startStage(Phase phase, std::int64_t now) noexcept {
    phase_ = phase;
    counter_ = 1;
    nextStepAt_ = now;
}

void VirtualPlayer::becomeJoined(std::int64_t now) noexcept {
    phase_ = Phase::Joined;
    nextKeepAliveAt_ = now;
    // A conflict lost while claiming is no reason to avoid that number next time.
    refused_.fill(false);
}

void VirtualPlayer::broadcast(const Built& packet) {
    Datagram datagram;
    datagram.broadcast = true;
    datagram.to = identity_.broadcast;
    datagram.packet = packet;
    outbox_.push_back(datagram);
}

void VirtualPlayer::sendTo(std::uint32_t to, const Built& packet) {
    Datagram datagram;
    datagram.broadcast = false;
    datagram.to = to;
    datagram.packet = packet;
    outbox_.push_back(datagram);
}

std::uint8_t VirtualPlayer::peerCount() const noexcept {
    return static_cast<std::uint8_t>(std::min<std::size_t>(devices_.size() + 1, 255));
}

void VirtualPlayer::sendStep() {
    switch (phase_) {
    case Phase::Hello:
        broadcast(buildHello(options_.name));
        break;
    case Phase::FirstClaim:
        broadcast(buildFirstClaim(options_.name, counter_, identity_.mac));
        break;
    case Phase::SecondClaim:
        broadcast(
            buildSecondClaim(options_.name, counter_, identity_.ip, identity_.mac, number_, true));
        break;
    case Phase::FinalClaim:
        broadcast(buildFinalClaim(options_.name, counter_, number_));
        break;
    default:
        return;
    }
    ++counter_;
}

void VirtualPlayer::advance(std::int64_t now) {
    // Devices that have gone quiet on every port.
    std::erase_if(devices_, [&](const Device& d) {
        const std::int64_t last =
            std::max({d.announcedAt, d.hasStatus ? d.statusAt : 0, d.heardBeat ? d.beatAt : 0});
        return now - last > options_.goneMicros;
    });
    // Who leads now, for `leader` to keep the next time more than one deck can.
    led_ = leader(now);
    switch (phase_) {
    case Phase::Listening:
        return;
    case Phase::Watching:
        if (now >= joinAt_) {
            startStage(Phase::Hello, now);
        }
        break;
    case Phase::AwaitingMixer:
        if (now >= mixerDeadline_) {
            // The mixer never assigned one: claimed as anywhere else.
            claimNext(now);
        }
        break;
    case Phase::NoNumber:
        if (now >= nextStepAt_) {
            claimNext(now);
        }
        break;
    case Phase::Joined:
        if (now >= nextKeepAliveAt_) {
            broadcast(buildKeepAlive(options_.name, number_, identity_.mac, identity_.ip,
                                     peerCount(), false));
            // On the beat of the interval, but never a burst after a stall.
            nextKeepAliveAt_ = std::max(nextKeepAliveAt_ + options_.keepAliveMicros,
                                        now + options_.keepAliveMicros / 2);
        }
        return;
    case Phase::Hello:
    case Phase::FirstClaim:
    case Phase::SecondClaim:
    case Phase::FinalClaim:
        break;
    }
    if (phase_ != Phase::Hello && phase_ != Phase::FirstClaim && phase_ != Phase::SecondClaim &&
        phase_ != Phase::FinalClaim) {
        return;
    }
    if (now < nextStepAt_) {
        return;
    }
    sendStep();
    nextStepAt_ = std::max(nextStepAt_ + options_.stepMicros, now + options_.stepMicros / 2);
    if (counter_ <= 3) {
        return;
    }
    // A stage's three packets sent: on to the next.
    switch (phase_) {
    case Phase::Hello:
        phase_ = Phase::FirstClaim;
        counter_ = 1;
        break;
    case Phase::FirstClaim:
        // The number is picked as late as it can be, from everything heard until now.
        claimNext(nextStepAt_);
        break;
    case Phase::SecondClaim:
        phase_ = Phase::FinalClaim;
        counter_ = 1;
        break;
    case Phase::FinalClaim:
        // Nobody cut it short: the number is held, and the first keep-alive is a step on, as a
        // player's is.
        becomeJoined(nextStepAt_);
        break;
    default:
        break;
    }
}

} // namespace takt4::timing::prolink
