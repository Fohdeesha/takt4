#pragma once

// Pro DJ Link devices simulated on the loopback for a test: a UDP socket of the test's own
// (`UdpPeer`), and a player and a mixer at 127.0.0.2 announcing themselves to a
// `timing::prolink::Network` under test and — once playing — sending its status and beats at
// 128 BPM (`SimulatedRig`). takt4 is at 127.0.0.1. The sandbox lets takt4 bind only ports a test's
// own receiver was given (`allowedPort`) and send only to such ports on the loopback, so the
// whole network runs in every suite.

#include "core/net/udp.hpp"
#include "core/sandbox.hpp"
#include "core/timing/prolink/network.hpp"
#include "core/timing/prolink/packets.hpp"

#include "support/loopback_receiver.hpp"
#include "support/prolink_capture.hpp"
#include "support/prolink_sim.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace takt4::testing::prolink_peer {

using namespace std::chrono_literals;
using timing::prolink::Interface;
using timing::prolink::Mac;
using timing::prolink::Network;
namespace sim = prolink_sim;

inline constexpr std::uint32_t kHere = ipv4(127, 0, 0, 1);
inline constexpr std::uint32_t kDevices = ipv4(127, 0, 0, 2);

inline std::int64_t steadyMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// A loopback port the sandbox lets this process bind and send to — one a receiver was given,
/// let go again for takt4 to take.
inline std::uint16_t allowedPort() {
    const LoopbackReceiver probe;
    return probe.port();
}

/// A UDP socket of the test's own, at `ip`: what a simulated device sends from and hears on.
class UdpPeer {
public:
    explicit UdpPeer(std::uint32_t ip, std::uint16_t port = 0) {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(socket_ != takt4::net::kInvalidSocket);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(ip);
        address.sin_port = htons(port);
        socklen_t length = sizeof address;
        REQUIRE(::bind(socket_, reinterpret_cast<const sockaddr*>(&address), length) == 0);
        REQUIRE(::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        takt4::sandbox::allowPort(port_);
    }
    ~UdpPeer() { takt4::net::closeSocket(socket_); }
    UdpPeer(const UdpPeer&) = delete;
    UdpPeer& operator=(const UdpPeer&) = delete;

    std::uint16_t port() const { return port_; }

    void sendTo(std::uint32_t ip, std::uint16_t port, const std::vector<std::uint8_t>& bytes) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(ip);
        to.sin_port = htons(port);
#if defined(_WIN32)
        const int length = static_cast<int>(bytes.size());
#else
        const std::size_t length = bytes.size();
#endif
        ::sendto(socket_, reinterpret_cast<const char*>(bytes.data()), length, 0,
                 reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }

    /// The next datagram to arrive within `millis`, with its sender.
    std::optional<std::vector<std::uint8_t>> receive(int millis, std::uint32_t* from = nullptr) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket_, &set);
        timeval wait{};
        wait.tv_sec = millis / 1000;
        wait.tv_usec = (millis % 1000) * 1000;
#if defined(_WIN32)
        const int ready = ::select(0, &set, nullptr, nullptr, &wait);
#else
        const int ready = ::select(socket_ + 1, &set, nullptr, nullptr, &wait);
#endif
        if (ready <= 0) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> buffer(2048);
        sockaddr_in sender{};
        socklen_t senderLength = sizeof sender;
#if defined(_WIN32)
        const int capacity = static_cast<int>(buffer.size());
#else
        const std::size_t capacity = buffer.size();
#endif
        const auto got = ::recvfrom(socket_, reinterpret_cast<char*>(buffer.data()), capacity, 0,
                                    reinterpret_cast<sockaddr*>(&sender), &senderLength);
        if (got <= 0) {
            return std::nullopt;
        }
        if (from != nullptr) {
            *from = ntohl(sender.sin_addr.s_addr);
        }
        buffer.resize(static_cast<std::size_t>(got));
        return buffer;
    }

private:
    takt4::net::WinsockGuard winsock_;
    takt4::net::Socket socket_ = takt4::net::kInvalidSocket;
    std::uint16_t port_ = 0;
};

inline bool eventually(const std::function<bool()>& done, std::chrono::milliseconds within) {
    const auto until = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < until) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return done();
}

/// The machine's one card, as far as the network under test knows: the loopback, whose
/// "broadcast" reaches the simulated devices.
inline std::vector<Interface> loopbackCard() {
    Interface card;
    card.name = "loopback";
    card.ip = kHere;
    card.mask = 0xff000000u;
    card.broadcast = kDevices;
    card.mac = Mac{0x02, 0, 0, 0, 0, 1};
    return {card};
}

/// The ports takt4 binds, and its virtual player's claim at test speed.
inline Network::Config testConfig(std::uint16_t peerAnnounce, std::function<std::int64_t()> clock) {
    Network::Config config;
    config.clock = std::move(clock);
    config.own.announce = allowedPort();
    config.own.beat = allowedPort();
    config.own.status = allowedPort();
    config.peerAnnounce = peerAnnounce;
    config.interfaces = loopbackCard;
    config.player.watchMicros = 200'000;
    config.player.stepMicros = 20'000;
    config.player.keepAliveMicros = 100'000;
    return config;
}

/// A player and a mixer at 127.0.0.2, announcing themselves to `network`'s port on their own
/// thread while it runs, and — once `playing` — the player's status and beats at 128 BPM.
class SimulatedRig {
public:
    SimulatedRig() : announce_(kDevices) {}
    ~SimulatedRig() { stop(); }

    std::uint16_t announcePort() const { return announce_.port(); }
    UdpPeer& announceSocket() { return announce_; }

    void start(const Network::Config& config) {
        ports_ = config.own;
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    void setPlaying(bool playing) { playing_ = playing; }
    /// When each beat was sent, on the steady clock.
    std::vector<std::int64_t> beatsSent() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return beatsSent_;
    }

private:
    void run() {
        UdpPeer out(kDevices);
        const Mac playerMac{0x74, 0x5e, 0x1c, 0, 0, 2};
        const Mac mixerMac{0x00, 0xe0, 0x36, 0, 0, 33};
        const double bpm = 128.0;
        const auto period = std::chrono::microseconds(std::llround(60e6 / bpm));
        auto nextAnnounce = std::chrono::steady_clock::now();
        auto nextStatus = nextAnnounce;
        auto nextBeat = nextAnnounce;
        int beat = 0;
        while (running_) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextAnnounce) {
                out.sendTo(kHere, ports_.announce, sim::keepAlive(2, kDevices, playerMac, 1, 3));
                out.sendTo(kHere, ports_.announce,
                           sim::keepAlive(33, kDevices, mixerMac, 2, 3, "DJM-900NXS2"));
                nextAnnounce += 100ms;
            }
            if (now >= nextStatus) {
                sim::PlayerState state;
                if (playing_) {
                    state.flags = sim::kPlaying | sim::kMaster;
                    state.playMode = sim::kModePlaying;
                }
                state.bpm = bpm;
                out.sendTo(kHere, ports_.status, sim::playerStatus(2, state));
                out.sendTo(kHere, ports_.status, sim::mixerStatus(33, false, bpm));
                nextStatus += 50ms;
            }
            if (playing_ && now >= nextBeat) {
                const auto bar = static_cast<std::uint8_t>(beat % 4 + 1);
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    beatsSent_.push_back(steadyMicros());
                }
                out.sendTo(kHere, ports_.beat, sim::beat(2, bpm, bar));
                ++beat;
                nextBeat += period;
            } else if (!playing_) {
                nextBeat = now;
            }
            // A beat on time to well under a millisecond, as a CDJ's are: a sleep can take a
            // whole system tick, so the last few milliseconds before one are spun.
            const auto next =
                std::min({nextAnnounce, nextStatus, playing_ ? nextBeat : nextStatus});
            if (next - std::chrono::steady_clock::now() > 3ms) {
                std::this_thread::sleep_for(1ms);
            } else {
                std::this_thread::yield();
            }
        }
    }

    UdpPeer announce_;
    Network::Ports ports_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::mutex mutex_;
    std::vector<std::int64_t> beatsSent_;
};

inline bool canUseSecondLoopbackAddress() {
    return LoopbackReceiver::canBind(kDevices);
}

} // namespace takt4::testing::prolink_peer
