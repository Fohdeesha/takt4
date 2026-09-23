#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takt4::output {

/// One place messages go — HANDOFF §5.6's *"Multiple simultaneous targets, each with its own
/// host, port, enabled state and rule subset."*
///
/// The rule subset is the half that had never been built: every rule went to every target,
/// so a rig with a media server and a lighting desk on it could not send a clip to one and a
/// cue to the other. A target has a **name** now, and §5.8's rules name the ones they go to.
///
/// **One type with a kind tag rather than two lists**, for the reason `trigger::Generator`
/// gives: these are edited by clicking, travel in a preset (Q7) and are shown in one list, so
/// what they want is a struct that copies and serialises rather than a hierarchy.
struct OutputTarget {
    enum class Kind : std::uint8_t {
        /// A UDP host and port. §5.6's generic namespace goes to every enabled one of these
        /// as well as whatever rules aim at it.
        Osc,
        /// A MIDI device, by the name `output::listMidiOutputPorts()` gives it. Rules send
        /// notes and CCs; §5.6's 24 PPQN clock is a separate setting and may name the same
        /// device — `Transports` opens each device once and shares it.
        Midi,
        /// An Art-Net node: a UDP host and port carrying some set of DMX universes.
        ///
        /// **Nothing routes to it by name, and that is the difference from the other two.** A
        /// rule sending OSC or MIDI picks its targets; a rule sending DMX picks its
        /// *fixtures*, and a fixture already says which universe it lives in. So the routing
        /// question is answered by the patch, and what a target says is only which universes
        /// this particular node is fed — which is a fact about the cabling, not about the
        /// show. `universes` is that, and it is usually left empty.
        ArtNet,
    };

    /// What a rule names it by, and what a UI shows. Unique within a set: two targets with
    /// one name are an editing mistake, and `findTarget` takes the first.
    std::string name;
    Kind kind = Kind::Osc;

    /// `Osc` and `ArtNet`: where to send. Ignored for `Midi`.
    std::string host = "127.0.0.1";
    std::uint16_t port = 7000;

    /// `Midi`: which device. Ignored for the other two.
    std::string device;

    /// `ArtNet`: which universes this node is fed, as flat Port-Addresses.
    ///
    /// **Empty means every universe the fixture patch uses**, which is the right answer for
    /// the ordinary rig of one node and the reason the field can be left alone until there
    /// are two. It is only worth filling in when a rig has several nodes and each carries a
    /// different slice — sending a node universes it does not own is harmless but wasteful,
    /// and on a node that merges it is worse than wasteful.
    std::vector<std::uint16_t> universes;

    /// Switched off sends nothing at all — neither §5.6's namespace nor a rule's message.
    /// A switch rather than deleting it, because an operator killing one feed mid-set wants
    /// it back afterwards with its addresses intact.
    bool enabled = true;

    /// Where this target's messages sit relative to the beat, in seconds — §5.5's latency
    /// offset, but per target rather than one number for the whole rig.
    ///
    /// One offset was never enough, because the lag being compensated does not belong to
    /// takt4: it belongs to the thing on the end of each cable. A media server keying a clip
    /// is a frame or two behind; a robot that has to physically *move* is a great deal more,
    /// and it is behind by an amount that has nothing to do with what the lighting desk on
    /// the next output is doing. §5.5's single slider moves all of them together, which is
    /// the one adjustment that cannot help a rig with two different lags in it.
    ///
    /// **Positive is later, negative is earlier, and both are real**: a target at −300 ms has
    /// a beat's messages 300 ms before the beat is in the music. It is the number an operator
    /// actually has in their head — *this device is 300 ms slow, take 300 ms off it* — and it
    /// stays right as the tempo moves, because it is measured from the beat, not from a clock.
    ///
    /// Earlier than a beat has been *heard* works because the output thread does not wait to
    /// hear it: while the tracker is locked it fires each beat ahead of time on a prediction,
    /// by as much as the earliest target needs (`BeatScheduler`, `Transports::leadSeconds`),
    /// and each target is then held until the beat's moment plus §5.5's rig-wide offset plus
    /// this. Until the audit (H4) a negative offset instead held every message for what was
    /// left of a beat — so a bar-1 cue landed just before beat 2, and a message about nothing
    /// in particular waited most of a beat. Two things a prediction cannot do: a beat heard
    /// while the tracker is hunting goes the moment it is heard, which is as early as it can;
    /// and a message about *now* — a manual fire, a lock change — goes now plus whatever of the
    /// offset is positive.
    ///
    /// Held on the output thread and sent when it comes due (`OscPublisher::flushDue`, and
    /// `RuleSink::releaseDue` for MIDI), so the resolution is that thread's round — 1 ms,
    /// which is why it raises Windows' timer granularity.
    ///
    /// **Ignored for `ArtNet`**, and the reason is what DMX is rather than an omission. The
    /// other two send *events*, and an event can be held in a queue and let go later. A DMX
    /// universe is a continuous stream of frames that is never not being sent, so there is no
    /// message to hold back — what would need moving is the moment an *effect starts*, which
    /// belongs to the rule that fired it and not to the cable it goes down. §5.5's rig-wide
    /// latency already moves that: `RuleSink` starts an effect at its beat's moment plus it.
    double delaySeconds = 0.0;

    friend bool operator==(const OutputTarget&, const OutputTarget&) = default;
};

/// How far either way a target may be offset. A whole second is longer than a beat anywhere
/// in the state space's 55-215 BPM, so any phase of any beat is reachable from either
/// direction; past that an operator is describing a rig problem rather than a latency.
///
/// A negative offset reaches as far ahead as the prediction does — `BeatScheduler::kMaxAhead`
/// beats past the last one heard. Further than that a beat is fired as soon as it is within
/// reach, and each target has it as close to its time as it can.
inline constexpr double kMaxOutputDelaySeconds = 1.0;
inline constexpr double kMinOutputDelaySeconds = -1.0;

/// How many targets a rule can be routed to by name.
///
/// A rule carries its routing as a bit per target (`trigger::Message::outputs`), which is
/// what keeps a fire allocation-free and a follow-up safe to hold after the rule set has
/// been replaced. Sixty-four is far past any rig anyone has described; targets past it still
/// receive from rules that go *everywhere*, which is the default, and `resolveOutputs` says
/// so rather than failing quietly.
inline constexpr std::size_t kMaxRoutableTargets = 64;

/// Every bit set: what a rule that names no target means, and the default.
inline constexpr std::uint64_t kAllOutputs = ~std::uint64_t{0};

/// The bits for `names` within `targets`. An empty list is `kAllOutputs` — *"send this
/// everywhere"*, which is what a rule written before anyone had two targets meant and what a
/// rule an operator has not routed still means.
///
/// A name that matches nothing contributes no bit. That is deliberate and is not an error
/// here: a preset written on a rig with a "lights" output, opened on one without, should keep
/// saying "lights" so that plugging it back in restores the routing. §5.9's editor is where
/// an operator is told the name currently reaches nothing.
std::uint64_t resolveOutputs(const std::vector<std::string>& names,
                             const std::vector<OutputTarget>& targets) noexcept;

/// Unnamed OSC targets as `OutputTarget`s, each named after its own address.
///
/// What a caller holding nothing but host/port pairs means — `takt4-cli`'s `--osc`, and a
/// settings file written before targets had names. Naming a target after its address is what
/// the outputs field always showed anyway, and is unique as often as two identical addresses
/// are not a mistake.
std::vector<OutputTarget>
oscOutputs(const std::vector<std::pair<std::string, std::uint16_t>>& targets);

/// The first target with this name, or null.
const OutputTarget* findTarget(const std::vector<OutputTarget>& targets, std::string_view name);

/// "main = 127.0.0.1:7000" and "lights = midi MOTU Pro Audio Midi Out 1", which is how a
/// settings file stores one and how §5.9's outputs field shows it. A switched-off target
/// leads with "off ", and one with a delay ends with " +120ms". Round-trips through
/// `parseOutputTarget`.
std::string formatOutputTarget(const OutputTarget& target);

/// Just the destination half: "127.0.0.1:7000", or "midi MOTU Pro Audio Midi Out 1".
///
/// What §5.9's outputs list puts in the address box, the name having a box of its own. It is
/// also what `formatOutputTarget` writes after the `=`, and `parseOutputTarget` takes it back
/// on its own — the name is optional there for exactly this reason.
std::string formatOutputAddress(const OutputTarget& target);

/// The inverse. Nothing when the text is not a target — never throws, because this reads a
/// file a person may have edited and a field they are half-way through typing.
///
/// Forgiving about what it can be: the name and the `=` may be left off, in which case the
/// host and port become the name too, so `127.0.0.1:7000` on its own is still a target and
/// the format an operator already knew still works.
bool parseOutputTarget(std::string_view text, OutputTarget& out) noexcept;

} // namespace takt4::output
