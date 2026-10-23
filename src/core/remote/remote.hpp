#pragma once

// Remote status and control, for a show controller on the network — a show controller, which
// hears takt4's global messages and fires cues on its beat (the operator's wish of 2026-10-19, and
// the answers of 2026-10-22).
//
// What it is made of, and on which thread each part lives:
//
//   * the **status**, a snapshot the window writes as it redraws (`StatusBoard`): what is running,
//     the lock, the source, the input, the outputs. The output thread sends it once a second with
//     the heartbeat, and the control socket answers queries from it — neither reads a window field;
//   * the **commands**, which change what the window's own controls change and so are carried out
//     by the window, between its redraws, through the functions its buttons call
//     (`CommandQueue`); and the verdict on each, sent back to whoever asked (`Replies`).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::remote {

/// Where a datagram came from, or goes: an IPv4 address and port, in host order.
struct Endpoint {
    std::uint32_t ip = 0;
    std::uint16_t port = 0;
    bool operator==(const Endpoint&) const = default;
    /// "192.168.1.60:9475".
    std::string text() const;
};

/// One output row, as `/takt4/ctl/outputs` lists it.
struct OutputRow {
    /// `output::OutputTarget::id`, which a command names the row by: never its name, which the
    /// operator may type twice or change.
    std::string id;
    std::string name;
    /// As the settings file spells it: "link", "osc", "midi", "artnet", "midiclock".
    std::string kind;
    /// The host, or the MIDI device; empty for Link.
    std::string host;
    int port = 0;
    bool enabled = false;
    double delayMs = 0.0;
    /// Ticked for takt4's own messages — the global ones, and these.
    bool global = false;
    /// Why it is reaching nothing, as its row says it in the window — a host that will not
    /// resolve, a MIDI device another program holds — or empty.
    std::string problem;
    bool operator==(const OutputRow&) const = default;
};

/// One audio input, as `/takt4/ctl/audio/devices` lists it.
struct DeviceRow {
    std::string host;
    std::string device;
    int inputs = 0;
    bool operator==(const DeviceRow&) const = default;
};

/// What takt4 says about itself. **What is in force, never what was asked for**: a device change
/// refused, or one that failed, leaves this saying the device that is open.
struct Status {
    /// START pressed and the run going: the audio's, or a source followed.
    bool running = false;
    bool locked = false;
    /// Running, and no lock: the tracker searching, or the source not there yet.
    bool hunting = false;
    /// The word the window's tempo panel says the state in: "stopped", "hunting", "locked",
    /// "holding", "no signal", "no audio"; following, "following", "coasting", "waiting" or what
    /// the source is waiting for.
    std::string state;
    double confidence = 0.0;
    double bpm = 0.0;
    /// The input meter's level, in dBFS; `kSilentDb` for none.
    double levelDb = -120.0;
    /// The worst audio callback of the last second, as a share of its buffer's time: 1 left the
    /// driver no time to spare. 0 with no audio open.
    double load = 0.0;
    /// Audio lost since START: the interface's overflows and the driver's resyncs, and the hops,
    /// frames and beats the engine dropped behind it.
    std::uint64_t dropouts = 0;
    /// Where the beats come from, as the settings file spells it: "audio", "link", "midi-clock",
    /// "prolink".
    std::string source = "audio";
    /// The source picked is giving beats: Link peers heard, a MIDI clock ticking, a CDJ followed,
    /// the audio locked.
    bool sourceOk = false;
    /// What the window says of it: "3 peers · 4 beats to the bar", "DJM-A9 · no clock".
    std::string sourceDetail;
    bool panic = false;
    std::string audioHost;
    std::string audioDevice;
    /// The input as the window counts them, from 1; the first of a pair in stereo.
    int audioChannel = 0;
    bool audioMono = false;
    int audioRate = 0;
    /// The latency in force: the audio's, or the followed source's own.
    double latencyMs = 0.0;
    std::string version;
    /// How long takt4 has been running, in whole seconds.
    std::int64_t uptimeSeconds = 0;
    std::vector<OutputRow> outputs;
    std::vector<DeviceRow> devices;
    bool operator==(const Status&) const = default;

    static constexpr double kSilentDb = -120.0;
};

/// The window's latest `Status`, for the threads that send it. A lock held only for a copy.
class StatusBoard {
public:
    using Clock = std::chrono::steady_clock;

    /// The window's, as it redraws.
    void publish(Status status, Clock::time_point at = Clock::now());
    /// The latest, and when the window wrote it — unset before it ever has.
    Status read(Clock::time_point* at = nullptr) const;
    /// The same, **without waiting**: false while the window holds the lock. The output thread's,
    /// which must never wait on the window.
    bool tryRead(Status& out, Clock::time_point& at) const;
    /// When the window last wrote it; unset before it ever has. Any thread.
    Clock::time_point writtenAt() const;

private:
    mutable std::mutex mutex_;
    Status status_;
    Clock::time_point at_{};
};

/// A change asked for over the network, for the window to carry out.
struct Command {
    enum class Kind : std::uint8_t {
        /// START and STOP as switches: one already in the state asked for is answered, and done.
        Start,
        Stop,
        /// The input stopped and started again — the outage path's own reopen, for a stream that
        /// has gone quiet or dropped out.
        Restart,
        /// `text` the host, `text2` the device — by name, as the settings file keeps it.
        Device,
        /// `number` the first input, from 1; `flag` mono when given.
        Channel,
        /// `text` the source, as the settings file spells it.
        Source,
        /// `text` the name, `text2` the kind, `text3` the host or MIDI device, `number` the port.
        OutputAdd,
        /// `text` the output's id; `flag` on or off.
        OutputEnable,
        /// `text` the output's id; `value` the delay in milliseconds.
        OutputDelay,
        /// `text` the output's id; `flag` whether it is sent the global messages.
        OutputGlobal,
    };
    Kind kind = Kind::Start;
    /// The address as it came, for the reply and for the log line.
    std::string address;
    Endpoint from;
    /// The sender's request number, when it gave one: then it is answered whatever the answer.
    std::optional<std::int32_t> seq;
    std::string text;
    std::string text2;
    std::string text3;
    std::int32_t number = 0;
    std::optional<bool> flag;
    double value = 0.0;
    /// When it was queued (`CommandQueue::push`): one the window comes to later than
    /// `CommandQueue::kStaleSeconds` after is refused, never carried out late.
    std::chrono::steady_clock::time_point queuedAt{};
};

/// Commands on their way from the control socket to the window. Bounded: the window drains it
/// every redraw, and one that is not being drained — a modal dialog up — is not to be queued
/// behind indefinitely.
class CommandQueue {
public:
    static constexpr std::size_t kCapacity = 64;
    /// How old a command may be when the window comes to it. A change asked for while the window
    /// was held up — a dialog open, a driver taking seconds to let go — is refused once it is
    /// this old: a show controller that asked for a START and was not answered has moved on, and
    /// one carried out minutes later is a surprise for everybody.
    static constexpr double kStaleSeconds = 2.0;
    /// False when full; the sender is then told takt4 is busy. Stamps `queuedAt`.
    bool push(Command command);
    std::vector<Command> drain();

private:
    std::mutex mutex_;
    std::deque<Command> queue_;
};

/// Where the verdict on a command goes: back to whoever sent it.
class Replies {
public:
    virtual ~Replies() = default;
    /// `ok`, or refused for `note`'s reason; `note` is also what was done, for an `ok` that has
    /// something to say ("o-1f3a9c20" for an output added). With a request number the sender is
    /// answered either way; without one, only a refusal is said.
    virtual void answer(const Command& command, bool ok, std::string_view note) = 0;
};

/// One line of what takt4 says about itself, for `/status/log`: what the status line says, and the
/// problems the window shows.
struct LogLine {
    /// "info", "warning" or "error".
    std::string level;
    std::string text;
};

/// The lines the window says, on their way to the output thread, which sends them a few a second.
/// **A line waits its turn rather than being lost**: the one that says who changed takt4 from the
/// network must not go in a burst of others. Bounded, and a line that would not fit is dropped and
/// counted; the oldest are kept, as a burst of the same trouble says it in its first lines.
class LogQueue {
public:
    static constexpr std::size_t kCapacity = 32;
    void push(LogLine line);
    /// The oldest `most` lines, taken off the queue; the rest wait.
    std::vector<LogLine> take(std::size_t most);
    /// Lines dropped for a full queue.
    std::uint64_t dropped() const;

private:
    mutable std::mutex mutex_;
    std::deque<LogLine> queue_;
    std::uint64_t dropped_ = 0;
};

/// The senders remote setup is allowed from, as an operator types them: IPv4 addresses separated
/// by commas, spaces or semicolons, in host order. What is not an address is put in `rejected`, as
/// typed, and left out. This machine (127.x.x.x) is never refused by a list: whatever runs on it
/// can change takt4's settings file anyway.
std::vector<std::uint32_t> senderList(std::string_view text, std::vector<std::string>* rejected);

/// Settings spellings of the sources, as `/ctl/source` takes them and `Status::source` says them.
std::string_view sourceName(int timingIndex) noexcept;
/// The window's index of a source, or nothing for a name this takt4 does not know — refused, never
/// turned into the audio.
std::optional<int> sourceIndex(std::string_view name) noexcept;

} // namespace takt4::remote
