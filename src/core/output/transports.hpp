#pragma once

#include "core/dmx/artnet_publisher.hpp"
#include "core/dmx/dmx_engine.hpp"
#include "core/dmx/fixture.hpp"
#include "core/output/link_session.hpp"
#include "core/output/midi_clock.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/output/output_target.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takt4::output {

/// Everything takt4 sends, behind one object: Ableton Link, OSC and MIDI clock (§5.6).
///
/// This owns the transports and the one part of driving them that is genuinely subtle —
/// how a called beat becomes a change to Link's timeline (§4.3, §5.6) — and knows nothing
/// about where the beats came from or which thread it is on. `OutputRunner` is the thread;
/// `takt4-cli track` and §5.9's window both go through the pair, so there is one
/// implementation of "what a beat does to the outputs" rather than one each.
///
/// Not thread-safe, and not meant to be: `OutputRunner` owns one and touches it from a
/// single thread. The exception is `setLatencySeconds`, which is documented below.
class Transports {
public:
    /// A host and port to send OSC to. Kept as the pair it always was for the callers that
    /// only ever wanted one — `setOscTargets` builds unnamed `OutputTarget`s from these.
    using OscTarget = std::pair<std::string, std::uint16_t>;

    struct Config {
        bool link = false;
        std::string oscPrefix = "/takt4";
        /// §5.6's *"multiple simultaneous targets"*, named so a rule can pick between them.
        std::vector<OutputTarget> outputs;
        std::optional<std::string> midiClockPort;
        /// §5.5's latency offset as the tracker has it, so the transports start out
        /// agreeing with it. `setLatencySeconds` keeps them agreeing when it is moved.
        double latencySeconds = 0.0;
        /// The lighting patch — what fixtures there are, where, and what their channels do.
        /// Empty is a rig with no lights on it, which costs nothing: no universes means no
        /// frames, and an Art-Net target with nothing to carry sends none.
        std::vector<dmx::Fixture> patch;
        /// How a MIDI device is opened by name. Empty is RtMidi, which is the application; a
        /// test supplies ports it can unplug, which is the only way to test a reconnect on a
        /// machine with nothing to unplug.
        std::function<std::unique_ptr<MidiOutput>(const std::string&)> openMidi;
    };

    /// How often a lost MIDI device is looked for again. Often enough that a cable plugged
    /// back in is sending within a beat or two; rarely enough that enumerating ports — which
    /// RtMidi does on every look — costs nothing.
    static constexpr double kMidiReconnectSeconds = 1.0;

    /// Builds the transports and applies `config` as their starting state. Throws if a
    /// named MIDI port is not on the machine.
    ///
    /// **Link and OSC are always built, whether or not they are switched on.** Link
    /// because `BeatEngine::setHostTimeSource` is handed a pointer to the session and the
    /// audio thread reads it every hop (§4.3): building the session on demand would mean
    /// destroying one under a running audio thread the first time an operator switched
    /// Link off, and no amount of ordering makes that safe. Neither costs anything idle —
    /// Link's constructor starts its service thread but "does not touch the network;
    /// nothing is visible to peers until the session is enabled", and a publisher with no
    /// targets sends to nobody. MIDI is the exception: it is a named device rather than a
    /// switch, so it is opened when one is chosen and closed when it is not.
    explicit Transports(const Config& config);

    Transports(const Transports&) = delete;
    Transports& operator=(const Transports&) = delete;

    /// Always there, switched on or not.
    LinkSession& link() const noexcept { return *link_; }
    OscPublisher& osc() const noexcept { return *osc_; }
    /// The lighting state and the effects running against it. Always there for the same
    /// reason Link and OSC are: an engine with no patch holds no universes and costs nothing,
    /// and building one on demand would mean replacing it under a running output thread.
    dmx::DmxEngine& dmx() const noexcept { return *dmx_; }
    dmx::ArtNetPublisher& artnet() const noexcept { return *artnet_; }
    /// Null unless a MIDI port is open. Non-const like `link()` and `osc()` above, and for
    /// the same reason: these belong to whichever thread owns the transports, and Phase 6's
    /// rules send notes and CCs down this one.
    MidiClock* midiClock() const noexcept { return midi_.get(); }
    /// The device the *clock* is going down, or null. A rule's notes no longer come this
    /// way — they go to whichever of `outputs()` the rule named, which may be this device
    /// or another one entirely. `midiTarget` is the routed answer.
    MidiOutput* midiPort() const noexcept;

    /// Whether anything is actually being sent. With nothing on, `publish` and `advance`
    /// still count beats and cost nothing else, which is what makes an app that has not
    /// been configured yet behave like one that has.
    bool any() const noexcept {
        return linkEnabled_ || osc_->targetCount() != 0 || midi_ || !midiDevices_.empty() ||
               artnet_->targetCount() != 0;
    }

    // --- what is switched on, and changing it -------------------------------------
    //
    // These mutate the transports, so they belong to whichever thread owns them — the
    // output thread, when an `OutputRunner` is driving. Post through the runner rather
    // than reaching for these; it applies them between rounds.

    bool linkEnabled() const noexcept { return linkEnabled_; }
    void setLinkEnabled(bool on);

    /// §5.6's targets, in the order a rule's routing mask indexes them.
    const std::vector<OutputTarget>& outputs() const noexcept { return outputs_; }
    /// Replaces the set. Rebuilds every OSC sender and opens or closes MIDI devices to
    /// match; a device that cannot be opened leaves that target unreachable and is reported
    /// through `lastError`-style throwing, as `setMidiClockPort` already is.
    void setOutputs(const std::vector<OutputTarget>& targets);

    /// Replaces the lighting patch and re-points every Art-Net target at whatever universes
    /// the new patch uses. Levels survive where they can — see `dmx::DmxEngine::setPatch`.
    void setPatch(std::vector<dmx::Fixture> patch);
    const std::vector<dmx::Fixture>& patch() const noexcept { return dmx_->patch(); }

    /// The OSC half of `outputs()`, as the host/port pairs the older callers use.
    std::vector<OscTarget> oscTargets() const;
    /// Replaces the outputs with these OSC targets, unnamed. What `takt4-cli` passes and
    /// what a settings file written before names existed still means.
    void setOscTargets(const std::vector<OscTarget>& targets);

    /// The MIDI device a rule routed to `name` should be sent down, or null when that
    /// target is not a MIDI one, is switched off, or could not be opened.
    MidiOutput* midiTarget(std::size_t index) const noexcept;
    /// Whether any target is selected by a rule's routing mask — see
    /// `OscPublisher::anyTargetIn`, which answers the same question for the OSC half.
    bool anyOutputIn(std::uint64_t outputs) const noexcept;

    /// The namespace every address is sent under (§5.6). Fixed at construction: it is
    /// what a receiver is configured to listen for, so changing it under a running rig
    /// would silently stop everything downstream.
    const std::string& oscPrefix() const noexcept { return oscPrefix_; }

    /// The port MIDI clock is going to, or empty for none. Opening throws if the port is
    /// not on the machine, and nothing is changed when it does.
    ///
    /// **The device may also be one of `outputs()`.** Each device is opened once and shared:
    /// a rig sending both 24 PPQN and §5.8's note messages down one cable is the ordinary
    /// case, and opening the same port twice is refused by some drivers.
    const std::optional<std::string>& midiClockPort() const noexcept { return midiClockPort_; }
    /// Picking the port that is already open is a no-op **unless that device is lost**, when it
    /// is an operator saying "try it now" and the port is reopened on the spot.
    void setMidiClockPort(const std::optional<std::string>& port);

    /// Every MIDI device in use that has stopped taking messages, by the name it was asked
    /// for — the clock's or a target's. Empty while everything is sending. See
    /// `MidiOutput::lost`; `advance` keeps trying to bring each one back.
    std::vector<std::string> lostMidiDevices() const;
    /// How many of them there are — the same question without building a list, for the output
    /// thread to ask every round.
    std::size_t lostMidiCount() const noexcept;

    /// Enables Link and starts the MIDI clock. `now` is the seconds-since-start clock
    /// `advance` and `publish` are given.
    void startOutputs(double now);
    void stopOutputs() noexcept;

    /// Ticks the MIDI clock up to `now` and republishes any OSC state that moved. Called
    /// every round, beat or no beat: the MIDI clock's 24 PPQN does not wait for one.
    void advance(double now, const tracking::TempoState& state);

    /// One beat, to whichever transports are on. `hostMicros` is the frame's §4.3 stamp
    /// — zero offline, where there is no host clock to align to and Link is left alone.
    ///
    /// The tempo state at the beat is not wanted here: `BeatEvent` already carries the
    /// tempo, the bar position and the meter, which is everything §5.6's addresses send.
    /// A caller that wants the rest of the state has it on `engine::EngineBeat`.
    void publish(const tracking::BeatEvent& event, std::int64_t hostMicros, double now);

    /// Follows §5.5's latency offset when the operator moves it. The tracker applies it
    /// to `event.time`; this is the same number applied to the host times the transports
    /// fire on, and the two must not be allowed to drift apart.
    ///
    /// It reaches all three transports. Link and the MIDI clock shift their grid by it in
    /// either direction; OSC can only ever wait, so a negative offset there lands the message
    /// ahead of the *next* beat instead. See `setOscOffsets`.
    ///
    /// The one member safe to call from another thread — a UI slider, §5.7's inbound OSC
    /// — because it writes a single atomic and reads nothing.
    void setLatencySeconds(double seconds) noexcept;
    double latencySeconds() const noexcept;

    /// Atomic so a UI can show them while the output thread is sending.
    std::uint64_t beats() const noexcept { return beats_.load(std::memory_order_relaxed); }
    std::uint64_t downbeats() const noexcept { return downbeats_.load(std::memory_order_relaxed); }

private:
    /// Hands the OSC publisher §5.5's offset and the current beat length, which is what lets
    /// a negative offset mean anything there. Called before anything is published or flushed.
    void setOscOffsets(double bpm) noexcept;

    void publishToLink(const tracking::BeatEvent& event, std::int64_t hostMicros,
                       std::int64_t latencyMicros);

    /// Opens `device` if it is not already open, and hands back the shared port. Null when
    /// the name is empty; throws when the device is not on the machine.
    MidiOutput* openDevice(const std::string& device);
    /// Closes every device nothing names any more — the clock's or a target's.
    void closeUnusedDevices() noexcept;
    /// Tries to bring back every lost MIDI device, at most once per `kMidiReconnectSeconds`.
    void maintainMidi(double now) noexcept;

    std::function<std::unique_ptr<MidiOutput>(const std::string&)> openMidi_;
    /// When `maintainMidi` last looked; negative before it ever has.
    double midiMaintainedAt_ = -1.0;

    std::atomic<std::int64_t> latencyMicros_;
    /// Never null, and never replaced: the audio thread holds a pointer to the session.
    std::unique_ptr<LinkSession> link_;
    std::unique_ptr<OscPublisher> osc_;
    /// The lighting half. Declared before `artnet_` because the publisher reads the engine
    /// every round and destruction runs in reverse.
    std::unique_ptr<dmx::DmxEngine> dmx_;
    std::unique_ptr<dmx::ArtNetPublisher> artnet_;
    /// Every MIDI device in use, by name, opened once however many things name it — the
    /// clock and any number of §5.8's rule targets. See `midiClockPort`.
    std::map<std::string, std::unique_ptr<MidiOutput>> midiDevices_;
    std::unique_ptr<MidiClock> midi_;
    bool linkEnabled_ = false;
    bool started_ = false;
    /// The most recent time the transports were driven with. A MIDI port opened mid-set
    /// has to start its clock from now: starting it from when the *outputs* started would
    /// make `advance` try to emit every tick since.
    double lastNow_ = 0.0;
    /// §5.6's targets, and the MIDI port each one resolved to — null for an OSC target, for
    /// one that is switched off, and for a device that would not open. Parallel to
    /// `outputs_` so a rule's routing bit indexes both.
    std::vector<OutputTarget> outputs_;
    std::vector<MidiOutput*> outputPorts_;
    std::string oscPrefix_;
    std::optional<std::string> midiClockPort_;
    double lastLinkBpm_ = -1.0;
    std::atomic<std::uint64_t> beats_{0};
    std::atomic<std::uint64_t> downbeats_{0};
};

} // namespace takt4::output
