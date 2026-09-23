#include "core/output/osc_publisher.hpp"

#include "core/output/osc_message.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
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

bool isValidOscPrefix(std::string_view prefix) {
    if (prefix.empty() || prefix.front() != '/' || prefix.back() == '/') {
        return false;
    }
    // The longest address built from it, which is the one most likely to be refused — and
    // every other one differs only in its last word.
    return OscMessage(std::string(prefix) + "/beat/bar").valid();
}

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
    targets_.push_back(Target{std::make_unique<OscSender>(host, port), selector, delay, bit, {}});
}

std::vector<std::pair<std::size_t, std::string>>
OscPublisher::setTargets(const std::vector<TargetSpec>& specs) {
    std::vector<std::pair<std::size_t, std::string>> failures;
    std::vector<Target> next;
    next.reserve(specs.size());
    // Where each old target went: its index in `next`, or nowhere.
    constexpr std::size_t kGone = static_cast<std::size_t>(-1);
    std::vector<std::size_t> moved(targets_.size(), kGone);
    bool added = false;
    for (const TargetSpec& spec : specs) {
        std::size_t was = kGone;
        for (std::size_t i = 0; i < targets_.size() && !spec.id.empty(); ++i) {
            if (moved[i] == kGone && targets_[i].id == spec.id) {
                was = i;
                break;
            }
        }
        Target target;
        target.bit = spec.bit < kMaxRoutableTargets ? (std::uint64_t{1} << spec.bit) : kAllOutputs;
        target.output = spec.bit;
        target.id = spec.id;
        target.delaySeconds =
            std::clamp(spec.delaySeconds, kMinOutputDelaySeconds, kMaxOutputDelaySeconds);
        if (was != kGone && targets_[was].sender->host() == spec.host &&
            targets_[was].sender->port() == spec.port) {
            target.sender = std::move(targets_[was].sender);
        } else {
            try {
                target.sender = std::make_unique<OscSender>(spec.host, spec.port);
            } catch (const std::exception& e) {
                failures.emplace_back(spec.bit, e.what());
                continue;
            }
            added = true;
        }
        if (was != kGone) {
            moved[was] = next.size();
        }
        next.push_back(std::move(target));
    }
    // What is queued follows its target to where it went; what was queued for a target that
    // is gone goes nowhere, which is what deleting or switching one off asks for.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        Pending& item = pending_[i];
        if (item.target >= moved.size() || moved[item.target] == kGone) {
            ++dropped_;
            continue;
        }
        item.target = moved[item.target];
        if (kept != i) {
            pending_[kept] = std::move(item);
        }
        ++kept;
    }
    pending_.resize(kept);
    targets_ = std::move(next);
    if (added) {
        // A target that is new has never been told the state, and only a change is sent
        // between beats — see `clearTargets`.
        lastBpm_ = -1.0;
        lastConfidence_ = -1.0;
        lastLocked_ = -1;
        lastMeter_ = 0;
    }
    return failures;
}

void OscPublisher::setDelay(std::size_t bit, double delaySeconds) noexcept {
    for (Target& target : targets_) {
        if (target.output == bit) {
            target.delaySeconds =
                std::clamp(delaySeconds, kMinOutputDelaySeconds, kMaxOutputDelaySeconds);
        }
    }
}

void OscPublisher::refresh() noexcept {
    for (Target& target : targets_) {
        (void)target.sender->ready();
    }
}

void OscPublisher::flushAll() {
    for (const Pending& item : pending_) {
        if (item.target < targets_.size() && targets_[item.target].sender->send(item.packet)) {
            ++sent_;
        } else {
            ++failed_;
        }
    }
    pending_.clear();
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

void OscPublisher::sendAddressTo(std::uint64_t outputs, std::string_view address,
                                 std::optional<double> moment) {
    OscMessage message(address);
    sendPacket(message, outputs, moment.value_or(now_));
}

void OscPublisher::sendAddressTo(std::uint64_t outputs, std::string_view address,
                                 std::int32_t value, std::optional<double> moment) {
    sendInt(address, value, outputs, moment.value_or(now_));
}

void OscPublisher::sendAddressTo(std::uint64_t outputs, std::string_view address, float value,
                                 std::optional<double> moment) {
    sendFloat(address, value, outputs, moment.value_or(now_));
}

void OscPublisher::sendAddressTo(std::uint64_t outputs, std::string_view address,
                                 std::string_view value, std::optional<double> moment) {
    OscMessage message(address);
    message.addString(value);
    sendPacket(message, outputs, moment.value_or(now_));
}

bool OscPublisher::anyTargetIn(std::uint64_t outputs) const noexcept {
    for (const Target& target : targets_) {
        if ((target.bit & outputs) != 0) {
            return true;
        }
    }
    return false;
}

void OscPublisher::sendPacket(OscMessage& message, std::uint64_t outputs, double moment) {
    const auto packet = message.packet();
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        const Target& target = targets_[i];
        if ((target.bit & outputs) == 0) {
            continue; // routed away from this one
        }
        // The moment the message is about, moved by the rig's offset and then by this target's
        // own. Already gone is sent now: earlier than now is not a time anything can be sent at,
        // and a message about a beat that was only heard after its moment is as early as it can
        // be the moment it is sent.
        const double due = moment + offsetSeconds_ + target.delaySeconds;
        if (due <= now_) {
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
        pending_.push_back(Pending{due, i, std::vector<std::byte>(packet.begin(), packet.end())});
    }
}

void OscPublisher::sendInt(std::string_view address, std::int32_t value, std::uint64_t outputs,
                           double moment) {
    OscMessage message(address);
    message.addInt(value);
    sendPacket(message, outputs, moment);
}

void OscPublisher::sendFloat(std::string_view address, float value, std::uint64_t outputs,
                             double moment) {
    OscMessage message(address);
    message.addFloat(value);
    sendPacket(message, outputs, moment);
}

void OscPublisher::sendChangedState(double bpm, double confidence, bool locked, std::uint32_t meter,
                                    bool force, double moment) {
    if (force || std::abs(bpm - lastBpm_) > kBpmEpsilon) {
        sendFloat(bpmAddress_, static_cast<float>(bpm), kAllOutputs, moment);
        lastBpm_ = bpm;
    }
    if (force || std::abs(confidence - lastConfidence_) > kConfidenceEpsilon) {
        sendFloat(confidenceAddress_, static_cast<float>(confidence), kAllOutputs, moment);
        lastConfidence_ = confidence;
    }
    const int lockedNow = locked ? 1 : 0;
    // §5.6's resync, on the one moment it means: the tracker has found the beat again after
    // having said it had lost it. Documented and never sent (the audit's M8). Not on a first
    // state that is already locked — nothing was lost — and not when a target is added, which
    // starts this memory again at "never said".
    const bool refound = lockedNow == 1 && lastLocked_ == 0;
    if (force || lockedNow != lastLocked_) {
        sendInt(lockedAddress_, lockedNow, kAllOutputs, moment);
        lastLocked_ = lockedNow;
    }
    if (refound) {
        sendInt(resyncAddress_, 1, kAllOutputs, moment);
    }
    if (force || meter != lastMeter_) {
        sendInt(meterAddress_, static_cast<std::int32_t>(meter), kAllOutputs, moment);
        lastMeter_ = meter;
    }
}

void OscPublisher::publishBeat(const tracking::BeatEvent& event, std::optional<double> moment) {
    const double at = moment.value_or(now_);
    // State first: a consumer that reads the beat and then looks at the tempo should see
    // the tempo of the beat it just got, not the one before it. Both at the beat's own moment,
    // so a target delayed or brought forward keeps them in that order.
    sendChangedState(event.bpm, event.confidence, event.locked, event.beatsPerBar, true, at);
    sendInt(beatAddress_, 1, kAllOutputs, at);
    if (event.beatInBar > 0) {
        sendInt(barAddress_, static_cast<std::int32_t>(event.beatInBar), kAllOutputs, at);
    }
    if (event.downbeat) {
        sendInt(downbeatAddress_, 1, kAllOutputs, at);
    }
}

void OscPublisher::publishState(const tracking::TempoState& state) {
    sendChangedState(state.bpm, state.confidence, state.locked, state.beatsPerBar, false, now_);
}

void OscPublisher::publishResync() {
    sendInt(resyncAddress_, 1, kAllOutputs, now_);
}

} // namespace takt4::output
