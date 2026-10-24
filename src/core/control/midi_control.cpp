#include "core/control/midi_control.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace takt4::control {
namespace {

/// The end of a press: a note at 0 (`readMidiEvent`), or a CC below the switch threshold
/// `argumentOf` reads as off.
bool isRelease(const MidiEvent& event) noexcept {
    return event.kind == MidiEvent::Kind::Note ? event.value == 0 : event.value < 64;
}

} // namespace

MidiControl::MidiControl(engine::BeatSource& beats, midi::MidiInputHub& inputs, Config config,
                         RuleControl* rules)
    : config_(std::move(config)), surface_(beats, rules), inputs_(inputs) {}

MidiControl::~MidiControl() {
    stop();
}

void MidiControl::start() {
    if (!config_.enabled || running()) {
        return;
    }
    // A set of taps does not span a stop.
    surface_.resetTaps();
    // Listening before the port is: the hub hands messages over from the moment `listen`
    // returns, and one turned away at `deliver`'s door would be a press lost.
    running_.store(true, std::memory_order_seq_cst);
    std::string name;
    try {
        name = inputs_.listen(config_.port, *this, "MIDI control: ");
    } catch (...) {
        running_.store(false, std::memory_order_seq_cst);
        throw;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    portName_ = std::move(name);
}

void MidiControl::stop() noexcept {
    // The flag first, so a message arriving from here on is turned away at `deliver`'s door —
    // and **then every message already past it, waited for**. A message that had passed the
    // flag when this began used to go on being handled after it — a tap or a PANIC acted on by
    // a surface that had been switched off, and on the way out, a dispatch into an object being
    // destroyed. Both sides are sequentially consistent: `deliver` raises the count and then
    // reads the flag, this lowers the flag and then reads the count, and only with both orders
    // total does one always see the other. Handling a message never waits on this thread, so
    // the wait is one message's length.
    running_.store(false, std::memory_order_seq_cst);
    while (inFlight_.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }
    // And off the hub, which closes the device unless something else still listens to it. When
    // that returns nothing more is handed over.
    inputs_.stopListening(*this);
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
                learnedRelease_ && event.kind == learnedRelease_->kind &&
                event.channel == learnedRelease_->channel &&
                event.number == learnedRelease_->number && isRelease(event);
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
        // And nor should that gesture's release — a CC pad's, and since a note's release
        // reaches the bindings, a note's: learning `lock` from a pad would otherwise pin
        // nothing and then let go.
        if (!isRelease(event)) {
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
        // **PANIC from a MIDI control only engages** — as the window's PANIC does. A CC reads
        // below 64 as "off", which made a momentary CC pad hold-to-panic: the halt let go the
        // moment the finger came up. Releasing is the window's RELEASE, or `panic/release`,
        // never the end of a button press. A note's release, which now reaches here, is the same.
        if (target.action == ControlAction::Panic && isRelease(event)) {
            continue;
        }
        if (!surface_.apply(target, argumentOf(event))) {
            refused_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    handled_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void MidiControl::onMidi(std::span<const unsigned char> message, std::int64_t) noexcept {
    deliver(message);
}

void MidiControl::deliver(std::span<const unsigned char> message) noexcept {
    inFlight_.fetch_add(1, std::memory_order_seq_cst);
    if (running_.load(std::memory_order_seq_cst)) {
        receive(message);
    }
    inFlight_.fetch_sub(1, std::memory_order_release);
}

void MidiControl::receive(std::span<const unsigned char> message) noexcept {
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
