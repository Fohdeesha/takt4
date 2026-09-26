#include "core/control/midi_control.hpp"

#include "core/output/midi_ports.hpp"
#include "core/sandbox.hpp"

#include <RtMidi.h>

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace takt4::control {
namespace {

/// A port specification is an index or a name, as `output::MidiOutput` takes — see
/// `output::findMidiPort`. Throws with the list of what was actually there, which is the useful
/// message when a controller is unplugged or named differently than the operator expected.
unsigned int findPort(RtMidiIn& in, std::string_view spec) {
    const unsigned int count = in.getPortCount();
    std::vector<std::string> names;
    names.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        names.push_back(in.getPortName(i));
    }
    if (const std::optional<std::size_t> found = output::findMidiPort(names, spec)) {
        return static_cast<unsigned int>(*found);
    }

    std::string message = "MIDI control: no input port matching \"" + std::string(spec) + "\"; ";
    if (names.empty()) {
        message += "there are none";
    } else {
        message += "there are " + std::to_string(names.size()) + ":";
        for (std::size_t i = 0; i < names.size(); ++i) {
            message += "\n  " + std::to_string(i) + ": " + names[i];
        }
    }
    throw std::runtime_error(message);
}

} // namespace

struct MidiControl::Impl {
    RtMidiIn in;
};

MidiControl::MidiControl(engine::BeatEngine& engine, Config config, RuleControl* rules)
    : config_(std::move(config)), surface_(engine, rules) {}

MidiControl::~MidiControl() {
    stop();
}

void MidiControl::start() {
    if (!config_.enabled || running()) {
        return;
    }

    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>();
    } catch (const RtMidiError& error) {
        // RtMidi reports "no usable MIDI API on this machine" by throwing from its own
        // constructor rather than offering an empty port list. A headless Linux box
        // without an ALSA sequencer is the ordinary case, and CI runs on one.
        throw std::runtime_error("MIDI control: cannot open \"" + config_.port +
                                 "\": no usable MIDI API on this machine (" +
                                 error.getMessage() + ")");
    }

    try {
        const unsigned int index = findPort(impl->in, config_.port);
        const std::string name = impl->in.getPortName(index);
        if (sandbox::active()) {
            // The test binaries' sandbox: this controller is the rig's. See `sandbox.hpp`.
            sandbox::refuse(sandbox::Refused::Midi);
            throw std::runtime_error("MIDI control: \"" + name +
                                     "\" is not opened in the test sandbox");
        }
        impl->in.openPort(index, "takt4 control");
        // A controller's clock and active-sensing streams are not control gestures, and
        // letting them through would spend a callback per tick doing nothing.
        impl->in.ignoreTypes(/*sysex=*/true, /*time=*/true, /*sense=*/true);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            portName_ = name;
        }
    } catch (const RtMidiError& error) {
        throw std::runtime_error("MIDI control: " + error.getMessage());
    }

    // A set of taps does not span a stop.
    surface_.resetTaps();
    impl_ = std::move(impl);
    // Last, so the callback cannot see a half-built object: RtMidi begins delivering the
    // moment this returns.
    running_.store(true, std::memory_order_release);
    impl_->in.setCallback(
        [](double, std::vector<unsigned char>* message, void* self) {
            if (message != nullptr && self != nullptr) {
                static_cast<MidiControl*>(self)->onMessage(
                    std::span<const unsigned char>(message->data(), message->size()));
            }
        },
        this);
}

void MidiControl::stop() noexcept {
    if (!impl_) {
        running_.store(false, std::memory_order_release);
        return;
    }
    // The flag first, then RtMidi's own teardown: cancelCallback joins its thread, so
    // after these two lines nothing can be inside onMessage.
    running_.store(false, std::memory_order_release);
    try {
        impl_->in.cancelCallback();
        impl_->in.closePort();
    } catch (const RtMidiError&) {
        // Closing a port that is already gone — an unplugged controller — is not
        // something a stop path can usefully report.
    }
    impl_.reset();
    const std::lock_guard<std::mutex> lock(mutex_);
    portName_.clear();
    learning_.reset();
}

void MidiControl::setPort(std::string port) {
    stop();
    config_.enabled = !port.empty();
    config_.port = std::move(port);
}

std::string MidiControl::portName() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return portName_;
}

void MidiControl::learn(ControlTarget target) {
    const std::lock_guard<std::mutex> lock(mutex_);
    learning_ = std::move(target);
}

void MidiControl::cancelLearn() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    learning_.reset();
}

std::optional<ControlTarget> MidiControl::learning() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return learning_;
}

std::vector<MidiBinding> MidiControl::bindings() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return bindings_;
}

void MidiControl::setBindings(std::vector<MidiBinding> bindings) {
    const std::lock_guard<std::mutex> lock(mutex_);
    bindings_.clear();
    for (const MidiBinding& binding : bindings) {
        // Later entries win, so a hand-edited file behaves the way learning twice does.
        const auto same = [&binding](const MidiBinding& held) {
            return held.kind == binding.kind && held.channel == binding.channel &&
                   held.number == binding.number;
        };
        std::erase_if(bindings_, same);
        if (binding.channel >= 1 && binding.channel <= 16 && binding.number <= 127) {
            bindings_.push_back(binding);
        }
    }
}

bool MidiControl::bind(const MidiBinding& binding) {
    if (binding.channel < 1 || binding.channel > 16 || binding.number > 127) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    // One control does one thing. Pressing the same pad while assigning a second action
    // is an operator changing their mind, not asking for both at once.
    std::erase_if(bindings_, [&binding](const MidiBinding& held) {
        return held.kind == binding.kind && held.channel == binding.channel &&
               held.number == binding.number;
    });
    bindings_.push_back(binding);
    return true;
}

std::size_t MidiControl::forget(ControlAction action) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return std::erase_if(
        bindings_, [action](const MidiBinding& held) { return held.target.action == action; });
}

std::size_t MidiControl::forget(const ControlTarget& target) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return std::erase_if(bindings_,
                         [&target](const MidiBinding& held) { return held.target == target; });
}

std::optional<MidiEvent> MidiControl::lastEvent() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return lastEvent_;
}

bool MidiControl::dispatch(const MidiEvent& event) {
    std::optional<ControlTarget> learned;
    std::vector<ControlTarget> matched;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        lastEvent_ = event;
        if (learning_) {
            learned = std::move(*learning_);
            learning_.reset();
        } else {
            // The release of the gesture that was just learned. See `learnedRelease_`.
            const bool releaseOfLearned =
                learnedRelease_ && event.kind == MidiEvent::Kind::ControlChange &&
                event.channel == learnedRelease_->channel &&
                event.number == learnedRelease_->number && event.value < 64;
            learnedRelease_.reset();
            if (releaseOfLearned) {
                handled_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            for (const MidiBinding& binding : bindings_) {
                if (binding.matches(event)) {
                    matched.push_back(binding.target);
                }
            }
        }
    }

    if (learned) {
        MidiBinding binding;
        binding.kind = event.kind;
        binding.channel = event.channel;
        binding.number = event.number;
        binding.target = *std::move(learned);
        (void)bind(binding);
        // Learned, not acted on. The gesture that assigns a control should not also fire
        // it: an operator binding `tempo/halve` would otherwise halve the tempo to do it.
        // And nor should that gesture's release, when the control is a CC that sends one.
        if (event.kind == MidiEvent::Kind::ControlChange) {
            const std::lock_guard<std::mutex> lock(mutex_);
            learnedRelease_ = event;
        }
        return true;
    }

    if (matched.empty()) {
        ignored_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // A table with two bindings for one control cannot happen through `bind`, but a
    // caller can build one; act on all of them rather than silently picking.
    for (const ControlTarget& target : matched) {
        // **PANIC from a MIDI control only engages** — as the window's PANIC does since the
        // audit's H18. A CC reads below 64 as "off", which made a momentary CC pad
        // hold-to-panic: the halt let go the moment the finger came up. Releasing is the
        // window's RELEASE, or OSC's explicit `panic 0`, never the end of a button press.
        if (target.action == ControlAction::Panic && event.kind == MidiEvent::Kind::ControlChange &&
            event.value < 64) {
            continue;
        }
        if (!surface_.apply(target, argumentOf(event))) {
            refused_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    handled_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void MidiControl::onMessage(std::span<const unsigned char> message) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    const std::optional<MidiEvent> event = readMidiEvent(message);
    if (!event) {
        return; // not a control gesture; see readMidiEvent
    }
    try {
        (void)dispatch(*event);
    } catch (...) {
        // Nothing in dispatch throws today beyond a bad_alloc, and a control surface must
        // not be able to take the process down through RtMidi's callback if one ever does.
    }
}

} // namespace takt4::control
