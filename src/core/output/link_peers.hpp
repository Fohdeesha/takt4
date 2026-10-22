#pragma once

#include "core/timing/link_reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace takt4::output {

/// What one of Link's discovery datagrams says about the peer that sent it.
///
/// Link finds its peers by multicasting its own state to 224.76.78.75:20808 a few times a
/// second, and its API says nothing about them but how many there are. SHOW PEERS wants more —
/// where each one is, the tempo it is at, whether it is in takt4's session — and every peer on
/// the network is already saying all of that to everyone listening. This reads it.
///
/// The format is Link's own v1 discovery message, as the vendored source writes it
/// (third_party/link: `discovery/v1/Messages.hpp`, `discovery/Payload.hpp`, and the entries in
/// `link/Timeline.hpp`, `link/SessionId.hpp` and `link/StartStopState.hpp`): the eight bytes
/// "_asdp_v\x01"; the message type, the ttl in seconds, a 16-bit group and the node's 8-byte
/// id; then entries, each a big-endian 32-bit key, a big-endian 32-bit size and that many bytes.
struct LinkAnnouncement {
    enum class Type : std::uint8_t { Alive = 1, Response = 2, ByeBye = 3 };
    using Id = std::array<std::uint8_t, 8>;

    Type type = Type::Alive;
    /// How long the peer is to be believed without another word, in seconds.
    std::uint8_t ttl = 0;
    Id node{};
    /// 'tmln': the tempo, in BPM. A bye-bye carries none.
    std::optional<double> bpm;
    /// 'sess': the session the peer is in, which is the id of the node that began it.
    std::optional<Id> session;
    /// 'stst': whether its session is playing — absent when nobody in the session shares start
    /// and stop, which Link says with an entry that was never stamped.
    std::optional<bool> playing;
};

/// Reads one datagram. False for anything that is not a well-formed Link announcement, which
/// that port can be sent by anything on the network. Never throws.
bool parseLinkAnnouncement(std::span<const std::uint8_t> datagram, LinkAnnouncement& out) noexcept;

/// One peer, as SHOW PEERS lists it.
struct LinkPeer {
    LinkAnnouncement::Id node{};
    /// Every address it has announced itself from, one per network it is on, in the order heard.
    std::vector<std::string> addresses;
    double bpm = 0.0;
    /// Whether it is in the same session as this process's own Link, following one timeline.
    /// False while this process's own Link has not been heard.
    bool sameSession = false;
    std::optional<bool> playing;
};

/// Listens to Link's discovery traffic and keeps the list of peers in it, other than this
/// process.
///
/// **Passive**: it joins the multicast group Link announces on and sends nothing, so it cannot
/// disturb a session. It is opened only while its list is on screen, and — as the
/// `timing::LinkListener` a Link follower listens through — while takt4 is waiting to hear a
/// session it may join. Link's own sockets share the port (both ask for address reuse), and a
/// multicast datagram is delivered to every socket joined to the group.
///
/// **This process's own Link is told apart by where it sends from.** Link's node id is random,
/// made afresh each time it is switched on, and not in its API; but each announcement leaves
/// from a socket Link opened on this machine, and that socket belongs to this process — which
/// the operating system will say (`sentFromHere`). Asked once per node, the first time it is
/// heard. The address as well as the port: a peer on another machine whose announcements
/// happened to leave from a port number one of takt4's sockets has here was hidden from the
/// list.
class LinkPeerWatch final : public timing::LinkListener {
public:
    /// Whether an announcement from `address`:`port` left from a socket of this process. Empty
    /// is the operating system's answer, `sentFromHere`; a test hands in its own.
    using OwnPort = std::function<bool(const std::string& address, std::uint16_t port)>;

    explicit LinkPeerWatch(OwnPort ownPort = {});
    ~LinkPeerWatch() override;
    LinkPeerWatch(const LinkPeerWatch&) = delete;
    LinkPeerWatch& operator=(const LinkPeerWatch&) = delete;

    /// Joins the discovery group on every IPv4 interface there is. False, with why, when it
    /// cannot — the port refused, or no interface would join. Opening an open watch is a no-op.
    bool open(std::string& problem) override;
    /// Leaves the group and forgets every peer.
    void close() noexcept override;
    bool isOpen() const noexcept;

    /// Reads every announcement waiting, and forgets each peer whose ttl has run out. `now` is
    /// any steady clock, in seconds. Never blocks.
    ///
    /// **And every `kRejoinSeconds` joins the group again on every interface that is up**: one
    /// that came up since — a cable plugged back in, the DJ network joined after START — is joined
    /// then, where it used to be joined only at `open`, so a follower that had listened through an
    /// outage heard loopback alone from then on. Joining one already
    /// joined is refused by the system and changes nothing.
    void poll(double now) override;
    static constexpr double kRejoinSeconds = 2.0;
    /// How many times the group has been joined again, for a test.
    std::uint64_t rejoins() const noexcept { return rejoins_; }
    /// One announcement from `address`:`port` — what `poll` does with each datagram, and how a
    /// test feeds one without a network.
    void take(std::span<const std::uint8_t> datagram, const std::string& address,
              std::uint16_t port, double now);

    /// The peers heard from and not yet gone, other than this process, in the order first heard.
    std::vector<LinkPeer> peers() const;
    /// The same, as a follower asks: when each was first and last heard, and its tempo.
    std::vector<timing::LinkListener::Heard> heard() const override;
    /// This process's own Link's session, once one of its announcements has been heard.
    std::optional<LinkAnnouncement::Id> ownSession() const;

private:
    struct Node {
        LinkAnnouncement::Id id{};
        bool own = false;
        std::vector<std::string> addresses;
        double bpm = 0.0;
        std::optional<LinkAnnouncement::Id> session;
        std::optional<bool> playing;
        double expires = 0.0;
        /// When it was first heard and when last, on `poll`'s clock.
        double firstHeard = 0.0;
        double lastHeard = 0.0;
    };
    struct Socket;

    OwnPort ownPort_;
    std::unique_ptr<Socket> socket_;
    std::vector<Node> nodes_;
    /// When the group was last joined on every interface, on `poll`'s clock; negative until the
    /// first poll after `open`, which joined it.
    double joinedAt_ = -1.0;
    std::uint64_t rejoins_ = 0;
};

/// Whether this process holds a UDP socket bound to `port`: Windows' table of sockets by owning
/// process, or Linux's /proc. False anywhere else, where takt4's own Link is then listed as a
/// peer of itself.
bool portOwnedHere(std::uint16_t port);

/// Whether `address` (dotted IPv4) is this machine's: loopback, or an interface that is up.
bool addressIsHere(const std::string& address);

/// Whether a datagram from `address`:`port` left from a socket of this process: the address is
/// this machine's (`addressIsHere`) and the port this process's (`portOwnedHere`).
bool sentFromHere(const std::string& address, std::uint16_t port);

} // namespace takt4::output
