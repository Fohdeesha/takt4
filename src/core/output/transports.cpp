#include "core/output/transports.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <utility>

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

Transports::Transports(const Config& config)
    : openMidi_(config.openMidi), latencyMicros_(toMicros(config.latencySeconds)),
      link_(std::make_unique<LinkSession>(120.0)),
      osc_(std::make_unique<OscPublisher>(config.oscPrefix)),
      dmx_(std::make_unique<dmx::DmxEngine>()), artnet_(std::make_unique<dmx::ArtNetPublisher>()),
      oscPrefix_(config.oscPrefix) {
    // The patch first: `setOutputs` points every Art-Net target at the universes the patch
    // uses, so a node configured for "everything" has to know what everything is.
    dmx_->setPatch(config.patch);
    setOutputs(config.outputs);
    setMidiClockPort(config.midiClockPort);
    linkEnabled_ = config.link;
}

void Transports::setPatch(std::vector<dmx::Fixture> patch) {
    dmx_->setPatch(std::move(patch));
}

void Transports::startOutputs(double now) {
    started_ = true;
    lastNow_ = now;
    if (linkEnabled_) {
        link_->enable(true);
    }
    if (midi_) {
        midi_->start(now);
    }
}

void Transports::stopOutputs() noexcept {
    started_ = false;
    if (midi_) {
        midi_->stop();
    }
    link_->enable(false);
}

void Transports::setLinkEnabled(bool on) {
    linkEnabled_ = on;
    // Only actually joined while the outputs are running: switching Link on before Start
    // says what to do, not to do it now.
    link_->enable(on && started_);
    if (!on) {
        // A session rejoined later starts from nothing, so the next beat must resend the
        // tempo rather than find it unchanged.
        lastLinkBpm_ = -1.0;
    }
}

MidiOutput* Transports::midiPort() const noexcept {
    if (!midiClockPort_) {
        return nullptr;
    }
    const auto found = midiDevices_.find(*midiClockPort_);
    return found == midiDevices_.end() ? nullptr : found->second.get();
}

MidiOutput* Transports::openDevice(const std::string& device) {
    if (device.empty()) {
        return nullptr;
    }
    // Opened once however many things name it: §5.6's clock and any number of §5.8's rule
    // targets down one cable is the ordinary rig, and some drivers refuse a second open.
    const auto found = midiDevices_.find(device);
    if (found != midiDevices_.end()) {
        return found->second.get();
    }
    // Throws if it is not on the machine.
    std::unique_ptr<MidiOutput> opened =
        openMidi_ ? openMidi_(device) : std::make_unique<MidiOutput>(device);
    MidiOutput* const port = opened.get();
    midiDevices_.emplace(device, std::move(opened));
    return port;
}

void Transports::closeUnusedDevices() noexcept {
    for (auto entry = midiDevices_.begin(); entry != midiDevices_.end();) {
        bool wanted = midiClockPort_ && *midiClockPort_ == entry->first;
        for (const OutputTarget& target : outputs_) {
            if (target.enabled && target.kind == OutputTarget::Kind::Midi &&
                target.device == entry->first) {
                wanted = true;
                break;
            }
        }
        entry = wanted ? std::next(entry) : midiDevices_.erase(entry);
    }
}

void Transports::setOutputs(const std::vector<OutputTarget>& targets) {
    outputs_ = targets;
    outputPorts_.assign(outputs_.size(), nullptr);
    osc_->clearTargets();
    artnet_->clearTargets();

    // The bit a rule's routing mask uses is the target's index in *this* list, so an OSC
    // publisher that only holds the OSC ones still has to be told which bit each is.
    std::string failures;
    for (std::size_t i = 0; i < outputs_.size(); ++i) {
        const OutputTarget& target = outputs_[i];
        if (!target.enabled) {
            continue; // switched off sends nothing, of either kind
        }
        try {
            if (target.kind == OutputTarget::Kind::Osc) {
                osc_->addTarget(target.host, target.port, i, target.delaySeconds);
            } else if (target.kind == OutputTarget::Kind::ArtNet) {
                dmx::ArtNetPublisher::TargetConfig node;
                node.host = target.host;
                node.port = target.port;
                node.universes = target.universes;
                node.bit = i;
                artnet_->addTarget(node);
            } else {
                outputPorts_[i] = openDevice(target.device);
            }
        } catch (const std::exception& e) {
            // One target that will not open must not cost the others: a controller unplugged
            // since the preset was written is an ordinary state of the world, and the rest of
            // the rig should still be sending. Collected and raised once at the end so the
            // caller can say which.
            if (!failures.empty()) {
                failures += "; ";
            }
            failures += target.name + ": " + e.what();
        }
    }
    closeUnusedDevices();
    if (!failures.empty()) {
        throw std::runtime_error(failures);
    }
}

std::vector<Transports::OscTarget> Transports::oscTargets() const {
    std::vector<OscTarget> targets;
    for (const OutputTarget& target : outputs_) {
        if (target.kind == OutputTarget::Kind::Osc) {
            targets.emplace_back(target.host, target.port);
        }
    }
    return targets;
}

void Transports::setOscTargets(const std::vector<OscTarget>& targets) {
    setOutputs(oscOutputs(targets));
}

MidiOutput* Transports::midiTarget(std::size_t index) const noexcept {
    return index < outputPorts_.size() ? outputPorts_[index] : nullptr;
}

bool Transports::anyOutputIn(std::uint64_t outputs) const noexcept {
    if (osc_->anyTargetIn(outputs)) {
        return true;
    }
    for (std::size_t i = 0; i < outputPorts_.size() && i < kMaxRoutableTargets; ++i) {
        if (outputPorts_[i] != nullptr && (outputs & (std::uint64_t{1} << i)) != 0) {
            return true;
        }
    }
    return false;
}

void Transports::setMidiClockPort(const std::optional<std::string>& port) {
    if (port == midiClockPort_ && (!port || midi_)) {
        // The same port again. Nothing to change — unless that device has gone, when picking
        // it again is the operator saying "try it now", and it used to be ignored because the
        // port was "already open" (the audit's H11a).
        if (MidiOutput* const current = midiPort(); current != nullptr && current->lost()) {
            (void)current->reconnect();
        }
        return;
    }
    if (!port) {
        if (midi_) {
            midi_->stop();
        }
        midi_.reset();
        midiClockPort_.reset();
        closeUnusedDevices(); // unless a rule target still names it
        return;
    }
    // Opened before anything is torn down, so a port that will not open leaves the one that
    // was working exactly where it was. `openDevice` shares it with any rule target on the
    // same cable rather than opening it twice.
    MidiOutput* const opened = openDevice(*port);
    auto clock = std::make_unique<MidiClock>(*opened, 120.0);
    if (midi_) {
        midi_->stop();
    }
    midi_ = std::move(clock);
    midiClockPort_ = port;
    closeUnusedDevices();
    if (started_) {
        // From now, not from when the outputs started: `advance` would otherwise try to
        // emit every tick of the intervening set at once.
        midi_->start(lastNow_);
    }
}

std::vector<std::string> Transports::lostMidiDevices() const {
    std::vector<std::string> lost;
    for (const auto& [name, device] : midiDevices_) {
        if (device->lost()) {
            lost.push_back(name);
        }
    }
    return lost;
}

std::size_t Transports::lostMidiCount() const noexcept {
    std::size_t lost = 0;
    for (const auto& entry : midiDevices_) {
        if (entry.second->lost()) {
            ++lost;
        }
    }
    return lost;
}

void Transports::maintainMidi(double now) noexcept {
    if (midiMaintainedAt_ >= 0.0 && now - midiMaintainedAt_ < kMidiReconnectSeconds) {
        return;
    }
    midiMaintainedAt_ = now;
    // Every device, not only the clock's: a lighting desk on a rule target unplugged and
    // plugged back in is the same failure and wants the same answer. The clock and every rule
    // hold the `MidiOutput` itself, never the port inside it, so a reconnect is invisible to
    // them — the next tick simply goes out.
    for (auto& [name, device] : midiDevices_) {
        if (device->lost()) {
            (void)device->reconnect();
        }
    }
}

void Transports::advance(double now, const tracking::TempoState& state) {
    lastNow_ = now;
    maintainMidi(now);
    if (midi_) {
        (void)midi_->advance(now);
    }
    osc_->setNow(now);
    setOscOffsets(state.bpm);
    // Before this round's state, so a message held from an earlier round goes out ahead of
    // one sent now. A per-target delay reorders against the *other* targets on purpose and
    // must never reorder a target against itself.
    osc_->flushDue();
    osc_->publishState(state);

    // The lighting half, and the reason it is here rather than in `publish`: a fade is not
    // waiting for a beat, it is waiting for a clock. Every running effect is advanced and the
    // frames that are due go out — at most 44 times a second per universe, and at least once
    // per keep-alive, both of which `ArtNetPublisher` decides.
    //
    // Ticked even with no Art-Net target configured. It costs nothing with no patch, and with
    // a patch and no node it keeps the levels a UI shows honest, so that an operator building
    // a rig can watch the numbers move before the node arrives.
    dmx_->tick(now);
    artnet_->publish(*dmx_, now);
}

void Transports::setOscOffsets(double bpm) noexcept {
    // §5.5's offset applies to OSC as well as to the two clock transports. It did not, for
    // most of this file's life, and that was a hole rather than a decision: an operator
    // pulling the slider back to put a media server's clip change on the beat moved Link and
    // the MIDI clock and left the OSC the media server actually listens to exactly where it
    // was. The three now agree about what the number means.
    //
    // What differs is what each can do with it, and only the sign is affected. Link and the
    // MIDI clock carry a running grid whose phase can be moved either way, so they take the
    // offset as it stands. An OSC message is one datagram about one beat that has already
    // happened, so "earlier" has to become "earlier than the next one" — see
    // `OscPublisher::holdFor`, which is why it needs the beat length.
    osc_->setOffsetSeconds(static_cast<double>(latencyMicros_.load(std::memory_order_relaxed)) /
                           1e6);
    // Only ever *updated*, never cleared. `advance` runs on every round and is handed the
    // published state, which reports no tempo while the tracker is between locks — and
    // forgetting the beat length there would make a negative offset stop holding for a bar
    // at a time, so a rig would slide on and off the beat as the tracker's confidence moved.
    // Zero until the first tempo is known, which is what `holdFor` treats as "no beat yet".
    if (bpm > 0.0) {
        osc_->setBeatSeconds(60.0 / bpm);
    }
}

void Transports::publish(const tracking::BeatEvent& event, std::int64_t hostMicros, double now) {
    lastNow_ = now;
    // A beat queued for a delayed target is due `delaySeconds` after *this* beat, not after
    // whenever the next round happens to run.
    osc_->setNow(now);
    // From the beat's own tempo rather than the last round's state: a negative offset is a
    // fraction of *this* beat, and on the beat that changes tempo the two differ.
    setOscOffsets(event.bpm);
    beats_.fetch_add(1, std::memory_order_relaxed);
    if (event.downbeat) {
        downbeats_.fetch_add(1, std::memory_order_relaxed);
    }
    const std::int64_t latencyMicros = latencyMicros_.load(std::memory_order_relaxed);

    osc_->publishBeat(event);
    if (midi_) {
        midi_->setTempo(event.bpm);
        // The beat's audio arrived a pipeline's worth of time ago; the latency offset is
        // the one place that is compensated (§5.5).
        midi_->syncToBeat(now + static_cast<double>(latencyMicros) / 1e6);
    }
    if (linkEnabled_) {
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
    // The rest is bar phase, and there is none to publish before the first downbeat.
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
