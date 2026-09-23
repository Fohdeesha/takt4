#include "core/output/link_session.hpp"

#include <ableton/Link.hpp>
#include <ableton/link/HostTimeFilter.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace takt4::output {

struct LinkSession::Impl {
    explicit Impl(double bpm) : link(bpm) {}
    ableton::Link link;
    /// Touched only by the audio thread; see the header.
    ableton::link::HostTimeFilter<ableton::link::platform::Clock> hostTime;
};

LinkSession::LinkSession(double initialTempoBpm) : impl_(std::make_unique<Impl>(initialTempoBpm)) {}

LinkSession::~LinkSession() = default;

void LinkSession::enable(bool on) {
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

std::int64_t LinkSession::hostMicrosForSample(double sampleTime) noexcept {
    return impl_->hostTime.sampleTimeToHostTime(sampleTime).count();
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
