#pragma once

#include "core/rt/spsc_ring.hpp"
#include "core/timing/prolink/packets.hpp"
#include "core/timing/prolink/virtual_player.hpp"
#include "core/timing/prolink_follower.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace takt4::timing::prolink {

/// One of this machine's IPv4 network cards: what takt4 announces itself as being at.
struct Interface {
    /// The name the system gives it — "Ethernet 2", "en5" — for the screen.
    std::string name;
    std::uint32_t ip = 0;
    std::uint32_t mask = 0;
    std::uint32_t broadcast = 0;
    Mac mac{};
};

/// This machine's IPv4 cards that are up. The loopback is among them — no device on a network
/// is ever on it, so it is joined only when asked for by its address, which is how the console
/// is tried against a recording played on this machine.
std::vector<Interface> listInterfaces();

/// The card whose network `address` is on, in host order — where a device heard at `address` can
/// be answered from. Nothing when no card is on that network.
std::optional<Interface> interfaceFor(const std::vector<Interface>& interfaces,
                                      std::uint32_t address);

/// "192.168.1.5", from host order.
std::string addressText(std::uint32_t address);
/// The other way, or nothing for anything that is not a dotted IPv4 address.
std::optional<std::uint32_t> parseAddress(const std::string& text);

/// **The Pro DJ Link network**, on sockets: a `VirtualPlayer` given a thread of its own, three
/// ports and a clock.
///
/// * `open` binds the announcement port (50000) and listens: the devices on the network are
///   listed (`snapshot`), and nothing is sent. Opening it is what first makes a firewall ask.
/// * `join` binds the beat port (50001) and the status port (50002), on the card the devices
///   were heard on — the first device heard picks it, unless `Config::interfaceAddress` names one
///   — and the player claims its number and starts its keep-alives. `leave` stops them and lets
///   the two ports go. Both take effect on the thread's next round, a few milliseconds on.
/// * Every datagram is stamped on `Config::clock` (Link's) the moment it is read, and the
///   followed device's beats go to the one consumer of `popBeat` — `ProLinkFollower`, on the
///   timing worker — through a lock-free ring.
///
/// Everything sent goes from the status socket, which is bound to the card's own address, so a
/// broadcast leaves by that card and no other.
///
/// rekordbox on the same machine holds these ports, and then `open` says so. The test binaries'
/// sandbox lets none of them be bound but those a test was given (`sandbox.hpp`).
class Network final : public CdjFeed {
public:
    struct Ports {
        std::uint16_t announce = kAnnouncePort;
        std::uint16_t beat = kBeatPort;
        std::uint16_t status = kStatusPort;
    };

    struct Config {
        /// Now, in microseconds — Link's clock, which every beat in takt4 is stamped on.
        std::function<std::int64_t()> clock;
        /// The ports bound here, and the port every packet sent goes to: a device listens for
        /// its announcements on 50000. Other than Pro DJ Link's own only in a test.
        Ports own;
        std::uint16_t peerAnnounce = kAnnouncePort;
        /// The card to join on, by its address; otherwise the one the first device heard is on.
        std::optional<std::uint32_t> interfaceAddress;
        /// This machine's cards — a test's own, in place of `listInterfaces`.
        std::function<std::vector<Interface>()> interfaces;
        VirtualPlayer::Options player;
        /// How long the thread waits for a datagram before it looks at the time.
        int roundMillis = 5;
    };

    /// One device, for the screen.
    struct DeviceView {
        std::uint8_t number = 0;
        std::string name;
        DeviceKind kind = DeviceKind::Unknown;
        std::uint32_t ip = 0;
        bool hasStatus = false;
        bool playing = false;
        bool master = false;
        bool synced = false;
        bool onAir = false;
        bool cueing = false;
        /// Master, but with a track that cannot lead — it sends no beats (status Mm 02).
        bool masterWithoutBeats = false;
        double bpm = 0.0;
        /// A beat heard from it in the last two seconds.
        bool beating = false;
    };

    struct Snapshot {
        bool open = false;
        /// Why the network cannot be listened to or joined, in words, or empty.
        std::string problem;
        VirtualPlayer::Phase phase = VirtualPlayer::Phase::Listening;
        /// The number held or being claimed; 0 while listening.
        std::uint8_t number = 0;
        std::uint8_t master = 0;
        std::uint8_t followed = 0;
        std::uint8_t follow = VirtualPlayer::kFollowMaster;
        std::vector<DeviceView> devices;
        /// The card joined on, once there is one.
        std::string interfaceName;
        std::uint32_t interfaceIp = 0;
        std::uint64_t beatsHeard = 0;
        std::uint64_t beatsDropped = 0;
        std::uint64_t cueBeatsDropped = 0;
        std::uint64_t numbersLost = 0;
        std::uint64_t datagramsRead = 0;
        std::uint64_t sendsFailed = 0;
    };

    /// Beats the follower may fall behind by: a minute of them, where it takes them every
    /// millisecond.
    static constexpr std::size_t kBeatCapacity = 256;

    explicit Network(Config config);
    ~Network() override;

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    /// Listens: binds the announcement port and starts the thread. False, with why, when the
    /// port cannot be had. Safe to call again while open.
    bool open(std::string& problem);
    /// Stops everything: the thread, the ports, any keep-alives. Safe to call twice.
    void close() noexcept;
    bool isOpen() const noexcept { return thread_.joinable(); }

    /// `VirtualPlayer::setFollow`, from any thread: at once, running or not.
    void setFollow(std::uint8_t follow) noexcept {
        follow_.store(follow, std::memory_order_relaxed);
    }
    /// The card to join on next, by address, or nothing for the first device's. While left.
    void setInterface(std::optional<std::uint32_t> address);

    /// The network as the thread last saw it. Any thread.
    Snapshot snapshot() const;

    // --- CdjFeed ---------------------------------------------------------------------------
    std::int64_t now() const noexcept override { return config_.clock(); }
    bool popBeat(VirtualPlayer::Heard& out) noexcept override { return beats_.tryPop(out); }
    void join() noexcept override { wantJoined_.store(true, std::memory_order_release); }
    void leave() noexcept override { wantJoined_.store(false, std::memory_order_release); }
    bool wantsJoined() const noexcept { return wantJoined_.load(std::memory_order_acquire); }

private:
    struct Sockets;

    void run() noexcept;
    /// One round of the thread: read what has come, follow the join wanted, move the player on,
    /// send what it says to.
    void round();
    void readAll(int which, std::int64_t& now);
    bool openJoined(std::int64_t now);
    void closeJoined() noexcept;
    void sendOutbox();
    void publish(std::int64_t now);
    void setProblem(std::string problem);

    Config config_;
    std::unique_ptr<Sockets> sockets_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> wantJoined_{false};
    std::atomic<std::uint8_t> follow_{VirtualPlayer::kFollowMaster};
    rt::SpscRing<VirtualPlayer::Heard, kBeatCapacity> beats_;

    // The thread's alone while it runs.
    VirtualPlayer player_;
    std::optional<Interface> interface_;
    bool joinedSockets_ = false;
    std::uint64_t beatsHeard_ = 0;
    std::uint64_t beatsDropped_ = 0;
    std::uint64_t datagramsRead_ = 0;
    std::uint64_t sendsFailed_ = 0;
    std::int64_t publishedAt_ = 0;
    /// When a join that could not be made is next tried, and whether one could not be.
    std::int64_t retryAt_ = 0;
    bool joinFailed_ = false;

    mutable std::mutex mutex_;
    Snapshot snapshot_;
    std::optional<std::uint32_t> interfaceWanted_;
};

} // namespace takt4::timing::prolink
