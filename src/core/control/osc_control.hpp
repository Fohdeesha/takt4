#pragma once

#include "core/control/osc_receiver.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/tracking/tap_tempo.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace takt4::control {

/// HANDOFF §5.7's inbound OSC: "so the app is operable from a Stream Deck or Bitfocus
/// Companion without touching the laptop".
///
/// A thread with a listening socket, an address table, and `BeatEngine::post` at the end
/// of it. It reaches the tracker exactly the way the window's buttons and the console's
/// keys do, and for the same reason: many producers, one consumer, applied between frames.
///
/// **What it does not carry yet**, and why:
///
///   * `/ctl/panic` and `/ctl/rule/<id>/enable` are Phase 6's. §5.7 defines panic as
///     "halt all rules immediately", and there are no rules; a stub would be inventing a
///     meaning for it.
///   * `/ctl/preset <name|index>` waits for presets to be files. Q7's portable layer has
///     a shape (`settings::Preset`) but nothing names or stores one yet.
///
/// `/ctl/lock <0|1>` **pins** the lock rather than setting it, because a `setLocked(true)`
/// the hysteresis unwinds 75 frames later would not be a lock at all. The argument is
/// required and is not a toggle; `TempoTracker::setLockPinned` carries the reasoning.
class OscControl {
public:
    struct Config {
        /// Off unless an operator asks for it. A listening socket is not something to
        /// open on somebody's behalf.
        bool enabled = false;
        std::uint16_t port = 7001;
        /// The namespace §5.7 addresses hang off: `<prefix>/ctl/tap` and the rest.
        std::string prefix = "/takt4";
        /// Loopback only unless asked otherwise. OSC has no authentication and this does
        /// not invent any, so opening the socket to the network is the operator's call —
        /// the same call they make when they put a Stream Deck on that network.
        bool localOnly = true;
    };

    /// The engine must outlive this. Nothing listens until `start()`.
    OscControl(engine::BeatEngine& engine, Config config);
    ~OscControl();

    OscControl(const OscControl&) = delete;
    OscControl& operator=(const OscControl&) = delete;

    /// Opens the socket and starts the thread. Throws `std::runtime_error` when the port
    /// is taken — an operator has to be told, because a control surface that silently
    /// does nothing is worse than one that will not start. Does nothing when the config
    /// says disabled, and nothing when already running.
    void start();
    void stop() noexcept;
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    const Config& config() const noexcept { return config_; }

    /// The port actually being listened on, which is `config().port` unless that was 0 —
    /// "any free one" — and 0 when nothing is listening. A UI has to show this rather than
    /// the request, or an operator points their Stream Deck at a port nobody is on.
    std::uint16_t port() const noexcept { return receiver_ ? receiver_->port() : 0; }

    /// Messages that matched an address and were acted on.
    std::uint64_t handled() const noexcept { return handled_.load(std::memory_order_relaxed); }
    /// Datagrams that were not §5.7 messages, or were addressed to nothing this knows.
    /// Worth showing: a control surface pointed at the wrong prefix looks exactly like a
    /// broken one until somebody can see that the packets are arriving.
    std::uint64_t ignored() const noexcept { return ignored_.load(std::memory_order_relaxed); }

    /// The last address acted on and where it came from, for a UI and for learn mode.
    std::string lastMessage() const;

    /// Acts on one address, as if it had arrived. The way a test drives this without a
    /// socket, and the seam a MIDI binding will use when learn mode arrives.
    /// `argument` is §5.7's `<0|1>` where an address takes one.
    bool dispatch(std::string_view address, std::optional<double> argument);

private:
    void run() noexcept;

    engine::BeatEngine& engine_;
    Config config_;
    std::unique_ptr<OscReceiver> receiver_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> handled_{0};
    std::atomic<std::uint64_t> ignored_{0};

    /// The taps arriving over the wire, which are their own set: a Stream Deck's tap
    /// button and the window's TAP are two surfaces, and interleaving them into one
    /// tempo would give an operator using both a tempo neither of them meant.
    tracking::TapTempo taps_;
    std::chrono::steady_clock::time_point started_{};

    mutable std::mutex lastMutex_;
    std::string last_;
};

} // namespace takt4::control
