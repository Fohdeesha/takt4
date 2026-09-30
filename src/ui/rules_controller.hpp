#pragma once

#include "core/dmx/fixture.hpp"
#include "core/output/output_runner.hpp"
#include "core/settings/settings.hpp"
#include "core/trigger/rule.hpp"
#include "ui/delete_guard.hpp"
#include "ui/model_rows.hpp"

#include "main_window.h" // generated; holds RulesWindow too — see src/ui/CMakeLists.txt

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

    /// The fixtures and groups a DMX rule may be aimed at. Set again whenever the patch
    /// changes, or the editor goes on offering a fixture that has been renamed out from under
    /// it — and a rule aiming at the old name would quietly stop reaching anything.
    void setPatch(std::vector<dmx::Fixture> patch);

    RulesController(const RulesController&) = delete;
    RulesController& operator=(const RulesController&) = delete;

    void setRulesChanged(RulesChanged changed) { changed_ = std::move(changed); }

    /// Shows the window, or brings it forward if it is already up.
    void show();
    void hide();
    bool visible() const noexcept { return visible_; }

    /// The window's folds — A, B, C, D and the event log — and the log's height, as a settings
    /// file left them (HANDOFF §0.5: "Folds are the window's and remembered across launches, like
    /// the main window's"). Machine-local, as those are.
    void applyLayout(const settings::MachineSettings& machine);
    /// And as they are now, into `machine`, for saving.
    void layoutInto(settings::MachineSettings& machine) const;
    /// A section's fold (0 to 3, A to D) or the log's (4), flipped — the arrows, which commit what
    /// was being typed first. A folded section's body is clipped, not removed, so a box in it still
    /// commits when it lets go.
    void toggleFold(int which);

    /// One round of refreshing what the window shows — fire counts, the log. Driven by the main
    /// window's redraw timer so the app has one timer, and cheap enough to call at 30 Hz whether
    /// or not the window is up.
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
    /// §5.7's per-rule mute, from the window rather than from a control surface. **Live state,
    /// not configuration**: it goes straight to the output thread and is never saved, for the
    /// reason `trigger::Rule::muted` gives — a preset that loaded silent would look exactly
    /// like a preset that did not load.
    void setMuted(bool on);
    /// The same, from row `index`'s dot in the list — whichever rule that is, picked or not
    /// (HANDOFF §0.5: "the dot is the mute switch"). The selection does not move.
    void setMutedAt(int index, bool on);
    /// A relative multiplier on the selected rule's interval — the ÷2 and ×2 buttons. Live
    /// like the mute, and mirrored here so the readout can say what the rule is doing without
    /// reading the output thread's rules, which is unsafe while it runs.
    void nudgeRate(double factor);
    void test();
    /// §5.8's PANIC: engages, and only engages — a second press while panicked does nothing,
    /// so a double-click cannot let go of it (the audit's H18). `releasePanic` is RELEASE.
    void panic();
    void releasePanic();

    void pickTrigger(int index);
    void setEvery(int every);
    void setPulses(int pulses);
    /// §5.6's rule subset, as a comma-separated list of output names. Empty is everywhere.
    /// Not reachable from the window any more — the routing is ticked from the rig's own
    /// list — but it is how a whole routing is set at once. The rule keeps the *ids* of the
    /// outputs named, so renaming one afterwards leaves it routed.
    void setOutputs(const std::string& text);
    /// One output, by its id, routed or not. Un-routing the last one is "every output" again:
    /// an empty list *is* everywhere (`output::resolveOutputs`), so the two cannot disagree.
    void setOutputChosen(const std::string& id, bool chosen);
    /// Back to reaching every output, which is what naming none of them means.
    void chooseAllOutputs();

    /// B's tick: whether the ONLY IF stage applies at all (`Rule::Config::conditionsOn`).
    void setConditionsOn(bool on);
    void setMinConfidence(double value);
    void setIntensity(int which, bool allowed);
    void setBpmRange(const std::string& text);
    void setProbability(double value);
    /// What was typed into the two sliders' readings: "0.7" for the confidence, "90" or "90%"
    /// for the probability. Read, clamped, and anything that is not a number said and refused.
    void setConfidenceTyped(const std::string& text);
    void setProbabilityTyped(const std::string& text);
    /// A's "at most once every N ms", in milliseconds as typed. Kept whatever the trigger, and
    /// read by the engine only where `trigger::takesCooldown` says.
    void setCooldown(const std::string& text);

    void pickSend(int index);
    void setAddress(const std::string& address);
    void setChannel(int channel);
    void pickHostPreset(int index);
    void setSendValue(bool on);

    // --- the lighting half, for a rule whose send kind is DMX --------------------------
    //
    // One fixture or group named or un-named, and "none" — which for a DMX rule is a rule
    // that reaches nothing and says so, unlike an empty *output* list. See
    // `dmx::resolveFixtures` for why the two defaults are opposites.
    /// `key` is a fixture's id or a group's label — what the fixture list's tick sends.
    void setFixtureChosen(const std::string& key, bool chosen);
    void chooseNoFixtures();
    /// Which effect — an index into `dmx::kEffectKinds`.
    void pickEffect(int index);
    /// Which channel it drives, for the effects that aim at one — an index into
    /// `dmx::kAimableRoles`.
    void pickRole(int index);
    void pickCurve(int index);
    void pickPathShape(int index);
    /// How long the effect runs, in whichever unit the row is on. Two numbers are kept apart
    /// on purpose, so switching units does not rewrite the one being edited — the same reason
    /// `FollowUp::delayBeats` gives.
    void setDuration(const std::string& text);
    void pickDurationUnit(int unit);
    /// The low end of a flash, pulse or strobe.
    void setBase(int level);
    /// How many times a repeating effect repeats within its duration.
    void setCycles(const std::string& text);
    /// A strobe's on-fraction, as a percentage.
    void setDuty(float percent);
    /// A hue sweep's two ends, in degrees.
    void setHueRange(const std::string& text);
    /// A path's radius, as a percentage of the movement window's half-width.
    void setSize(float percent);
    /// Whether the color comes from a palette or from three component generators — an index
    /// into `trigger::kColorModes`. See `trigger::ColorMode`.
    void pickColorMode(int index);

    // --- the palette, for a color rule in `ColorMode::Palette` ------------------------
    //
    // The entries of the color generator's list, each a swatch with a picker behind it.
    // **A palette is a `List` of text values and nothing more** — see `DmxSend::color` —
    // so these edit `values` and the generator machinery does the rest.
    void addPaletteColor();
    void removePaletteColor(int index);
    /// One entry from its picker: hue in degrees, saturation and brightness as percentages.
    void setPaletteColor(int index, float hue, float saturation, float brightness);
    /// And from the hex box beside it, which is how a color gets pasted in from anywhere.
    void setPaletteHex(int index, const std::string& text);

    /// Sends one color to the fixtures this rule names, right now and with no fade — what
    /// the pickers do as they are dragged, so an operator is choosing against the light
    /// coming out of the fixture rather than against a square on a screen. Nothing when the
    /// rule is not a lighting rule or names no fixture.
    void previewColor(dmx::Color color);

    /// Told by each picker's `PopupWindow` as it opens and closes. A swatch's code box lives in
    /// its picker, and a click outside drops the picker with the box in it before the box can say
    /// it lost the focus — so a close commits what was typed there.
    ///
    /// **And no repeater is ever built again while a picker is open, because none is built again
    /// at all**: the crash of 2026-09-16 was the palette's repeater rebuilt under the hue slider
    /// being dragged, because a std widget in it had set its own value and had to come back as a
    /// new element. Every control in the window is weltformat.slint's now, which never does, so
    /// every row is updated where it stands (`writeRows`).
    void setPickerOpen(bool open);
    /// Whether one is open. Public so a test can find a swatch by clicking until a picker opens
    /// rather than by holding a coordinate that would rot the first time a row moved.
    bool pickerOpen() const noexcept { return pickerOpen_; }

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
    /// `slot` indexes what the window is showing, which `trigger::slotLayout` decides.
    void pickSlotKind(int slot, int kind);
    void setSlotPool(int slot, bool list);
    void setSlotRange(int slot, const std::string& text);
    void setSlotValues(int slot, const std::string& text);
    /// "BPM normalised"'s tempo range, as "20 - 500". Refused, with a status, when it is not two
    /// numbers with the second above the first.
    void setSlotNormalise(int slot, const std::string& text);
    /// A weighted generator's values and their weights, as "7:3, 12:1" — a bare value weighs 1.
    void setSlotWeights(int slot, const std::string& text);
    void setSlotNoRepeat(int slot, int within);
    void setSlotFixed(int slot, const std::string& text);
    /// A color slot from the picker: hue in degrees, saturation and brightness as
    /// percentages. Written back to the generator as the `#rrggbb` the effect parses, so the
    /// hex box and the sliders are two views of one value rather than two settings.
    void setSlotColor(int slot, float hue, float saturation, float brightness);
    void pickSlotLive(int slot, int source);
    void pickSlotShape(int slot, int shape);
    void setSlotRampBars(int slot, int bars);
    void setSlotRampFloat(int slot, bool asFloat);

    void clearLog();

private:
    /// The rule with this id, or null. Rule ids have to be unique: §5.7 addresses one by
    /// id and `TriggerEngine::find` takes the first of a duplicate pair.
    const trigger::Rule::Config* findRule(std::string_view id) const noexcept;
    /// A seed no rule of the set has, nor any of `pending` (rules about to be added): past the
    /// largest in use, by the stride seeds have always been spaced by. `add` counted the rules
    /// and `duplicate` stepped from its source, and both handed out a seed another rule already
    /// had once one had been deleted or copied — two rules then drew the same "random" values
    /// (the 2026-09-25 audit's L9).
    std::uint64_t freshSeed(const std::vector<trigger::Rule::Config>& pending = {}) const;
    /// The selected rule, or null when there is none.
    trigger::Rule::Config* current() noexcept;
    const trigger::Rule::Config* current() const noexcept;
    /// The generator a slot index names, in the order `publishSlots` lists them — both are
    /// `trigger::slotLayout`, which is the order `Rule::lastSlots` records.
    trigger::Generator::Config* slotConfig(int slot) noexcept;
    /// Says an edit to `slot` has landed: if that slot is a MIDI rule's note or controller, the
    /// number is one the operator chose and the rule may fire (the audit's C7). Called by each
    /// slot setter just before it commits, so an edit that was refused does not count.
    void choseSlot(int slot) noexcept;
    /// Where a box being typed into sits. See `typing_`.
    enum class TypedIn : std::uint8_t { Slot, FollowUp, Palette, Rule };
    /// The rule's own boxes, numbered as `rule-typed` numbers them — the row index of a
    /// `TypedIn::Rule` is always 0 and this is its field.
    enum RuleBox : int {
        kAddressBox = 0,
        kBpmRangeBox,
        kCooldownBox,
        kDurationBox,
        kCyclesBox,
        kHueBox,
        kEveryBox,
        kPulsesBox,
        kChannelBox,
        kBaseBox,
    };
    /// A keystroke in a box; `index` is the row and `field` which box of it — the numbering is
    /// the markup's (`slot-typed`, `follow-typed`, `palette-typed`, `rule-typed`). See `typing_`.
    void noteTyping(TypedIn where, int index, int field, std::string text);
    /// That box committed on its own, so there is nothing left to carry over.
    void typed(TypedIn where, int index, int field) noexcept;
    /// Row `index` of `where` was removed, so what was being typed into a row follows it.
    void rowRemoved(TypedIn where, int index) noexcept;
    /// Whether this rule box has keystrokes nobody has committed yet, on the rule showing — so a
    /// republish from outside (`setTargets`, `setPatch`) does not write over them.
    bool typingInto(RuleBox box) const noexcept;
    /// Whether a row box's own commit only repeats what `commitTyping` already committed for
    /// it. See `echo_`.
    bool echoes(TypedIn where, int field, const slint::SharedString& text) noexcept;
    /// Commits what `typing_` holds to the rule it was typed for, if that is still the rule
    /// showing. Called before **every** action — see the constructor, where each callback is
    /// wired — so that what an action acts on is what is on the screen.
    void commitTyping();
    /// Takes in what a control surface changed on the live rules — enabled, mute, rate — so
    /// the card shows what is really running (the audit's H7). Enabled is configuration and is
    /// saved, so it goes into `rules_` and out through the changed callback; mute and rate are
    /// live gestures and only reach the mirrors. Touches only those controls: a full republish
    /// would overwrite a box somebody is typing in.
    void adoptLive(const std::vector<output::OutputRunner::LiveRule>& live);
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
    /// The same for fixtures: every fixture and group this rig has, ticked where the rule
    /// names it, plus any name the rule carries that this rig has not got.
    void publishFixtureChoices();
    /// The DMX effect's own controls — which effect, which channel, how long, and the handful
    /// of numbers each effect uses.
    void publishDmx();
    /// What the "on <channel>" dropdown should say underneath itself: whether the channel the
    /// effect is aimed at exists on the fixtures the rule names, and what happens where it
    /// does not. Empty when there is nothing worth saying.
    std::string describeRoleReach(const trigger::DmxSend& send) const;
    void publishSlots();
    /// The palette swatches, for a color rule whose generator draws from a list.
    void publishPalette();
    /// The selected rule's color generator, or null when it has not got one — the palette
    /// editor's target, which is `DmxSend::color` and never one of the three components.
    trigger::Generator::Config* paletteConfig() noexcept;
    /// The whole range this slot's value can take, where that is a fact about the value rather
    /// than the operator's choice: a DMX level is a byte, a pan is a percentage. Nothing for an
    /// OSC segment or a MIDI value. What a slot switched to shuffle seeds its range from.
    std::optional<std::pair<int, int>> slotRange(int slot) noexcept;
    /// The THEN SEND rows, and the list of kinds this rule's send kind allows one to be.
    void publishFollowUps();
    /// The line each section's heading reads while it is folded — what it holds — for the rule
    /// in front of the operator (`rule_text::describeWhen` and the rest).
    void publishSummaries();
    /// The event log, newest first, from `log_`.
    void publishLog();
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
    /// What fixtures and groups this rig has, so the editor can say which of a rule's names
    /// reach something. Only the names and groups are needed; the addressing is the patch
    /// editor's business.
    std::vector<dmx::Fixture> patch_;

    slint::ComponentHandle<RulesWindow> window_;
    std::shared_ptr<slint::VectorModel<RuleRow>> listModel_;
    Repeater<OutputChoice> choiceRows_;
    Repeater<OutputChoice> fixtureRows_;
    Repeater<SlotRow> slotRows_;
    /// A rule row's ×, and a follow-up row's: a double-click on either is one deletion.
    DeleteGuard ruleMarks_;
    DeleteGuard followMarks_;
    /// The palette swatches. A model of its own rather than a field of `SlotRow`, because a
    /// rule has at most one color generator and a repeater nested inside a repeater's own
    /// struct is not a thing Slint models do.
    Repeater<PaletteEntry> paletteRows_;
    Repeater<FollowRow> followRows_;
    std::shared_ptr<slint::VectorModel<LogLine>> logModel_;
    bool visible_ = false;

    /// Fires seen per rule, so each card can say how often it has gone off — the number in
    /// the rule list. Counted here rather than read from `trigger::Rule::fires()`, because
    /// the live rules belong to the output thread and are safe to read only while it is
    /// stopped; `OutputRunner::Fired` is the seam that crosses that boundary.
    ///
    /// **Keyed by rule id, not by position.** A vector indexed by row lost its meaning the
    /// moment a rule was deleted from the middle — every count below it would shift up one
    /// and start describing a different rule. An id survives reordering; a loaded set starts
    /// the counts again whatever its ids (`setRules`).
    ///
    /// §5.6's release half is not counted: see `OutputRunner::Fired::followUp`.
    std::unordered_map<std::string, std::uint64_t> firesSeen_;
    /// What each rule's generators last produced, by rule id — `trigger::Rule::lastSlots`,
    /// kept per rule so selecting another card shows *its* last values rather than nothing
    /// until it happens to fire again.
    std::unordered_map<std::string, std::vector<trigger::Value>> slotsSeen_;
    /// The live per-rule gestures, by rule id: muted, and the interval multiplier.
    ///
    /// **Mirrored here rather than read back from the rules**, for the reason `firesSeen_`
    /// gives: the live rules belong to the output thread and are safe to read only while it
    /// is stopped. Keyed by id so that deleting a rule from the middle does not shift every
    /// state below it onto a different rule.
    ///
    /// Not saved, and forgotten when a set is loaded (`setRules`): a loaded set starts with none,
    /// on the running side too (`TriggerEngine::setRules`'s `fresh`), whatever its ids.
    std::unordered_map<std::string, bool> mutedSeen_;
    std::unordered_map<std::string, double> rateSeen_;
    /// `OutputRunner::liveRulesVersion` as `tick` last adopted it. See `adoptLive`.
    std::uint64_t liveSeen_ = 0;
    /// What is in a box that has not been committed yet — kept from its keystrokes, so that
    /// switching rules commits it to the rule it was typed for (the audit's M16). Switching used
    /// to rebuild the rows at once, which destroyed the box and what was in it; and a box that
    /// survived committed, when it lost the focus, into whichever rule was showing by then.
    ///
    /// **Every box, and before every action** (the audit of 2026-09-25, M18). A box commits when
    /// it loses the keyboard, a turn of the event loop *after* the click that took it — so TEST
    /// fired the rule as it was before the edit, and + ADD, MUTE, ÷2 or a follow-up's × wrote the
    /// stored value back over what was typed. Only one box has the keyboard, so one is enough.
    struct Typing {
        std::string ruleId;
        TypedIn where = TypedIn::Slot;
        int index = 0;
        int field = 0;
        std::string text;
    };
    std::optional<Typing> typing_;
    /// The last thing `commitTyping` committed for a box, until that box's own late commit
    /// arrives. By then the rows may have moved — a follow-up's × above the box shifts it onto
    /// the next row — and the box would commit what was typed for one row into another. Cleared
    /// by the next keystroke anywhere, since only typing can put different text in a box.
    std::optional<Typing> echo_;

    /// Which rule, and which send kind, the generator rows currently on screen were built
    /// for. When either changes the rows are built from nothing rather than updated in place,
    /// so a box that had the keyboard does not go on holding what was typed for the rule
    /// before — see `publishSlots`.
    std::string slotsBuiltFor_;
    /// And which rule the follow-up rows were built for — see `publishFollowUps`.
    std::string followsBuiltFor_;
    trigger::Message::Kind slotsKind_ = trigger::Message::Kind::Osc;
    /// And which DMX effect, which decides *which* generators a rule has — see
    /// `publishSlots`, where changing it counts as changing the kind.
    dmx::EffectKind slotsEffect_ = dmx::EffectKind::Level;
    /// And the color mode, for the same reason: one color chip or three component chips.
    trigger::ColorMode slotsColorMode_ = trigger::ColorMode::Palette;

    /// Where a color picker's three sliders are, by slot.
    ///
    /// **Held rather than derived from the color**, because the derivation is lossy at both
    /// ends: black has no hue and a grey has no saturation, so re-reading them off the swatch
    /// would snap the sliders to red the moment somebody dragged brightness to zero. Cleared
    /// with the rows, so a different rule's picker starts from that rule's own color.
    struct Hsv {
        float hue = 0.0f;
        float saturation = 100.0f;
        float brightness = 100.0f;
    };
    std::unordered_map<int, Hsv> pickedColors_;
    /// And the same for the palette's own swatches, by entry. Same reasoning, same lifetime:
    /// cleared with the rows.
    std::unordered_map<int, Hsv> pickedPalette_;

    /// Whether a color picker's popup is on the screen, from `PopupWindow::is-open`. See
    /// `setPickerOpen`.
    bool pickerOpen_ = false;
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
    /// One line of the event log: when (seconds on the runner's clock, as the log shows it), the
    /// rule by the name the list shows, what went out, and whether it was muted — three columns
    /// and a suffix drawn apart (HANDOFF §0.5).
    struct LogEntry {
        std::string when;
        std::string who;
        std::string message;
        bool muted = false;
    };
    std::vector<LogEntry> log_;
    /// The last message any rule sent, for the main window's TRIGGERS row. The editor's own "last
    /// sent" bar is gone (HANDOFF §0.5): the log says it.
    std::string lastFiredAnywhere_;
};

} // namespace takt4::ui
