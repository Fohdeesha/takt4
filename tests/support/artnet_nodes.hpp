#pragma once

// Several Art-Net nodes on the loopback, and what each was sent when — so a test can say which
// node had a change, and at what moment of the clock the test drives. Shared by
// tests/dmx/artnet_test.cpp and tests/output/transports_test.cpp: an Art-Net node's delay is a
// claim about *when* a frame reaches it, and one receiver cannot tell two nodes apart.
//
// REQUIRE is used inside, so this must be included from a Catch2 translation unit.

#include "core/dmx/artnet_packet.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace takt4::testing {

/// One 44 Hz frame: the most an Art-Net node's pacing can hold a change back by.
inline constexpr double kArtNetFrame = 1.0 / 44.0;

class ArtNetNodes {
public:
    explicit ArtNetNodes(std::size_t count) : heard_(count) {
        for (std::size_t i = 0; i < count; ++i) {
            nodes_.push_back(std::make_unique<LoopbackReceiver>());
        }
    }

    std::uint16_t port(std::size_t node) const { return nodes_[node]->port(); }

    /// Calls `round` once a millisecond from `fromMs` up to `untilMs`, with the time in seconds;
    /// it hands back how many datagrams it sent, and each is filed under the node it reached and
    /// the round that sent it.
    void run(const std::function<std::size_t(double)>& round, int fromMs, int untilMs) {
        for (int ms = fromMs; ms < untilMs; ++ms) {
            const double now = static_cast<double>(ms) / 1000.0;
            std::size_t left = round(now);
            int waited = 0;
            while (left > 0) {
                bool took = false;
                for (std::size_t i = 0; i < nodes_.size() && left > 0; ++i) {
                    while (left > 0 && nodes_[i]->ready(0)) {
                        const std::string datagram = nodes_[i]->receive();
                        REQUIRE(datagram.size() == dmx::kArtDmxMaxSize);
                        heard_[i].push_back({now, static_cast<std::uint8_t>(datagram[18])});
                        --left;
                        took = true;
                    }
                }
                if (!took) {
                    REQUIRE(++waited < 500); // half a second for a loopback datagram is lost
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }
        }
    }

    /// The first round at or after `after` that sent `node` channel 1 at `level`, or -1.
    double first(std::size_t node, std::uint8_t level, double after = 0.0) const {
        for (const Heard& heard : heard_[node]) {
            if (heard.at >= after && heard.channel1 == level) {
                return heard.at;
            }
        }
        return -1.0;
    }

    /// One frame a node was sent: the round that sent it, and its channel 1.
    struct Heard {
        double at = 0.0;
        std::uint8_t channel1 = 0;
    };
    /// Every frame `node` was sent, in order.
    const std::vector<Heard>& heard(std::size_t node) const { return heard_[node]; }

private:
    std::vector<std::unique_ptr<LoopbackReceiver>> nodes_;
    std::vector<std::vector<Heard>> heard_;
};

} // namespace takt4::testing
