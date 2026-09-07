#include "core/output/output_runner.hpp"

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
std::string describe(const trigger::Message& message) {
    if (message.kind == trigger::Message::Kind::Osc) {
        std::string text = message.address;
        if (message.hasArgument) {
            text += ' ';
            message.argument.appendTo(text);
        }
        return text;
    }
    const bool note = message.kind == trigger::Message::Kind::MidiNote;
    return std::string(note ? "note " : "cc ") + std::to_string(message.number) + " ch " +
           std::to_string(message.channel) + " = " + std::to_string(message.value);
}

} // namespace

OutputRunner::OutputRunner(engine::BeatEngine& engine, const Transports::Config& config)
    : engine_(engine), transports_(config), sink_(transports_), triggers_(sink_) {
    // §5.9's last-fired line and event log. Copied out of the output thread and into a
    // buffer a UI drains, which is what keeps `TriggerEngine` single-threaded.
    triggers_.setFireObserver([this](std::string_view ruleId, const trigger::Message& message,
                                     bool followUp, std::span<const trigger::Value> slots) {
        Fired entry;
        entry.ruleId = ruleId;
        entry.message = describe(message);
        entry.when = elapsed();
        entry.followUp = followUp;
        entry.slots.assign(slots.begin(), slots.end());
        const std::lock_guard<std::mutex> lock(firedMutex_);
        if (fired_.size() >= kFiredCapacity) {
            // The oldest goes. A log that stops recording when it is full stops being a
            // log, and what an operator wants is the last few seconds, not the first.
            fired_.erase(fired_.begin());
        }
        fired_.push_back(std::move(entry));
    });
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
    started_ = std::chrono::steady_clock::now();
    raisedTimer_ = raiseTimerResolution();
    // Before the thread: startOutputs enables Link and starts the MIDI clock, and the
    // thread's first round must not find them half up.
    transports_.startOutputs(0.0);
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run(); });
}

void OutputRunner::stop() noexcept {
    if (!worker_.joinable()) {
        return;
    }
    running_.store(false, std::memory_order_release);
    worker_.join();

    // Anything posted in the moments before the stop still meant something, and the
    // transports are this thread's now.
    applyCommands();

    // The last beats of a set are still beats: the engine may have called one between the
    // final round and the join. Safe on this thread now — the only other consumer of that
    // ring has been joined.
    try {
        drainOnce(elapsed());
    } catch (...) {
        // Nothing useful to do while shutting down, and letting it out of a noexcept
        // function would call std::terminate.
        errors_.fetch_add(1, std::memory_order_relaxed);
    }
    transports_.stopOutputs();
    restoreTimerResolution(raisedTimer_);
    raisedTimer_ = false;
}

void OutputRunner::post(OutputCommand command) {
    if (!running()) {
        // Nothing else is touching the transports, so there is no reason to make an
        // operator press Start before a setting takes: this is how an app is configured
        // before it is running at all.
        apply(command);
        return;
    }
    const std::lock_guard<std::mutex> lock(commandMutex_);
    pending_.push_back(std::move(command));
}

std::string OutputRunner::lastError() const {
    const std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

void OutputRunner::apply(const OutputCommand& command) {
    try {
        switch (command.kind) {
        case OutputCommand::Kind::LinkEnabled:
            transports_.setLinkEnabled(command.enabled);
            break;
        case OutputCommand::Kind::OscTargets:
            transports_.setOscTargets(command.targets);
            resolveRouting();
            break;
        case OutputCommand::Kind::Outputs:
            transports_.setOutputs(command.outputTargets);
            resolveRouting();
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
            } else {
                triggers_.release();
            }
            // After the engine, so a reader that sees the flag knows the halt has already
            // happened rather than being about to.
            panicked_.store(command.enabled, std::memory_order_relaxed);
            break;
        case OutputCommand::Kind::RuleEnabled:
            if (trigger::Rule* rule = triggers_.find(command.ruleId)) {
                rule->setEnabled(command.enabled);
            }
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
}

void OutputRunner::resolveRouting() noexcept {
    // The one place a rule's output *names* become the bits its messages carry. Neither
    // `Rule` nor `TriggerEngine` knows what an output is, and `Transports` does not know
    // what a rule is; this owns both and is where the two meet.
    const std::vector<OutputTarget>& targets = transports_.outputs();
    for (std::size_t i = 0; i < triggers_.ruleCount(); ++i) {
        trigger::Rule& rule = triggers_.rule(i);
        rule.setOutputMask(resolveOutputs(rule.config().outputs, targets));
    }
}

void OutputRunner::applyCommands() noexcept {
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
        try {
            drainOnce(elapsed());
        } catch (...) {
            // A transport that throws must not take the process down with it: an OSC
            // target can go away mid-set, and Link's networking has its own opinions
            // about sockets. Counted rather than swallowed, so it is visible.
            errors_.fetch_add(1, std::memory_order_relaxed);
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
}

} // namespace takt4::output
