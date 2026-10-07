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
/// cue to the other. A target has an **id** now, and §5.8's rules list the ones they go to.
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
        /// notes and CCs; a `MidiClock` output may name the same device — `Transports` opens
        /// each device once and shares it.
        Midi,
        /// An Art-Net node: a UDP host and port, fed every universe the fixture patch uses.
        ///
        /// **Nothing routes to it by name, and that is the difference from OSC and MIDI.** A
        /// rule sending OSC or MIDI picks its targets; a rule sending DMX picks its
        /// *fixtures*, and a fixture already says which universe it lives in. A node sends on
        /// only the universes it is set up for and ignores the rest, so every node is fed
        /// every universe. (Until 2026-09-25 a target could list which universes it carried;
        /// nothing needed it, and it was one more box to misread. A list in an older settings
        /// file is read and left out.)
        ArtNet,
        /// §5.6's 24 PPQN MIDI clock, to `device` — as many as there are devices to clock, a
        /// DAW and a drum machine each on its own. Nothing routes to one by name: it sends the
        /// clock and nothing else. Until 2026-09-25 the clock was one setting of its own, not
        /// an output with a delay.
        MidiClock,
        /// Ableton Link. **There is exactly one, always first in the list** (`ensureLinkOutput`):
        /// its switch joins or leaves the session, its delay moves the timeline takt4 puts
        /// under the music, and it has no destination — Link finds its peers itself. Until
        /// 2026-09-25 it was a switch of its own beside the outputs.
        Link,
    };

    /// What a rule is routed to it by: generated once (`newOutputId`), saved with it, and
    /// never shown or edited. **Not the name**, which is the operator's to change — rules
    /// routed by name all stopped reaching an output the moment it was renamed, and had to be
    /// routed again one at a time. Unique within a set; `ensureOutputIds` makes it so.
    std::string id;
    /// What a UI shows. The operator's to write and to change; nothing refers to it.
    std::string name;
    Kind kind = Kind::Osc;

    /// `Osc` and `ArtNet`: where to send. Ignored for the rest.
    std::string host = "127.0.0.1";
    std::uint16_t port = 7000;

    /// `Midi` and `MidiClock`: which device. Ignored for the rest.
    std::string device;

    /// Switched off sends nothing at all — neither §5.6's namespace nor a rule's message.
    /// A switch rather than deleting it, because an operator killing one feed mid-set wants
    /// it back afterwards with its addresses intact.
    bool enabled = true;

    /// `Osc`: whether takt4's own messages go here as well as the rules aimed at it — §5.6's
    /// generic namespace, `/takt4/bpm`, `/beat`, `/beat/bar`, `/downbeat`, `/confidence`,
    /// `/locked`, `/meter` and `/resync` (`OscPublisher`), several on every beat.
    ///
    /// **Off unless asked for** (the operator, 2026-10-02), which §5.6's "always publish" was not:
    /// a receiver that takes only what a rule sends it complains about the rest (2026-10-01: *"my
    /// robot egm bridge does not like the random osc spam"*), and every output a rig had got them
    /// whether it wanted them or not. The main window's "/takt4 global messages to" picks the
    /// outputs that do. A rule aimed here goes either way. Written as "global" in the line
    /// (`formatOutputTarget`); an older line has no word, and is off. Ignored for the other kinds.
    bool sendsNamespace = false;

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
    /// **Every kind honours it, the same way** (the operator's call, 2026-09-25): everything
    /// the output sends is moved by it, on top of the rig's latency.
    ///   * `ArtNet`: a universe is a stream of frames, so what is delayed is the stream — the
    ///     node is sent the frame the patch had that long ago (`dmx::ArtNetPublisher`). For a
    ///     node set *earlier* the lighting runs ahead by the largest such lead, and every other
    ///     node is sent frames that much older again.
    ///   * `MidiClock`: the clock's grid is steered onto the beat plus it (`MidiClock`).
    ///   * `Link`: the timeline is put under the beat plus it.
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

/// **A release goes before a press it shares a moment with**, and "the same moment" is this
/// close: the jitter between a moment predicted and one heard, and between a release timed to
/// the beat spacing it was fired at and the beat that then came. A release a hair after the next
/// press on its target goes with it, ahead of it, never more than this early — so "release after
/// one beat" ends the note it started rather than the one after it. `OutputRunner` hands such a
/// release over first; `RuleSink` and `OscPublisher` keep it first.
inline constexpr double kReleaseFirstSeconds = 0.010;

/// How many targets a rule can be routed to by id.
///
/// A rule carries its routing as a bit per target (`trigger::Message::outputs`), which is
/// what keeps a fire allocation-free and a follow-up safe to hold after the rule set has
/// been replaced. Sixty-four is far past any rig anyone has described; targets past it still
/// receive from rules that go *everywhere*, which is the default, and `resolveOutputs` says
/// so rather than failing quietly.
inline constexpr std::size_t kMaxRoutableTargets = 64;

/// Every bit set: what a rule that names no target means, and the default.
inline constexpr std::uint64_t kAllOutputs = ~std::uint64_t{0};

/// Makes `targets` hold exactly one `Link` output, first: one is added (switched on as
/// `enabledIfAdded` says, named "Link") when there is none, a second is dropped, and one that
/// is not first is moved there. Every set of outputs goes through this — a settings file
/// written before Link was an output, a preset imported, the defaults. True when it changed
/// anything.
bool ensureLinkOutput(std::vector<OutputTarget>& targets, bool enabledIfAdded);

/// The bits for the targets `ids` lists. An empty list is `kAllOutputs` — *"send this
/// everywhere"*, which is what a rule written before anyone had two targets meant and what a
/// rule an operator has not routed still means.
///
/// An id that matches nothing contributes no bit. That is deliberate and is not an error
/// here: the output may have been deleted, and §5.9's editor is where an operator is told the
/// routing reaches nothing.
std::uint64_t resolveOutputs(const std::vector<std::string>& ids,
                             const std::vector<OutputTarget>& targets) noexcept;

/// A fresh id no target in `existing` has. Random rather than counted, so an output deleted
/// and another added can never be handed the id a rule still holds for the first.
std::string newOutputId(const std::vector<OutputTarget>& existing);

/// Gives every target without an id one, and a second target holding an id already taken a
/// new one — a line typed or pasted by hand has none, and a duplicated line has two of one.
void ensureOutputIds(std::vector<OutputTarget>& targets);

/// Re-points routing written by name at the ids of the targets with those names: what every
/// settings file written before targets had ids holds, and what a hand-written preset may. An
/// entry that is already an id is left alone, and one that names nothing is kept as it is.
void routeByIds(std::vector<std::string>& routing, const std::vector<OutputTarget>& targets);

/// Unnamed OSC targets as `OutputTarget`s, each named after its own address.
///
/// What a caller holding nothing but host/port pairs means — `takt4-cli`'s `--osc`, and a
/// settings file written before targets had names. Naming a target after its address is what
/// the outputs field always showed anyway, and is unique as often as two identical addresses
/// are not a mistake. Each is sent takt4's own messages (`sendsNamespace`): a destination given
/// by nothing but its address has no rule aimed at it, and `--osc` is "send the namespace there".
std::vector<OutputTarget>
oscOutputs(const std::vector<std::pair<std::string, std::uint16_t>>& targets);

/// The target with this id, or null.
const OutputTarget* findTarget(const std::vector<OutputTarget>& targets, std::string_view id);

/// "main = 127.0.0.1:7000", "lights = midi MOTU Pro Audio Midi Out 1", "drums = midiclock
/// TR-8S" and "Link = link", which is how a settings file stores one. A switched-off target
/// leads with "off ", an OSC target sent takt4's own messages has " global" after its address,
/// one with a delay ends with " +120ms", and the id, where there is one, comes last as
/// " #o-1a2b3c4d". A name that would read back as something else — "off stage",
/// "a=b", one with a comma or a quote in it, or spaces at its ends — is written in double
/// quotes: "\"off stage\" = 127.0.0.1:7000". Round-trips through `parseOutputTarget`, whatever
/// the name.
std::string formatOutputTarget(const OutputTarget& target);

/// Just the destination half: "127.0.0.1:7000", "midi MOTU Pro Audio Midi Out 1",
/// "artnet 10.0.0.20:6454", "midiclock TR-8S", or "link".
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
/// the format an operator already knew still works. The `#id` may be left off too; the
/// target then has none until `ensureOutputIds` gives it one.
bool parseOutputTarget(std::string_view text, OutputTarget& out) noexcept;

} // namespace takt4::output
