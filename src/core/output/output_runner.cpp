#include "core/output/output_runner.hpp"

#include "core/rt/thread_priority.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <optional>
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

/// Whether a count the engine keeps has moved since `seen`, bringing `seen` up to date.
///
/// **A count lower than the one last seen is a new run**, not a count that moved: the engine
/// zeroes its counts every time the tracker starts, and since the audit's H5 this runner keeps
/// going across a Stop and a Start. Compared for plain inequality, the first round of every
/// run after the first saw the old count "move" to zero and fired every onset rule once for
/// nothing. A new run counts as moved only if it has already counted something.
bool countMoved(std::uint64_t now, std::uint64_t& seen) noexcept {
    const bool moved = now < seen ? now > 0 : now != seen;
    seen = now;
    return moved;
}

/// Where each output of `before` is in `after`, by `OutputTarget::id`: the new index, or -1
/// for one that is gone. See `trigger::remapBits`.
std::vector<int> movedOutputs(const std::vector<OutputTarget>& before,
                              const std::vector<OutputTarget>& after) {
    std::vector<int> moved(std::min(before.size(), kMaxRoutableTargets), -1);
    for (std::size_t i = 0; i < moved.size(); ++i) {
        for (std::size_t j = 0; j < after.size() && j < kMaxRoutableTargets; ++j) {
            if (!before[i].id.empty() && after[j].id == before[i].id) {
                moved[i] = static_cast<int>(j);
                break;
            }
        }
    }
    return moved;
}

/// The same for the patch, by `dmx::Fixture::id` — or by name for a fixture built in code with
/// no id, which is the one kind that can have none.
std::vector<int> movedFixtures(const std::vector<dmx::Fixture>& before,
                               const std::vector<dmx::Fixture>& after) {
    std::vector<int> moved(std::min(before.size(), dmx::kMaxRoutableFixtures), -1);
    for (std::size_t i = 0; i < moved.size(); ++i) {
        for (std::size_t j = 0; j < after.size() && j < dmx::kMaxRoutableFixtures; ++j) {
            const bool same = !before[i].id.empty() || !after[j].id.empty()
                                  ? before[i].id == after[j].id
                                  : before[i].name == after[j].name;
            if (same) {
                moved[i] = static_cast<int>(j);
                break;
            }
        }
    }
    return moved;
}

/// How many rounds — milliseconds — between two looks at whether the output targets have found
/// their addresses. A name server answers in tens of milliseconds or not at all.
constexpr std::uint32_t kRefreshRounds = 250;

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
        text += " pan " + std::to_string(std::lround(static_cast<double>(payload.pan) * 100.0)) + "% tilt " +
                std::to_string(std::lround(static_cast<double>(payload.tilt) * 100.0)) + "%";
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
        text += " over " + std::to_string(std::lround(static_cast<double>(payload.durationSeconds) * 1000.0)) + "ms";
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
            // log, and what an operator wants is the last few seconds, not the first. Off
            // the front of a deque: off a vector's, it moved all 511 others every time.
            fired_.pop_front();
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
    taken.outputProblems = transports_.outputProblems();
    taken.trouble = currentTrouble();
    const std::lock_guard<std::mutex> lock(snapshotMutex_);
    snapshot_ = std::move(taken);
    snapshotVersion_.fetch_add(1, std::memory_order_release);
}

OutputRunner::Snapshot::Trouble OutputRunner::currentTrouble() const {
    Snapshot::Trouble trouble;
    trouble.roundErrors = errors_.load(std::memory_order_relaxed);
    trouble.lastRoundError = lastRoundError_;
    trouble.undeliverable = sink_.undeliverable();
    trouble.heldDropped = sink_.dropped() + transports_.osc().dropped();
    trouble.clockTicksSkipped = transports_.clockTicksSkipped();
    return trouble;
}

template <typename Stage>
void OutputRunner::guarded(const char* stage, Stage&& body) noexcept {
    try {
        std::forward<Stage>(body)();
    } catch (const std::exception& e) {
        noteRoundError(stage, e.what());
    } catch (...) {
        noteRoundError(stage, "something that is not a std::exception");
    }
}

void OutputRunner::noteRoundError(const char* stage, const char* what) noexcept {
    errors_.fetch_add(1, std::memory_order_relaxed);
    try {
        lastRoundError_ = std::string(stage) + ": " + what;
    } catch (...) {
        // No memory for the words. The count has moved, which is what says so.
    }
}

OutputRunner::Snapshot OutputRunner::snapshot() const {
    const std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshot_;
}

void OutputRunner::publishLiveRules() {
    std::vector<LiveRule> now;
    now.reserve(triggers_.ruleCount());
    for (std::size_t i = 0; i < triggers_.ruleCount(); ++i) {
        const trigger::Rule& rule = triggers_.rule(i);
        now.push_back(LiveRule{rule.id(), rule.enabled(), rule.muted(), rule.rate()});
    }
    const std::lock_guard<std::mutex> lock(liveMutex_);
    // Whichever set these came from, even when the switches read the same: that is what says
    // the set has been got to. See `liveRulesCurrent`.
    liveGeneration_ = rulesApplied_;
    if (now != live_) {
        live_ = std::move(now);
        liveVersion_.fetch_add(1, std::memory_order_release);
    }
}

std::vector<OutputRunner::LiveRule> OutputRunner::liveRules() const {
    const std::lock_guard<std::mutex> lock(liveMutex_);
    return live_;
}

bool OutputRunner::liveRulesCurrent() const {
    std::uint64_t posted = 0;
    {
        const std::lock_guard<std::mutex> lock(commandMutex_);
        posted = rulesPosted_;
    }
    const std::lock_guard<std::mutex> lock(liveMutex_);
    return liveGeneration_ == posted;
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
    std::deque<Fired> taken;
    {
        const std::lock_guard<std::mutex> lock(firedMutex_);
        taken.swap(fired_);
    }
    return {std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end())};
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
    // count that had "moved" and fire every onset rule once for nothing. (The tracker also
    // restarts under a runner that keeps going — see `countMoved`, which is what handles that.)
    onsetsSeen_ = 0;
    barsDeclaredSeen_ = 0;
    // And nothing predicted from the last run's beats.
    scheduler_.reset();
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
    threadRaised_.store(false, std::memory_order_relaxed);
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
            // And everything held for an output's offset, releases included — a held datagram
            // used to sit in the queue until the next Start (the audit's H6).
            sink_.flushQueued();
            transports_.osc().flushAll();
            // Then the lights out — the operator's call for quit as for Stop (Q3) — and that
            // frame actually sent. Art-Net only goes out in `advance`, paced at 44 Hz, and there
            // is no round after this one: a node left holding the last frame before it would
            // hold the rig lit after takt4 had gone.
            const double now = elapsed();
            transports_.dmx().blackout(now);
            (void)transports_.artnet().flush(transports_.dmx(), now);
        } catch (const std::exception& e) {
            // Nothing useful to do while shutting down, and letting it out of a noexcept
            // function would call std::terminate.
            noteRoundError("stopping", e.what());
        } catch (...) {
            noteRoundError("stopping", "something that is not a std::exception");
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
        // Numbered here, under the lock that orders the queue, so the numbers are the order in
        // which the sets will be applied. See `liveRulesCurrent`.
        if (command.kind == OutputCommand::Kind::Rules) {
            command.generation = ++rulesPosted_;
        }
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
    //
    // **One reading of the clock for the whole command.** A rule's message is held until the
    // moment it is about, and a TEST fired with a context read a few microseconds after the
    // sink's "now" was a message about the future: held, and — with the thread stopped and
    // nothing flushing — never sent at all.
    const double now = elapsed();
    sink_.setNow(now);
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
            rulesApplied_ = command.generation;
            resolveRouting();
            break;
        case OutputCommand::Kind::Panic:
            if (command.enabled) {
                // Every release owed goes now — except the lighting's, which would move the lamps
                // PANIC is about to freeze where they are. Most are still held for their moment
                // and dropped below; a release the rig's negative offset has already made due
                // would otherwise start at once (the audit's M6; see `holdLighting`).
                sink_.holdLighting(true);
                triggers_.panic(contextAt(now));
                sink_.holdLighting(false);
                // What is held for an output's offset goes now too. A release among it must not
                // wait, and a press among it is at most one lead's worth early — and would
                // otherwise be a rule firing after PANIC was pressed. Except the lighting, which
                // PANIC freezes: an effect started now would be frozen on its first frame, and a
                // flash frozen there is a lamp held at full.
                sink_.dropQueuedLighting();
                sink_.flushQueued();
                transports_.osc().flushAll();
                // And the lights stop animating — but keep their levels, and keep being sent.
                // The operator's own call on 2026-09-16: takt4 may be one source among several
                // on a rig, and a panic that blacked out the stage would take down lights that
                // were not takt4's to take. See `dmx::DmxEngine::cancelAll`.
                transports_.dmx().cancelAll();
                // On every node at once: a delayed one was sent its last second of animation
                // after the freeze (the audit of 2026-09-25, M5).
                transports_.artnet().forgetHistory();
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
        case OutputCommand::Kind::Patch: {
            // A held effect's fixture mask, and a queued follow-up's, name fixtures by their
            // place in the patch — which is about to move. They follow their fixtures by id.
            const std::vector<dmx::Fixture> before = transports_.patch();
            transports_.setPatch(command.fixtures);
            const std::vector<int> moved = movedFixtures(before, transports_.patch());
            triggers_.remapPending({}, moved);
            sink_.remap({}, moved);
            resolveRouting();
            break;
        }
        case OutputCommand::Kind::OutputDelay:
            (void)transports_.setOutputDelay(command.ruleId, command.factor);
            break;
        case OutputCommand::Kind::Effect:
            // Straight to the engine, past the rules. `sink_.setNow` was called at the top of
            // `apply`, and this uses the same clock so that a hand-fired effect and a rule's
            // land on one timeline.
            //
            // **Not while PANIC is engaged** (the audit's M17). It went past the rules, and so
            // past the halt too: a color-picker drag or an IDENTIFY lit fixtures in the middle
            // of a panic. The editors check `panicked()` and say why nothing happened.
            if (!panicked_.load(std::memory_order_relaxed)) {
                transports_.dmx().start(command.fixtureMask, command.payload, now);
            }
            break;
        case OutputCommand::Kind::ChannelTest:
            if (!panicked_.load(std::memory_order_relaxed)) { // M17, as above
                transports_.dmx().holdChannel(command.universe, command.channel, command.level,
                                              command.factor, now);
            }
            break;
        case OutputCommand::Kind::Tracking:
            if (command.enabled) {
                // A new run: nothing predicted from the last one's beats, and the clock ticking
                // from the press that starts listening — its Start waits for a locked downbeat.
                quiet_ = false;
                triggers_.setListening(true);
                scheduler_.reset();
                transports_.startClock(now);
            } else {
                // **STOP: nothing fires after it** (the audit of 2026-09-25, H3, and the
                // operator's answer to its Q3). Up to two predicted beats went on firing their
                // rules after the blackout, because nothing reset the scheduler and nothing
                // gated it — so a flash, a colour or a clip came back on with takt4 stopped. Now:
                // the predictions go, and so do the beats the tracker called on its way down;
                quiet_ = true;
                triggers_.setListening(false);
                scheduler_.reset();
                engine::EngineBeat late;
                while (engine_.popBeat(late)) {
                }
                // every MIDI and OSC release owed goes out now, as it does on quit, so no clip
                // or laser is left latched — and every lighting one is dropped, which would
                // otherwise light a lamp again at its release level (see `holdLighting`);
                sink_.holdLighting(true);
                triggers_.flushFollowUps();
                sink_.holdLighting(false);
                sink_.dropQueuedLighting();
                sink_.flushQueued();
                transports_.osc().flushAll();
                // and the lights go out and stay out — sent, because the outputs keep running
                // (Q3 of 2026-09-23).
                transports_.stopClock();
                transports_.dmx().blackout(now);
            }
            tracking_.store(command.enabled, std::memory_order_relaxed);
            break;
        case OutputCommand::Kind::Sync:
            break;
        case OutputCommand::Kind::Manual:
            triggers_.manual(contextAt(now));
            break;
        case OutputCommand::Kind::TestRule:
            (void)triggers_.test(command.ruleId, contextAt(now));
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
    switch (command.kind) {
    case OutputCommand::Kind::Rules:
    case OutputCommand::Kind::RuleEnabled:
    case OutputCommand::Kind::RuleMuted:
    case OutputCommand::Kind::RuleRate:
        publishLiveRules();
        break;
    default:
        break;
    }
    if (command.ticket != 0) {
        // Somebody is waiting on this one in `postAndWait`: its own answer, which the next
        // command could otherwise overwrite before the waiter wakes.
        std::string answer = lastError();
        {
            const std::lock_guard<std::mutex> lock(answerMutex_);
            // Bounded: a waiter that gave up never collects its answer.
            if (answers_.size() >= 16) {
                answers_.erase(answers_.begin());
            }
            answers_.emplace_back(command.ticket, std::move(answer));
        }
        answered_.notify_all();
    }
}

std::optional<std::string> OutputRunner::postAndWait(OutputCommand command) {
    std::uint64_t ticket = 0;
    {
        const std::lock_guard<std::mutex> lock(answerMutex_);
        ticket = ++lastTicket_;
    }
    command.ticket = ticket;
    post(std::move(command));
    std::unique_lock<std::mutex> lock(answerMutex_);
    const auto mine = [this, ticket] {
        return std::find_if(answers_.begin(), answers_.end(),
                            [ticket](const auto& answer) { return answer.first == ticket; });
    };
    if (!answered_.wait_for(lock, kWaitForApply, [&] { return mine() != answers_.end(); })) {
        // Not applied yet. Its answer is thrown away when it comes — `lastError` still has it.
        std::erase_if(answers_, [ticket](const auto& answer) { return answer.first < ticket; });
        return std::nullopt;
    }
    const auto found = mine();
    std::string answer = std::move(found->second);
    answers_.erase(found);
    return answer;
}

void OutputRunner::setTargets(const std::vector<OutputTarget>& targets) {
    // What is held and what is owed names its output by its place in the list, which is about
    // to move — so each follows its output by id afterwards, rather than being flushed early
    // or sent to whatever takes its place (the audit's H12).
    const std::vector<OutputTarget> before = transports_.outputs();
    const auto follow = [&] {
        const std::vector<int> moved = movedOutputs(before, transports_.outputs());
        triggers_.remapPending(moved, {});
        sink_.remap(moved, {});
        resolveRouting();
    };
    // Either way — see the declaration. A target that would not open still took its place in
    // the list, so every bit below it has moved.
    try {
        transports_.setOutputs(targets);
    } catch (...) {
        follow();
        throw;
    }
    follow();
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
    // Ahead of the window's drawing (the audit's M13): a round that waits behind a redraw is a
    // late MIDI tick and a late cue. See rt/thread_priority.hpp.
    const rt::PriorityScope priority(rt::ThreadWork::Output);
    threadRaised_.store(priority.raised(), std::memory_order_release);
    while (running_.load(std::memory_order_acquire)) {
        applyCommands();
        {
            const std::lock_guard<std::mutex> owner(ownerMutex_);
            // Each stage has its own guard — see `drainOnce`. This one is for what is left
            // between them, which is reading the engine's state.
            guarded("the output round", [this] { drainOnce(elapsed()); });
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

void OutputRunner::fireBeat(const ScheduledBeat& beat, double now) {
    transports_.publishBeat(beat.event, beat.moment, now);
    // §5.8's beat-counting triggers, off the state *at this beat* rather than off the engine's
    // newest: a round can fire several beats, and a rule counting bars has to see each of them
    // where it happened. And *about* the beat's own moment, which every message it sends is
    // then held to — see `trigger::Message::moment`.
    trigger::Context context = contextAt(now);
    context.bpm = beat.event.bpm;
    context.confidence = beat.event.confidence;
    context.locked = beat.event.locked;
    context.meter = beat.event.beatsPerBar;
    context.beatInBar = beat.event.beatInBar;
    context.beats = beat.beats;
    context.bars = beat.bars;
    context.moment = beat.moment;
    triggers_.onBeat(context);
}

void OutputRunner::drainOnce(double now) {
    // Before any rule is evaluated: a lighting effect starts at an instant and runs for a
    // duration, so the sink has to know where on this clock the round is. See
    // `RuleSink::setNow`.
    sink_.setNow(now);
    // Link's clock and this one read together, which is what turns a beat's §4.3 stamp — the
    // moment it was in the audio, on Link's clock — into a moment on the clock the rounds run on.
    const std::int64_t linkNow = transports_.link().now().count();
    const double lead = transports_.leadSeconds();
    // How late a beat may be heard and still be worth firing: as late as the latest output
    // wants it, plus the pipeline and some. See `kStaleSeconds`.
    const double staleAfter = transports_.tailSeconds() + kStaleSeconds;
    // **Every stage on its own, and every beat through each** (the audit's M12). One try round
    // the whole round meant a stage that threw once skipped every stage after it, and one that
    // threw every round switched them all off, silently: a clock throwing on each beat would
    // have taken the rules, the lighting and the held releases with it. A stage that throws is
    // counted and named now — `Snapshot::Trouble` — and the round carries on past it.
    engine::EngineBeat beat;
    while (engine_.popBeat(beat)) {
        if (quiet_) {
            continue; // stopped: see `Tracking` in `apply`
        }
        // Offline, and in the tests that feed audio faster than it plays, there is no host
        // clock and no timeline: such a beat fires as it arrives. See `BeatScheduler::heard`.
        std::optional<double> moment;
        if (beat.hostMicros != 0) {
            moment = now - static_cast<double>(linkNow - beat.hostMicros) / 1e6;
        }
        // The two clocks, on every beat heard: they are grids running on from the last beat
        // and want every one, fired already or not.
        guarded("the clocks", [&] {
            transports_.publishClocks(beat.event, beat.hostMicros, moment.value_or(now));
        });
        guarded("a beat's rules", [&] {
            if (const std::optional<ScheduledBeat> late =
                    scheduler_.heard(beat, moment, now, staleAfter)) {
                fireBeat(*late, now);
            }
        });
        if (observer_) {
            guarded("the beat observer", [&] { observer_(beat); });
        }
    }
    // And the beats that are due before they are heard — the audit's H4. See `BeatScheduler`.
    // None while stopped: the engine's last locked state stays published, and the scheduler
    // would go on predicting from it (the audit of 2026-09-25, H3).
    guarded("a predicted beat's rules", [&] {
        if (quiet_) {
            return;
        }
        scheduler_.restate(engine_.state());
        while (const std::optional<ScheduledBeat> next = scheduler_.due(now, lead, staleAfter)) {
            fireBeat(*next, now);
        }
    });
    // Every round, beat or no beat: the MIDI clock's 24 PPQN does not wait for one, and
    // OSC's state addresses are how a peer learns the tempo drifted.
    guarded("the outputs", [&] { transports_.advance(now, engine_.state()); });

    const trigger::Context context = contextAt(now);
    // The output thread never sees a frame, so an onset reaches it as a count that moved.
    // Coalesced to one call however far it moved: two onsets inside one millisecond would
    // be one hit as far as anything downstream is concerned, and the classifier's own
    // minimum gap is sixty.
    if (countMoved(engine_.intensity().onsets, onsetsSeen_) && !quiet_) {
        guarded("the onset rules", [&] { triggers_.onOnset(context); });
    }
    // A bar a late DOWNBEAT press declared, whose first beat had already gone out as another
    // (the audit's M4): its bar and downbeat rules fire now, as that bar's.
    const tracking::TempoState state = engine_.state();
    if (countMoved(state.barsDeclared, barsDeclaredSeen_)) {
        guarded("a declared bar's rules", [&] {
            trigger::Context declared = context;
            declared.beatInBar = 1;
            declared.bars = state.declaredBar;
            triggers_.onBarDeclared(declared);
        });
    }
    // Last: the triggers that do not wait for a beat, and any follow-up now due.
    guarded("the rules", [&] { triggers_.advance(context); });
    // Then whatever MIDI and lighting a rule sent this round or earlier that has come due — after
    // the triggers, so a message with nothing to wait for goes in the round it was sent.
    guarded("the held messages", [&] { sink_.releaseDue(now); });
    // And a copy of the lighting frames for anything watching at redraw rate. After the
    // triggers, so a fade started this round is in the very frame that is mirrored.
    guarded("the lighting readout", [&] { mirrorLevels(now); });
    guarded("the window's copy", [&] {
        // A MIDI device went lost or came back this round — neither is a command, so nothing
        // else would refresh what a reader sees. Only when the count moves: a snapshot copies
        // the targets and the patch, which is nothing to do a thousand times a second.
        bool retake = transports_.lostMidiCount() != lostInSnapshot_;
        // And now and then, the targets are let find their addresses — a name looked up on a
        // thread of its own answers whenever it answers — and anything that changed is
        // reported, the round's own trouble included.
        if (++sinceRefresh_ >= kRefreshRounds) {
            sinceRefresh_ = 0;
            transports_.refreshTargets();
            std::vector<std::string> problems = transports_.outputProblems();
            const Snapshot::Trouble trouble = currentTrouble();
            const std::lock_guard<std::mutex> lock(snapshotMutex_);
            retake = retake || problems != snapshot_.outputProblems || trouble != snapshot_.trouble;
        }
        if (retake) {
            takeSnapshot();
        }
    });
}

} // namespace takt4::output
