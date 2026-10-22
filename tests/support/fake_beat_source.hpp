#pragma once

// A beat source a test fills by hand: beats it makes up, pushed onto a ring the output thread
// drains, and every command posted to it kept — so a test can say exactly what reached the rig
// from which source, and what a button asked of it.

#include "core/engine/beat_source.hpp"
#include "core/engine/control.hpp"
#include "core/rt/published.hpp"
#include "core/rt/spsc_ring.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace takt4::testing {

class FakeBeatSource final : public engine::BeatSource {
public:
    bool popBeat(engine::EngineBeat& out) noexcept override { return beats_.tryPop(out); }
    tracking::TempoState state() const noexcept override { return state_.load(); }
    engine::EngineIntensity intensity() const noexcept override { return intensity_.load(); }
    std::uint64_t runNumber() const noexcept override {
        return run_.load(std::memory_order_acquire);
    }
    bool post(const engine::Command& command) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        posted_.push_back(command.kind);
        return accepts_;
    }

    /// The ring's one producer: the test's thread. Stamped with this run, and its state
    /// published first, as a source publishes the state before the beat.
    bool push(engine::EngineBeat beat) noexcept {
        beat.run = runNumber();
        state_.publish(beat.state);
        return beats_.tryPush(beat);
    }
    /// A new state with no beat — the edge of a silence, a lock lost — as a source publishes one
    /// between beats.
    void publish(const tracking::TempoState& state) noexcept { state_.publish(state); }
    /// A new run, as a source's own start makes one: what is left on the ring is the last run's.
    void start() noexcept { run_.fetch_add(1, std::memory_order_acq_rel); }
    /// Beats pushed and not yet taken.
    std::size_t waiting() const noexcept { return beats_.size(); }

    /// What was posted, in order.
    std::vector<engine::Command::Kind> posted() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return posted_;
    }
    /// Whether `post` says it took a command.
    void setAccepts(bool accepts) {
        const std::lock_guard<std::mutex> lock(mutex_);
        accepts_ = accepts;
    }

private:
    rt::SpscRing<engine::EngineBeat, 64> beats_;
    rt::Published<tracking::TempoState> state_;
    rt::Published<engine::EngineIntensity> intensity_;
    std::atomic<std::uint64_t> run_{1};
    mutable std::mutex mutex_;
    std::vector<engine::Command::Kind> posted_;
    bool accepts_ = true;
};

} // namespace takt4::testing
