#include "core/output/osc_publisher.hpp"

#include "core/output/osc_message.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace takt4::output {

namespace {

/// How many delayed datagrams may be waiting at once, across every target.
///
/// A second of delay at the fastest thing anyone sends — §5.6's namespace is a handful per
/// beat, and a rule on 32nd notes at 215 BPM is 115 a second — is well under this. Past it
/// something upstream is wrong, and dropping the newest with a count is better than growing
/// a queue on the thread that has a MIDI clock to keep.
constexpr std::size_t kMaxPending = 512;

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

void OscPublisher::addTarget(std::string_view host, std::uint16_t port, std::size_t bit,
                             double delaySeconds) {
    // Past the mask's width a target cannot be named individually, so it is given every bit
    // instead: it then receives from rules that go everywhere, which is the default, and
    // nothing that was routed away from it. Losing routing is better than losing the feed.
    const std::uint64_t selector =
        bit < kMaxRoutableTargets ? (std::uint64_t{1} << bit) : kAllOutputs;
    const double delay =
        std::clamp(delaySeconds, kMinOutputDelaySeconds, kMaxOutputDelaySeconds);
    targets_.push_back(Target{std::make_unique<OscSender>(host, port), selector, delay});
}

double OscPublisher::holdFor(double delaySeconds) const noexcept {
    const double offset = delaySeconds + offsetSeconds_;
    if (offset >= 0.0) {
        return offset;
    }
    // Earlier than a beat we have already heard is not a thing that can be sent, so it is
    // taken off the *next* beat instead: hold for what is left of a beat after the offset.
    // With no tempo yet there is no beat to take it off, and holding an arbitrary amount
    // would be worse than not holding at all.
    if (beatSeconds_ <= 0.0) {
        return 0.0;
    }
    // Clamped rather than stepped back another beat: an offset longer than the beat is an
    // operator describing a rig problem, and two beats of anticipation would be a stranger
    // answer than "as early as it can go".
    return std::max(0.0, beatSeconds_ + offset);
}

void OscPublisher::flushDue() {
    if (pending_.empty()) {
        return;
    }
    // Everything due, in the order it was queued. Partitioned rather than scanned-and-erased
    // per item: one round can retire a whole beat's worth, and erasing from the front of a
    // vector once per message is the one way to make a queue this short expensive.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        Pending& item = pending_[i];
        if (item.due > now_) {
            if (kept != i) {
                pending_[kept] = std::move(item);
            }
            ++kept;
            continue;
        }
        // A target list replaced under a queued message leaves nothing to send it to. It is
        // dropped rather than sent somewhere else: the index it was queued against named a
        // socket that is gone, and guessing at a replacement would send a robot's cue to a
        // lighting desk.
        if (item.target < targets_.size() && targets_[item.target].sender->send(item.packet)) {
            ++sent_;
        } else {
            ++failed_;
        }
    }
    pending_.resize(kept);
}

void OscPublisher::clearTargets() noexcept {
    targets_.clear();
    // Whatever was waiting was waiting for a socket that no longer exists. See flushDue.
    pending_.clear();
    // Back to exactly what a freshly built publisher holds, so a target added after this
    // is told the same things a target present from the start would have been.
    lastBpm_ = -1.0;
    lastConfidence_ = -1.0;
    lastLocked_ = -1;
    lastMeter_ = 0;
}

void OscPublisher::sendAddress(std::string_view address) {
    sendAddressTo(kAllOutputs, address);
}

void OscPublisher::sendAddress(std::string_view address, std::int32_t value) {
    sendAddressTo(kAllOutputs, address, value);
}

void OscPublisher::sendAddress(std::string_view address, float value) {
    sendAddressTo(kAllOutputs, address, value);
}

void OscPublisher::sendAddress(std::string_view address, std::string_view value) {
    sendAddressTo(kAllOutputs, address, value);
}

void OscPublisher::sendAddressTo(std::uint64_t outputs, std::string_view address) {
    OscMessage message(address);
    sendPacket(message, outputs);
}

void OscPublisher::sendAddressTo(std::uint64_t outputs, std::string_view address,
                                 std::int32_t value) {
    sendInt(address, value, outputs);
}

void OscPublisher::sendAddressTo(std::uint64_t outputs, std::string_view address, float value) {
    sendFloat(address, value, outputs);
}

void OscPublisher::sendAddressTo(std::uint64_t outputs, std::string_view address,
                                 std::string_view value) {
    OscMessage message(address);
    message.addString(value);
    sendPacket(message, outputs);
}

bool OscPublisher::anyTargetIn(std::uint64_t outputs) const noexcept {
    for (const Target& target : targets_) {
        if ((target.bit & outputs) != 0) {
            return true;
        }
    }
    return false;
}

void OscPublisher::sendPacket(OscMessage& message, std::uint64_t outputs) {
    const auto packet = message.packet();
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        const Target& target = targets_[i];
        if ((target.bit & outputs) == 0) {
            continue; // routed away from this one
        }
        const double hold = holdFor(target.delaySeconds);
        if (hold <= 0.0) {
            if (target.sender->send(packet)) {
                ++sent_;
            } else {
                ++failed_;
            }
            continue;
        }
        // Held for this target and this target only. The others on the same message have
        // already gone, which is the point: one beat, several destinations, each hearing it
        // when the thing on the end of it needs to.
        if (pending_.size() >= kMaxPending) {
            ++dropped_;
            continue;
        }
        pending_.push_back(
            Pending{now_ + hold, i, std::vector<std::byte>(packet.begin(), packet.end())});
    }
}

void OscPublisher::sendInt(std::string_view address, std::int32_t value, std::uint64_t outputs) {
    OscMessage message(address);
    message.addInt(value);
    sendPacket(message, outputs);
}

void OscPublisher::sendFloat(std::string_view address, float value, std::uint64_t outputs) {
    OscMessage message(address);
    message.addFloat(value);
    sendPacket(message, outputs);
}

void OscPublisher::sendChangedState(double bpm, double confidence, bool locked, std::uint32_t meter,
                                    bool force) {
    if (force || std::abs(bpm - lastBpm_) > kBpmEpsilon) {
        sendFloat(bpmAddress_, static_cast<float>(bpm), kAllOutputs);
        lastBpm_ = bpm;
    }
    if (force || std::abs(confidence - lastConfidence_) > kConfidenceEpsilon) {
        sendFloat(confidenceAddress_, static_cast<float>(confidence), kAllOutputs);
        lastConfidence_ = confidence;
    }
    const int lockedNow = locked ? 1 : 0;
    if (force || lockedNow != lastLocked_) {
        sendInt(lockedAddress_, lockedNow, kAllOutputs);
        lastLocked_ = lockedNow;
    }
    if (force || meter != lastMeter_) {
        sendInt(meterAddress_, static_cast<std::int32_t>(meter), kAllOutputs);
        lastMeter_ = meter;
    }
}

void OscPublisher::publishBeat(const tracking::BeatEvent& event) {
    // State first: a consumer that reads the beat and then looks at the tempo should see
    // the tempo of the beat it just got, not the one before it.
    sendChangedState(event.bpm, event.confidence, event.locked, event.beatsPerBar, true);
    sendInt(beatAddress_, 1, kAllOutputs);
    if (event.beatInBar > 0) {
        sendInt(barAddress_, static_cast<std::int32_t>(event.beatInBar), kAllOutputs);
    }
    if (event.downbeat) {
        sendInt(downbeatAddress_, 1, kAllOutputs);
    }
}

void OscPublisher::publishState(const tracking::TempoState& state) {
    sendChangedState(state.bpm, state.confidence, state.locked, state.beatsPerBar, false);
}

void OscPublisher::publishResync() {
    sendInt(resyncAddress_, 1, kAllOutputs);
}

} // namespace takt4::output
