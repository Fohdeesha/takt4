#pragma once

#include "core/audio/host_time.hpp"
#include "core/control/rule_control.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/output/rule_sink.hpp"
#include "core/output/transports.hpp"
#include "core/trigger/trigger_engine.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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
    static OutputCommand midiClockPort(std::optional<std::string> port) {
        OutputCommand command;
        command.kind = Kind::MidiClockPort;
        command.port = std::move(port);
        return command;
    }

    Kind kind = Kind::LinkEnabled;
    bool enabled = false;
    std::vector<Transports::OscTarget> targets;
    std::optional<std::string> port;
    std::vector<trigger::Rule::Config> ruleConfigs;
    std::string ruleId;
};

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
/// `takt4-cli track` obeys that already. §5.9's window still drains beats and throws them
/// away, purely so the ring does not fill, and **that loop has to go the moment the window
/// owns one of these**; `ui::WindowController::tick` says so at the point it happens.
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
    /// nothing. The seconds-since-start clock the transports are given begins here.
    void start();

    /// Stops the thread, drains whatever was still on the ring on the calling thread —
    /// the last beats of a set are still beats — and then stops the transports. Safe to
    /// call twice, and called by the destructor.
    void stop() noexcept;

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Rounds the loop has made. What a test watches to tell "the thread is running" from
    /// "the thread does the right thing", which are two separate claims.
    std::uint64_t rounds() const noexcept { return rounds_.load(std::memory_order_relaxed); }

    /// Rounds that threw. A transport failing must not take the process down — an OSC
    /// target can go away mid-set — but it must not be silent either.
    std::uint64_t errors() const noexcept { return errors_.load(std::memory_order_relaxed); }

    /// Seconds since `start()`, on the steady clock the transports are driven from.
    double elapsed() const noexcept;

    /// The transports, for reading. Their counters are atomic and Link's own state is
    /// safe to query, so a UI may call this while the thread is sending; the members that
    /// change what is sent are not reachable through it, and that is deliberate.
    const Transports& transports() const noexcept { return transports_; }

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

private:
    void run() noexcept;
    /// One round: every beat waiting, then the clock. On the output thread, or on the
    /// caller's in `stop()` once the thread has been joined — never on both at once.
    void drainOnce(double now);
    /// Everything posted since the last round, in order. On whichever thread owns the
    /// transports at the time.
    void applyCommands() noexcept;
    void apply(const OutputCommand& command);
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
    /// The onset count last seen from the engine. The output thread has no frames of its
    /// own, so a count that moved is how it learns one happened.
    std::uint64_t onsetsSeen_ = 0;
    BeatObserver observer_;

    mutable std::mutex commandMutex_;
    std::vector<OutputCommand> pending_;
    /// Drained into, and reused, so applying commands allocates nothing after the first.
    std::vector<OutputCommand> applying_;
    mutable std::mutex errorMutex_;
    std::string lastError_;
    /// §5.9's fired messages, written by the output thread and drained by a UI. A mutex
    /// rather than a ring because the entries hold strings and both sides are far from the
    /// audio thread — the output thread already takes two of these every round.
    mutable std::mutex firedMutex_;
    std::vector<Fired> fired_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    /// A reader-safe mirror of `triggers_.panicked()`; see `panicked()`.
    std::atomic<bool> panicked_{false};
    std::atomic<std::uint64_t> rounds_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::chrono::steady_clock::time_point started_{};
    /// Whether this runner is the one holding the platform's timer resolution up.
    bool raisedTimer_ = false;
};

} // namespace takt4::output
