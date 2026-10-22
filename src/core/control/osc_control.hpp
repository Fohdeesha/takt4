#pragma once

#include "core/control/control_action.hpp"
#include "core/control/osc_parse.hpp"
#include "core/control/osc_receiver.hpp"
#include "core/engine/beat_source.hpp"
#include "core/remote/remote.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace takt4::control {

/// the inbound OSC: "so the app is operable from a Stream Deck or Bitfocus
/// Companion without touching the laptop".
///
/// A thread with a listening socket, an address table, and `engine::BeatSource::post` at the
/// end of it. It reaches the beats exactly the way the window's buttons and the console's
/// keys do, and for the same reason: many producers, one consumer, applied between frames.
///
/// **What it does not carry yet**, and why: `/ctl/preset <name|index>` waits for presets to
/// be files. the portable layer has a shape (`settings::Preset`) but nothing names or
/// stores one yet, so a stub would be inventing a meaning for it.
///
/// `/ctl/panic` and `/ctl/rule/<id>/enable` reach the *rules* rather than the tracker, which
/// live behind `output::OutputRunner` on the own thread — so they need a `RuleControl` as
/// well as a beat source, and are refused where nothing supplies one.
///
/// `/ctl/lock <0|1>` **pins** the lock rather than setting it, because a `setLocked(true)`
/// the hysteresis unwinds 75 frames later would not be a lock at all. The argument is
/// required and is not a toggle; `TempoTracker::setLockPinned` carries the reasoning.
class OscControl final : public remote::Replies {
public:
    struct Config {
        /// Off unless an operator asks for it. A listening socket is not something to
        /// open on somebody's behalf.
        bool enabled = false;
        std::uint16_t port = 7001;
        /// The namespace the design addresses hang off: `<prefix>/ctl/tap` and the rest.
        std::string prefix = "/takt4";
        /// Loopback only unless asked otherwise. OSC has no authentication and this does
        /// not invent any, so opening the socket to the network is the operator's call —
        /// the same call they make when they put a Stream Deck on that network.
        bool localOnly = true;
    };

    /// The beat source must outlive this. Nothing listens until `start()`.
    ///
    /// `rules` is where the `panic` and `rule/<id>/enable` go — `output::OutputRunner` in
    /// an app, null where there is none, in which case those two addresses are answered
    /// "not understood" rather than accepted and dropped. It must outlive this where it is
    /// not null; `setRuleControl` exists because an owner usually builds the runner second.
    OscControl(engine::BeatSource& beats, Config config, RuleControl* rules = nullptr);
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

    /// Points it at a different port, or opens it to the network. **Stops first**, because
    /// every field here is decided when the socket is bound and none of them can be changed
    /// under a bound one; the caller starts it again.
    ///
    /// A setter rather than building a new one for the reason `MidiControl::setPort` has:
    /// this holds a mutex and a thread, so it is neither copyable nor movable, and an owner
    /// that keeps one as a member cannot replace it.
    void setConfig(Config config);

    /// Where the two rule addresses go. See the constructor.
    void setRuleControl(RuleControl* rules) noexcept { surface_.setRuleControl(rules); }

    /// The port actually being listened on, which is `config().port` unless that was 0 —
    /// "any free one" — and 0 when nothing is listening. A UI has to show this rather than
    /// the request, or an operator points their Stream Deck at a port nobody is on.
    std::uint16_t port() const noexcept { return receiver_ ? receiver_->port() : 0; }

    /// Messages that matched an address and were acted on.
    std::uint64_t handled() const noexcept { return handled_.load(std::memory_order_relaxed); }
    /// Datagrams that were not the design messages, or were addressed to nothing this knows.
    /// Worth showing: a control surface pointed at the wrong prefix looks exactly like a
    /// broken one until somebody can see that the packets are arriving.
    std::uint64_t ignored() const noexcept { return ignored_.load(std::memory_order_relaxed); }

    /// The last address acted on and where it came from, for a UI and for learn mode.
    std::string lastMessage() const;

    /// Acts on one address, as if it had arrived — the way a test drives this without a
    /// socket. `argument` is the `<0|1>` where an address takes one.
    bool dispatch(std::string_view address, std::optional<double> argument);
    /// A remote status or control message, as if it had arrived from `from` — the way a test
    /// drives a sender on another machine, which no test's sender can be. True when it was one.
    bool dispatchRemote(std::span<const std::byte> packet, const remote::Endpoint& from);

    // --- remote status and control (a show controller's, 2026-10-22) -------------------------------

    /// Where queries are answered from, and where commands go for the window to carry out. Both
    /// must outlive this; set before `start`. Without them, the remote addresses are refused.
    void setRemote(const remote::StatusBoard* board, remote::CommandQueue* commands) noexcept {
        board_ = board;
        commands_ = commands;
    }
    /// **Who may change takt4's set-up from the network** — START, STOP, the input, the source
    /// and the outputs (the operator's answer of 2026-10-22). Off unless the operator ticks
    /// "allow remote setup", which is a second step after "listen" and, from another machine,
    /// "allow other machines". With addresses in `from` (IPv4, host order), only those; empty, any
    /// that reaches the socket — and this machine (127.x.x.x) whatever the list says: what runs
    /// on it can change the settings file anyway. The queries — status, the devices, the outputs —
    /// need none of it: they change nothing. Any thread.
    struct RemoteSetup {
        bool allowed = false;
        std::vector<std::uint32_t> from;
    };
    void setRemoteSetup(RemoteSetup setup);
    RemoteSetup remoteSetup() const;

    /// The verdict on a command, to whoever sent it — `remote::Replies`. Any thread; the window
    /// calls it as it carries a command out.
    void answer(const remote::Command& command, bool ok, std::string_view note) override;

    /// Queries answered, commands handed to the window, and remote messages refused here — the
    /// switch off, a sender not on the list, a window not taking changes, a message misspelt.
    std::uint64_t queriesAnswered() const noexcept {
        return queriesAnswered_.load(std::memory_order_relaxed);
    }
    std::uint64_t commandsQueued() const noexcept {
        return commandsQueued_.load(std::memory_order_relaxed);
    }
    std::uint64_t remoteRefused() const noexcept {
        return remoteRefused_.load(std::memory_order_relaxed);
    }
    /// Replies not sent because the sender was asking for more than `kRepliesPerSecond`.
    std::uint64_t repliesHeld() const noexcept {
        return repliesHeld_.load(std::memory_order_relaxed);
    }

    /// How many replies a second takt4 sends, all senders together: a query is answered with
    /// a datagram per device or per output, and a sender could otherwise have takt4 send far more
    /// than it was sent — at an address it never was (UDP's source can be forged).
    static constexpr double kRepliesPerSecond = 200.0;
    /// How long since the window last wrote the status before a command is refused as "busy": a
    /// modal dialog stops the window's redraws, and a command would wait behind it unanswered.
    static constexpr double kWindowAwaySeconds = 2.0;

private:
    void run() noexcept;
    /// One message — a datagram, or one of a bundle's — acted on and counted.
    void handle(std::span<const std::byte> packet) noexcept;
    /// A remote status or control address, answered: true when `message` was one of them.
    bool handleRemote(const OscView& message, const remote::Endpoint& from);
    /// One reply datagram to `to`, within `kRepliesPerSecond`.
    bool reply(const remote::Endpoint& to, std::span<const std::byte> packet) noexcept;
    /// `command` refused here, before the window, and said to whoever sent it.
    void refuse(const remote::Command& command, std::string_view why);

    Config config_;
    /// This surface's own route in, including its own tap set — the taps arriving over the
    /// wire are not the window's or a MIDI pad's. See `ControlSurface`.
    ControlSurface surface_;
    std::unique_ptr<OscReceiver> receiver_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> handled_{0};
    std::atomic<std::uint64_t> ignored_{0};

    mutable std::mutex lastMutex_;
    std::string last_;

    const remote::StatusBoard* board_ = nullptr;
    remote::CommandQueue* commands_ = nullptr;
    mutable std::mutex setupMutex_;
    RemoteSetup setup_;
    std::atomic<std::uint64_t> queriesAnswered_{0};
    std::atomic<std::uint64_t> commandsQueued_{0};
    std::atomic<std::uint64_t> remoteRefused_{0};
    std::atomic<std::uint64_t> repliesHeld_{0};
    /// The reply allowance (`kRepliesPerSecond`): tokens, and when they were last topped up.
    std::mutex replyMutex_;
    double replyTokens_ = kRepliesPerSecond;
    std::chrono::steady_clock::time_point replyRefilled_{};
};

} // namespace takt4::control
