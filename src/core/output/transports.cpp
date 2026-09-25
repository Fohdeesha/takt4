#include "core/output/transports.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
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
    // The patch first: every Art-Net node is fed the universes the patch uses.
    dmx_->setPatch(config.patch);
    std::vector<OutputTarget> outputs = config.outputs;
    if (config.link) {
        (void)ensureLinkOutput(outputs, true);
        outputs.front().enabled = true;
    }
    if (config.midiClockPort && !config.midiClockPort->empty()) {
        const bool named = std::any_of(outputs.begin(), outputs.end(), [&](const OutputTarget& t) {
            return t.kind == OutputTarget::Kind::MidiClock && t.device == *config.midiClockPort;
        });
        if (!named) {
            OutputTarget clock;
            clock.kind = OutputTarget::Kind::MidiClock;
            clock.device = *config.midiClockPort;
            clock.name = clock.device;
            clock.id = newOutputId(outputs);
            outputs.push_back(std::move(clock));
        }
    }
    setOutputs(outputs);
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
}

void Transports::stopOutputs() noexcept {
    stopClock();
    started_ = false;
    link_->enable(false);
}

void Transports::startClock(double now) {
    clockRunning_ = true;
    lastNow_ = now;
    for (Clock& clock : clocks_) {
        // Ticks from the press, so a receiver has the tempo; Start waits for the first locked
        // downbeat (the audit's M19). See `publishClocks`.
        clock.clock->startTicking(now);
    }
}

void Transports::stopClock() noexcept {
    if (clockRunning_) {
        for (Clock& clock : clocks_) {
            clock.clock->stop();
        }
    }
    clockRunning_ = false;
}

void Transports::setLinkEnabled(bool on) {
    for (OutputTarget& target : outputs_) {
        if (target.kind == OutputTarget::Kind::Link) {
            target.enabled = on;
        }
    }
    applyLinkEnabled(on);
}

void Transports::applyLinkEnabled(bool on) {
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

MidiClock* Transports::midiClock() const noexcept {
    return clocks_.empty() ? nullptr : clocks_.front().clock.get();
}

MidiOutput* Transports::midiPort() const noexcept {
    if (clocks_.empty()) {
        return nullptr;
    }
    const auto found = midiDevices_.find(clocks_.front().device);
    return found == midiDevices_.end() ? nullptr : found->second.get();
}

std::uint64_t Transports::clockTicksSkipped() const noexcept {
    std::uint64_t skipped = 0;
    for (const Clock& clock : clocks_) {
        skipped += clock.clock->ticksSkipped();
    }
    return skipped;
}

std::optional<std::string> Transports::midiClockPort() const {
    for (const OutputTarget& target : outputs_) {
        if (target.enabled && target.kind == OutputTarget::Kind::MidiClock) {
            return target.device;
        }
    }
    return std::nullopt;
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
        bool wanted = false;
        for (const OutputTarget& target : outputs_) {
            if (target.enabled &&
                (target.kind == OutputTarget::Kind::Midi ||
                 target.kind == OutputTarget::Kind::MidiClock) &&
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
    clockProblems_.clear();
    std::vector<Clock> previous = std::move(clocks_);
    clocks_.clear();
    // Link's switch and delay come from the Link output — and a list with none leaves both as
    // they were, since a caller that never had one (`takt4-cli`, `setOscTargets`) switches Link
    // with `setLinkEnabled` and must not have it switched off by replacing its OSC targets.
    bool linkListed = false;
    bool linkOn = false;
    double linkDelay = linkDelay_;

    // The bit a rule's routing mask uses is the target's index in *this* list, so an OSC
    // publisher that only holds the OSC ones still has to be told which bit each is.
    std::string failures;
    const auto fail = [&failures](const std::string& name, const std::string& why) {
        // One target that will not open must not cost the others: a controller unplugged
        // since the preset was written is an ordinary state of the world, and the rest of
        // the rig should still be sending. Collected and raised once at the end so the
        // caller can say which.
        failures += failures.empty() ? "" : "; ";
        failures += name + ": " + why;
    };
    std::vector<OscPublisher::TargetSpec> osc;
    std::vector<dmx::ArtNetPublisher::TargetConfig> nodes;
    for (std::size_t i = 0; i < outputs_.size(); ++i) {
        const OutputTarget& target = outputs_[i];
        if (target.kind == OutputTarget::Kind::Link && !linkListed) {
            linkListed = true;
            linkDelay =
                std::clamp(target.delaySeconds, kMinOutputDelaySeconds, kMaxOutputDelaySeconds);
        }
        if (!target.enabled) {
            continue; // switched off sends nothing, of either kind
        }
        const double delay =
            std::clamp(target.delaySeconds, kMinOutputDelaySeconds, kMaxOutputDelaySeconds);
        switch (target.kind) {
        case OutputTarget::Kind::Osc:
            osc.push_back({target.id, target.host, target.port, i, delay});
            break;
        case OutputTarget::Kind::ArtNet: {
            dmx::ArtNetPublisher::TargetConfig node;
            node.host = target.host;
            node.port = target.port;
            node.delaySeconds = delay;
            node.bit = i;
            node.id = target.id;
            nodes.push_back(std::move(node));
            break;
        }
        case OutputTarget::Kind::Midi:
            try {
                outputPorts_[i] = openDevice(target.device);
            } catch (const std::exception& e) {
                fail(target.name, e.what());
            }
            break;
        case OutputTarget::Kind::MidiClock: {
            // One clock a device: two would tick it twice as fast.
            const auto ticking = std::find_if(clocks_.begin(), clocks_.end(), [&](const Clock& c) {
                return c.device == target.device;
            });
            if (ticking != clocks_.end()) {
                clockProblems_[i] = "\"" + outputs_[ticking->output].name +
                                    "\" already sends the clock to that device";
                break;
            }
            MidiOutput* port = nullptr;
            try {
                port = openDevice(target.device);
            } catch (const std::exception& e) {
                clockProblems_[i] = e.what();
                fail(target.name, e.what());
                break;
            }
            if (port == nullptr) {
                clockProblems_[i] = "no MIDI device chosen";
                break;
            }
            Clock clock;
            clock.output = i;
            clock.device = target.device;
            clock.delay = delay;
            // The same device's clock carries on — its ticks, its tempo and whether its
            // receiver has been told to play — so an edit to another row, or to this one's
            // delay or name, is not a Stop and a Start at the far end.
            const auto kept = std::find_if(previous.begin(), previous.end(), [&](const Clock& c) {
                return c.clock != nullptr && c.device == target.device;
            });
            if (kept != previous.end()) {
                clock.clock = std::move(kept->clock);
            } else {
                clock.clock = std::make_unique<MidiClock>(*port, 120.0);
                if (clockRunning_) {
                    // From now, not from when the clock started: `advance` would otherwise try
                    // to emit every tick of the set so far. And started on the next locked
                    // downbeat, like any other, rather than at the moment it was added.
                    clock.clock->startTicking(lastNow_);
                }
            }
            clocks_.push_back(std::move(clock));
            break;
        }
        case OutputTarget::Kind::Link:
            linkOn = true;
            break;
        }
    }
    // A clock no row names any more is told to stop, so its receiver is not left playing.
    for (Clock& gone : previous) {
        if (gone.clock != nullptr) {
            gone.clock->stop();
        }
    }
    previous.clear(); // before `closeUnusedDevices` closes a device one of them sent down
    linkDelay_ = linkDelay;
    if (linkListed && linkOn != linkEnabled_) {
        applyLinkEnabled(linkOn);
    }
    for (const auto& [bit, why] : osc_->setTargets(osc)) {
        fail(outputs_[bit].name, why);
    }
    for (const auto& [bit, why] : artnet_->setTargets(nodes)) {
        fail(outputs_[bit].name, why);
    }
    closeUnusedDevices();
    if (!failures.empty()) {
        throw std::runtime_error(failures);
    }
}

bool Transports::setOutputDelay(std::string_view id, double seconds) {
    for (std::size_t i = 0; i < outputs_.size(); ++i) {
        if (id.empty() || outputs_[i].id != id) {
            continue;
        }
        const double delay = std::clamp(seconds, kMinOutputDelaySeconds, kMaxOutputDelaySeconds);
        outputs_[i].delaySeconds = delay;
        switch (outputs_[i].kind) {
        case OutputTarget::Kind::Osc:
            osc_->setDelay(i, delay);
            break;
        case OutputTarget::Kind::ArtNet:
            (void)artnet_->setDelay(i, delay);
            break;
        case OutputTarget::Kind::MidiClock:
            for (Clock& clock : clocks_) {
                if (clock.output == i) {
                    clock.delay = delay;
                }
            }
            break;
        case OutputTarget::Kind::Link:
            linkDelay_ = delay;
            break;
        case OutputTarget::Kind::Midi:
            break; // `RuleSink` reads it from `outputs()` as each message is held
        }
        return true;
    }
    return false;
}

std::vector<std::string> Transports::outputProblems() const {
    // One per output, in the order the outputs are listed, so a status line built from these
    // reads in the order the rows do.
    std::vector<std::string> byOutput(outputs_.size());
    const auto note = [&](std::size_t bit, const std::string& why) {
        // Still being looked up is not a problem yet, and saying so would flash a warning on
        // every launch with a named target.
        if (why.empty() || why.rfind("looking up", 0) == 0 || bit >= outputs_.size()) {
            return;
        }
        byOutput[bit] = outputs_[bit].name + ": " + why;
    };
    for (std::size_t i = 0; i < osc_->targetCount(); ++i) {
        note(osc_->outputOf(i), osc_->target(i).problem());
    }
    for (std::size_t i = 0; i < artnet_->targetCount(); ++i) {
        note(artnet_->outputOf(i), artnet_->target(i).problem());
    }
    // And a MIDI output whose device would not open. It used to be said once, from the command
    // that opened it, and then written over by whatever the status line said next — a host
    // that would not resolve, most often, which is found out later.
    for (std::size_t i = 0; i < outputs_.size() && i < outputPorts_.size(); ++i) {
        const OutputTarget& target = outputs_[i];
        if (target.enabled && target.kind == OutputTarget::Kind::Midi &&
            outputPorts_[i] == nullptr) {
            byOutput[i] = target.name + ": no MIDI device called \"" + target.device +
                          "\" — plug it in and press RESCAN";
        }
    }
    // And a MIDI clock that could not be had: its device is not here, or another clock has it.
    for (const auto& [row, why] : clockProblems_) {
        if (row >= outputs_.size()) {
            continue;
        }
        const OutputTarget& target = outputs_[row];
        byOutput[row] =
            target.name + ": " +
            (target.device.empty() || why.find("already") != std::string::npos ||
                     why == "no MIDI device chosen"
                 ? why
                 : "no MIDI device called \"" + target.device + "\" — plug it in and press RESCAN");
    }
    std::vector<std::string> problems;
    for (std::string& problem : byOutput) {
        if (!problem.empty()) {
            problems.push_back(std::move(problem));
        }
    }
    return problems;
}

void Transports::refreshTargets() noexcept {
    osc_->refresh();
    artnet_->refresh();
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
    const bool wanted = port && !port->empty();
    if (wanted && clocks_.size() == 1 && clocks_.front().device == *port) {
        // The same port again. Nothing to change — unless that device has gone, when picking
        // it again is the operator saying "try it now", and it used to be ignored because the
        // port was "already open" (the audit's H11a).
        if (MidiOutput* const current = midiPort(); current != nullptr && current->lost()) {
            (void)current->reconnect();
        }
        return;
    }
    if (wanted) {
        // Opened before anything is changed, so a port that will not open leaves the clock
        // that was working exactly where it was. `openDevice` shares it with any rule target
        // on the same cable rather than opening it twice.
        (void)openDevice(*port);
    }
    std::vector<OutputTarget> outputs;
    for (const OutputTarget& target : outputs_) {
        if (target.kind != OutputTarget::Kind::MidiClock) {
            outputs.push_back(target);
        }
    }
    if (wanted) {
        OutputTarget clock;
        clock.kind = OutputTarget::Kind::MidiClock;
        clock.device = *port;
        clock.name = *port;
        clock.id = newOutputId(outputs);
        outputs.push_back(std::move(clock));
    }
    setOutputs(outputs);
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
    for (Clock& clock : clocks_) {
        (void)clock.clock->advance(now);
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
        // The kinds a beat's messages are held for: OSC and MIDI, and Art-Net, whose lighting
        // starts early by the earliest node's lead. A switched-off output sends nothing, so its
        // delay asks for nothing either.
        if (target.enabled &&
            (target.kind == OutputTarget::Kind::Osc || target.kind == OutputTarget::Kind::Midi ||
             target.kind == OutputTarget::Kind::ArtNet)) {
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
    for (Clock& clock : clocks_) {
        MidiClock& midi = *clock.clock;
        midi.setTempo(event.bpm);
        // The beat's own time, not the round that drained it: the audio arrived a pipeline's
        // worth of time before the beat was called, and the clock used to be synced to when it
        // was *drained* — late by that pipeline, plus the offset (§5.5). The clock steers
        // towards the beat rather than jumping to it; see `MidiClock::syncToBeat`. And this
        // clock's own delay on top, like every output's.
        const double heardAt = beatTime + static_cast<double>(latencyMicros) / 1e6 + clock.delay;
        midi.syncToBeat(heardAt);
        // A receiver's bar 1 is the first tick after Start, so Start waits for a locked beat to
        // say where the bars are, and then for the downbeat of the grid it sits on. Given again
        // on every locked beat until it has gone, so the grid follows the tempo meanwhile.
        if (event.locked && event.bpm > 0.0 && midi.waitingToStart()) {
            const double beat = 60.0 / event.bpm;
            const std::uint32_t meter = std::max<std::uint32_t>(event.beatsPerBar, 1);
            const std::uint32_t inBar = std::clamp<std::uint32_t>(event.beatInBar, 1, meter);
            midi.startOnDownbeat(heardAt - static_cast<double>(inBar - 1) * beat,
                                 static_cast<double>(meter) * beat);
        }
    }
    if (linkEnabled_) {
        // The Link output's delay goes where the rig's latency does: under the beat's stamp.
        publishToLink(event, hostMicros, latencyMicros + toMicros(linkDelay_));
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
