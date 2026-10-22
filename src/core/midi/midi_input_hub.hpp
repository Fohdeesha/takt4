#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::midi {

/// Something that wants a MIDI input's messages: the control input, a MIDI clock follower.
class MidiInputListener {
public:
    virtual ~MidiInputListener() = default;

    MidiInputListener(const MidiInputListener&) = delete;
    MidiInputListener& operator=(const MidiInputListener&) = delete;

    /// One whole message as it arrived, and when: on the hub's clock — Link's, which every beat
    /// in takt4 is stamped on — in microseconds, read the moment the driver handed it over.
    /// Called on the driver's thread, or a test's. Must not throw, and must not call the hub.
    virtual void onMidi(std::span<const unsigned char> message, std::int64_t atMicros) noexcept = 0;

    /// Whether this listener wants a clock's messages — Timing Clock, Start, Continue, Stop and
    /// Song Position — and a timecode's quarter frames (`isTimingMessage`). Asked when it starts
    /// listening. The control input does not: they are not gestures.
    virtual bool wantsClock() const noexcept { return false; }

protected:
    MidiInputListener() = default;
};

/// **Each MIDI input device opened once, and its messages handed to everyone listening to it.**
///
/// On Windows a MIDI input is one program's at a time, and one open within it: the control input
/// and a MIDI clock follower on the same device — a mixer that sends both, a DAW's one virtual
/// port — cannot each open it. So neither opens anything; both listen here, and the device is
/// opened when the first starts listening and closed when the last stops.
///
/// **Every message is stamped as it arrives**, with the clock this is given (Link's, in the
/// application). RtMidi's own Windows timestamps are whole milliseconds apart — the driver's
/// counter — which at 128 BPM is a twentieth of the gap between two clock ticks.
///
/// SysEx and active sensing never reach a listener. A clock's and a timecode's messages reach only
/// the listeners that ask for them (`MidiInputListener::wantsClock`). They are taken from the
/// driver whoever is listening, and passed over here for the rest: changing what the driver drops
/// while a device is open would be writing what its thread reads.
class MidiInputHub {
public:
    /// Now, in microseconds. Called on the driver's thread with every message, so it has to be
    /// cheap and thread-safe — `output::LinkSession::now` is both.
    using Clock = std::function<std::int64_t()>;

    explicit MidiInputHub(Clock clock);
    ~MidiInputHub();

    MidiInputHub(const MidiInputHub&) = delete;
    MidiInputHub& operator=(const MidiInputHub&) = delete;

    /// Starts handing `listener` the messages of the input `port` names — an index, a whole name,
    /// the same device under another number, or part of a name (`output::findMidiPort`) —
    /// opening it if nobody is listening to it yet. Returns the port's full name. A listener
    /// listens to one port at a time; listening again moves it.
    ///
    /// Throws `output::MidiPortMissing` when no input matches, `output::MidiPortBusy` when one
    /// does and will not open — another program has it — and `std::runtime_error` when there is
    /// no usable MIDI API, or in the test binaries' sandbox, which opens no device. `who` begins
    /// each message ("MIDI control: ").
    std::string listen(std::string_view port, MidiInputListener& listener, std::string_view who);

    /// Stops handing `listener` anything, closing its input if it was the last listening. When
    /// this returns, no message is being handed to it and none will be. Safe for a listener that
    /// is not listening.
    void stopListening(MidiInputListener& listener) noexcept;

    /// Now, on the clock every message is stamped with: what a listener reads the present on, so
    /// its messages' moments and the present are on one clock. Any thread.
    std::int64_t now() const { return clock_(); }

    /// The full name of the input `listener` is listening to, or empty.
    std::string portOf(const MidiInputListener& listener) const;
    /// How many inputs are open.
    std::size_t openPorts() const;

    /// **Inputs unplugged, and plugged back in.** A device pulled out
    /// says nothing to whoever has it open — RtMidi's Windows input drops the driver's "closed"
    /// notice — so its listeners went on waiting for messages that never came, and plugged back
    /// in it was never heard again. Whoever owns this calls `recheck` now and then with the inputs
    /// on the machine now (`output::listMidiInputPorts`): each input listened to that is not among
    /// them is closed and said to be lost, every listener kept; each lost one that is among them
    /// again is opened again under every listener. An input is found again as `listen` finds one
    /// — whole name, the same device under another number (Windows renumbers the inputs after one
    /// that goes), or part of a name — so a device still plugged in is never taken for gone.
    struct Recheck {
        /// The inputs found gone, and found back, this time.
        std::vector<std::string> lost;
        std::vector<std::string> back;
        /// Inputs back on the list that would not open — another program has them — with why.
        std::vector<std::string> refused;
    };
    Recheck recheck(const std::vector<std::string>& listed);
    /// The inputs listened to that are not plugged in now, by name.
    std::vector<std::string> lostPorts() const;
    /// Whether the input `listener` listens to is not plugged in now.
    bool isLost(const MidiInputListener& listener) const;

    /// **For the tests**: a message as the driver would hand it over on the input called `name` —
    /// stamped now, and given to whoever listens to that name. Nothing needs to be open: a test
    /// with no device uses `attachWithoutPort`.
    void deliver(std::string_view name, std::span<const unsigned char> message) noexcept;
    /// **For the tests**: `listener` listening to an input called `name` that nothing opens — the
    /// test sandbox opens no device — so `deliver` can play the driver's part.
    void attachWithoutPort(std::string_view name, MidiInputListener& listener);

private:
    struct Port;
    /// The driver's callback: stamps the message and hands it to the listeners of `port`.
    static void received(double, std::vector<unsigned char>* message, void* port);
    /// Hands one message to the listeners of `port`. The caller holds the lock.
    static void handLocked(const Port& port, std::span<const unsigned char> message,
                           std::int64_t atMicros) noexcept;
    Port* findLocked(std::string_view name) const noexcept;
    /// Takes a listener off every port; returns the ports it leaves with nobody listening, taken
    /// out of the list, to be closed once the lock is let go. The caller holds the lock.
    std::vector<std::unique_ptr<Port>> detachLocked(const MidiInputListener& listener);
    /// Stops the driver's callbacks and closes the device. Never under the lock: closing hands
    /// back the driver's buffers through the very callback that takes it.
    static void close(Port& port) noexcept;

    Clock clock_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Port>> ports_;
};

/// Whether a message is a clock's — Timing Clock, Start, Continue, Stop or Song Position — or a
/// timecode's quarter frame, which only a listener that wants a clock is handed.
bool isTimingMessage(std::span<const unsigned char> message) noexcept;

} // namespace takt4::midi
