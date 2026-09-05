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

    /// Opens whatever the config asks for and nothing else. Throws if a named MIDI port
    /// is not on the machine, or if Link cannot open its sockets.
    explicit Transports(const Config& config);

    Transports(const Transports&) = delete;
    Transports& operator=(const Transports&) = delete;

    /// Null when that transport was not asked for.
    LinkSession* link() const noexcept { return link_.get(); }
    OscPublisher* osc() const noexcept { return osc_.get(); }
    MidiClock* midiClock() const noexcept { return midi_.get(); }
    const MidiOutput* midiPort() const noexcept { return midiPort_.get(); }

    /// Whether anything is actually being sent. With nothing on, `publish` and `advance`
    /// still count beats and cost nothing else, which is what makes an app that has not
    /// been configured yet behave like one that has.
    bool any() const noexcept { return link_ || osc_ || midi_; }

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

    std::uint64_t beats() const noexcept { return beats_; }
    std::uint64_t downbeats() const noexcept { return downbeats_; }

private:
    void publishToLink(const tracking::BeatEvent& event, std::int64_t hostMicros,
                       std::int64_t latencyMicros);

    std::atomic<std::int64_t> latencyMicros_;
    std::unique_ptr<LinkSession> link_;
    std::unique_ptr<OscPublisher> osc_;
    std::unique_ptr<MidiOutput> midiPort_;
    std::unique_ptr<MidiClock> midi_;
    double lastLinkBpm_ = -1.0;
    std::uint64_t beats_ = 0;
    std::uint64_t downbeats_ = 0;
};

} // namespace takt4::output
