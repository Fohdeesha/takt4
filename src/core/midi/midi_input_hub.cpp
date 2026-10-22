#include "core/midi/midi_input_hub.hpp"

#include "core/output/midi_ports.hpp"
#include "core/output/rtmidi_errors.hpp"
#include "core/sandbox.hpp"

#include <RtMidi.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace takt4::midi {

struct MidiInputHub::Port {
    std::string name;
    MidiInputHub* hub = nullptr;
    /// Null for a port a test attached without a device. `errors` before `in`, which reports to
    /// it and so must go first.
    std::unique_ptr<output::RtMidiErrors> errors;
    std::unique_ptr<RtMidiIn> in;
    struct Listening {
        MidiInputListener* listener = nullptr;
        bool clock = false;
    };
    std::vector<Listening> listeners;
    /// Not on the machine's list at the last `recheck`, and closed: opened again when it is.
    bool lost = false;
    /// A device was opened for it — false only for a test's (`attachWithoutPort`), which has
    /// nothing to open again.
    bool wasOpened = false;
};

namespace {

/// Stops a device's callbacks and closes it. Never under the hub's lock: see `close`.
void closeDevice(output::RtMidiErrors* errors, RtMidiIn* in) noexcept {
    if (in == nullptr) {
        return;
    }
    try {
        in->cancelCallback();
        in->closePort();
        if (errors != nullptr) {
            errors->raise();
        }
    } catch (...) {
        // Closing a device that has already gone — unplugged — is nothing a stop can report.
    }
}

} // namespace

bool isTimingMessage(std::span<const unsigned char> message) noexcept {
    if (message.empty()) {
        return false;
    }
    switch (message[0]) {
    case 0xF1: // timecode quarter frame
    case 0xF2: // Song Position Pointer
    case 0xF8: // Timing Clock
    case 0xFA: // Start
    case 0xFB: // Continue
    case 0xFC: // Stop
        return true;
    default:
        return false;
    }
}

MidiInputHub::MidiInputHub(Clock clock) : clock_(std::move(clock)) {}

MidiInputHub::~MidiInputHub() {
    std::vector<std::unique_ptr<Port>> ports;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ports.swap(ports_);
    }
    for (const std::unique_ptr<Port>& port : ports) {
        close(*port);
    }
}

void MidiInputHub::received(double, std::vector<unsigned char>* message, void* port) {
    if (message == nullptr || message->empty() || port == nullptr) {
        return;
    }
    Port& from = *static_cast<Port*>(port);
    // Stamped before anything else: the moment the driver handed it over is the measurement.
    const std::int64_t at = from.hub->clock_();
    const std::lock_guard<std::mutex> lock(from.hub->mutex_);
    handLocked(from, std::span<const unsigned char>(message->data(), message->size()), at);
}

void MidiInputHub::handLocked(const Port& port, std::span<const unsigned char> message,
                              std::int64_t atMicros) noexcept {
    const bool timing = isTimingMessage(message);
    for (const Port::Listening& listening : port.listeners) {
        if (!timing || listening.clock) {
            listening.listener->onMidi(message, atMicros);
        }
    }
}

MidiInputHub::Port* MidiInputHub::findLocked(std::string_view name) const noexcept {
    const auto found =
        std::find_if(ports_.begin(), ports_.end(),
                     [name](const std::unique_ptr<Port>& port) { return port->name == name; });
    return found == ports_.end() ? nullptr : found->get();
}

std::vector<std::unique_ptr<MidiInputHub::Port>>
MidiInputHub::detachLocked(const MidiInputListener& listener) {
    std::vector<std::unique_ptr<Port>> idle;
    for (auto port = ports_.begin(); port != ports_.end();) {
        std::erase_if((*port)->listeners, [&listener](const Port::Listening& listening) {
            return listening.listener == &listener;
        });
        if ((*port)->listeners.empty()) {
            idle.push_back(std::move(*port));
            port = ports_.erase(port);
        } else {
            ++port;
        }
    }
    return idle;
}

void MidiInputHub::close(Port& port) noexcept {
    closeDevice(port.errors.get(), port.in.get());
}

std::string MidiInputHub::listen(std::string_view spec, MidiInputListener& listener,
                                 std::string_view who) {
    // A listener listens to one port: whatever it listened to before is let go of first.
    stopListening(listener);
    const std::string prefix(who);
    const std::string asked(spec);

    // The inputs as the driver lists them now, asked afresh: a device unplugged and plugged back
    // in can come back at another place in the list.
    auto errors = std::make_unique<output::RtMidiErrors>();
    std::unique_ptr<RtMidiIn> in;
    try {
        in = std::make_unique<RtMidiIn>();
    } catch (const RtMidiError& error) {
        // RtMidi says "no usable MIDI API on this machine" by throwing from its constructor — a
        // headless Linux box without an ALSA sequencer, which is what CI runs on.
        throw std::runtime_error(prefix + "cannot open \"" + asked +
                                 "\": no usable MIDI API on this machine (" + error.getMessage() +
                                 ")");
    }
    errors->watch(*in);
    std::vector<std::string> names;
    try {
        const unsigned int count = in->getPortCount();
        for (unsigned int i = 0; i < count; ++i) {
            names.push_back(in->getPortName(i));
        }
        errors->raise();
    } catch (const RtMidiError& error) {
        throw std::runtime_error(prefix + error.getMessage());
    }
    const std::optional<std::size_t> index = output::findMidiPort(names, spec);
    if (!index) {
        std::string message = prefix + "no input port matching \"" + asked + "\"; ";
        if (names.empty()) {
            message += "there are none";
        } else {
            message += "there are " + std::to_string(names.size()) + ":";
            for (std::size_t i = 0; i < names.size(); ++i) {
                message += "\n  " + std::to_string(i) + ": " + names[i];
            }
        }
        throw output::MidiPortMissing(message);
    }
    const std::string name = names[*index];
    const bool clock = listener.wantsClock();
    {
        // Already open for somebody else: listen alongside them.
        const std::lock_guard<std::mutex> lock(mutex_);
        if (Port* open = findLocked(name); open != nullptr) {
            open->listeners.push_back({&listener, clock});
            return name;
        }
    }
    if (sandbox::active()) {
        // The test binaries' sandbox: this input is the rig's. See `sandbox.hpp`.
        sandbox::refuse(sandbox::Refused::Midi);
        throw std::runtime_error(prefix + "\"" + name + "\" is not opened in the test sandbox");
    }
    auto port = std::make_unique<Port>();
    port->name = name;
    port->hub = this;
    port->wasOpened = true;
    port->errors = std::move(errors);
    port->in = std::move(in);
    try {
        port->in->openPort(static_cast<unsigned int>(*index), "takt4");
        port->errors->raise();
    } catch (const RtMidiError& error) {
        // Listed and refused: a WinMM input is one program's at a time, and a DAW that opened
        // every input it saw is the usual holder. Not "no such port" — the operator can see it.
        throw output::MidiPortBusy(prefix, name, error.getMessage());
    }
    // SysEx and active sensing are nobody's business here; a clock's messages are taken and
    // passed only to whoever wants them (see the class).
    port->in->ignoreTypes(/*sysex=*/true, /*time=*/false, /*sense=*/true);
    std::unique_ptr<Port> unused;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (Port* open = findLocked(name); open != nullptr) {
            // Opened by another thread while this one was opening it — which a WinMM driver
            // would have refused — so listen there, and this one goes.
            open->listeners.push_back({&listener, clock});
            unused = std::move(port);
        } else {
            port->listeners.push_back({&listener, clock});
            Port& opened = *port;
            ports_.push_back(std::move(port));
            // Last, so the driver cannot deliver to a port that is not in the list yet. A message
            // arriving now waits for the lock.
            opened.in->setCallback(&MidiInputHub::received, &opened);
        }
    }
    if (unused) {
        close(*unused);
    }
    return name;
}

void MidiInputHub::stopListening(MidiInputListener& listener) noexcept {
    std::vector<std::unique_ptr<Port>> idle;
    {
        // Under the lock, which every delivery holds: once it is let go, nothing is being handed
        // to `listener` and nothing will be.
        const std::lock_guard<std::mutex> lock(mutex_);
        idle = detachLocked(listener);
    }
    for (const std::unique_ptr<Port>& port : idle) {
        close(*port);
    }
}

std::string MidiInputHub::portOf(const MidiInputListener& listener) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const std::unique_ptr<Port>& port : ports_) {
        for (const Port::Listening& listening : port->listeners) {
            if (listening.listener == &listener) {
                return port->name;
            }
        }
    }
    return {};
}

std::size_t MidiInputHub::openPorts() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(ports_.begin(), ports_.end(),
                      [](const std::unique_ptr<Port>& port) { return port->in != nullptr; }));
}

void MidiInputHub::deliver(std::string_view name, std::span<const unsigned char> message) noexcept {
    const std::int64_t at = clock_();
    const std::lock_guard<std::mutex> lock(mutex_);
    // An input that is not plugged in says nothing.
    if (const Port* port = findLocked(name); port != nullptr && !port->lost && !message.empty()) {
        handLocked(*port, message, at);
    }
}

MidiInputHub::Recheck MidiInputHub::recheck(const std::vector<std::string>& listed) {
    Recheck result;
    // What is gone, taken off its port under the lock and closed after it; and what is back, by
    // the name it is listed under now, to be opened outside it.
    struct Device {
        std::unique_ptr<output::RtMidiErrors> errors;
        std::unique_ptr<RtMidiIn> in;
    };
    std::vector<Device> gone;
    struct Back {
        Port* port = nullptr;
        std::string name;
    };
    std::vector<Back> back;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const std::unique_ptr<Port>& port : ports_) {
            const std::optional<std::size_t> at = output::findMidiPort(listed, port->name);
            if (!port->lost && !at) {
                port->lost = true;
                result.lost.push_back(port->name);
                gone.push_back({std::move(port->errors), std::move(port->in)});
            } else if (port->lost && at) {
                back.push_back({port.get(), listed[*at]});
            } else if (at && listed[*at] != port->name) {
                // Still plugged in, and listed under another number now that another input has
                // gone: the same device, under the name a `listen` for it would find.
                port->name = listed[*at];
            }
        }
    }
    for (Device& device : gone) {
        closeDevice(device.errors.get(), device.in.get());
    }

    for (const Back& found : back) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const auto still = std::find_if(ports_.begin(), ports_.end(), [&](const auto& port) {
                return port.get() == found.port;
            });
            if (still == ports_.end()) {
                continue; // everyone stopped listening meanwhile
            }
            if (!found.port->wasOpened) {
                // A test's input, which has no device: listed again, it is back.
                found.port->lost = false;
                found.port->name = found.name;
                result.back.push_back(found.name);
                continue;
            }
        }
        if (sandbox::active()) {
            continue; // the test binaries open no device; see `listen`
        }
        // Opened by the place it has on the list of the very instance that opens it, as `listen`
        // does: the list handed in can be a moment old.
        auto errors = std::make_unique<output::RtMidiErrors>();
        std::unique_ptr<RtMidiIn> in;
        std::string name;
        try {
            in = std::make_unique<RtMidiIn>();
            errors->watch(*in);
            std::vector<std::string> names;
            const unsigned int count = in->getPortCount();
            for (unsigned int i = 0; i < count; ++i) {
                names.push_back(in->getPortName(i));
            }
            errors->raise();
            const std::optional<std::size_t> at = output::findMidiPort(names, found.name);
            if (!at) {
                continue; // gone again already
            }
            name = names[*at];
            in->openPort(static_cast<unsigned int>(*at), "takt4");
            errors->raise();
            in->ignoreTypes(/*sysex=*/true, /*time=*/false, /*sense=*/true);
        } catch (const RtMidiError& error) {
            // Listed, and would not open: another program took it while it was out.
            result.refused.push_back(found.name + ": " + error.getMessage());
            continue;
        }
        bool installed = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const auto still = std::find_if(ports_.begin(), ports_.end(), [&](const auto& port) {
                return port.get() == found.port;
            });
            if (still != ports_.end() && found.port->lost) {
                found.port->errors = std::move(errors);
                found.port->in = std::move(in);
                found.port->lost = false;
                found.port->name = name;
                // Last, as `listen` does: a message arriving now waits for the lock.
                found.port->in->setCallback(&MidiInputHub::received, found.port);
                installed = true;
                result.back.push_back(name);
            }
        }
        if (!installed) {
            closeDevice(errors.get(), in.get()); // the port went while this one opened
        }
    }
    return result;
}

std::vector<std::string> MidiInputHub::lostPorts() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> lost;
    for (const std::unique_ptr<Port>& port : ports_) {
        if (port->lost) {
            lost.push_back(port->name);
        }
    }
    return lost;
}

bool MidiInputHub::isLost(const MidiInputListener& listener) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const std::unique_ptr<Port>& port : ports_) {
        for (const Port::Listening& listening : port->listeners) {
            if (listening.listener == &listener) {
                return port->lost;
            }
        }
    }
    return false;
}

void MidiInputHub::attachWithoutPort(std::string_view name, MidiInputListener& listener) {
    stopListening(listener);
    const std::lock_guard<std::mutex> lock(mutex_);
    Port* port = findLocked(name);
    if (port == nullptr) {
        auto made = std::make_unique<Port>();
        made->name = std::string(name);
        made->hub = this;
        port = made.get();
        ports_.push_back(std::move(made));
    }
    port->listeners.push_back({&listener, listener.wantsClock()});
}

} // namespace takt4::midi
