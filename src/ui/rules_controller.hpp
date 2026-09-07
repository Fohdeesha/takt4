#pragma once

#include "core/output/output_runner.hpp"
#include "core/trigger/rule.hpp"

#include "main_window.h" // generated; holds RulesWindow too — see src/ui/CMakeLists.txt

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace takt4::ui {

/// HANDOFF §5.9's rule editor, behind its own window.
///
/// §8's exit criterion for Phase 6 is entirely about this: *"A rule that fires a
/// non-repeating random clip on every fourth downbeat can be built entirely by clicking, in
/// under a minute, by someone who has not read the docs."* Everything under that sentence
/// has worked since Phase 6's engine half.
///
/// **It edits its own copy of the rules and posts the set whole.** The live rules belong to
/// §4.2's output thread — `OutputRunner::triggers()` is documented as safe to read only
/// while that thread is stopped — and an editor has to work during a set. So every edit
/// changes `rules_` and hands the whole set over through `OutputCommand::Rules`, which is
/// the command's stated purpose and the same shape `WindowController::postOptions` uses for
/// a slider. Replacing a short vector of small structs a few times a second is nothing next
/// to what the output thread does between rounds anyway.
///
/// Nothing here touches a Slint property outside `tick()` and the callbacks, both of which
/// run on the UI thread — §7.5's rule.
class RulesController {
public:
    /// Called whenever the rule set changes, so the owner can save it (Q7) and keep its own
    /// copy in step. The controller does not know where settings live and does not want to.
    using RulesChanged = std::function<void(const std::vector<trigger::Rule::Config>&)>;

    /// How many messages the event log remembers. Phase 6's list asks for a log tab; this is
    /// the pane. Bounded because a rule on every beat at 214 BPM is 3.6 lines a second, and
    /// an unbounded log of a four-hour set is a memory leak with a scrollbar.
    static constexpr std::size_t kLogLines = 200;

    /// The runner must outlive this. Nothing is shown until `show()`.
    RulesController(output::OutputRunner& runner, std::vector<trigger::Rule::Config> rules);

    /// The outputs a rule may be routed to, for the editor to show and check names against.
    /// Set again whenever the main window's outputs field changes.
    void setTargets(std::vector<output::OutputTarget> targets);

    RulesController(const RulesController&) = delete;
    RulesController& operator=(const RulesController&) = delete;

    void setRulesChanged(RulesChanged changed) { changed_ = std::move(changed); }

    /// Shows the window, or brings it forward if it is already up.
    void show();
    void hide();
    bool visible() const noexcept { return visible_; }

    /// One round of refreshing what the window shows — fire counts, the last-fired line,
    /// the log. Driven by the main window's redraw timer so the app has one timer, and
    /// cheap enough to call at 30 Hz whether or not the window is up.
    void tick();

    RulesWindow& window() { return *window_; }

    /// The last message any rule sent, as it went out. The main window's TRIGGERS row shows
    /// it too — an operator watching the tracker should not have to open the editor to see
    /// whether anything is leaving the machine. Empty until something fires.
    const std::string& lastFiredAnywhere() const noexcept { return lastFiredAnywhere_; }

    /// How many times the rule with this id has fired, releases not counted. Zero for a rule
    /// that has not fired and for one this controller has never heard of.
    std::uint64_t firesOf(std::string_view ruleId) const;

    const std::vector<trigger::Rule::Config>& rules() const noexcept { return rules_; }
    /// Replaces the set from outside — a preset load. Keeps the selection where it can.
    void setRules(std::vector<trigger::Rule::Config> rules);

    /// Which rule the editor is on, or -1 when the set is empty.
    int selected() const noexcept { return selected_; }

    /// What the window's callbacks do, reachable directly as well as through a click —
    /// which is how `takt4_ui_tests` drives them, Slint's element-level testing API being
    /// behind SLINT_FEATURE_EXPERIMENTAL (§6).
    void pick(int index);
    void add();
    void remove();
    void duplicate();
    /// Adds a whole rig at once — §5.6's presets, but several rules rather than one address.
    /// "Random clips on three layers" is three rules with three seeds, and nobody should
    /// have to build the same rule three times. Index 0 is the picker's own label and adds
    /// nothing. Every rule it makes is switched off, like every rule the editor makes.
    void addRig(int index);
    void rename(const std::string& name);
    void setEnabled(bool on);
    void test();
    void panic();

    void pickTrigger(int index);
    void setEvery(int every);
    void setPulses(int pulses);
    /// §5.6's rule subset, as a comma-separated list of output names. Empty is everywhere.
    /// Not reachable from the window any more — the routing is ticked from the rig's own
    /// list — but it is still how a preset arrives and how a whole routing is set at once.
    void setOutputs(const std::string& text);
    /// One output named or un-named. Un-naming the last one is "every output" again: an
    /// empty list *is* everywhere (`output::resolveOutputs`), so the two cannot disagree.
    void setOutputChosen(const std::string& name, bool chosen);
    /// Back to reaching every output, which is what naming none of them means.
    void chooseAllOutputs();

    void setMinConfidence(double value);
    void setIntensity(int which, bool allowed);
    void setBpmRange(const std::string& text);
    void setProbability(double value);
    void setCooldownMs(double milliseconds);

    void pickSend(int index);
    void setAddress(const std::string& address);
    void setChannel(int channel);
    void pickHostPreset(int index);
    void setSendValue(bool on);
    void setFollowUp(bool on);
    void setFollowUpValue(const std::string& text);
    void setFollowUpMs(double milliseconds);

    /// The generators, one per `{...}` of the address plus the value and the MIDI note.
    /// `slot` indexes what the window is showing, which `slotConfigs()` decides.
    void pickSlotKind(int slot, int kind);
    void setSlotPool(int slot, bool list);
    void setSlotRange(int slot, const std::string& text);
    void setSlotValues(int slot, const std::string& text);
    void setSlotNoRepeat(int slot, int within);
    void setSlotFixed(int slot, const std::string& text);
    void pickSlotLive(int slot, int source);
    void pickSlotShape(int slot, int shape);
    void setSlotRampBars(int slot, int bars);
    void setSlotRampFloat(int slot, bool asFloat);

    void clearLog();

private:
    /// The rule with this id, or null. Rule ids have to be unique: §5.7 addresses one by
    /// id and `TriggerEngine::find` takes the first of a duplicate pair.
    const trigger::Rule::Config* findRule(std::string_view id) const noexcept;
    /// The selected rule, or null when there is none.
    trigger::Rule::Config* current() noexcept;
    const trigger::Rule::Config* current() const noexcept;
    /// The generator a slot index names, in the order `publishSlots` lists them: the
    /// address's placeholders, then the value, then the MIDI number.
    trigger::Generator::Config* slotConfig(int slot) noexcept;

    /// Hands the set to the output thread and tells the owner. Every edit ends here.
    void commit();
    /// Keeps `segments` as long as the address has placeholders — §5.8's validity is mostly
    /// that count, so an edit that adds a `{}` should give the operator a chip rather than a
    /// rule that refuses to fire until they work out why.
    void matchSegmentsToAddress(trigger::Rule::Config& rule);

    void publishAll();
    void publishList();
    void publishSelected();
    /// The "send to" list: every output this rig has, ticked where the rule names it, plus
    /// any name the rule carries that this rig has not got.
    void publishOutputChoices();
    void publishSlots();
    void publishFiring();
    void setStatus(const std::string& text, bool error);

    output::OutputRunner& runner_;
    std::vector<trigger::Rule::Config> rules_;
    int selected_ = -1;
    RulesChanged changed_;
    /// What §5.6's targets are called on this rig, so the editor can say which of a rule's
    /// names reach something. Only the names are needed; the addresses are the main
    /// window's business.
    std::vector<output::OutputTarget> targets_;

    slint::ComponentHandle<RulesWindow> window_;
    std::shared_ptr<slint::VectorModel<RuleRow>> listModel_;
    std::shared_ptr<slint::VectorModel<OutputChoice>> choiceModel_;
    std::shared_ptr<slint::VectorModel<SlotRow>> slotModel_;
    std::shared_ptr<slint::VectorModel<slint::SharedString>> logModel_;
    bool visible_ = false;

    /// Fires seen per rule, so each card can say how often it has gone off — the number in
    /// the rule list. Counted here rather than read from `trigger::Rule::fires()`, because
    /// the live rules belong to the output thread and are safe to read only while it is
    /// stopped; `OutputRunner::Fired` is the seam that crosses that boundary.
    ///
    /// **Keyed by rule id, not by position.** A vector indexed by row lost its meaning the
    /// moment a rule was deleted from the middle — every count below it would shift up one
    /// and start describing a different rule. An id survives reordering, and a preset load
    /// brings new ids, which is exactly when the counts should start again.
    ///
    /// §5.6's release half is not counted: see `OutputRunner::Fired::followUp`.
    std::unordered_map<std::string, std::uint64_t> firesSeen_;
    /// What each rule's generators last produced, by rule id — `trigger::Rule::lastSlots`,
    /// kept per rule so selecting another card shows *its* last values rather than nothing
    /// until it happens to fire again.
    std::unordered_map<std::string, std::vector<trigger::Value>> slotsSeen_;
    std::vector<std::string> log_;
    /// What the selected rule last sent and when, on this controller's own clock.
    std::string lastFired_;
    double lastFiredAt_ = -1.0;
    /// The same for the rig as a whole, for the main window's TRIGGERS row. Kept apart from
    /// `lastFired_` because that one is the *selected* rule's and goes blank when a card
    /// that has never fired is picked, which is right there and wrong on a status row.
    std::string lastFiredAnywhere_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

} // namespace takt4::ui
