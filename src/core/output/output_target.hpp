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
    };

    /// What a rule names it by, and what a UI shows. Unique within a set: two targets with
    /// one name are an editing mistake, and `findTarget` takes the first.
    std::string name;
    Kind kind = Kind::Osc;

    /// `Osc`: where to send. Ignored for `Midi`.
    std::string host = "127.0.0.1";
    std::uint16_t port = 7000;

    /// `Midi`: which device. Ignored for `Osc`.
    std::string device;

    /// Switched off sends nothing at all — neither §5.6's namespace nor a rule's message.
    /// A switch rather than deleting it, because an operator killing one feed mid-set wants
    /// it back afterwards with its addresses intact.
    bool enabled = true;

    friend bool operator==(const OutputTarget&, const OutputTarget&) = default;
};

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
/// leads with "off ". Round-trips through `parseOutputTarget`.
std::string formatOutputTarget(const OutputTarget& target);

/// The inverse. Nothing when the text is not a target — never throws, because this reads a
/// file a person may have edited and a field they are half-way through typing.
///
/// Forgiving about what it can be: the name and the `=` may be left off, in which case the
/// host and port become the name too, so `127.0.0.1:7000` on its own is still a target and
/// the format an operator already knew still works.
bool parseOutputTarget(std::string_view text, OutputTarget& out) noexcept;

} // namespace takt4::output
