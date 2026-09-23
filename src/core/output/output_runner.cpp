#include "core/output/output_runner.hpp"

#include <cmath>
#include <exception>
#include <span>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
// timeapi.h must follow windows.h.
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace takt4::output {
namespace {

/// Windows' default timer granularity is 15.6 ms, and this loop wants 1 ms; without
/// raising it a `sleep_for(1ms)` is a `sleep_for(15.6ms)` and every MIDI tick inherits
/// that as jitter. Process-wide, reference-counted by the OS — so nesting inside a caller
/// that has already raised it is safe — and reverted when the runner stops.
bool raiseTimerResolution() noexcept {
#if defined(_WIN32)
    return ::timeBeginPeriod(1) == TIMERR_NOERROR;
#else
    return false;
#endif
}

void restoreTimerResolution(bool raised) noexcept {
#if defined(_WIN32)
    if (raised) {
        ::timeEndPeriod(1);
    }
#else
    (void)raised;
#endif
}

/// One message as a person reads it: the address for OSC, the target for MIDI.
///
/// The *sent* form, with every `{}` already filled in — §5.9 asks for "the actually-sent
/// message", and a card showing the template would be showing what the operator typed back
/// at them rather than what happened.
std::string describeDmx(const trigger::Message& message) {
    const dmx::Payload& payload = message.payload;
    std::string text(dmx::labelOf(payload.kind));
    switch (payload.kind) {
    case dmx::EffectKind::Level:
    case dmx::EffectKind::Flash:
    case dmx::EffectKind::Pulse:
    case dmx::EffectKind::Strobe:
        text += ' ';
        text += dmx::labelOf(payload.role);
        text += " = " + std::to_string(static_cast<int>(payload.level));
        break;
    case dmx::EffectKind::Color:
    case dmx::EffectKind::HueSweep:
        text += ' ';
        text += dmx::formatColor(payload.color);
        break;
    case dmx::EffectKind::Position:
        // Percentages of each fixture's own window, which is what the operator typed and the
        // only form that means the same thing on two differently-rigged heads.
        text += " pan " + std::to_string(std::lround(payload.pan * 100.0)) + "% tilt " +
                std::to_string(std::lround(payload.tilt * 100.0)) + "%";
        break;
    case dmx::EffectKind::Path:
        text += ' ';
        text += dmx::labelOf(payload.shape);
        break;
    case dmx::EffectKind::Home:
    case dmx::EffectKind::Blackout:
        break;
    }
    if (payload.durationSeconds > 0.0f) {
        // Milliseconds, whatever unit the rule spelled it in: by the time it is a message the
        // duration has been settled against the tempo that was playing, and showing "2 bars"
        // here would be showing the configuration rather than what happened.
        text += " over " + std::to_string(std::lround(payload.durationSeconds * 1000.0)) + "ms";
    }
    return text;
}

std::string describe(const trigger::Message& message) {
    if (message.kind == trigger::Message::Kind::Osc) {
        std::string text = message.address;
        if (message.hasArgument) {
            text += ' ';
            message.argument.appendTo(text);
        }
        return text;
    }
    if (message.kind == trigger::Message::Kind::Dmx) {
        return describeDmx(message);
    }
    // Every kind by its own name. This said "note" for one of them and "cc" for the other
    // five, so a Note Off logged as a CC and the one line §5.9 calls "what turns a config
    // screen into an instrument" was describing a message that had not been sent.
    const std::string channel = " ch " + std::to_string(message.channel);
    switch (message.kind) {
    case trigger::Message::Kind::MidiNote:
        return "note on " + std::to_string(message.number) + channel + " = " +
               std::to_string(message.value);
    case trigger::Message::Kind::MidiNoteOff:
        return "note off " + std::to_string(message.number) + channel;
    case trigger::Message::Kind::MidiCc:
        return "cc " + std::to_string(message.number) + channel + " = " +
               std::to_string(message.value);
    case trigger::Message::Kind::MidiProgramChange:
        // Two bytes, and no value to print: printing one would say a number went out that
        // did not. See `Message::Kind`.
        return "program " + std::to_string(message.number) + channel;
    case trigger::Message::Kind::MidiPitchBend:
        // 14-bit, centre 8192, and no number at all.
        return "bend" + channel + " = " + std::to_string(message.value);
    case trigger::Message::Kind::Osc:
    case trigger::Message::Kind::Dmx:
        break; // both handled above
    }
    return {};
}

} // namespace

OutputRunner::OutputRunner(engine::BeatEngine& engine, const Transports::Config& config)
    : engine_(engine), transports_(config), sink_(transports_), triggers_(sink_) {
    // §5.9's last-fired line and event log. Copied out of the output thread and into a
    // buffer a UI drains, which is what keeps `TriggerEngine` single-threaded.
    triggers_.setFireObserver([this](std::string_view ruleId, const trigger::Message& message,
                                     bool followUp, std::span<const trigger::Value> slots,
                                     bool muted) {
        Fired entry;
        entry.ruleId = ruleId;
        entry.message = describe(message);
        entry.when = elapsed();
        entry.followUp = followUp;
        entry.muted = muted;
        entry.slots.assign(slots.begin(), slots.end());
        const std::lock_guard<std::mutex> lock(firedMutex_);
        if (fired_.size() >= kFiredCapacity) {
            // The oldest goes. A log that stops recording when it is full stops being a
            // log, and what an operator wants is the last few seconds, not the first.
            fired_.erase(fired_.begin());
        }
        fired_.push_back(std::move(entry));
    });
    // Before anything else can read it, and on this thread, where nothing else is running.
    takeSnapshot();
}

void OutputRunner::takeSnapshot() {
    Snapshot taken;
    taken.outputs = transports_.outputs();
    taken.link = transports_.linkEnabled();
    taken.midiClockPort = transports_.midiClockPort();
    taken.midiClockOpen = transports_.midiClock() != nullptr;
    taken.oscPrefix = transports_.oscPrefix();
    taken.patch = transports_.patch();
    taken.lostMidi = transports_.lostMidiDevices();
    lostInSnapshot_ = taken.lostMidi.size();
    const std::lock_guard<std::mutex> lock(snapshotMutex_);
    snapshot_ = std::move(taken);
}

OutputRunner::Snapshot OutputRunner::snapshot() const {
    const std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshot_;
}

void OutputRunner::mirrorLevels(double now) {
    // Rate-limited, because the engine moves a thousand times a second and a screen does not.
    // Without this a fade would take a 512-byte copy and a lock every round for no reader.
    if (mirroredAt_ >= 0.0 && now - mirroredAt_ < 1.0 / kMirrorHz) {
        return;
    }
    mirroredAt_ = now;
    const dmx::DmxEngine& engine = transports_.dmx();
    const std::vector<dmx::PortAddress>& universes = engine.universes();
    const std::lock_guard<std::mutex> lock(levelsMutex_);
    levels_.resize(universes.size());
    for (std::size_t i = 0; i < universes.size(); ++i) {
        const std::span<const std::uint8_t> frame = engine.levels(universes[i]);
        levels_[i].universe = universes[i];
        levels_[i].levels.assign(frame.begin(), frame.end());
    }
}

std::vector<std::uint8_t> OutputRunner::levelsOf(dmx::PortAddress universe) const {
    const std::lock_guard<std::mutex> lock(levelsMutex_);
    for (const MirroredUniverse& mirrored : levels_) {
        if (mirrored.universe == universe) {
            return mirrored.levels;
        }
    }
    return {};
}

std::vector<OutputRunner::Fired> OutputRunner::takeFired() {
    std::vector<Fired> out;
    const std::lock_guard<std::mutex> lock(firedMutex_);
    out.swap(fired_);
    return out;
}

OutputRunner::~OutputRunner() {
    stop();
}

void OutputRunner::setBeatObserver(BeatObserver observer) {
    observer_ = std::move(observer);
}

double OutputRunner::elapsed() const noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

void OutputRunner::start() {
    if (running()) {
        return;
    }
    // **The clock is not restarted here, and that is the whole of a bug reported from a rig
    // on 2026-09-12**: *"when hitting stop on the main window, when I hit start again, none of
    // the triggers started firing again."*
    //
    // `started_` used to be taken here, so `elapsed()` — which is `Context::now` — went back
    // to zero on every Start while the state measured against it did not. `Rule::lastFired_`
    // still held the time of the last fire of the previous run, so §5.8's cooldown test read
    // `now - lastFired < cooldown` as a large *negative* number, which is below even a zero
    // cooldown. Every rule that had ever fired was then blocked until the clock climbed back
    // past where it had been — five minutes of playing, for a rig stopped five minutes in.
    //
    // A clock that only goes forwards is the fix, and it is the fix for the whole class:
    // pending follow-ups, the log's timestamps and §5.9's "12s ago" were all measured against
    // it too. `started_` is taken once, at construction, and nothing writes it again — which
    // also takes the reader on the UI thread out of a race with `start()`.
    raisedTimer_ = raiseTimerResolution();
    // A count from the run that just ended means nothing to the run beginning: `BeatEngine::
    // start` zeroes its own, so a stale one here made the first round after a restart see a
    // count that had "moved" and fire every onset rule once for nothing.
    onsetsSeen_ = 0;
    try {
        // Before the thread: startOutputs enables Link and starts the MIDI clock, and the
        // thread's first round must not find them half up. On the same clock the rounds use,
        // so the MIDI clock's tick spacing is measured from where the loop actually is.
        //
        // Under `ownerMutex_` like everything else that touches them: a control surface can
        // be inside `apply` on its own thread at this moment, and this is the transition
        // that would otherwise have no lock on either side of it.
        const std::lock_guard<std::mutex> owner(ownerMutex_);
        transports_.startOutputs(elapsed());
    } catch (...) {
        // A MIDI port that went away between one run and the next. Nothing is running, so
        // put back what was raised rather than leaving the platform timer held by a runner
        // that never started.
        restoreTimerResolution(raisedTimer_);
        raisedTimer_ = false;
        throw;
    }
    running_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread([this] { run(); });
    } catch (...) {
        // A thread that could not be created leaves the transports up and the flag set, and
        // `stop()` would return early on a worker that is not joinable — so nothing would
        // ever put them back. Wound all the way down instead, as the failed `startOutputs`
        // above is.
        running_.store(false, std::memory_order_release);
        const std::lock_guard<std::mutex> owner(ownerMutex_);
        transports_.stopOutputs();
        restoreTimerResolution(raisedTimer_);
        raisedTimer_ = false;
        throw;
    }
}

void OutputRunner::stop() noexcept {
    if (!worker_.joinable()) {
        return;
    }
    running_.store(false, std::memory_order_release);
    worker_.join();

    // Anything posted in the moments before the stop still meant something, and the
    // transports are this thread's now. (It takes `ownerMutex_` itself, so the block below
    // takes it again rather than one call holding it across both — two functions agreeing
    // about who locks is how a lock ends up held twice or not at all.)
    applyCommands();

    {
        const std::lock_guard<std::mutex> owner(ownerMutex_);
        // The last beats of a set are still beats: the engine may have called one between
        // the final round and the join. Safe on this thread now — the only other consumer of
        // that ring has been joined.
        try {
            drainOnce(elapsed());
            // Every release still owed, now rather than when it was due. A note on whose
            // note off had not yet come round is a laser still lit and a clip still held,
            // and an operator pressing Stop has said the opposite. Same argument as `panic`
            // and `setRules`; this is the third place that owed a flush and did not have one.
            triggers_.flushFollowUps();
        } catch (...) {
            // Nothing useful to do while shutting down, and letting it out of a noexcept
            // function would call std::terminate.
            errors_.fetch_add(1, std::memory_order_relaxed);
        }
        transports_.stopOutputs();
    }
    restoreTimerResolution(raisedTimer_);
    raisedTimer_ = false;
}

void OutputRunner::post(OutputCommand command) {
    // **Queued first, always.** Applying it here would be a command that is lost if the
    // output thread starts between the test and the call, and applied twice if it stops;
    // on the queue it is taken exactly once, by whichever thread gets to it.
    {
        const std::lock_guard<std::mutex> lock(commandMutex_);
        pending_.push_back(std::move(command));
    }
    if (running()) {
        return; // the output thread takes it at the top of its next round, a millisecond off
    }
    // Nothing is draining it, so this thread is the one that applies it: there is no reason
    // to make an operator press Start before a setting takes, and this is how an app is
    // configured before it is running at all. Under `ownerMutex_` — see its declaration —
    // because there is more than one thread that can be here.
    applyCommands();
}

std::string OutputRunner::lastError() const {
    const std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

void OutputRunner::apply(const OutputCommand& command) {
    // `Manual`, `TestRule` and `Panic` all fire rules from here rather than from `drainOnce`,
    // and a lighting effect needs to know when it started. Set once for every command rather
    // than on the three that need it, so a fourth cannot be added without one.
    sink_.setNow(elapsed());
    try {
        switch (command.kind) {
        case OutputCommand::Kind::LinkEnabled:
            transports_.setLinkEnabled(command.enabled);
            break;
        case OutputCommand::Kind::OscTargets:
            setTargets(oscOutputs(command.targets));
            break;
        case OutputCommand::Kind::Outputs:
            setTargets(command.outputTargets);
            break;
        case OutputCommand::Kind::MidiClockPort:
            transports_.setMidiClockPort(command.port);
            break;
        case OutputCommand::Kind::Rules:
            triggers_.setRules(command.ruleConfigs);
            resolveRouting();
            break;
        case OutputCommand::Kind::Panic:
            if (command.enabled) {
                triggers_.panic(contextAt(elapsed()));
                // And the lights stop animating — but keep their levels, and keep being sent.
                // The operator's own call on 2026-09-16: takt4 may be one source among several
                // on a rig, and a panic that blacked out the stage would take down lights that
                // were not takt4's to take. See `dmx::DmxEngine::cancelAll`.
                transports_.dmx().cancelAll();
            } else {
                triggers_.release();
            }
            // After the engine, so a reader that sees the flag knows the halt has already
            // happened rather than being about to.
            panicked_.store(command.enabled, std::memory_order_relaxed);
            break;
        case OutputCommand::Kind::RuleEnabled:
            forEachNamed(command.ruleId,
                         [&](trigger::Rule& rule) { rule.setEnabled(command.enabled); });
            break;
        case OutputCommand::Kind::RuleMuted:
            forEachNamed(command.ruleId,
                         [&](trigger::Rule& rule) { rule.setMuted(command.enabled); });
            break;
        case OutputCommand::Kind::RuleRate:
            forEachNamed(command.ruleId, [&](trigger::Rule& rule) {
                // Relative multiplies what the rule is already at, which is what lets a
                // "twice as often" button be pressed twice; absolute replaces it, which is
                // what a fader and a reset both want.
                rule.setRate(command.relative ? rule.rate() * command.factor : command.factor);
            });
            break;
        case OutputCommand::Kind::Patch:
            transports_.setPatch(command.fixtures);
            resolveRouting();
            break;
        case OutputCommand::Kind::Effect:
            // Straight to the engine, past the rules. `sink_.setNow` was called at the top of
            // `apply`, and this uses the same clock so that a hand-fired effect and a rule's
            // land on one timeline.
            transports_.dmx().start(command.fixtureMask, command.payload, elapsed());
            break;
        case OutputCommand::Kind::ChannelTest:
            transports_.dmx().holdChannel(command.universe, command.channel, command.level,
                                          command.factor, elapsed());
            break;
        case OutputCommand::Kind::Manual:
            triggers_.manual(contextAt(elapsed()));
            break;
        case OutputCommand::Kind::TestRule:
            (void)triggers_.test(command.ruleId, contextAt(elapsed()));
            break;
        }
        const std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_.clear();
    } catch (const std::exception& e) {
        // A MIDI port that is not there. Transports leaves what was working alone, so the
        // failure is only that the change did not happen — which somebody has to be told.
        const std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = e.what();
    }
    // Whether it worked or not: what a reader must see is what the transports are *now*, and
    // a command that threw part-way through has still changed some of them.
    takeSnapshot();
}

void OutputRunner::setTargets(const std::vector<OutputTarget>& targets) {
    // The routing is resolved either way — see the declaration. A target that would not open
    // still took its place in the list, so every bit below it has moved.
    try {
        transports_.setOutputs(targets);
    } catch (...) {
        resolveRouting();
        throw;
    }
    resolveRouting();
}

void OutputRunner::resolveRouting() noexcept {
    // The one place a rule's output and fixture *names* become the bits its messages carry.
    // Neither `Rule` nor `TriggerEngine` knows what an output or a fixture is, and neither
    // `Transports` nor `DmxEngine` knows what a rule is; this owns all of them and is where
    // they meet.
    const std::vector<OutputTarget>& targets = transports_.outputs();
    const std::vector<dmx::Fixture>& patch = transports_.patch();
    for (std::size_t i = 0; i < triggers_.ruleCount(); ++i) {
        trigger::Rule& rule = triggers_.rule(i);
        rule.setOutputMask(resolveOutputs(rule.config().outputs, targets));
        rule.setFixtureMask(dmx::resolveFixtures(patch, rule.config().dmx.fixtures));
    }
}

void OutputRunner::forEachNamed(std::string_view id,
                                const std::function<void(trigger::Rule&)>& act) {
    // "all" is a reserved id meaning every rule — see `kAllRules`. A name that matches nothing
    // acts on nothing and is not an error: a control surface holding a button for a rule the
    // current preset no longer has is an ordinary state of the world (`RuleControl`).
    if (id == kAllRules) {
        for (std::size_t i = 0; i < triggers_.ruleCount(); ++i) {
            act(triggers_.rule(i));
        }
        return;
    }
    if (trigger::Rule* rule = triggers_.find(id)) {
        act(*rule);
    }
}

void OutputRunner::applyCommands() noexcept {
    // `applying_` is a member reused across calls, and `apply` touches the transports and
    // the rules, so both want `ownerMutex_` rather than only the queue's own lock.
    const std::lock_guard<std::mutex> owner(ownerMutex_);
    {
        const std::lock_guard<std::mutex> lock(commandMutex_);
        if (pending_.empty()) {
            return;
        }
        applying_.swap(pending_);
    }
    for (const OutputCommand& command : applying_) {
        apply(command);
    }
    applying_.clear();
}

void OutputRunner::run() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        applyCommands();
        {
            const std::lock_guard<std::mutex> owner(ownerMutex_);
            try {
                drainOnce(elapsed());
            } catch (...) {
                // A transport that throws must not take the process down with it: an OSC
                // target can go away mid-set, and Link's networking has its own opinions
                // about sockets. Counted rather than swallowed, so it is visible.
                errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        rounds_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(kPeriod);
    }
}

trigger::Context OutputRunner::contextAt(double now) const {
    const tracking::TempoState state = engine_.state();
    const engine::EngineIntensity intensity = engine_.intensity();
    trigger::Context context;
    context.bpm = state.bpm;
    context.confidence = state.confidence;
    context.locked = state.locked;
    context.meter = state.beatsPerBar;
    context.beatInBar = state.beatInBar;
    context.beats = state.beats;
    context.bars = state.bars;
    context.intensity = intensity.level;
    context.now = now;
    return context;
}

void OutputRunner::drainOnce(double now) {
    // Before any rule is evaluated: a lighting effect starts at an instant and runs for a
    // duration, so the sink has to know where on this clock the round is. See
    // `RuleSink::setNow`.
    sink_.setNow(now);
    engine::EngineBeat beat;
    while (engine_.popBeat(beat)) {
        transports_.publish(beat.event, beat.hostMicros, now);
        // §5.8's beat-counting triggers, off the state *at this beat* rather than off the
        // engine's newest: a round can drain several beats, and a rule counting bars has to
        // see each of them where it happened.
        trigger::Context context = contextAt(now);
        context.bpm = beat.state.bpm;
        context.confidence = beat.state.confidence;
        context.locked = beat.state.locked;
        context.meter = beat.state.beatsPerBar;
        context.beatInBar = beat.state.beatInBar;
        context.beats = beat.state.beats;
        context.bars = beat.state.bars;
        triggers_.onBeat(context);
        if (observer_) {
            observer_(beat);
        }
    }
    // Every round, beat or no beat: the MIDI clock's 24 PPQN does not wait for one, and
    // OSC's state addresses are how a peer learns the tempo drifted.
    transports_.advance(now, engine_.state());

    const trigger::Context context = contextAt(now);
    // The output thread never sees a frame, so an onset reaches it as a count that moved.
    // Coalesced to one call however far it moved: two onsets inside one millisecond would
    // be one hit as far as anything downstream is concerned, and the classifier's own
    // minimum gap is sixty.
    const std::uint64_t onsets = engine_.intensity().onsets;
    if (onsets != onsetsSeen_) {
        onsetsSeen_ = onsets;
        triggers_.onOnset(context);
    }
    // Last: the triggers that do not wait for a beat, and any follow-up now due.
    triggers_.advance(context);
    // And a copy of the lighting frames for anything watching at redraw rate. After the
    // triggers, so a fade started this round is in the very frame that is mirrored.
    mirrorLevels(now);
    // A MIDI device went lost or came back this round — neither is a command, so nothing else
    // would refresh what a reader sees. Only when the count moves: a snapshot copies the
    // targets and the patch, which is nothing to do a thousand times a second.
    if (transports_.lostMidiCount() != lostInSnapshot_) {
        takeSnapshot();
    }
}

} // namespace takt4::output
