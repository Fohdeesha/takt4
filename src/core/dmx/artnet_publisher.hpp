#pragma once

#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/artnet_sender.hpp"
#include "core/dmx/dmx_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace takt4::dmx {

/// Every Art-Net node takt4 sends to, and the clock that decides when.
///
/// The mirror of `output::OscPublisher`, for the protocol that works the other way round.
/// OSC publishes *events* and a target that is quiet is a target with nothing to say; Art-Net
/// publishes *state* and a target that is quiet is a target that has gone away. So this class
/// is mostly a scheduler: two rules, both from the specification, both of which a rig can
/// actually feel.
///
///   * **Never faster than 44 Hz per universe.** *"For a gateway outputting DMX512, this will
///     always be the maximum rate of 44Hz."* A node handed frames faster drops them, and
///     takt4's output thread comes round every millisecond — a thousand frames a second into
///     a node that wants forty-four is how a fade turns into a stutter.
///   * **Never slower than the keep-alive.** A universe nobody has touched is still re-sent
///     every `kKeepAliveSeconds`, because that is what tells a node the controller is alive.
///     Nodes that stop hearing a source hold, fade or release depending on how they are set,
///     and none of those is what an operator meant by "the lights are not changing right now".
///
/// The pacing is per **target and universe**, not per publisher: two nodes fed the same
/// universe keep their own clocks, so one that was added mid-set gets its first frame at once
/// rather than waiting for the other one's turn.
///
/// **Every node is fed every universe the patch uses.** A node sends on the universes it is set
/// up for and ignores the rest, so a list per node only ever saved a few datagrams, and was one
/// more box to misread; it went on 2026-09-25.
///
/// **A node can be delayed**, like every other output (`output::OutputTarget::delaySeconds`).
/// A universe is a stream of frames rather than a message to hold back, so what is delayed is
/// the stream: the node is sent the frame the patch had that long ago, from a history of each
/// universe's frames kept while any node needs one. A node set *earlier* than the rest cannot be
/// sent a frame nobody has made yet — so the lighting itself runs ahead by the largest such lead
/// (`leadSeconds`, which `RuleSink` starts a beat's effects early by), and every node is sent a
/// frame that much older again: the earliest node gets the lighting as it is made, the others
/// their delay behind it.
///
/// On the output thread, like everything it touches. Nothing here throws once it is built —
/// a node that has been unplugged is counted, not raised, because one dead node must not stop
/// the others being fed.
class ArtNetPublisher {
public:
    struct TargetConfig {
        std::string host;
        std::uint16_t port = kArtNetPort;
        /// How far behind the lighting this node is sent it, in seconds; negative is ahead of
        /// the other nodes. See the class comment.
        double delaySeconds = 0.0;
        /// The target's place in the list of outputs — its routing bit — which is what
        /// `outputOf` reports a node's problem under and what `setDelay` finds a node by. A DMX
        /// rule is not routed by it: it is routed by *fixture*, and a fixture names its universe.
        std::size_t bit = 0;
        /// `output::OutputTarget::id`, which is what `setTargets` recognises a node by.
        std::string id;
    };

    /// Adds a node. Throws `std::runtime_error` only when a socket cannot be had at all; a
    /// name that does not resolve is the sender's `problem()`, not an error here. **The tests'
    /// way in**: the application hands over every node at once, with `setTargets`.
    void addTarget(const TargetConfig& config);

    /// Replaces the nodes with `configs`, **keeping a node's sender and its pacing while its id
    /// and its host and port are the same** (the audit's H12). Rebuilding them all on every
    /// output edit reset every clock and sequence number, so a dragged slider on another row
    /// sent frames faster than the 44 Hz a node takes. Returns, for each config whose sender
    /// could not be made, its bit and why.
    ///
    /// **A node that goes — switched off, deleted, or pointed somewhere else — is sent every
    /// universe all zeros before it is let go**: its lights dark and its laser zones disarmed (the
    /// operator, 2026-10-06: *"unchecking the liberation artnet output in the main window doesnt
    /// disarm them"*, and then *"untickng artnet should black out lights / send zeroed"*). A node
    /// holds the last frame it was sent, and Liberation did — that frame armed the zone, so the
    /// beam stayed up with nothing driving it. The zeros go at the 44 Hz pace for
    /// `kFarewellSeconds`, so one lost datagram costs nothing, and then nothing: what a universe
    /// that has left the patch is sent (`DmxEngine::released`), for the same reason, so that
    /// whatever takes the node over next is not fought for it. A node brought back at the same
    /// address within that time takes its sender back and is fed as before, rather than being
    /// fought by its own farewell.
    std::vector<std::pair<std::size_t, std::string>>
    setTargets(const std::vector<TargetConfig>& configs);

    /// How long a node that has gone is sent its zeros: a released universe's time. See
    /// `setTargets`.
    static constexpr double kFarewellSeconds = DmxEngine::kReleasedSeconds;
    /// Nodes still being sent their farewell.
    std::size_t leaving() const noexcept { return leaving_.size(); }

    /// Lets every node find its address and open its socket — see `ArtNetSender::ready`.
    void refresh() noexcept;

    /// Which output node `index` is — its routing bit.
    std::size_t outputOf(std::size_t index) const noexcept { return targets_[index].bit; }

    std::size_t targetCount() const noexcept { return targets_.size(); }
    const ArtNetSender& target(std::size_t index) const noexcept { return *targets_[index].sender; }

    /// Moves the delay of the node on routing bit `bit`, and nothing else — what a dragged delay
    /// slider sends. False when no node is on that bit.
    bool setDelay(std::size_t bit, double seconds) noexcept;
    /// How far ahead the lighting must run for the earliest node to have it on time: the most
    /// negative node delay, as a positive number, or zero. See the class comment.
    double leadSeconds() const noexcept;
    /// How far behind the lighting as it is made node `index` is sent it: its own delay plus
    /// `leadSeconds`. Never negative.
    double lagOf(std::size_t index) const noexcept;

    /// PANIC: the history is forgotten, so every node — delayed or not — is sent the lighting as
    /// it is now, frozen (the audit of 2026-09-25, M5). A node sent the lighting a second late
    /// went on replaying the second of strobe and movement before the freeze, out of the
    /// operator's reach, while the node beside it had stopped. From here the history holds the
    /// frozen frame and nothing older, which the oldest-frame rule of `publish` sends a delayed
    /// node until its delay has run on past the freeze — after RELEASE too, so what it is sent
    /// then is the stillness and not what came before it.
    void forgetHistory() noexcept;

    /// How finely the history is kept: a frame at most this often while a universe changes. A
    /// node takes 44 frames a second, so a frame every 2 ms is far finer than it can show.
    static constexpr double kHistoryStep = 0.002;
    /// How far back the history reaches: the longest delay behind the longest lead, and a margin.
    static constexpr double kHistorySpan = 2.05;

    /// Sends every frame that is due, and says how many datagrams left. Call every round.
    ///
    /// **No frame goes in the 44 Hz period before a lighting cue that is known to be coming**
    /// (`setUpcomingCues`, `cueStarted`), so the cue's own frame goes the round it is due. The
    /// pacing used to run free: the period was counted from wherever the last frame happened to
    /// go, so in a universe a fade kept busy — a colour rule fading over a beat keeps it so for
    /// good — every cue waited anything from nothing to 23 ms behind the fade's last frame, a
    /// different amount every beat. Never more than 44 frames a second either way.
    std::size_t publish(const DmxEngine& engine, double now);

    /// Lighting cues held for a moment still to come, as the universe each reaches and when it
    /// starts — `output::RuleSink` says so every round, before the frames are made. Replaces what
    /// was said before.
    void setUpcomingCues(std::span<const std::pair<PortAddress, double>> cues);
    /// A lighting cue started at `at` on `universe`. A node sent the lighting late sees it its lag
    /// later, so it is held clear for the cue then, as an on-time node is for one still to come.
    void cueStarted(PortAddress universe, double at);

    /// Sends every universe's current frame to every node **now**, whatever the pacing says —
    /// the last thing takt4 transmits on the way out. A frame changed a millisecond after the
    /// last one sent would otherwise wait out the 44 Hz spacing for a round that never comes,
    /// and a node left holding the frame before the blackout holds the rig lit (the audit's H6).
    std::size_t flush(const DmxEngine& engine, double now);

    /// How long from `now` until every node may be sent another frame of every universe at the
    /// 44 Hz the protocol allows: 0 when nothing was sent in the last period. What quitting
    /// waits before `flush`, whose frame would otherwise follow the last round's by a
    /// millisecond, and a node that drops frames arriving that fast would keep the look it
    /// had (the audit of 2026-09-25, L7). Never more than one period.
    double secondsUntilPaced(double now) const noexcept;

    /// Datagrams that left, and datagrams a socket refused.
    std::uint64_t sent() const noexcept { return sent_; }
    std::uint64_t failed() const noexcept { return failed_; }
    /// How many times a delayed node's frame has been looked up in the history, how many frames
    /// those look-ups examined, and how many universes' histories are held — what a round
    /// costs, and what the history weighs.
    std::uint64_t historyLookups() const noexcept { return lookups_; }
    std::uint64_t historySteps() const noexcept { return steps_; }
    std::size_t histories() const noexcept { return history_.size(); }

private:
    /// One universe as it was at `at`.
    struct Frame {
        double at = 0.0;
        std::uint64_t revision = 0;
        std::array<std::uint8_t, kChannelsPerUniverse> levels{};
    };
    /// One universe's frames, newest last, in a ring allocated once when it is first wanted.
    struct History {
        PortAddress universe = 0;
        std::vector<Frame> ring;
        std::size_t newest = 0;
        std::size_t count = 0;
        std::uint64_t lastRevision = 0;
    };
    /// Records every universe's frame into its history, if it has moved. Only while a node lags.
    void record(const DmxEngine& engine, double now);
    /// The frame `universe` had at `at` — the newest recorded at or before it, or the oldest the
    /// history holds when it does not reach that far back. Null when there is no history yet.
    const Frame* frameAt(PortAddress universe, double at) const noexcept;

    /// What one target knows about one universe: which frame it last sent and when.
    struct Paced {
        PortAddress universe = 0;
        double lastSentAt = -1.0;
        std::uint64_t lastRevision = 0;
        bool everSent = false;
    };

    struct Target {
        std::unique_ptr<ArtNetSender> sender;
        double delay = 0.0;
        /// The lag this node is sent the lighting at now: `lagOf`, eased towards it no faster than
        /// time passes when it grows, and taken at once when it shrinks — see `publish`. Negative
        /// until its first frame.
        double lag = -1.0;
        std::size_t bit = 0;
        std::string id;
        /// One per universe actually being fed, found or made on the first frame. Held per
        /// target so that two nodes on one universe pace independently.
        std::vector<Paced> paced;
    };

    /// This target's clock for `universe`, made if it is the first frame.
    Paced& pacedFor(Target& target, PortAddress universe);
    /// The same, or null when nothing has been sent it yet; makes nothing.
    static const Paced* findPaced(const Target& target, PortAddress universe) noexcept;

    std::vector<Target> targets_;
    /// A node that has gone, and until when it is sent its farewell — negative until the first
    /// `publish` after it went, which is the first moment this object knows the time.
    struct Leaving {
        Target target;
        double until = -1.0;
    };
    std::vector<Leaving> leaving_;
    /// Sends one farewell frame of `universe` — all zeros — to `target`, now. False when the
    /// patch has no such universe, patched or released.
    bool sendFarewell(const DmxEngine& engine, Target& target, PortAddress universe, double now);
    std::vector<History> history_;
    /// Whether a cue is seen by a node `lag` behind within the period from `now`.
    bool cueSoon(PortAddress universe, double lag, double now) const noexcept;
    /// See `setUpcomingCues` and `cueStarted`; the cues started are kept as long as a lag can be.
    std::vector<std::pair<PortAddress, double>> upcoming_;
    std::vector<std::pair<PortAddress, double>> started_;
    /// The last `publish`, for easing a lag that grew; negative before the first.
    double lastPublish_ = -1.0;
    /// Whether any node is sent the lighting late, as the last `publish` found.
    bool lagging_ = false;
    /// A cue started since the last `publish`: the history keeps this round's frame.
    bool cueThisRound_ = false;
    std::uint64_t sent_ = 0;
    std::uint64_t failed_ = 0;
    mutable std::uint64_t lookups_ = 0;
    mutable std::uint64_t steps_ = 0;
};

} // namespace takt4::dmx
