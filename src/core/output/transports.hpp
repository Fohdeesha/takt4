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
        /// A Link output switched on, for a caller that has no outputs list of its own —
        /// `takt4-cli --link`. Added to `outputs` when they have no Link output; switches the
        /// one they have on when they do.
        bool link = false;
        std::string oscPrefix = "/takt4";
        /// §5.6's *"multiple simultaneous targets"*, named so a rule can pick between them —
        /// and, since 2026-09-25, Link and the MIDI clocks with them.
        std::vector<OutputTarget> outputs;
        /// A MIDI clock output to this device, for a caller with no outputs list — `takt4-cli
        /// --midi-clock`. Added to `outputs` unless a clock there already names the device.
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
    /// The first MIDI clock output's clock, or null when there is none open — what a caller
    /// with one clock in mind reads (`takt4-cli`, the tests). `clocks` has every one.
    MidiClock* midiClock() const noexcept;
    /// The device the first clock is going down, or null.
    MidiOutput* midiPort() const noexcept;
    /// How many MIDI clock outputs are open and ticking.
    std::size_t clockCount() const noexcept { return clocks_.size(); }
    /// MIDI clock ticks skipped after a stall, across every clock.
    std::uint64_t clockTicksSkipped() const noexcept;

    /// Whether anything is actually being sent. With nothing on, `publish` and `advance`
    /// still count beats and cost nothing else, which is what makes an app that has not
    /// been configured yet behave like one that has.
    bool any() const noexcept {
        return linkEnabled_ || osc_->targetCount() != 0 || !clocks_.empty() ||
               !midiDevices_.empty() || artnet_->targetCount() != 0;
    }

    // --- what is switched on, and changing it -------------------------------------
    //
    // These mutate the transports, so they belong to whichever thread owns them — the
    // output thread, when an `OutputRunner` is driving. Post through the runner rather
    // than reaching for these; it applies them between rounds.

    bool linkEnabled() const noexcept { return linkEnabled_; }
    /// Switches Link on or off — the Link output's switch, when there is one, and the session
    /// either way. What `takt4-cli --link` and §5.7's control surfaces set.
    void setLinkEnabled(bool on);
    /// The Link output's delay: the timeline is put under each beat this much later.
    double linkDelaySeconds() const noexcept { return linkDelay_; }

    /// §5.6's targets, in the order a rule's routing mask indexes them.
    const std::vector<OutputTarget>& outputs() const noexcept { return outputs_; }
    /// Replaces the set, **keeping every sender whose output did not change** — see
    /// `OscPublisher::setTargets` and `dmx::ArtNetPublisher::setTargets` — and opening or
    /// closing MIDI devices to match. Nothing here waits on a name server: a host typed as a
    /// name is looked up on a thread of its own (`net::AsyncAddress`), and one that will not
    /// resolve is an `outputProblems` entry rather than a stall of the output thread (the
    /// audit's H12). A device or socket that cannot be had at all leaves that target
    /// unreachable and is reported by throwing, as `setMidiClockPort` already is.
    ///
    /// Link follows the Link output in `targets` — its switch and its delay — and a list with no
    /// Link output leaves Link as it was, for the callers that never had one and switch it with
    /// `setLinkEnabled` instead.
    void setOutputs(const std::vector<OutputTarget>& targets);

    /// Moves one output's delay — the one with this `OutputTarget::id` — and nothing else:
    /// what a dragged delay slider sends, where a whole `setOutputs` per pixel reopened senders
    /// and flushed what was held (the audit's H12). Every kind: an OSC sender's queue, a MIDI
    /// target's held notes, an Art-Net node's frames, a clock's grid, Link's timeline. False
    /// when no output has that id.
    bool setOutputDelay(std::string_view id, double seconds);
    /// How far ahead of a beat's moment its lighting has to start for the earliest Art-Net node
    /// to have it on time — see `dmx::ArtNetPublisher::leadSeconds`. `RuleSink` starts a beat's
    /// effects this much early; every node is then sent the lighting its own delay after that.
    double lightingLeadSeconds() const noexcept { return artnet_->leadSeconds(); }

    /// Every output that cannot be sent to and why, as "name: reason" — a host name that will
    /// not resolve, most often. Not one still being looked up: that is not a problem yet.
    std::vector<std::string> outputProblems() const;
    /// Lets every OSC and Art-Net target find its address and open its socket, and try a
    /// failed look-up again when it is due. The output thread calls it now and then, so a
    /// target that nothing is being sent to still resolves.
    void refreshTargets() noexcept;

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

    /// The device the first MIDI clock output names, or empty for none.
    ///
    /// **A clock's device may also be one of the MIDI outputs.** Each device is opened once and
    /// shared: a rig sending both 24 PPQN and §5.8's note messages down one cable is the
    /// ordinary case, and opening the same port twice is refused by some drivers.
    std::optional<std::string> midiClockPort() const;
    /// One MIDI clock output to `port`, in place of every clock there was, or none — what a
    /// caller with one clock in mind sets (`takt4-cli --midi-clock`, the tests). Opening throws
    /// if the port is not on the machine, and nothing is changed when it does. Picking the port
    /// that is already open is a no-op **unless that device is lost**, when it is an operator
    /// saying "try it now" and the port is reopened on the spot.
    void setMidiClockPort(const std::optional<std::string>& port);

    /// Every MIDI device in use that has stopped taking messages, by the name it was asked
    /// for — the clock's or a target's. Empty while everything is sending. See
    /// `MidiOutput::lost`; `advance` keeps trying to bring each one back.
    std::vector<std::string> lostMidiDevices() const;
    /// How many of them there are — the same question without building a list, for the output
    /// thread to ask every round.
    std::size_t lostMidiCount() const noexcept;

    /// Joins Link, if its switch is on. `now` is the seconds-since-start clock `advance` and
    /// `publish` are given. **Not the MIDI clock** — see `startClock`.
    void startOutputs(double now);
    /// Leaves Link and stops the MIDI clock.
    void stopOutputs() noexcept;

    /// Begins ticking the MIDI clock, and sends Start on the first locked downbeat after it
    /// (the audit's M19; see `MidiClock::startOnDownbeat`); `stopClock` sends Stop.
    ///
    /// Its own switch, not the outputs', since the outputs run for the application's whole
    /// life (the audit's H5): a drum machine or a sequencer given a Start the moment takt4
    /// opened would play at the clock's opening tempo before anything was listening. It follows
    /// the tracker's Start and Stop instead — `OutputRunner::setTracking`.
    void startClock(double now);
    void stopClock() noexcept;
    bool clockRunning() const noexcept { return clockRunning_; }

    /// Ticks the MIDI clock up to `now` and republishes any OSC state that moved. Called
    /// every round, beat or no beat: the MIDI clock's 24 PPQN does not wait for one.
    void advance(double now, const tracking::TempoState& state);

    /// One beat's effect on the two clocks: the MIDI clock is steered towards it and Link's
    /// timeline is put under it. Called as each beat is **detected**, whenever that is — both
    /// are grids that run on from a beat already heard, so neither needs it predicted.
    ///
    /// `beatTime` is when the beat was in the audio, on the clock `advance` is given;
    /// `hostMicros` is the same instant on Link's clock, the frame's §4.3 stamp — zero
    /// offline, where there is no host clock to align to and Link is left alone.
    ///
    /// The tempo state at the beat is not wanted here: `BeatEvent` already carries the
    /// tempo, the bar position and the meter, which is everything §5.6's addresses send.
    /// A caller that wants the rest of the state has it on `engine::EngineBeat`.
    void publishClocks(const tracking::BeatEvent& event, std::int64_t hostMicros,
                       double beatTime);
    /// One beat's messages: §5.6's namespace to every OSC target, each held until the beat's
    /// `moment` plus the rig's offset plus its own delay. The moment is on the clock `advance`
    /// is given, and ahead of `now` when the output thread fires the beat on a prediction.
    /// Counted in `beats()`.
    void publishBeat(const tracking::BeatEvent& event, double moment, double now);
    /// Both at once, with the beat's moment taken to be now — for a caller with no scheduler
    /// of its own: `takt4-cli`'s file mode, and the tests.
    void publish(const tracking::BeatEvent& event, std::int64_t hostMicros, double beatTime);

    /// How far ahead of a beat's own moment it must be fired for the earliest output to have
    /// it on time: the rig's offset plus the most negative delay of any OSC, MIDI or Art-Net
    /// output, and never later than the beat itself. Zero or negative. See `OutputRunner`. A
    /// MIDI clock and Link are grids that run on from a beat already heard, so they ask for
    /// nothing here whatever their delay.
    double leadSeconds() const noexcept;
    /// How long after a beat's own moment the latest output still wants it: the rig's offset
    /// plus the largest delay, and never less than zero. What tells a beat heard late from one
    /// so stale that firing it would be a burst of cues for music long gone.
    double tailSeconds() const noexcept;

    /// Follows §5.5's latency offset when the operator moves it. The tracker applies it
    /// to `event.time`; this is the same number applied to the times the transports fire on,
    /// and the two must not be allowed to drift apart. `ui::WindowController::postOptions` is
    /// what sends it, every time the setting changes — until the audit (C6) nothing did, and the
    /// slider moved nothing until the next launch.
    ///
    /// It reaches everything. Link and the MIDI clock shift their grid by it in either
    /// direction. A beat's messages — the namespace and every rule's OSC, MIDI and lighting —
    /// go at the beat's moment plus it, and a negative offset is honoured because the output
    /// thread fires a locked beat ahead of time on a prediction. A message about something
    /// already happened can only go now; see `RuleSink`.
    ///
    /// The one member safe to call from another thread — a UI slider, §5.7's inbound OSC
    /// — because it writes a single atomic and reads nothing.
    void setLatencySeconds(double seconds) noexcept;
    double latencySeconds() const noexcept;

    /// Atomic so a UI can show them while the output thread is sending.
    std::uint64_t beats() const noexcept { return beats_.load(std::memory_order_relaxed); }
    std::uint64_t downbeats() const noexcept { return downbeats_.load(std::memory_order_relaxed); }

private:
    /// Hands the OSC publisher §5.5's offset. Called before anything is published or flushed.
    void setOscOffset() noexcept;

    /// Link's half of `publishClocks`: nothing while hunting, a forced phase on the first
    /// locked beat, and a nudged tempo after that. See the definition.
    void publishToLink(const tracking::BeatEvent& event, std::int64_t hostMicros,
                       std::int64_t latencyMicros);

    /// Opens `device` if it is not already open, and hands back the shared port. Null when
    /// the name is empty; throws when the device is not on the machine.
    MidiOutput* openDevice(const std::string& device);
    /// Closes every device nothing names any more — the clock's or a target's.
    void closeUnusedDevices() noexcept;
    /// Tries to bring back every lost MIDI device, at most once per `kMidiReconnectSeconds`.
    void maintainMidi(double now) noexcept;
    /// Joins or leaves Link's session, and starts its phase afresh.
    void applyLinkEnabled(bool on);

    /// One MIDI clock output: its row, its device and the clock ticking down it.
    struct Clock {
        std::size_t output = 0;
        std::string device;
        std::unique_ptr<MidiClock> clock;
        double delay = 0.0;
    };

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
    /// Every MIDI clock output that is switched on and open, in the order of `outputs_`.
    std::vector<Clock> clocks_;
    /// Clock outputs that could not be had, by row: a device not on the machine, or one
    /// another clock already ticks. Said by `outputProblems`.
    std::map<std::size_t, std::string> clockProblems_;
    bool linkEnabled_ = false;
    double linkDelay_ = 0.0;
    bool started_ = false;
    /// Whether the MIDI clock is meant to be ticking — see `startClock`.
    bool clockRunning_ = false;
    /// The most recent time the transports were driven with. A MIDI port opened mid-set
    /// has to start its clock from now: starting it from when the *outputs* started would
    /// make `advance` try to emit every tick since.
    double lastNow_ = 0.0;
    /// §5.6's targets, and the MIDI port each MIDI one resolved to — null for every other
    /// kind, for one that is switched off, and for a device that would not open. Parallel to
    /// `outputs_` so a rule's routing bit indexes both.
    std::vector<OutputTarget> outputs_;
    std::vector<MidiOutput*> outputPorts_;
    std::string oscPrefix_;
    double lastLinkBpm_ = -1.0;
    /// Whether Link's phase has been forced under the music since the tracker last locked —
    /// false again whenever it hunts, Link is switched on, the outputs start or a peer joins.
    bool linkSnapped_ = false;
    /// Link's peer count at the last beat, so a new peer is noticed.
    std::size_t linkPeers_ = 0;
    /// Locked beats in a row on which Link's session sat on a different beat of the bar from
    /// the tracker's. A bar's worth forces the phase again.
    std::uint32_t barsApart_ = 0;
    std::atomic<std::uint64_t> beats_{0};
    std::atomic<std::uint64_t> downbeats_{0};
};

} // namespace takt4::output
