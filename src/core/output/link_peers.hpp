#pragma once

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
/// disturb a session, and it is opened only while its list is on screen. Link's own sockets
/// share the port (both ask for address reuse), and a multicast datagram is delivered to every
/// socket joined to the group.
///
/// **This process's own Link is told apart by the port it sends from.** Link's node id is
/// random, made afresh each time it is switched on, and not in its API; but each announcement
/// leaves from a socket Link opened, and that socket belongs to this process — which the
/// operating system will say (`portOwnedHere`). Asked once per node, the first time it is heard.
class LinkPeerWatch {
public:
    /// Whether a UDP port on this machine belongs to this process. Empty is the operating
    /// system's answer, `portOwnedHere`; a test hands in its own.
    using OwnPort = std::function<bool(std::uint16_t)>;

    explicit LinkPeerWatch(OwnPort ownPort = {});
    ~LinkPeerWatch();
    LinkPeerWatch(const LinkPeerWatch&) = delete;
    LinkPeerWatch& operator=(const LinkPeerWatch&) = delete;

    /// Joins the discovery group on every IPv4 interface there is. False, with why, when it
    /// cannot — the port refused, or no interface would join. Opening an open watch is a no-op.
    bool open(std::string& problem);
    /// Leaves the group and forgets every peer.
    void close() noexcept;
    bool isOpen() const noexcept;

    /// Reads every announcement waiting, and forgets each peer whose ttl has run out. `now` is
    /// any steady clock, in seconds. Never blocks.
    void poll(double now);
    /// One announcement from `address`:`port` — what `poll` does with each datagram, and how a
    /// test feeds one without a network.
    void take(std::span<const std::uint8_t> datagram, const std::string& address,
              std::uint16_t port, double now);

    /// The peers heard from and not yet gone, other than this process, in the order first heard.
    std::vector<LinkPeer> peers() const;
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
    };
    struct Socket;

    OwnPort ownPort_;
    std::unique_ptr<Socket> socket_;
    std::vector<Node> nodes_;
};

/// Whether this process holds a UDP socket bound to `port`: Windows' table of sockets by owning
/// process, or Linux's /proc. False anywhere else, where takt4's own Link is then listed as a
/// peer of itself.
bool portOwnedHere(std::uint16_t port);

} // namespace takt4::output
