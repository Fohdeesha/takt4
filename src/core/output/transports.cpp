#include "core/output/transports.hpp"

#include <algorithm>
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

/// A phase error in beats below which Link is sent the plain tempo and nothing is nudged. A
/// beat's stamp jitters by a few milliseconds, and nudging on that would put the jitter into
/// every peer's tempo display; 0.02 of a beat is 9 ms at 128 BPM.
constexpr double kLinkPhaseDeadband = 0.02;
/// How many beats a nudge spreads a phase error over. Eight: a quarter-beat error is gone to
/// within a hundredth in about three bars, and no peer's tempo moves by more than the clamp.
constexpr double kLinkNudgeBeats = 8.0;
/// The most a nudge moves the tempo Link is sent, either way — 2.6 BPM at 128. Gentle enough
/// that a peer's own tempo display barely wavers, and a peer following Link's tempo — Resolume
/// scrubbing video to it — does not visibly speed up.
constexpr double kLinkMaxNudge = 0.02;

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
    // A session joined now may already be somebody else's, with their phase; the first locked
    // beat has to put it under the music again. See `publishToLink`.
    linkSnapped_ = false;
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
    // A session joined or rejoined starts from nothing — or from a peer's timeline, which is
    // older than ours and wins on join — so the next locked beat snaps it and resends the tempo
    // rather than finding it unchanged.
    lastLinkBpm_ = -1.0;
    linkSnapped_ = false;
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
    setOscOffset();
    // Before this round's state, so a message held from an earlier round goes out ahead of
    // one sent now.
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

void Transports::setOscOffset() noexcept {
    // §5.5's offset applies to OSC as well as to the two clock transports. It did not, for
    // most of this file's life, and that was a hole rather than a decision: an operator
    // pulling the slider back to put a media server's clip change on the beat moved Link and
    // the MIDI clock and left the OSC the media server actually listens to exactly where it
    // was. The three now agree about what the number means, and so does a rule's MIDI and
    // lighting — see `RuleSink`.
    osc_->setOffsetSeconds(latencySeconds());
}

double Transports::leadSeconds() const noexcept {
    double earliest = 0.0;
    for (const OutputTarget& target : outputs_) {
        // The two kinds that hold a message to a delay of their own. An Art-Net node has none;
        // a switched-off target sends nothing, so its delay asks for nothing either.
        if (target.enabled &&
            (target.kind == OutputTarget::Kind::Osc || target.kind == OutputTarget::Kind::Midi)) {
            earliest = std::min(earliest, std::clamp(target.delaySeconds, kMinOutputDelaySeconds,
                                                     kMaxOutputDelaySeconds));
        }
    }
    return std::min(0.0, latencySeconds() + earliest);
}

double Transports::tailSeconds() const noexcept {
    double latest = 0.0;
    for (const OutputTarget& target : outputs_) {
        if (target.enabled &&
            (target.kind == OutputTarget::Kind::Osc || target.kind == OutputTarget::Kind::Midi)) {
            latest = std::max(latest, std::clamp(target.delaySeconds, kMinOutputDelaySeconds,
                                                 kMaxOutputDelaySeconds));
        }
    }
    return std::max(0.0, latencySeconds() + latest);
}

void Transports::publishClocks(const tracking::BeatEvent& event, std::int64_t hostMicros,
                               double beatTime) {
    const std::int64_t latencyMicros = latencyMicros_.load(std::memory_order_relaxed);
    if (midi_) {
        midi_->setTempo(event.bpm);
        // The beat's own time, not the round that drained it: the audio arrived a pipeline's
        // worth of time before the beat was called, and the clock used to be synced to when it
        // was *drained* — late by that pipeline, plus the offset (§5.5). The clock steers
        // towards the beat rather than jumping to it; see `MidiClock::syncToBeat`.
        midi_->syncToBeat(beatTime + static_cast<double>(latencyMicros) / 1e6);
    }
    if (linkEnabled_) {
        publishToLink(event, hostMicros, latencyMicros);
    }
}

void Transports::publishBeat(const tracking::BeatEvent& event, double moment, double now) {
    // A beat queued for a delayed target is due its delay after *this* beat's moment, not
    // after whenever the next round happens to run.
    osc_->setNow(now);
    setOscOffset();
    beats_.fetch_add(1, std::memory_order_relaxed);
    if (event.downbeat) {
        downbeats_.fetch_add(1, std::memory_order_relaxed);
    }
    osc_->publishBeat(event, moment);
}

void Transports::publish(const tracking::BeatEvent& event, std::int64_t hostMicros,
                         double beatTime) {
    lastNow_ = beatTime;
    publishClocks(event, hostMicros, beatTime);
    publishBeat(event, beatTime, beatTime);
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
    // **Nothing while the tracker is hunting** — neither tempo nor phase. Before the first lock
    // the tempo is the hunt's, octave flips and all, and every Start used to overwrite
    // Resolume's tempo with it for a few seconds (the audit's H15). And the next lock snaps
    // again: whatever the session did in between, it is put back under the music then.
    if (!event.locked || !(event.bpm > 0.0)) {
        linkSnapped_ = false;
        return;
    }
    // A peer joining can bring its own timeline — the older session wins on join — so a new
    // peer is a reason to snap again too. Cheap: one beat, one question.
    const std::size_t peers = link_->numPeers();
    if (peers > linkPeers_) {
        linkSnapped_ = false;
    }
    linkPeers_ = peers;

    const std::chrono::microseconds at{hostMicros + latencyMicros};
    // Bar phase needs a bar: before the first downbeat only the tempo goes.
    const bool phased = event.beatInBar > 0 && event.beatsPerBar > 0;
    const double beat = phased ? static_cast<double>(event.beatInBar - 1) : 0.0;
    const double quantum = phased ? static_cast<double>(event.beatsPerBar) : 1.0;
    const auto sendTempo = [&](double bpm) {
        if (std::abs(bpm - lastLinkBpm_) > kLinkTempoEpsilon) {
            link_->setTempo(bpm, at);
            lastLinkBpm_ = bpm;
        }
    };

    // **Snap once, then nudge** — the operator's call of 2026-09-23 (the audit's C3, Q4).
    //
    // With a peer in the session, `requestBeatAtTime` does not move the session's phase at all:
    // Link's own header says the request is moved to "the next time value greater than the
    // given time with the same phase". This used to request on every beat, so with Resolume on
    // Link the bar sat wherever the session happened to be — measured, 180 ms off the music for
    // every beat of a 32-beat run and the bar position 1.6 beats wrong. So the first locked beat
    // *forces* the phase, and so does a DOWNBEAT (§5.6 reserves force for that, and Link's
    // header names bridging an external clock as its legitimate use).
    if (phased && (!linkSnapped_ || event.snapped)) {
        link_->snap(event.bpm, beat, at, quantum);
        lastLinkBpm_ = event.bpm;
        linkSnapped_ = true;
        barsApart_ = 0;
        return;
    }
    if (!phased) {
        sendTempo(event.bpm);
        return;
    }

    // Afterwards the phase is *measured* on every beat and pulled back with the tempo, a little
    // at a time, which peers follow smoothly — where a force on every beat would jerk every
    // peer's playhead by whatever the beat's stamp jittered.
    double apart = link_->phaseAtTime(at, quantum) - beat; // positive: the session is ahead
    apart -= quantum * std::floor(apart / quantum + 0.5);    // the nearer way round the bar
    const double wholeBeats = std::round(apart);
    const double error = apart - wholeBeats; // within a beat, -0.5 to 0.5
    if (wholeBeats != 0.0) {
        // The session is on a different beat of the bar from the one the tracker is counting —
        // the tracker changed its mind about where the bar starts, which is a musical event,
        // not drift. A tempo nudge would take a hundred beats over a whole beat, so a bar's
        // worth of disagreement in a row is answered the way a DOWNBEAT is.
        if (++barsApart_ >= event.beatsPerBar) {
            link_->snap(event.bpm, beat, at, quantum);
            lastLinkBpm_ = event.bpm;
            barsApart_ = 0;
            return;
        }
    } else {
        barsApart_ = 0;
    }
    if (std::abs(error) < kLinkPhaseDeadband) {
        sendTempo(event.bpm);
        return;
    }
    // A session ahead is slowed and one behind is hurried, by the share of the error a beat
    // should take out. `setTempo` pins the beat at `at` and changes the tempo from there, so
    // the nudge moves nobody's playhead: it changes how fast the next beat arrives.
    const double nudge = std::clamp(error / kLinkNudgeBeats, -kLinkMaxNudge, kLinkMaxNudge);
    sendTempo(event.bpm * (1.0 - nudge));
}

} // namespace takt4::output
