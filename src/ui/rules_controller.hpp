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

    /// Which rule the editor is on, or -1 when the set is empty. Always one of `chosen()`.
    int selected() const noexcept { return selected_; }
    /// Every rule the next duplicate or delete would act on, in row order.
    ///
    /// **A set rather than one row**, on the user's ask of 2026-09-08: *"you should be able to
    /// ctrl click or shift click in this trigger list to duplicate or kill multiple of them at
    /// once"*. A rig with three layers plus a preset's worth of rules is a list somebody wants
    /// to prune in one gesture, not six.
    std::vector<int> chosen() const;

    /// What the window's callbacks do, reachable directly as well as through a click — which
    /// is how `takt4_ui_tests` drives most of them, Slint's element-*finding* API being behind
    /// SLINT_FEATURE_EXPERIMENTAL (§6).
    ///
    /// Dispatching real pointer and key events is **not** behind that flag and works against
    /// the headless platform, so anything that is about the markup rather than about these
    /// functions is tested that way instead — see "what was typed is kept when the operator
    /// clicks away", which clicks into a box, types into it and tabs out.
    void pick(int index);
    /// A click with its modifiers, as the list sends them. Control toggles this row in and
    /// out of the selection; shift takes the run from the anchor — the last row picked
    /// without shift — to this one. Neither together is a plain pick.
    void pickWith(int index, bool control, bool shift);
    void add();
    /// Deletes **every** chosen rule, and duplicates every chosen rule. With one row chosen —
    /// which is what a plain click leaves — these are what they always were.
    void remove();
    void duplicate();
    /// The same two from a row's own mark. A row inside the selection acts on the selection;
    /// a row outside it becomes the selection first, so pressing × on an unselected row does
    /// what it looks like it does rather than deleting six rules somewhere else.
    void removeAt(int index);
    void duplicateAt(int index);
    /// Adds a whole rig at once — §5.6's presets, but several rules rather than one address.
    /// "Random clips on three layers" is three rules with three seeds, and nobody should
    /// have to build the same rule three times. Index 0 is "no preset" and adds nothing; the
    /// menu counts from one. Every rule it makes is armed, like every rule the editor makes.
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
    void setCooldown(const std::string& text);

    void pickSend(int index);
    void setAddress(const std::string& address);
    void setChannel(int channel);
    void pickHostPreset(int index);
    void setSendValue(bool on);

    /// §5.8's follow-ups, which are now a list — see `trigger::FollowUp`. `index` is a row of
    /// the selected rule's own `Config::followUps`; anything outside it does nothing.
    ///
    /// A new entry is a **release** at one beat, because that is what an operator adding one
    /// nearly always means: let go of what was just pressed, a beat later. §5.6's
    /// press-then-release with the delay spelled musically.
    void addFollowUp();
    void removeFollowUp(int index);
    /// 0 is a release; 1 and up index `followKinds_`, which holds only the kinds this rule's
    /// own send kind allows.
    void pickFollowKind(int index, int choice);
    void setFollowNumber(int index, int number);
    void setFollowValue(int index, const std::string& text);
    /// The delay, in whichever unit the row is on. The two numbers are kept apart on purpose,
    /// so switching units does not rewrite the one being edited.
    void setFollowDelay(int index, const std::string& text);
    void pickFollowUnit(int index, int unit);

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
    /// The selected rule's follow-up at this row, or null.
    trigger::FollowUp* followConfig(int index) noexcept;

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
    /// The THEN SEND rows, and the list of kinds this rule's send kind allows one to be.
    void publishFollowUps();
    void publishFiring();
    /// Builds the two repeaters from nothing, so every text box in them comes back *bound*.
    /// Called by `tick` when a publisher found a row it could not honestly update in place —
    /// see `rowsDirty_`.
    void rebuildRows();
    void setStatus(const std::string& text, bool error);

    /// Puts the selection back in step with `rules_` after the set has changed shape, keeping
    /// `selected_` inside it and never leaving it empty while there is a rule to choose.
    void resettle(int wanted);

    output::OutputRunner& runner_;
    std::vector<trigger::Rule::Config> rules_;
    int selected_ = -1;
    /// One flag per rule, parallel to `rules_` — see `chosen()`. `selected_` is the row the
    /// editor shows and is always one of these; `anchor_` is the last row picked *without*
    /// shift, which is what a shift-click measures its run from, so that shift-clicking twice
    /// grows and shrinks one run rather than walking away from where it started.
    std::vector<bool> chosen_;
    int anchor_ = -1;
    RulesChanged changed_;
    /// What §5.6's targets are called on this rig, so the editor can say which of a rule's
    /// names reach something. Only the names are needed; the addresses are the main
    /// window's business.
    std::vector<output::OutputTarget> targets_;

    slint::ComponentHandle<RulesWindow> window_;
    std::shared_ptr<slint::VectorModel<RuleRow>> listModel_;
    std::shared_ptr<slint::VectorModel<OutputChoice>> choiceModel_;
    std::shared_ptr<slint::VectorModel<SlotRow>> slotModel_;
    std::shared_ptr<slint::VectorModel<FollowRow>> followModel_;
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

    /// Which rule, and which send kind, the generator rows currently on screen were built
    /// for. When either changes the rows are rebuilt rather than updated in place, so the
    /// text boxes come back *bound* — see `publishSlots`, which explains why a box that has
    /// been typed into stops following the model and what that looked like to the operator.
    std::string slotsBuiltFor_;
    trigger::Message::Kind slotsKind_ = trigger::Message::Kind::Osc;

    /// Set by a publisher that found a row it could not honestly update in place, and
    /// consumed by `tick`, which rebuilds both repeaters.
    ///
    /// **Deferred rather than done there and then**, because a publisher runs *inside* the
    /// callback of the very widget being replaced: the dropdown just picked from, the box
    /// Enter was pressed in. One redraw later is 33 ms, which nobody sees, and it means no
    /// element is ever destroyed from within its own handler.
    bool rowsDirty_ = false;
    /// What a follow-up row's kind dropdown offers past "release", in its own order — only
    /// the kinds on the same side of the OSC/MIDI divide as the rule (`trigger::
    /// followUpFits`). Rebuilt by `publishFollowUps` whenever the rule's send kind changes.
    std::vector<trigger::Message::Kind> followKinds_;
    /// What that dropdown's first entry currently reads as — "release (same note)" and the
    /// rest. It names what a release inherits, which depends on the rule's send kind and *not*
    /// on `followKinds_`: every MIDI kind allows the same followers, so a rule switched from
    /// note to CC leaves that list identical and would keep a first entry naming the wrong
    /// thing. Held so the labels are rebuilt when either moves.
    std::string followRelease_;
    std::vector<std::string> log_;
    /// What the selected rule last sent and when, on this controller's own clock.
    std::string lastFired_;
    double lastFiredAt_ = -1.0;
    /// The same for the rig as a whole, for the main window's TRIGGERS row. Kept apart from
    /// `lastFired_` because that one is the *selected* rule's and goes blank when a card
    /// that has never fired is picked, which is right there and wrong on a status row.
    std::string lastFiredAnywhere_;
};

} // namespace takt4::ui
