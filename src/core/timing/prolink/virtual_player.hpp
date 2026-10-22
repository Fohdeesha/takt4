#pragma once

#include "core/timing/prolink/packets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace takt4::timing::prolink {

/// **takt4 as a player on a Pro DJ Link network** — the whole protocol, with no socket, no
/// thread and no clock of its own: datagrams in (`receive`), time moved on (`advance`), datagrams
/// out (`outbox`). `Network` puts it on sockets; a test drives it one packet at a time.
///
/// Status — which deck is playing, which is tempo master — is sent only to a device that has
/// announced itself, so takt4 announces itself as a player, the way Deep Symmetry's analysis
/// describes a "virtual CDJ":
///
///   * **Listening** sends nothing. It keeps the list of devices from their keep-alives, for the
///     operator to pick from, and that is all it does until it is asked to join.
///   * **Joining** watches for `Options::watchMicros` from when the listening began, so every
///     device's keep-alive has been heard at least twice, then claims **the first number from 7
///     to 15 that nothing is using** — clear of the four players of an older rig and the CDJ-3000's
///     5 and 6. It says hello three times, then the three stages of the claim three times each,
///     300 ms apart, as a player does. A device that already holds the number answers with a
///     conflict, and the next free one is tried; a device already on the network answers the final
///     stage with "assignment finished", which ends it there, as it ends a real player's. A mixer
///     that says it will assign a number (its channel's port) is asked for one, once a join, and
///     the number it gives taken only if it is one from 7 to 15 that nothing uses: a channel's port
///     gives that channel's number, a deck's, and an XDJ-XZ answers every claim on its network port
///     with "use that one", nought and its own decks' numbers included. Otherwise the number is
///     claimed as anywhere else.
///   * **Joined** sends a keep-alive every `Options::keepAliveMicros`, and answers a device that
///     claims takt4's number with a conflict. Two takt4s that claim one number at once part on
///     their addresses: the lower keeps it.
///
/// **Which device is followed**: one player named by the operator (`setFollow`), or "the tempo
/// master" (`leader`) — which is the master only while it leads the music. The role stays with a
/// player that has stopped: a stopped master hands it on only to a deck that is both synced and
/// playing, so a DJ who mixes by ear leaves it on the deck played first, paused, while the next
/// one plays to the room (measured: a CDJ-2000nexus kept it through both its pauses). So the
/// master — the player whose status says so, or the mixer, the most recent to take the role when
/// two say so during a hand-off — is followed while it is playing a track with a beat grid, and
/// on the air whenever a mixer is telling the players which of them are; otherwise the one player
/// that is, the one already followed staying while it still is; otherwise nobody, and the beats
/// coast. A followed device's beat packets are the beats (`heard`); every other device's are
/// not. A beat a player sends while its status says it is cueing — the cue button held, a hot cue
/// previewed while paused, a search — is the DJ's headphones, not the music, and is dropped
/// (measured on two CDJ-2000nexus players: every cue press sends one, about 12 ms after the status
/// that says so).
class VirtualPlayer {
public:
    struct Options {
        /// The name it announces, 20 ASCII characters at most.
        std::string name = "takt4";
        std::int64_t watchMicros = 4'000'000;
        /// Between the packets of the claim.
        std::int64_t stepMicros = 300'000;
        std::int64_t keepAliveMicros = 1'500'000;
        /// A device not heard from for this long has gone. Players announce every two seconds
        /// and mixers every one and a half; this rides out one lost keep-alive from a player, and
        /// two from a mixer.
        std::int64_t goneMicros = 5'000'000;
        /// A status older than this says nothing about who is master: a player sends one about
        /// five times a second.
        std::int64_t statusFreshMicros = 1'500'000;
        /// How long a mixer that said it would assign a number is waited for before claiming
        /// one as anywhere else.
        std::int64_t mixerWaitMicros = 1'000'000;
        /// The numbers it will claim, first to last.
        std::uint8_t firstNumber = 7;
        std::uint8_t lastNumber = 15;
    };

    /// Where it is on the network: its address, the network's broadcast address and its MAC, for
    /// the packets that carry them. In host order.
    struct Identity {
        std::uint32_t ip = 0;
        std::uint32_t broadcast = 0;
        Mac mac{};
    };

    enum class Phase : std::uint8_t {
        /// Listening, and saying nothing.
        Listening,
        /// Asked to join, and watching the network before claiming a number.
        Watching,
        Hello,
        FirstClaim,
        SecondClaim,
        FinalClaim,
        /// A mixer said it would assign the number; waiting for it to.
        AwaitingMixer,
        /// On the network as a player: number held, keep-alives going.
        Joined,
        /// Every number from first to last is taken. Looked at again every keep-alive.
        NoNumber,
    };

    /// Which port a datagram came to.
    enum class Channel : std::uint8_t { Announce, Beat, Status };

    /// A packet to send: a port-50000 packet to the broadcast address or to one device.
    struct Datagram {
        Channel channel = Channel::Announce;
        bool broadcast = true;
        std::uint32_t to = 0;
        Built packet;
    };

    /// One device on the network, as last heard.
    struct Device {
        std::uint8_t number = 0;
        Name name{};
        std::uint32_t ip = 0;
        Mac mac{};
        DeviceKind kind = DeviceKind::Unknown;
        /// The system's index of the card its last keep-alive came in on, or 0 where the system
        /// does not say. What `Network` joins on, when two cards are on its network.
        std::uint32_t via = 0;
        /// Its last keep-alive, its last status and its last beat, on the clock it was given.
        std::int64_t announcedAt = 0;
        bool hasStatus = false;
        std::int64_t statusAt = 0;
        StatusPacket status;
        /// When its status last went from "not master" to "master".
        std::int64_t masterSince = 0;
        std::int64_t beatAt = 0;
        bool heardBeat = false;
    };

    /// A beat of the device followed, stamped with when it arrived.
    struct Heard {
        std::int64_t at = 0;
        std::uint8_t device = 0;
        /// Pitch and all; 0 when the packet carried none.
        double bpm = 0.0;
        std::uint32_t nextBeatMs = 0;
        std::uint32_t pitch = 0;
        /// 1 to 4, or 0 when the track's beat grid gives none.
        std::uint8_t beatInBar = 0;
    };

    /// `setFollow`'s "the tempo master".
    static constexpr std::uint8_t kFollowMaster = 0;

    VirtualPlayer();
    explicit VirtualPlayer(Options options);

    /// Listening starts — or starts again, the device list forgotten — at `now`.
    void listen(std::int64_t now);
    /// Joins the network as `identity`, from `now`: the claim begins once it has listened for
    /// `Options::watchMicros`. Asked again while joining or joined, nothing changes.
    void join(const Identity& identity, std::int64_t now);
    /// Stops announcing, and goes back to listening with the devices it knows.
    void leave() noexcept;

    /// The device whose beats are followed: `kFollowMaster`, or a player's number.
    void setFollow(std::uint8_t follow) noexcept { follow_ = follow; }
    std::uint8_t follow() const noexcept { return follow_; }

    /// A datagram that came to `channel` from `from` (host order) at `at`, on the card the system
    /// numbers `via` (0 where it does not say). Its own, looped back by the network, are told apart
    /// by their address and ignored.
    void receive(Channel channel, std::span<const std::uint8_t> datagram, std::uint32_t from,
                 std::int64_t at, std::uint32_t via = 0);
    /// Time moved on to `now`: the claim's next step, a keep-alive due, a device gone.
    void advance(std::int64_t now);

    /// What to send, in order; the caller sends and clears it.
    std::vector<Datagram>& outbox() noexcept { return outbox_; }
    /// The followed device's beats since the caller last cleared it, oldest first.
    std::vector<Heard>& heard() noexcept { return heard_; }

    Phase phase() const noexcept { return phase_; }
    bool joined() const noexcept { return phase_ == Phase::Joined; }
    /// The number held, or being claimed; 0 while listening.
    std::uint8_t number() const noexcept { return number_; }
    const Identity& identity() const noexcept { return identity_; }
    /// Every device heard and not gone, in the order first heard.
    const std::vector<Device>& devices() const noexcept { return devices_; }
    /// The tempo master's number at `now`, or 0 when no fresh status says one is — playing or not.
    std::uint8_t master(std::int64_t now) const noexcept;
    /// What "the tempo master" follows at `now`: the master while it can lead (`canLead`);
    /// otherwise the one player that can, or — when more than one can — the one this last named,
    /// if it still can. 0 when none.
    std::uint8_t leader(std::int64_t now) const noexcept;
    /// Whether any player's fresh status at `now` says it is on the air: a mixer is telling them,
    /// and a fader is up.
    bool onAirTold(std::int64_t now) const noexcept;
    /// Whether `device` could lead at `now`, its status fresh: a mixer, only as the master; a
    /// player, playing — by its status, or by a beat sent since its last status — a track with a
    /// beat grid that it can lead with (status Mm 02 says it cannot), not cueing, and on the air
    /// when `onAir` (`onAirTold`) says the players are told.
    bool canLead(const Device& device, std::int64_t now, bool onAir) const noexcept;
    /// The device whose beats are followed at `now`, or 0.
    std::uint8_t followed(std::int64_t now) const noexcept;
    /// Beats dropped as a player's cueing, and conflicts that cost a number, since construction.
    std::uint64_t cueBeatsDropped() const noexcept { return cueDropped_; }
    std::uint64_t numbersLost() const noexcept { return numbersLost_; }

private:
    Device* find(std::uint8_t number) noexcept;
    const Device* find(std::uint8_t number) const noexcept;
    Device& deviceFor(std::uint8_t number, std::int64_t at);
    bool fresh(const Device& device, std::int64_t now) const noexcept {
        return device.hasStatus && now - device.statusAt <= options_.statusFreshMicros;
    }
    void announce(const AnnouncePacket& packet, std::uint32_t from, std::int64_t at,
                  std::uint32_t via);
    void status(const StatusPacket& packet, std::int64_t at);
    void beat(const BeatPacket& packet, std::int64_t at);
    /// Claims the next number nothing uses, from the first stage — or says there is none.
    void claimNext(std::int64_t now);
    bool numberUsed(std::uint8_t number, std::int64_t now) const noexcept;
    /// A number a mixer assigned can be taken: one from first to last that nothing uses.
    bool assignable(std::uint8_t number, std::int64_t now) const noexcept;
    /// The number held or being claimed is someone else's: not tried again this join, and the
    /// claim begun afresh for another.
    void giveUpNumber(std::int64_t now) noexcept;
    void startStage(Phase phase, std::int64_t now) noexcept;
    void sendStep();
    void becomeJoined(std::int64_t now) noexcept;
    /// Another device wants the number this one holds or is claiming: kept by the lower address.
    bool otherKeeps(std::uint32_t otherIp) const noexcept { return otherIp < identity_.ip; }
    void broadcast(const Built& packet);
    void sendTo(std::uint32_t to, const Built& packet);
    std::uint8_t peerCount() const noexcept;

    Options options_;
    Phase phase_ = Phase::Listening;
    Identity identity_;
    bool haveIdentity_ = false;
    std::int64_t listeningSince_ = 0;
    std::int64_t joinAt_ = 0;
    std::uint8_t number_ = 0;
    std::uint8_t counter_ = 0;
    std::int64_t nextStepAt_ = 0;
    std::int64_t nextKeepAliveAt_ = 0;
    std::int64_t mixerDeadline_ = 0;
    std::uint32_t mixerIp_ = 0;
    /// A mixer has been asked for a number since the join began: another's intention is not
    /// answered, and the claim goes on as anywhere else.
    bool mixerAsked_ = false;
    std::uint8_t follow_ = kFollowMaster;
    /// What `leader` named at the last `advance`: kept while it still can lead and another can too.
    std::uint8_t led_ = 0;

    std::vector<Device> devices_;
    /// Numbers other devices have been heard claiming, and when — avoided for a while even before
    /// their claim is finished.
    std::array<std::int64_t, 256> claimedAt_{};
    std::array<bool, 256> claimed_{};
    /// Numbers defended against this one since it last joined: not tried again.
    std::array<bool, 256> refused_{};

    std::vector<Datagram> outbox_;
    std::vector<Heard> heard_;
    std::uint64_t cueDropped_ = 0;
    std::uint64_t numbersLost_ = 0;
};

} // namespace takt4::timing::prolink
