#include "core/output/transports.hpp"

#include <chrono>
#include <cmath>

namespace takt4::output {
namespace {

/// Link is only told about a tempo that really moved. Below this the change is under a
/// hundredth of a BPM, which no peer can act on and which the refined tempo produces on
/// almost every beat.
constexpr double kLinkTempoEpsilon = 0.005;

std::int64_t toMicros(double seconds) noexcept {
    return static_cast<std::int64_t>(seconds * 1e6);
}

} // namespace

Transports::Transports(const Config& config) : latencyMicros_(toMicros(config.latencySeconds)) {
    if (!config.oscTargets.empty()) {
        osc_ = std::make_unique<OscPublisher>(config.oscPrefix);
        for (const auto& [host, port] : config.oscTargets) {
            osc_->addTarget(host, port);
        }
    }
    if (config.midiClockPort) {
        midiPort_ = std::make_unique<MidiOutput>(*config.midiClockPort);
        midi_ = std::make_unique<MidiClock>(*midiPort_, 120.0);
    }
    if (config.link) {
        link_ = std::make_unique<LinkSession>(120.0);
    }
}

void Transports::startOutputs(double now) {
    if (link_) {
        link_->enable(true);
    }
    if (midi_) {
        midi_->start(now);
    }
}

void Transports::stopOutputs() noexcept {
    if (midi_) {
        midi_->stop();
    }
    if (link_) {
        link_->enable(false);
    }
}

void Transports::advance(double now, const tracking::TempoState& state) {
    if (midi_) {
        (void)midi_->advance(now);
    }
    if (osc_) {
        osc_->publishState(state);
    }
}

void Transports::publish(const tracking::BeatEvent& event, std::int64_t hostMicros, double now) {
    ++beats_;
    if (event.downbeat) {
        ++downbeats_;
    }
    const std::int64_t latencyMicros = latencyMicros_.load(std::memory_order_relaxed);

    if (osc_) {
        osc_->publishBeat(event);
    }
    if (midi_) {
        midi_->setTempo(event.bpm);
        // The beat's audio arrived a pipeline's worth of time ago; the latency offset is
        // the one place that is compensated (§5.5).
        midi_->syncToBeat(now + static_cast<double>(latencyMicros) / 1e6);
    }
    if (link_) {
        publishToLink(event, hostMicros, latencyMicros);
    }
}

void Transports::setLatencySeconds(double seconds) noexcept {
    latencyMicros_.store(toMicros(seconds), std::memory_order_relaxed);
}

double Transports::latencySeconds() const noexcept {
    return static_cast<double>(latencyMicros_.load(std::memory_order_relaxed)) / 1e6;
}

void Transports::publishToLink(const tracking::BeatEvent& event, std::int64_t hostMicros,
                               std::int64_t latencyMicros) {
    // Without a host time source — the offline path — there is nothing meaningful to
    // align to, so Link is left alone.
    if (hostMicros == 0) {
        return;
    }
    const std::chrono::microseconds at{hostMicros + latencyMicros};
    if (std::abs(event.bpm - lastLinkBpm_) > kLinkTempoEpsilon) {
        link_->setTempo(event.bpm, at);
        lastLinkBpm_ = event.bpm;
    }
    // §5.6: phase through requestBeatAtTime with the detected meter as the quantum. The
    // beat number is the bar position, so peers line up on our downbeat; before the first
    // downbeat the bar phase is unknown and only the tempo is published.
    if (event.beatInBar > 0 && event.beatsPerBar > 0) {
        const double beat = static_cast<double>(event.beatInBar - 1);
        const double quantum = static_cast<double>(event.beatsPerBar);
        if (event.snapped) {
            // §5.6 reserves forceBeatAtTime for exactly this beat. A requestBeat here
            // would be moved to the next time the session's phase already matches — which
            // is the phase the operator just said was wrong — so the snap would move
            // takt4's own bar and leave every peer where it was.
            link_->forceBeat(beat, at, quantum);
        } else {
            link_->requestBeat(beat, at, quantum);
        }
    }
}

} // namespace takt4::output
