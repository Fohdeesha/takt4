#pragma once

#include "core/dmx/fixture.hpp"
#include "core/output/output_runner.hpp"
#include "core/trigger/rule.hpp"

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
    /// §5.7's per-rule mute, from the window rather than from a control surface. **Live state,
    /// not configuration**: it goes straight to the output thread and is never saved, for the
    /// reason `trigger::Rule::muted` gives — a preset that loaded silent would look exactly
    /// like a preset that did not load.
    void setMuted(bool on);
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

    // --- the lighting half, for a rule whose send kind is DMX --------------------------
    //
    // One fixture or group named or un-named, and "none" — which for a DMX rule is a rule
    // that reaches nothing and says so, unlike an empty *output* list. See
    // `dmx::resolveFixtures` for why the two defaults are opposites.
    void setFixtureChosen(const std::string& name, bool chosen);
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

    /// Told by each picker's `PopupWindow` as it opens and closes, and the whole of it is
    /// this: **a repeater is not rebuilt while one of its items owns an open popup.**
    ///
    /// A picker lives inside the repeated item its swatch belongs to, so rebuilding that
    /// repeater destroys the popup — and the slider the operator is holding goes with it.
    /// That is the crash of 2026-09-16; `pickingColor_` is the other half. A rebuild asked
    /// for while one is open is not dropped, it waits: `rowsDirty_` stays set and `tick`
    /// takes it on the first redraw after the picker closes.
    void setPickerOpen(bool open);
    /// Whether one is open, which is what `rebuildRows` asks. Public so a test can find a
    /// swatch by clicking until a picker opens rather than by holding a coordinate that would
    /// rot the first time a row moved.
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
    /// `slot` indexes what the window is showing, which `slotConfigs()` decides.
    void pickSlotKind(int slot, int kind);
    void setSlotPool(int slot, bool list);
    void setSlotRange(int slot, const std::string& text);
    void setSlotValues(int slot, const std::string& text);
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
    /// What fixtures and groups this rig has, so the editor can say which of a rule's names
    /// reach something. Only the names and groups are needed; the addressing is the patch
    /// editor's business.
    std::vector<dmx::Fixture> patch_;

    slint::ComponentHandle<RulesWindow> window_;
    std::shared_ptr<slint::VectorModel<RuleRow>> listModel_;
    std::shared_ptr<slint::VectorModel<OutputChoice>> choiceModel_;
    std::shared_ptr<slint::VectorModel<OutputChoice>> fixtureModel_;
    std::shared_ptr<slint::VectorModel<SlotRow>> slotModel_;
    /// The palette swatches. A model of its own rather than a field of `SlotRow`, because a
    /// rule has at most one color generator and a repeater nested inside a repeater's own
    /// struct is not a thing Slint models do.
    std::shared_ptr<slint::VectorModel<PaletteEntry>> paletteModel_;
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
    /// The live per-rule gestures, by rule id: muted, and the interval multiplier.
    ///
    /// **Mirrored here rather than read back from the rules**, for the reason `firesSeen_`
    /// gives: the live rules belong to the output thread and are safe to read only while it
    /// is stopped. Keyed by id so that deleting a rule from the middle does not shift every
    /// state below it onto a different rule.
    ///
    /// Not saved. A preset load brings new ids, which is exactly when these should start
    /// again — see `trigger::Rule::reset`, which clears the same two on the other side.
    std::unordered_map<std::string, bool> mutedSeen_;
    std::unordered_map<std::string, double> rateSeen_;

    /// Which rule, and which send kind, the generator rows currently on screen were built
    /// for. When either changes the rows are rebuilt rather than updated in place, so the
    /// text boxes come back *bound* — see `publishSlots`, which explains why a box that has
    /// been typed into stops following the model and what that looked like to the operator.
    std::string slotsBuiltFor_;
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

    /// Set by a publisher that found a row it could not honestly update in place, and
    /// consumed by `tick`, which rebuilds both repeaters.
    ///
    /// **Deferred rather than done there and then**, because a publisher runs *inside* the
    /// callback of the very widget being replaced: the dropdown just picked from, the box
    /// Enter was pressed in. One redraw later is 33 ms, which nobody sees, and it means no
    /// element is ever destroyed from within its own handler.
    bool rowsDirty_ = false;
    /// Set while a color picker's own slider is what is changing the row, and read by
    /// `publishSlots` and `publishPalette` when they decide whether the repeater has to be
    /// built again.
    ///
    /// **This is the 2026-09-16 crash.** *"I moved the hue slider and it completely
    /// crashed."* The picker is a `PopupWindow` belonging to a repeated item, so rebuilding
    /// the repeater destroys the popup — and the row the drag changes is, of course, a row
    /// that changed, so `rowsNeedRebuild` said yes on every pixel of the drag and the next
    /// redraw tore down the popup and the slider inside it while the pointer still had it.
    /// Measured: ninety resets for a ninety-pixel drag, on both pickers.
    ///
    /// A rebuild exists to re-bind a box somebody typed into (see `model_rows.hpp`). A slider
    /// the operator is holding is the opposite case: the value in the row came *from* that
    /// element, so the element is already showing it and there is nothing to restore. Hence a
    /// flag rather than a comparator that ignores the color fields — `SlotRow::fixed` is a
    /// text box on every slot that is not a color, and that box does need the rebuild.
    bool pickingColor_ = false;
    /// Whether a color picker's popup is on the screen, from `PopupWindow::is-open`.
    ///
    /// The second half of the same crash, and the part that holds for causes this controller
    /// has not thought of: **no repeater is rebuilt while one of its items owns an open
    /// popup.** `rowsDirty_` stays set, so the rebuild happens on the first redraw after the
    /// picker closes rather than being lost.
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
