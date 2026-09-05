#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace takt4::control {

/// A UDP socket listening for §5.7's control messages.
///
/// The mirror of `output::OscSender`: a datagram socket and nothing more, no library and
/// no event loop. It blocks with a timeout rather than spinning, so the thread that owns
/// it costs nothing while nobody is sending — which matters, because unlike the output
/// thread this one has no work of its own to do between messages.
///
/// **Bound to the loopback or to everything, and that is a real choice.** Binding every
/// interface is what makes a Stream Deck on the same network able to reach it, and is
/// also what puts a socket that halves the tempo on that network. There is no
/// authentication in OSC and this does not invent any; `Config::localOnly` is the honest
/// control, and it defaults to true.
class OscReceiver {
public:
    /// Bigger than anything §5.7 sends and small enough to sit on the stack of the thread
    /// draining it. A datagram longer than this is read up to here and refused by the
    /// parser rather than reassembled.
    static constexpr std::size_t kMaxDatagram = 2048;

    /// Binds `port`. Throws `std::runtime_error` when the port is already taken, which is
    /// the failure an operator has to be told about: a control surface that silently does
    /// nothing is worse than one that will not start.
    OscReceiver(std::uint16_t port, bool localOnly);
    ~OscReceiver();

    OscReceiver(const OscReceiver&) = delete;
    OscReceiver& operator=(const OscReceiver&) = delete;

    /// Waits up to `timeout` for one datagram. Empty when nothing arrived, or when the
    /// socket reported an error — a receiver that has been closed under a waiting thread
    /// looks the same as a quiet one, which is what lets `stop()` be a flag and a close.
    ///
    /// The buffer is reused, so what comes back is valid until the next call.
    std::span<const std::byte> receive(std::chrono::milliseconds timeout) noexcept;

    std::uint16_t port() const noexcept { return port_; }
    /// Where the last datagram came from, as "host:port". For a UI, and for learn mode to
    /// say which box is talking.
    const std::string& lastSender() const noexcept { return lastSender_; }
    std::uint64_t datagrams() const noexcept { return datagrams_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint16_t port_ = 0;
    std::string lastSender_;
    std::uint64_t datagrams_ = 0;
};

} // namespace takt4::control
