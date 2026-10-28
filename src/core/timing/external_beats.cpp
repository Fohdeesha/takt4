#include "core/timing/external_beats.hpp"

#include "core/rt/thread_priority.hpp"

#include <optional>
#include <thread>

namespace takt4::timing {

ExternalBeats::ExternalBeats() {
    // Sized now, so the worker's first drain allocates nothing (see `ControlQueue::drain`).
    commands_.reserve(engine::ControlQueue::kCapacity);
    state_.publish(grid_.state());
}

ExternalBeats::~ExternalBeats() {
    stop();
}

bool ExternalBeats::post(const engine::Command& command) {
    using Kind = engine::Command::Kind;
    switch (command.kind) {
    case Kind::Halve:
    case Kind::Redouble:
    case Kind::SnapDownbeat:
        return controls_.post(command);
    case Kind::SetTempoOptions:
        // The tracker's window, gate and latency: taken, and nothing here for them to change.
        return true;
    case Kind::InputIdle:
        // An audio input's silence: a followed source says when it has gone by itself.
        return true;
    case Kind::SeedTempo:
    case Kind::SetLockPinned:
    case Kind::HoldTempo:
        break;
    }
    // A tapped tempo, a pin or a hold: the tempo is the source's, and the lock is the source
    // being there. Refused out loud, so a control surface's TAP is seen to do nothing rather
    // than silently doing nothing.
    refused_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void ExternalBeats::start(GridSource& source, const BeatGrid::Options& options) {
    // The presses made while stopped go first, as the tracker's do: a ÷2 or a DOWNBEAT pressed on
    // a control surface between sets is not for the set about to start.
    controls_.dropPresses();
    stop();
    // **Not the beat ring**: its one consumer is the output thread. What the last run left there
    // is stamped with that run, and the consumer drops it.
    run_.fetch_add(1, std::memory_order_acq_rel);
    grid_.reset(options);
    state_.publish(grid_.state());
    rounds_.store(0, std::memory_order_relaxed);
    sent_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    displayDropped_.store(0, std::memory_order_relaxed);
    coasting_.store(false, std::memory_order_relaxed);
    workerRaised_.store(false, std::memory_order_relaxed);
    lateness_.reset();
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(
        [this, &source, beatsPerBar = options.beatsPerBar] { run(source, beatsPerBar); });
}

void ExternalBeats::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ExternalBeats::run(GridSource& source, std::uint32_t beatsPerBar) noexcept {
    // A late round is a late beat, as a late round of the output thread is a late cue.
    const rt::PriorityScope priority(rt::ThreadWork::Output);
    workerRaised_.store(priority.raised(), std::memory_order_release);
    rt::RoundTimer timer(&lateness_);
    source.begin(beatsPerBar);
    while (running_.load(std::memory_order_acquire)) {
        round(source);
        rounds_.fetch_add(1, std::memory_order_relaxed);
        timer.wait(kPeriod);
    }
    source.end();
}

void ExternalBeats::applyCommands() noexcept {
    controls_.drain(commands_);
    for (const engine::Command& command : commands_) {
        switch (command.kind) {
        case engine::Command::Kind::Halve:
            grid_.halve();
            break;
        case engine::Command::Kind::Redouble:
            grid_.redouble();
            break;
        case engine::Command::Kind::SnapDownbeat:
            grid_.snapDownbeat();
            break;
        default:
            break; // nothing else is queued: see `post`
        }
    }
}

void ExternalBeats::round(GridSource& source) noexcept {
    applyCommands();
    const GridReading reading = source.read();
    std::optional<engine::EngineBeat> beat = grid_.advance(reading);
    // The state before the ring, as the tracker publishes it: a reader of `state()` never sees a
    // beat on the ring that is newer than the state.
    state_.publish(grid_.state());
    coasting_.store(grid_.coasting(), std::memory_order_relaxed);
    if (!beat) {
        return;
    }
    beat->run = run_.load(std::memory_order_relaxed);
    if (beats_.tryPush(*beat)) {
        sent_.fetch_add(1, std::memory_order_relaxed);
    } else {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    DisplayBeat shown;
    shown.hostMicros = beat->hostMicros;
    shown.beatInBar = beat->event.beatInBar;
    shown.downbeat = beat->event.downbeat;
    shown.coasted = grid_.coasting();
    shown.run = beat->run;
    if (!display_.tryPush(shown)) {
        displayDropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace takt4::timing
