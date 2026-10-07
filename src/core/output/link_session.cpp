#include "core/output/link_session.hpp"

#include "core/audio/host_time_fit.hpp"
#include "core/audio/rates.hpp"
#include "core/sandbox.hpp"

#include <ableton/Link.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace takt4::output {

struct LinkSession::Impl {
    explicit Impl(double bpm) : link(bpm), hostTime(1e6 / audio::kInternalSampleRate) {}
    ableton::Link link;
    /// Touched only by the audio thread — and by `resetHostTimeFilter`, which runs while no
    /// stream is open. See the header.
    audio::HostTimeFit hostTime;
};

LinkSession::LinkSession(double initialTempoBpm) : impl_(std::make_unique<Impl>(initialTempoBpm)) {}

LinkSession::~LinkSession() = default;

void LinkSession::enable(bool on) {
    if (on && sandbox::active()) {
        // The test binaries' sandbox: joining would put a peer in the rig's own session, and
        // Resolume and Live follow its tempo. See `sandbox.hpp`.
        sandbox::refuse(sandbox::Refused::Link);
        return;
    }
    impl_->link.enable(on);
}

bool LinkSession::enabled() const {
    return impl_->link.isEnabled();
}

void LinkSession::enableStartStopSync(bool on) {
    impl_->link.enableStartStopSync(on);
}

bool LinkSession::startStopSyncEnabled() const {
    return impl_->link.isStartStopSyncEnabled();
}

std::size_t LinkSession::numPeers() const {
    return impl_->link.numPeers();
}

double LinkSession::tempoBpm() const {
    return impl_->link.captureAppSessionState().tempo();
}

std::chrono::microseconds LinkSession::now() const {
    return impl_->link.clock().micros();
}

void LinkSession::observe(double sampleTime, std::int64_t steadyMicros) noexcept {
    // Onto Link's clock by what lies between the two now, read together. On Windows they are the
    // same QueryPerformanceCounter count from the same zero, and this moves nothing; elsewhere
    // Link keeps a clock of its own (CLOCK_MONOTONIC_RAW against the steady clock's
    // CLOCK_MONOTONIC on Linux), and a moment on the one is not a moment on the other.
    const std::int64_t linkNow = impl_->link.clock().micros().count();
    const std::int64_t steadyNow = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count();
    impl_->hostTime.observe(sampleTime, static_cast<double>(steadyMicros + (linkNow - steadyNow)));
}

std::int64_t LinkSession::hostMicrosForSample(double sampleTime) noexcept {
    return static_cast<std::int64_t>(std::llround(impl_->hostTime.at(sampleTime)));
}

void LinkSession::resetHostTimeFilter() noexcept {
    impl_->hostTime.reset();
}

void LinkSession::setTempo(double bpm, std::chrono::microseconds at) {
    // Every change goes capture -> modify -> commit; a session state held across calls
    // would overwrite whatever a peer did in between.
    auto state = impl_->link.captureAppSessionState();
    state.setTempo(bpm, at);
    impl_->link.commitAppSessionState(state);
    ++tempoUpdates_;
}

void LinkSession::requestBeat(double beat, std::chrono::microseconds at, double quantum) {
    auto state = impl_->link.captureAppSessionState();
    state.requestBeatAtTime(beat, at, quantum);
    impl_->link.commitAppSessionState(state);
    ++beatRequests_;
}

void LinkSession::forceBeat(double beat, std::chrono::microseconds at, double quantum) {
    auto state = impl_->link.captureAppSessionState();
    state.forceBeatAtTime(beat, at, quantum);
    impl_->link.commitAppSessionState(state);
    ++beatRequests_;
}

void LinkSession::snap(double bpm, double beat, std::chrono::microseconds at, double quantum) {
    auto state = impl_->link.captureAppSessionState();
    state.setTempo(bpm, at);
    state.forceBeatAtTime(beat, at, quantum);
    impl_->link.commitAppSessionState(state);
    ++tempoUpdates_;
    ++beatRequests_;
}

double LinkSession::beatAtTime(std::chrono::microseconds at, double quantum) const {
    return impl_->link.captureAppSessionState().beatAtTime(at, quantum);
}

double LinkSession::phaseAtTime(std::chrono::microseconds at, double quantum) const {
    return impl_->link.captureAppSessionState().phaseAtTime(at, quantum);
}

} // namespace takt4::output
