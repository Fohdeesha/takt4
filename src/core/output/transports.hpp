#pragma once

#include "core/output/link_session.hpp"
#include "core/output/midi_clock.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
    /// A host and port to send OSC to.
    using OscTarget = std::pair<std::string, std::uint16_t>;

    struct Config {
        bool link = false;
        std::string oscPrefix = "/takt4";
        std::vector<OscTarget> oscTargets;
        std::optional<std::string> midiClockPort;
        /// §5.5's latency offset as the tracker has it, so the transports start out
        /// agreeing with it. `setLatencySeconds` keeps them agreeing when it is moved.
        double latencySeconds = 0.0;
    };

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
    /// Null unless a MIDI port is open. Non-const like `link()` and `osc()` above, and for
    /// the same reason: these belong to whichever thread owns the transports, and Phase 6's
    /// rules send notes and CCs down this one.
    MidiClock* midiClock() const noexcept { return midi_.get(); }
    MidiOutput* midiPort() const noexcept { return midiPort_.get(); }

    /// Whether anything is actually being sent. With nothing on, `publish` and `advance`
    /// still count beats and cost nothing else, which is what makes an app that has not
    /// been configured yet behave like one that has.
    bool any() const noexcept { return linkEnabled_ || osc_->targetCount() != 0 || midi_; }

    // --- what is switched on, and changing it -------------------------------------
    //
    // These mutate the transports, so they belong to whichever thread owns them — the
    // output thread, when an `OutputRunner` is driving. Post through the runner rather
    // than reaching for these; it applies them between rounds.

    bool linkEnabled() const noexcept { return linkEnabled_; }
    void setLinkEnabled(bool on);

    const std::vector<OscTarget>& oscTargets() const noexcept { return oscTargets_; }
    void setOscTargets(const std::vector<OscTarget>& targets);

    /// The namespace every address is sent under (§5.6). Fixed at construction: it is
    /// what a receiver is configured to listen for, so changing it under a running rig
    /// would silently stop everything downstream.
    const std::string& oscPrefix() const noexcept { return oscPrefix_; }

    /// The port MIDI clock is going to, or empty for none. Opening throws if the port is
    /// not on the machine, and nothing is changed when it does.
    const std::optional<std::string>& midiClockPort() const noexcept { return midiClockPort_; }
    void setMidiClockPort(const std::optional<std::string>& port);

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
    /// The one member safe to call from another thread — a UI slider, §5.7's inbound OSC
    /// — because it writes a single atomic and reads nothing.
    void setLatencySeconds(double seconds) noexcept;
    double latencySeconds() const noexcept;

    /// Atomic so a UI can show them while the output thread is sending.
    std::uint64_t beats() const noexcept { return beats_.load(std::memory_order_relaxed); }
    std::uint64_t downbeats() const noexcept { return downbeats_.load(std::memory_order_relaxed); }

private:
    void publishToLink(const tracking::BeatEvent& event, std::int64_t hostMicros,
                       std::int64_t latencyMicros);

    std::atomic<std::int64_t> latencyMicros_;
    /// Never null, and never replaced: the audio thread holds a pointer to the session.
    std::unique_ptr<LinkSession> link_;
    std::unique_ptr<OscPublisher> osc_;
    std::unique_ptr<MidiOutput> midiPort_;
    std::unique_ptr<MidiClock> midi_;
    bool linkEnabled_ = false;
    bool started_ = false;
    /// The most recent time the transports were driven with. A MIDI port opened mid-set
    /// has to start its clock from now: starting it from when the *outputs* started would
    /// make `advance` try to emit every tick since.
    double lastNow_ = 0.0;
    std::vector<OscTarget> oscTargets_;
    std::string oscPrefix_;
    std::optional<std::string> midiClockPort_;
    double lastLinkBpm_ = -1.0;
    std::atomic<std::uint64_t> beats_{0};
    std::atomic<std::uint64_t> downbeats_{0};
};

} // namespace takt4::output
