#include "core/output/osc_publisher.hpp"

#include "core/output/osc_message.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace takt4::output {

namespace {

/// Below this a tempo or a confidence has not really moved, and resending it would only
/// add traffic. A hundredth of a BPM is finer than anything downstream can act on.
constexpr double kBpmEpsilon = 0.005;
constexpr double kConfidenceEpsilon = 0.005;

} // namespace

OscPublisher::OscPublisher(std::string prefix) : prefix_(std::move(prefix)) {
    if (prefix_.empty() || prefix_.front() != '/' || prefix_.back() == '/') {
        throw std::invalid_argument("OscPublisher: the prefix must start with '/' and not end "
                                    "with one, as in \"/takt4\"");
    }
    bpmAddress_ = prefix_ + "/bpm";
    beatAddress_ = prefix_ + "/beat";
    barAddress_ = prefix_ + "/beat/bar";
    downbeatAddress_ = prefix_ + "/downbeat";
    confidenceAddress_ = prefix_ + "/confidence";
    lockedAddress_ = prefix_ + "/locked";
    meterAddress_ = prefix_ + "/meter";
    resyncAddress_ = prefix_ + "/resync";
    // Every address is built from the prefix, so one bad prefix is caught here rather
    // than silently dropping messages later.
    if (!OscMessage(barAddress_).valid()) {
        throw std::invalid_argument("OscPublisher: \"" + prefix_ +
                                    "\" does not make a legal OSC address");
    }
}

void OscPublisher::addTarget(std::string_view host, std::uint16_t port) {
    targets_.push_back(std::make_unique<OscSender>(host, port));
}

void OscPublisher::clearTargets() noexcept {
    targets_.clear();
    // Back to exactly what a freshly built publisher holds, so a target added after this
    // is told the same things a target present from the start would have been.
    lastBpm_ = -1.0;
    lastConfidence_ = -1.0;
    lastLocked_ = -1;
    lastMeter_ = 0;
}

void OscPublisher::sendInt(std::string_view address, std::int32_t value) {
    OscMessage message(address);
    message.addInt(value);
    const auto packet = message.packet();
    for (const auto& target : targets_) {
        if (target->send(packet)) {
            ++sent_;
        } else {
            ++failed_;
        }
    }
}

void OscPublisher::sendFloat(std::string_view address, float value) {
    OscMessage message(address);
    message.addFloat(value);
    const auto packet = message.packet();
    for (const auto& target : targets_) {
        if (target->send(packet)) {
            ++sent_;
        } else {
            ++failed_;
        }
    }
}

void OscPublisher::sendChangedState(double bpm, double confidence, bool locked, std::uint32_t meter,
                                    bool force) {
    if (force || std::abs(bpm - lastBpm_) > kBpmEpsilon) {
        sendFloat(bpmAddress_, static_cast<float>(bpm));
        lastBpm_ = bpm;
    }
    if (force || std::abs(confidence - lastConfidence_) > kConfidenceEpsilon) {
        sendFloat(confidenceAddress_, static_cast<float>(confidence));
        lastConfidence_ = confidence;
    }
    const int lockedNow = locked ? 1 : 0;
    if (force || lockedNow != lastLocked_) {
        sendInt(lockedAddress_, lockedNow);
        lastLocked_ = lockedNow;
    }
    if (force || meter != lastMeter_) {
        sendInt(meterAddress_, static_cast<std::int32_t>(meter));
        lastMeter_ = meter;
    }
}

void OscPublisher::publishBeat(const tracking::BeatEvent& event) {
    // State first: a consumer that reads the beat and then looks at the tempo should see
    // the tempo of the beat it just got, not the one before it.
    sendChangedState(event.bpm, event.confidence, event.locked, event.beatsPerBar, true);
    sendInt(beatAddress_, 1);
    if (event.beatInBar > 0) {
        sendInt(barAddress_, static_cast<std::int32_t>(event.beatInBar));
    }
    if (event.downbeat) {
        sendInt(downbeatAddress_, 1);
    }
}

void OscPublisher::publishState(const tracking::TempoState& state) {
    sendChangedState(state.bpm, state.confidence, state.locked, state.beatsPerBar, false);
}

void OscPublisher::publishResync() {
    sendInt(resyncAddress_, 1);
}

} // namespace takt4::output
