#pragma once

#include "core/audio/host_time.hpp"
#include "core/control/rule_control.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/output/beat_scheduler.hpp"
#include "core/output/rule_sink.hpp"
#include "core/output/transports.hpp"
#include "core/trigger/trigger_engine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace takt4::output {

/// A change to what the transports are sending.
///
/// Posted by whoever owns the controls — a window, §5.7's inbound OSC later — and applied
/// by the output thread between rounds, because the transports belong to it. Exactly the
/// shape `engine::Command` has for the tracker, and for the same reason: many producers,
/// one consumer, and no caller reaching into something another thread is using.
struct OutputCommand {
    enum class Kind : std::uint8_t {
        LinkEnabled,
        OscTargets,
        MidiClockPort,
        /// §5.6's named targets, whole. Every rule's routing is resolved again afterwards,
        /// because a name means a *bit*, and the bit a name resolves to moves the moment a
        /// target above it is added or deleted.
        Outputs,
        /// §5.8's rules, whole. Replacing the set rather than editing one is what a preset
        /// load does and what §5.9's editor will do on every change; a rule is small and the
        /// set is short, so there is no reason for a finer command.
        Rules,
        /// §5.8's PANIC, and letting go of it.
        Panic,
        /// §5.7's `/ctl/rule/<id>/enable <0|1>`.
        RuleEnabled,
        /// §5.8's *"on manual hotkey"*, and §5.9's per-rule `[test]` button.
        Manual,
        TestRule,
        /// The lighting patch, whole — the same shape and the same reasoning as `Rules`.
        /// Every rule's fixture routing is resolved again afterwards, because a name means a
        /// *bit* and the bit moves the moment a fixture above it is added or deleted.
        Patch,
        /// §5.7's `/ctl/rule/<id>/mute <0|1>`. Distinct from `RuleEnabled`, which is a
        /// different state — see `trigger::Rule::muted`.
        RuleMuted,
        /// §5.7's `/ctl/rule/<id>/double`, `halve`, `rate <f>` and `reset`, which are all one
        /// thing: a multiplier on how often the rule fires. `factor` is absolute when
        /// `relative` is false and is applied to whatever the rule is already at when it is
        /// true, which is what lets "double" be pressed twice.
        RuleRate,
        /// One lighting effect, fired by hand rather than by a rule — the patch editor's
        /// IDENTIFY, which drives a fixture to full so an operator in the truss can see which
        /// lamp they are addressing.
        ///
        /// Its own command rather than a rule, because it is not one: it has no trigger, no
        /// conditions and no place in the show, and making it a temporary rule would put it in
        /// the fire log and the fire counts.
        Effect,
        /// One raw DMX channel held at a value for a moment — the patch editor's per-channel
        /// TEST. `DmxEngine::holdChannel` says why this is addressed by channel rather than by
        /// role, and why it is not an `Effect`.
        ChannelTest,
        /// The tracker started or stopped listening. See `OutputRunner::setTracking`.
        Tracking,
        /// Nothing at all: what `OutputRunner::sync` waits on, so that everything posted before
        /// it has been applied by the time it has.
        Sync,
        /// One output's delay, by `OutputTarget::id` in `ruleId`, seconds in `factor` — what
        /// the delay slider sends while it is dragged, where a whole `Outputs` per pixel
        /// reopened senders and flushed what was held (the audit's H12).
        OutputDelay,
    };

    static OutputCommand linkEnabled(bool on) {
        OutputCommand command;
        command.kind = Kind::LinkEnabled;
        command.enabled = on;
        return command;
    }
    static OutputCommand rules(std::vector<trigger::Rule::Config> configs) {
        OutputCommand command;
        command.kind = Kind::Rules;
        command.ruleConfigs = std::move(configs);
        return command;
    }
    static OutputCommand panic(bool on) {
        OutputCommand command;
        command.kind = Kind::Panic;
        command.enabled = on;
        return command;
    }
    static OutputCommand ruleEnabled(std::string id, bool on) {
        OutputCommand command;
        command.kind = Kind::RuleEnabled;
        command.ruleId = std::move(id);
        command.enabled = on;
        return command;
    }
    static OutputCommand manual() {
        OutputCommand command;
        command.kind = Kind::Manual;
        return command;
    }
    static OutputCommand testRule(std::string id) {
        OutputCommand command;
        command.kind = Kind::TestRule;
        command.ruleId = std::move(id);
        return command;
    }
    static OutputCommand oscTargets(std::vector<Transports::OscTarget> targets) {
        OutputCommand command;
        command.kind = Kind::OscTargets;
        command.targets = std::move(targets);
        return command;
    }
    static OutputCommand outputs(std::vector<OutputTarget> targets) {
        OutputCommand command;
        command.kind = Kind::Outputs;
        command.outputTargets = std::move(targets);
        return command;
    }
    static OutputCommand midiClockPort(std::optional<std::string> port) {
        OutputCommand command;
        command.kind = Kind::MidiClockPort;
        command.port = std::move(port);
        return command;
    }
    static OutputCommand patch(std::vector<dmx::Fixture> fixtures) {
        OutputCommand command;
        command.kind = Kind::Patch;
        command.fixtures = std::move(fixtures);
        return command;
    }
    static OutputCommand ruleMuted(std::string id, bool on) {
        OutputCommand command;
        command.kind = Kind::RuleMuted;
        command.ruleId = std::move(id);
        command.enabled = on;
        return command;
    }
    static OutputCommand ruleRate(std::string id, double factor, bool relative) {
        OutputCommand command;
        command.kind = Kind::RuleRate;
        command.ruleId = std::move(id);
        command.factor = factor;
        command.relative = relative;
        return command;
    }
    static OutputCommand effect(std::uint64_t fixtures, const dmx::Payload& payload) {
        OutputCommand command;
        command.kind = Kind::Effect;
        command.fixtureMask = fixtures;
        command.payload = payload;
        return command;
    }
    static OutputCommand tracking(bool on) {
        OutputCommand command;
        command.kind = Kind::Tracking;
        command.enabled = on;
        return command;
    }
    static OutputCommand outputDelay(std::string outputId, double seconds) {
        OutputCommand command;
        command.kind = Kind::OutputDelay;
        command.ruleId = std::move(outputId);
        command.factor = seconds;
        return command;
    }
    static OutputCommand channelTest(dmx::PortAddress universe, std::uint16_t channel,
                                     std::uint8_t level, double seconds) {
        OutputCommand command;
        command.kind = Kind::ChannelTest;
        command.universe = universe;
        command.channel = channel;
        command.level = level;
        command.factor = seconds;
        return command;
    }

    Kind kind = Kind::LinkEnabled;
    /// Non-zero for a command somebody is waiting on — see `OutputRunner::postAndWait`.
    std::uint64_t ticket = 0;
    bool enabled = false;
    std::vector<Transports::OscTarget> targets;
    std::vector<OutputTarget> outputTargets;
    std::optional<std::string> port;
    std::vector<trigger::Rule::Config> ruleConfigs;
    std::vector<dmx::Fixture> fixtures;
    std::string ruleId;
    /// `Effect`'s target and instruction. The mask indexes the patch, exactly as a rule's
    /// `Message::fixtures` does.
    std::uint64_t fixtureMask = 0;
    dmx::Payload payload;
    /// `ChannelTest`'s target: a universe, a 1-based DMX channel and the byte to hold it at.
    /// How long for rides in `factor`, which is otherwise `RuleRate`'s.
    dmx::PortAddress universe = 0;
    std::uint16_t channel = 0;
    std::uint8_t level = 0;
    /// `RuleRate`'s multiplier, and whether it multiplies the rule's current rate or replaces
    /// it. See `Kind::RuleRate`. Also `ChannelTest`'s duration in seconds.
    double factor = 1.0;
    bool relative = false;
};

/// The rule id that means **every rule** — §5.7's `/ctl/rule/all/enable` and its neighbours.
///
/// A reserved word rather than a second address shape, because a Stream Deck button that
/// mutes the lighting rules and one that mutes a single rule should differ by one word in the
/// address and by nothing else. A rule actually *called* "all" is shadowed by it; that is a
/// naming collision the editor can warn about and not a reason to spell the common case
/// differently.
inline constexpr std::string_view kAllRules = "all";

/// HANDOFF §4.2's output thread.
///
/// §4.2 wants the transports off the audio thread *and* off the caller's loop, so that
/// neither a UI redraw nor a console print can decide when a beat reaches a peer. The
/// tracking already does not wait on anyone — the engine's inference thread runs at the
/// audio's pace — so what this buys is punctuality: a MIDI clock tick is 19 ms apart at
/// 130 BPM, and a window that redraws at 30 Hz would hand it 33 ms of jitter.
///
/// **This is the single consumer of `BeatEngine::popBeat`.** That ring is single-consumer
/// by `rt::SpscRing`'s contract, so nothing else may drain it while this runs. The *frame*
/// ring is a different ring with a different consumer — §5.9's activation trace — and is
/// not touched here.
///
/// The window owns one of these and reads beats only through it. `takt4-cli track`
/// drains the ring itself and runs no runner, which is the same rule kept the other way.
///
/// The caller still owns two things that have to happen around it:
///
///   * `engine.setHostTimeSource(&runner.hostTimeClock())` **before the audio stream is
///     opened**, because §4.3's stamp is taken on the audio thread and there has to be a
///     clock in place before there is one.
///   * Draining `popFrame`, if anything wants the frames. Nobody has to, but a ring that
///     nobody drains fills and the engine starts counting frames lost.
///
/// **It is also §5.7's `control::RuleControl`**, which is the answer to "how does an inbound
/// OSC message reach a rule". The rules are behind this queue and nothing else can reach
/// them, so the interface a control surface is written against is implemented by the one
/// object that owns them — the same dependency inversion `RuleSink` makes in the other
/// direction, and for the same reason: neither `core/control` nor `core/trigger` should have
/// to know what the other is.
class OutputRunner final : public control::RuleControl {
public:
    /// Called on the output thread for every beat, after the transports have had it — a
    /// console that prints them, a UI that flashes on one. Keep it short: it runs between
    /// a beat and the next MIDI tick, and a slow one is jitter.
    using BeatObserver = std::function<void(const engine::EngineBeat&)>;

    /// How often the thread wakes. A MIDI clock tick is 19 ms apart at 130 BPM, so the
    /// loop has to come round well inside that or the ticks inherit its period as jitter.
    static constexpr std::chrono::milliseconds kPeriod{1};

    /// How much later than the latest output wants it a beat may be heard and still be fired.
    /// The pipeline from audio to a called beat is tens of milliseconds; a beat heard a third
    /// of a second late is a backlog from a worker that stalled, and firing the lot would be a
    /// burst of cues for music already gone (the audit's M15). The clocks still take it.
    static constexpr double kStaleSeconds = 0.35;

    /// The engine must outlive this. The transports are built here and owned here, which
    /// is what makes "one thread touches them" structural rather than a comment: nothing
    /// else can reach a mutating member of them. Changes go through `post`.
    ///
    /// Throws whatever `Transports` throws — a MIDI port that is not on the machine.
    /// Nothing is sent until `start()`.
    OutputRunner(engine::BeatEngine& engine, const Transports::Config& config);
    ~OutputRunner() override;

    OutputRunner(const OutputRunner&) = delete;
    OutputRunner& operator=(const OutputRunner&) = delete;

    /// Set before `start()`; the thread reads it without synchronisation.
    void setBeatObserver(BeatObserver observer);

    /// Enables the transports and starts the thread. `start()` on a running runner does
    /// nothing.
    ///
    /// **An application starts this once and leaves it running** — the audit's H5, and the
    /// operator's call of 2026-09-23. It used to start and stop with the tracker, so before
    /// Start nothing was transmitted at all: the patch editor's IDENTIFY and TEST changed a
    /// buffer nobody sent, a Stop to change input dropped the Art-Net keep-alive so nodes fell
    /// back to their failsafe, and a control surface's PANIC reached rules that could not
    /// hear it. The tracker's Start and Stop reach this as `setTracking`.
    ///
    /// **The clock does not restart.** `elapsed()` runs from construction and only ever goes
    /// forwards, across as many stops and starts as an operator makes — see the note in the
    /// definition, which is a bug report. Throws what `Transports` throws, having put back
    /// anything it raised.
    void start();

    /// Stops the thread, drains whatever was still on the ring on the calling thread —
    /// the last beats of a set are still beats — sends every release still owed, blacks the
    /// lights out and transmits that last frame, and then stops the transports. The way out of
    /// the application. Safe to call twice, and called by the destructor.
    void stop() noexcept;

    /// The tracker has started, or stopped, listening. Any thread; it is a `post`.
    ///
    /// Started, the MIDI clock is sent Start and begins ticking. Stopped, it is sent Stop, and
    /// **the lights are blacked out** — every light-emitting channel to zero, and kept being
    /// sent: the operator's call of 2026-09-23 (Q3), for the rig takt4 is running. PANIC is
    /// different and freezes; see `dmx::DmxEngine::cancelAll`. Everything else goes on — Link on
    /// its own switch, OSC state, a rule fired by hand or from a control surface, and every
    /// release still owed, when it is due.
    void setTracking(bool on) { post(OutputCommand::tracking(on)); }
    /// Whether the last `setTracking` said the tracker was listening. Any thread.
    bool tracking() const noexcept { return tracking_.load(std::memory_order_relaxed); }

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Rounds the loop has made. What a test watches to tell "the thread is running" from
    /// "the thread does the right thing", which are two separate claims.
    std::uint64_t rounds() const noexcept { return rounds_.load(std::memory_order_relaxed); }

    /// Whether the output thread got a raised priority when it started (the audit's M13; see
    /// rt/thread_priority.hpp), as the thread itself saw it. False before it has started.
    bool threadRaised() const noexcept { return threadRaised_.load(std::memory_order_acquire); }

    /// Stages of a round that threw. A transport failing must not take the process down — an
    /// OSC target can go away mid-set — but it must not be silent either, and it must not take
    /// the rest of the round with it: see `guarded`, and `Snapshot::Trouble` for what is said.
    std::uint64_t errors() const noexcept { return errors_.load(std::memory_order_relaxed); }

    /// Seconds since this runner was **constructed**, on the steady clock the transports are
    /// driven from — and the clock `trigger::Context::now` is, so §5.8's cooldowns and
    /// follow-up delays are measured on it.
    ///
    /// Monotonic for the runner's whole life, stops and starts included. Anything comparing a
    /// remembered instant against "now" has to use this one and no other: a second clock with
    /// a different origin is what made §5.9's "12s ago" read minutes wrong. Safe from any
    /// thread, because nothing writes the origin after construction.
    double elapsed() const noexcept;

    /// The transports, for reading their **counters** and Link's own state, which are atomic
    /// and thread-safe respectively.
    ///
    /// **Not for anything else while the thread is running.** The mutating members are not
    /// reachable through a const reference, which is deliberate — but the *readable* ones are
    /// not safe either: `outputs()` hands back a reference to a vector of strings that the
    /// output thread replaces whole on an `Outputs` command, and `midiClockPort()` an optional
    /// string it reassigns. A 30 Hz reader copying that vector while a delay-slider drag posts
    /// sixty replacements through it is a data race with a freed buffer in the middle of it,
    /// not merely a number read half-written. Use `snapshot()`, which is taken under a lock.
    const Transports& transports() const noexcept { return transports_; }

    /// What the transports are set to, copied under a lock — everything a UI reads at redraw
    /// rate that `transports()` cannot safely hand it.
    ///
    /// Taken again by the output thread after every command it applies, so it is at worst one
    /// round — a millisecond — behind what is really being sent.
    struct Snapshot {
        /// §5.6's targets, in the order a rule's routing mask indexes them.
        std::vector<OutputTarget> outputs;
        bool link = false;
        /// The port MIDI clock is going down, and whether it is actually open. The two differ
        /// exactly when a device has been named that this machine has not got, which is the
        /// one failure an operator has to be told about.
        std::optional<std::string> midiClockPort;
        bool midiClockOpen = false;
        /// §5.6's namespace prefix. Fixed at construction and never reassigned, so reading it
        /// off the transports would in fact be safe — it is here so that the rule can be the
        /// simple one: a thread that is not the output thread reads the snapshot and nothing
        /// else. `WindowController::currentSettings` is the caller that has to save it.
        std::string oscPrefix;
        /// The lighting patch as the output thread has it, in the order a rule's fixture mask
        /// indexes it. Copied here for the reason `outputs` is: the live one is a vector the
        /// output thread replaces whole, and a UI reading it at 30 Hz would be reading a buffer
        /// being freed underneath it.
        std::vector<dmx::Fixture> patch;
        /// MIDI devices that have stopped taking messages, by the name they were asked for —
        /// `Transports::lostMidiDevices`. The output thread keeps trying them; the window says
        /// which, because a lighting desk or a sequencer that has stopped hearing takt4 is not
        /// otherwise visible from here at all.
        std::vector<std::string> lostMidi;
        /// Outputs that cannot be sent to and why — `Transports::outputProblems`. A host name
        /// that will not resolve is found out on a thread of its own, after the command that
        /// set it has long been answered, so this is where it is reported.
        std::vector<std::string> outputProblems;
        /// What went wrong on the output thread that nothing else says — the audit's M12,
        /// whose counters were kept and never shown. Counted since the runner started, which is
        /// launch: the thread runs for the application's life.
        struct Trouble {
            /// Stages of a round that threw (`errors`), and the last one, as "stage: what".
            std::uint64_t roundErrors = 0;
            std::string lastRoundError;
            /// Rule messages that reached no output — `RuleSink::undeliverable`.
            std::uint64_t undeliverable = 0;
            /// Messages held for an output's offset and dropped because too many were already
            /// waiting, in the sink and in the OSC publisher.
            std::uint64_t heldDropped = 0;
            /// MIDI clock ticks skipped after the thread stalled — `MidiClock::ticksSkipped`.
            std::uint64_t clockTicksSkipped = 0;
            bool operator==(const Trouble&) const = default;
        };
        Trouble trouble;
    };
    Snapshot snapshot() const;

    /// One rule's live switches as the output thread has them: the three things a control
    /// surface can change behind §5.9's editor. See `liveRules`.
    struct LiveRule {
        std::string id;
        bool enabled = true;
        bool muted = false;
        double rate = 1.0;
        bool operator==(const LiveRule&) const noexcept = default;
    };
    /// Every rule's live switches, copied under a lock after any command that could change one.
    /// The editor reads this so that a rule a Stream Deck muted or switched off shows as muted
    /// or off there too — it used to go on showing what the editor itself had last set, and the
    /// next edit it made put the rule back (the audit's H7).
    std::vector<LiveRule> liveRules() const;
    /// Moves whenever `liveRules` would give a different answer, so a redraw can skip the copy.
    std::uint64_t liveRulesVersion() const noexcept {
        return liveVersion_.load(std::memory_order_acquire);
    }

    /// One universe's 512 levels as the output thread last sent them, copied under a lock.
    /// Empty when the patch does not use that universe.
    ///
    /// **A mirror rather than a read through `transports()`**, and the reason is the one that
    /// method's own note gives. `DmxEngine::levels` looks its universe up in a vector the
    /// output thread *replaces* on every re-patch; a 30 Hz readout walking that vector while
    /// a patch edit swaps it is a data race with a freed buffer in the middle of it, not
    /// merely a byte read half-written.
    ///
    /// Refreshed at most `kMirrorHz` times a second, which is more than a screen can show and
    /// far less than the thousand rounds a second the engine actually moves in. So the number
    /// on screen can be up to a frame stale, which is not a difference anybody can see.
    std::vector<std::uint8_t> levelsOf(dmx::PortAddress universe) const;

    /// How often the level mirror is refreshed. Twice a typical redraw, so a UI reading it at
    /// 30 Hz never shows the same frame twice in a row.
    static constexpr double kMirrorHz = 60.0;

    /// Link's clock, for `engine::BeatEngine::setHostTimeSource`.
    ///
    /// §4.3's stamp is taken on the audio thread, so this has to be installed before the
    /// stream is opened. It is stable for this runner's life — the session is built once
    /// and switched on and off, never replaced — which is exactly why switching Link off
    /// mid-set cannot leave the audio thread holding a destroyed clock.
    audio::HostTimeSource& hostTimeClock() noexcept { return transports_.link(); }

    /// §5.5's latency offset. Any thread: it writes one atomic.
    void setLatencySeconds(double seconds) noexcept { transports_.setLatencySeconds(seconds); }

    /// Asks for a change to what is being sent. Any thread; applied by the output thread
    /// before its next round, so it has happened within a millisecond. Applied
    /// immediately on the calling thread when the runner is not running, which is what
    /// lets an app be configured before it is started.
    void post(OutputCommand command);

    /// `post`, and wait for the output thread to have applied it: what went wrong applying
    /// **this** command, empty for nothing — or no answer at all when it has not been applied
    /// within `kWaitForApply`, when the caller should rely on `lastError` later.
    ///
    /// For the few changes a window reports on the spot — a MIDI port that is not on the
    /// machine, an output that will not open. Since the runner runs for the application's
    /// whole life a `post` is always asynchronous, and `lastError()` read straight after one is
    /// the *previous* command's answer. From any thread but the output thread.
    std::optional<std::string> postAndWait(OutputCommand command);

    /// Waits until everything posted before this call has been applied. True when it has been,
    /// within `kWaitForApply`. For whoever has to read what a change did — a test, mostly.
    bool sync() {
        OutputCommand command;
        command.kind = OutputCommand::Kind::Sync;
        return postAndWait(std::move(command)).has_value();
    }

    /// Calls `look(rules, transports, sink)` **between two of the output thread's rounds**, and
    /// returns what it returns — the one safe way to read the rules or the transports from
    /// another thread while the runner is running, which since the audit's H5 is always. It
    /// holds the output thread up for as long as `look` takes, so keep it to copying things out.
    template <typename Look>
    decltype(auto) inspect(Look&& look) const {
        const std::lock_guard<std::mutex> owner(ownerMutex_);
        return std::forward<Look>(look)(static_cast<const trigger::TriggerEngine&>(triggers_),
                                        static_cast<const Transports&>(transports_),
                                        static_cast<const RuleSink&>(sink_));
    }

    /// How long `postAndWait` waits. A round is a millisecond; this is long enough for a busy
    /// one and short enough that a window never visibly stalls on a change — a hostname being
    /// resolved on the output thread can take the resolver's whole timeout.
    static constexpr std::chrono::milliseconds kWaitForApply{250};

    /// §5.7's `/ctl/panic` and `/ctl/rule/<id>/enable`, for a `control::ControlSurface`.
    ///
    /// Both are `post` under another name — the rules belong to the output thread, so there
    /// is no other way in — and both are safe from a socket's thread or RtMidi's callback
    /// for exactly that reason. A rule id that names nothing is applied to nothing and is
    /// not an error; see `RuleControl`.
    void panic(bool engaged) override { post(OutputCommand::panic(engaged)); }
    void setRuleEnabled(std::string_view id, bool enabled) override {
        post(OutputCommand::ruleEnabled(std::string(id), enabled));
    }
    void setRuleMuted(std::string_view id, bool muted) override {
        post(OutputCommand::ruleMuted(std::string(id), muted));
    }
    void setRuleRate(std::string_view id, double factor, bool relative) override {
        post(OutputCommand::ruleRate(std::string(id), factor, relative));
    }
    void fireManual() override { post(OutputCommand::manual()); }

    /// What went wrong applying the last posted change, or empty. A MIDI port that is not
    /// on the machine is the one that happens; an operator has to be told rather than
    /// left wondering why nothing ticks.
    std::string lastError() const;

    /// §5.8's rules, for reading — how many there are, whether one is valid, what it has
    /// fired. **Only while the thread is stopped**: the rules belong to the output thread,
    /// like the transports, and for the same reason. Change them through `post`.
    const trigger::TriggerEngine& triggers() const noexcept { return triggers_; }

    /// Whether §5.8's halt is engaged. **Any thread, running or not** — unlike everything
    /// reached through `triggers()`, which is the output thread's alone.
    ///
    /// Its own atomic rather than `triggers_.panicked()`, because that is a plain `bool`
    /// written on the output thread and a console loop or a 30 Hz redraw reading it while
    /// the thread runs is a data race. A PANIC button has to show its own state, so this
    /// gets read exactly that way. Only `Kind::Panic` moves it, so the mirror cannot drift.
    bool panicked() const noexcept { return panicked_.load(std::memory_order_relaxed); }
    /// Rule messages that reached a transport, and those with nowhere to go. Atomic-free
    /// counters on the output thread; a snapshot from anywhere else.
    const RuleSink& ruleSink() const noexcept { return sink_; }

    /// One message a rule sent, as §5.9's last-fired line and Phase 6's event log want it.
    struct Fired {
        /// `Rule::Config::id` — which rule, so a card can show only its own.
        std::string ruleId;
        /// The OSC address, or "note 36 ch 10", exactly as it went out with every `{}`
        /// already filled in. §5.9: *"showing the actually-sent message"*, not the template.
        std::string message;
        /// Seconds since `start()`, on the clock the transports are driven from.
        double when = 0.0;
        /// §5.6's release half rather than a fire of its own. It belongs in the log — a
        /// release that never left is a clip left held, and nothing else would say so — but
        /// **a rule that fired once has to count once**, and a Resolume connect sends a 1 and
        /// then a 0. See `trigger::TriggerEngine::FireObserver`.
        bool followUp = false;
        /// What each generator produced, in the order §5.9's editor draws the chips. Empty
        /// for a follow-up. See `trigger::Rule::lastSlots`.
        std::vector<trigger::Value> slots;
        /// The rule fired and was **not sent**, because it is muted. In the log for the reason
        /// `TriggerEngine::FireObserver` gives: from outside, a muted rule and a rule that has
        /// stopped triggering look identical, and only this tells them apart.
        bool muted = false;
    };

    /// Everything rules have sent since the last call, oldest first, and clears it.
    ///
    /// **A drain rather than a snapshot**, and from any thread. A UI reading this at 30 Hz
    /// would otherwise copy the whole history every tick; taking it means each message is
    /// copied once. The buffer is capped at `kFiredCapacity` and drops the *oldest* when it
    /// overflows, because a log that stops recording once it is full stops being a log —
    /// and an operator who has not looked for ten minutes wants the last ten seconds.
    std::vector<Fired> takeFired();

    /// How many messages are held between drains. A rule on every beat at 214 BPM is 3.6 a
    /// second, so this is a minute and a half of one — far more than a 30 Hz reader needs,
    /// and small enough that nothing has to think about it.
    static constexpr std::size_t kFiredCapacity = 512;

    /// When beats have been fired, for a test to read. Output thread only, like `triggers()`.
    const BeatScheduler& scheduler() const noexcept { return scheduler_; }

private:
    void run() noexcept;
    /// One round: every beat heard, every beat predicted and due, then the clock. **The caller
    /// must hold `ownerMutex_`.**
    void drainOnce(double now);
    /// One beat to §5.6's namespace and §5.8's beat triggers, about its own moment.
    void fireBeat(const ScheduledBeat& beat, double now);
    /// Everything posted since the last round, in order, under `ownerMutex_` — so this is
    /// the way in for a thread that is not the output thread, and the output thread's own
    /// way in as well.
    void applyCommands() noexcept;
    void apply(const OutputCommand& command);
    /// Copies what the transports are set to into `snapshot_`. Called by whichever thread
    /// owns them, at the end of every `apply`, so a reader never has to touch the live ones.
    void takeSnapshot();
    /// `Snapshot::Trouble` as it stands. Whichever thread owns the transports.
    Snapshot::Trouble currentTrouble() const;
    /// Runs one stage of a round, and if it throws, counts it and keeps what it said — and the
    /// round goes on to the next stage. See `drainOnce`.
    template <typename Stage>
    void guarded(const char* stage, Stage&& body) noexcept;
    void noteRoundError(const char* stage, const char* what) noexcept;
    /// Copies every rule's live switches into `live_` when they differ from what is there. By
    /// whichever thread owns the rules, after a command that could have changed one.
    void publishLiveRules();
    /// Copies the universe buffers into `levels_`, at most `kMirrorHz` times a second. Called
    /// at the end of every round by the thread that owns them. See `levelsOf`.
    void mirrorLevels(double now);
    /// Replaces §5.6's targets and resolves every rule's routing against the new list.
    ///
    /// **The routing is resolved even when the replacement failed**, and that is the whole
    /// reason this is a function rather than two lines in `apply`. `Transports::setOutputs`
    /// swaps its list in and *then* raises whatever would not open, so the bit a rule's name
    /// resolves to has moved either way — and a rule left holding the old bit fires at
    /// whichever target now occupies it. One unplugged MIDI device was enough to send a
    /// rule's clips to the lighting desk. Rethrows what `Transports` raised, so the failure
    /// still reaches `lastError`.
    void setTargets(const std::vector<OutputTarget>& targets);
    /// Turns every rule's output and fixture *names* into the bit masks its messages carry.
    /// Run after the rules change, after the targets do and after the patch does, because any
    /// of the three moves the answer.
    void resolveRouting() noexcept;
    /// Applies `act` to the rule `id` names, or to every rule when it is `kAllRules`.
    void forEachNamed(std::string_view id, const std::function<void(trigger::Rule&)>& act);
    /// The instant every rule in this round is judged against — §5.8's ONLY IF stage, made
    /// once so that two rules with the same condition cannot disagree about it.
    trigger::Context contextAt(double now) const;

    engine::BeatEngine& engine_;
    Transports transports_;
    /// Declared after the transports it holds a reference to, and before the engine that
    /// holds a reference to it: destruction runs in reverse, so this is the order that keeps
    /// both references alive for as long as they are used.
    RuleSink sink_;
    trigger::TriggerEngine triggers_;
    /// When each beat is fired: as it is heard, or ahead of it on a prediction. See
    /// `BeatScheduler`.
    BeatScheduler scheduler_;
    /// The onset count last seen from the engine. The output thread has no frames of its
    /// own, so a count that moved is how it learns one happened.
    std::uint64_t onsetsSeen_ = 0;
    /// `TempoState::barsDeclared` last seen, likewise.
    std::uint64_t barsDeclaredSeen_ = 0;
    BeatObserver observer_;

    /// **Who may touch the transports and the rules right now.** Held for a whole round by
    /// the output thread, and by whichever thread applies a command while that thread is not
    /// running.
    ///
    /// It exists because `post` applies on the *caller's* thread while the runner is
    /// stopped — which is what lets an app be configured before it is started — and there is
    /// more than one caller. §5.7's `panic` and `rule/<id>/enable` are documented safe from
    /// RtMidi's callback and from the OSC receiver's loop, and the window opens both of
    /// those ports before Start on purpose, so an inbound message and a keystroke in the rule
    /// editor really can be inside `apply` at the same time. Both mutate the same vector of
    /// rules. Nothing else here can be reached that way: `snapshot`, `lastError`, `takeFired`
    /// and `panicked` have their own locks or are atomic, so a UI at redraw rate never waits
    /// behind a round.
    mutable std::mutex ownerMutex_;
    mutable std::mutex commandMutex_;
    std::vector<OutputCommand> pending_;
    /// Drained into, and reused, so applying commands allocates nothing after the first.
    std::vector<OutputCommand> applying_;
    mutable std::mutex errorMutex_;
    std::string lastError_;
    /// `postAndWait`'s side: the last ticket handed out, and each waited-on command's answer
    /// until its waiter collects it.
    std::uint64_t lastTicket_ = 0;
    std::mutex answerMutex_;
    std::condition_variable answered_;
    std::vector<std::pair<std::uint64_t, std::string>> answers_;
    /// See `snapshot()`. Its own lock rather than `errorMutex_`'s, so a UI reading it at 30 Hz
    /// never waits behind a command reporting what went wrong.
    mutable std::mutex snapshotMutex_;
    Snapshot snapshot_;
    /// See `liveRules`.
    mutable std::mutex liveMutex_;
    std::vector<LiveRule> live_;
    std::atomic<std::uint64_t> liveVersion_{0};
    /// See `levelsOf`. A universe and its 512 bytes, copied off the output thread at
    /// `kMirrorHz` so a patch editor can show a fade happening.
    struct MirroredUniverse {
        dmx::PortAddress universe = 0;
        std::vector<std::uint8_t> levels;
    };
    mutable std::mutex levelsMutex_;
    std::vector<MirroredUniverse> levels_;
    /// When the mirror was last refreshed, on `elapsed()`. Negative before it ever has been.
    double mirroredAt_ = -1.0;
    /// Rounds since the output targets were last asked to find their addresses. See
    /// `Transports::refreshTargets`.
    std::uint32_t sinceRefresh_ = 0;
    /// How many lost MIDI devices the snapshot last recorded. A device goes lost or comes back
    /// in the middle of a round rather than in a command, so the round compares against this
    /// and takes a fresh snapshot only when the count moves.
    std::size_t lostInSnapshot_ = 0;
    /// §5.9's fired messages, written by the output thread and drained by a UI. A mutex
    /// rather than a ring because the entries hold strings and both sides are far from the
    /// audio thread — the output thread already takes two of these every round.
    mutable std::mutex firedMutex_;
    std::vector<Fired> fired_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    /// A reader-safe mirror of `triggers_.panicked()`; see `panicked()`.
    std::atomic<bool> panicked_{false};
    /// See `tracking()`.
    std::atomic<bool> tracking_{false};
    std::atomic<std::uint64_t> rounds_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<bool> threadRaised_{false};
    /// The last stage that threw, as "stage: what". Whichever thread owns the transports; a
    /// reader has it from the snapshot.
    std::string lastRoundError_;
    /// Where `elapsed()` counts from. Const, so the monotonicity `start()` depends on is
    /// structural rather than a thing to remember — and so no thread can read it while
    /// another writes.
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    /// Whether this runner is the one holding the platform's timer resolution up.
    bool raisedTimer_ = false;
};

} // namespace takt4::output
